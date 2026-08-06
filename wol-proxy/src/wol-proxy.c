/*
 * wol-proxy - Wake-on-LAN broadcast relay for WireGuard
 *
 * A phone inside a WireGuard network sends a magic packet to the LAN
 * broadcast address (e.g. 192.168.100.255:9). The router receives the
 * packet on wg0, but the Linux kernel does not forward such
 * subnet-directed broadcasts to the LAN interface, so the target PC
 * never sees it.
 *
 * This daemon listens on UDP port 9, extracts the target MAC address
 * from the received magic packet, builds a fresh magic packet and sends
 * it to the LAN broadcast address out of the LAN interface, where the
 * PC's network card can pick it up.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WOL_SYNC_LEN    6
#define WOL_MAC_LEN     6
#define WOL_MAC_REPEATS 16
#define WOL_PKT_LEN     (WOL_SYNC_LEN + WOL_MAC_LEN * WOL_MAC_REPEATS)

#define DEFAULT_PORT    9
#define DEFAULT_IFNAME  "eth0"
#define MAX_PAYLOAD     4096
#define DUP_MAX         32
#define DUP_WINDOW_SECS 5

struct wol_config {
	char     bind_addr[INET_ADDRSTRLEN];
	char     lan_ifname[IFNAMSIZ];
	char     lan_broadcast[INET_ADDRSTRLEN];
	char     allow_cidr[64];
	uint16_t port;
	bool     verbose;
};

struct lan_info {
	int      ifindex;
	uint32_t addr;
	uint32_t netmask;
	uint32_t broadcast;
};

struct dup_entry {
	uint32_t src;
	uint8_t  mac[WOL_MAC_LEN];
	time_t   seen;
};

static void copy_str(char *dst, size_t dstsz, const char *src)
{
	strncpy(dst, src, dstsz - 1);
	dst[dstsz - 1] = '\0';
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"WireGuard -> LAN Wake-on-LAN broadcast proxy.\n"
		"\n"
		"  -b, --bind ADDR        UDP address to listen on (default: 0.0.0.0)\n"
		"  -p, --port PORT        UDP port to listen/send on (default: 9)\n"
		"  -i, --ifname IFACE     LAN interface to send magic packets out of (default: eth0)\n"
		"  -B, --broadcast ADDR   LAN broadcast address override\n"
		"  -a, --allow CIDR       only relay requests from this source (default: 0.0.0.0/0)\n"
		"  -v, --verbose          log every received packet\n"
		"  -h, --help             show this help\n",
		prog);
}

static int parse_cidr(const char *s, uint32_t *addr, uint32_t *mask)
{
	char buf[64];
	char *slash;
	char *end = NULL;
	int plen;
	uint32_t m;

	copy_str(buf, sizeof(buf), s);
	slash = strchr(buf, '/');

	if (slash) {
		*slash++ = '\0';
		plen = (int)strtol(slash, &end, 10);
		if (!end || *end != '\0' || plen < 0 || plen > 32)
			return -1;
	} else {
		plen = 32;
	}

	if (inet_pton(AF_INET, buf, addr) != 1)
		return -1;

	m = plen == 0 ? 0 : (uint32_t)0xffffffffU << (32 - plen);
	*mask = htonl(m);
	return 0;
}

static bool parse_magic(const uint8_t *buf, size_t len, uint8_t mac[WOL_MAC_LEN])
{
	int i;

	if (len < WOL_PKT_LEN)
		return false;

	for (i = 0; i < WOL_SYNC_LEN; i++) {
		if (buf[i] != 0xff)
			return false;
	}

	memcpy(mac, buf + WOL_SYNC_LEN, WOL_MAC_LEN);
	for (i = 1; i < WOL_MAC_REPEATS; i++) {
		if (memcmp(buf + WOL_SYNC_LEN + i * WOL_MAC_LEN,
			   mac, WOL_MAC_LEN) != 0)
			return false;
	}

	return true;
}

static void build_magic(const uint8_t mac[WOL_MAC_LEN],
			uint8_t out[WOL_PKT_LEN])
{
	int i;

	memset(out, 0xff, WOL_SYNC_LEN);
	for (i = 0; i < WOL_MAC_REPEATS; i++)
		memcpy(out + WOL_SYNC_LEN + i * WOL_MAC_LEN, mac, WOL_MAC_LEN);
}

static int get_lan_info(const char *ifname, struct lan_info *info)
{
	struct ifaddrs *ifa = NULL;
	struct ifaddrs *cur;
	int ret = -1;

	memset(info, 0, sizeof(*info));
	info->ifindex = (int)if_nametoindex(ifname);
	if (info->ifindex == 0)
		return -1;

	if (getifaddrs(&ifa) != 0)
		return -1;

	for (cur = ifa; cur; cur = cur->ifa_next) {
		struct sockaddr_in *sin;
		struct sockaddr_in *nm;
		struct sockaddr_in *bc;

		if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET)
			continue;
		if (strcmp(cur->ifa_name, ifname) != 0)
			continue;

		sin = (struct sockaddr_in *)cur->ifa_addr;
		info->addr = sin->sin_addr.s_addr;

		if (cur->ifa_netmask) {
			nm = (struct sockaddr_in *)cur->ifa_netmask;
			info->netmask = nm->sin_addr.s_addr;
		} else {
			info->netmask = htonl(0xffffff00U);
		}

		if (cur->ifa_broadaddr) {
			bc = (struct sockaddr_in *)cur->ifa_broadaddr;
			info->broadcast = bc->sin_addr.s_addr;
		} else {
			info->broadcast =
				(info->addr & info->netmask) | ~info->netmask;
		}

		ret = 0;
		break;
	}

	freeifaddrs(ifa);
	return ret;
}

static bool source_allowed(uint32_t src, uint32_t allow_addr, uint32_t allow_mask)
{
	return (src & allow_mask) == (allow_addr & allow_mask);
}

static bool dup_check(struct dup_entry *tab, size_t n, uint32_t src,
		      const uint8_t mac[WOL_MAC_LEN], time_t now)
{
	size_t i;
	size_t oldest = 0;
	time_t oldest_t = tab[0].seen;

	for (i = 0; i < n; i++) {
		if (tab[i].seen != 0 &&
		    tab[i].src == src &&
		    memcmp(tab[i].mac, mac, WOL_MAC_LEN) == 0 &&
		    (now - tab[i].seen) < DUP_WINDOW_SECS)
			return true;
		if (tab[i].seen < oldest_t) {
			oldest = i;
			oldest_t = tab[i].seen;
		}
	}

	tab[oldest].src = src;
	memcpy(tab[oldest].mac, mac, WOL_MAC_LEN);
	tab[oldest].seen = now;
	return false;
}

static int send_magic(const struct wol_config *cfg, const struct lan_info *lan,
		      uint32_t target, const uint8_t pkt[WOL_PKT_LEN])
{
	struct sockaddr_in dst;
	char addrbuf[INET_ADDRSTRLEN];
	int fd;
	int one = 1;
	int ret;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "wol-proxy: socket() failed: %s\n",
			strerror(errno));
		return -1;
	}

	(void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

	/*
	 * Pin the egress interface with SO_BINDTODEVICE instead of sending
	 * an IP_PKTINFO control message.  On kernels before ~4.19 the
	 * IP_PKTINFO ipi_spec_dst field is unconditionally taken as the
	 * source address for the route lookup (ip_cmsg_send() in
	 * net/ipv4/ip_sockglue.c), and __ip_route_output_key() then fails
	 * sendmsg() with EINVAL whenever that address is not a valid local
	 * address.  SO_BINDTODEVICE only pins the output device and lets the
	 * kernel pick a valid source address itself.
	 */
	if (lan->ifindex > 0)
		(void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
				 cfg->lan_ifname, strlen(cfg->lan_ifname));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(cfg->port);
	dst.sin_addr.s_addr = target;

	ret = (int)sendto(fd, pkt, WOL_PKT_LEN, 0,
			  (struct sockaddr *)&dst, sizeof(dst));
	if (ret < 0) {
		if (!inet_ntop(AF_INET, &dst.sin_addr, addrbuf,
			       sizeof(addrbuf)))
			strcpy(addrbuf, "?");
		fprintf(stderr, "wol-proxy: sendmsg to %s:%u failed: %s\n",
			addrbuf, (unsigned)cfg->port, strerror(errno));
	}

	close(fd);
	return ret;
}

static int run(const struct wol_config *cfg)
{
	struct sockaddr_in bindaddr;
	uint32_t allow_addr = 0;
	uint32_t allow_mask = 0;
	int fd;
	int one = 1;
	struct dup_entry dup_tab[DUP_MAX] = { 0 };
	int lo_index;

	if (parse_cidr(cfg->allow_cidr, &allow_addr, &allow_mask) != 0) {
		fprintf(stderr,
			"wol-proxy: invalid --allow '%s', allowing all sources\n",
			cfg->allow_cidr);
		allow_addr = 0;
		allow_mask = 0;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "wol-proxy: socket() failed: %s\n",
			strerror(errno));
		return 1;
	}

	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	(void)setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));

	memset(&bindaddr, 0, sizeof(bindaddr));
	bindaddr.sin_family = AF_INET;
	bindaddr.sin_port = htons(cfg->port);
	if (inet_pton(AF_INET, cfg->bind_addr, &bindaddr.sin_addr) != 1) {
		fprintf(stderr, "wol-proxy: invalid bind address '%s'\n",
			cfg->bind_addr);
		close(fd);
		return 1;
	}

	if (bind(fd, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
		fprintf(stderr, "wol-proxy: bind %s:%u failed: %s\n",
			cfg->bind_addr, (unsigned)cfg->port,
			strerror(errno));
		close(fd);
		return 1;
	}

	fprintf(stderr,
		"wol-proxy: listening on %s:%u, LAN iface %s, allow %s%s\n",
		cfg->bind_addr, (unsigned)cfg->port, cfg->lan_ifname,
		cfg->allow_cidr,
		cfg->lan_broadcast[0] ? ", broadcast override" : "");

	lo_index = (int)if_nametoindex("lo");

	for (;;) {
		uint8_t buf[MAX_PAYLOAD];
		uint8_t mac[WOL_MAC_LEN];
		uint8_t pkt[WOL_PKT_LEN];
		struct sockaddr_in peer;
		struct in_pktinfo *pi = NULL;
		struct cmsghdr *cmsg;
		char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
		struct iovec iov;
		struct msghdr msg;
		struct lan_info lan;
		uint32_t dst_addr = 0;
		uint32_t target;
		ssize_t n;
		char srcbuf[INET_ADDRSTRLEN];
		char dstbuf[INET_ADDRSTRLEN];
		char macbuf[18];
		struct in_addr target_addr;

		memset(&peer, 0, sizeof(peer));
		memset(cbuf, 0, sizeof(cbuf));
		iov.iov_base = buf;
		iov.iov_len = sizeof(buf);
		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &peer;
		msg.msg_namelen = sizeof(peer);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);

		n = recvmsg(fd, &msg, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "wol-proxy: recvmsg() failed: %s\n",
				strerror(errno));
			break;
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
		     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == IPPROTO_IP &&
			    cmsg->cmsg_type == IP_PKTINFO) {
				pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
				break;
			}
		}
		if (pi)
			dst_addr = pi->ipi_addr.s_addr;

		if (!inet_ntop(AF_INET, &peer.sin_addr, srcbuf,
			       sizeof(srcbuf)))
			strcpy(srcbuf, "?");

		/* The kernel loops our own broadcast back through "lo";
		 * without this check the daemon would relay its own packets
		 * forever.  Packets from the outside never arrive on "lo".
		 */
		if (pi && lo_index > 0 && pi->ipi_ifindex == lo_index) {
			if (cfg->verbose)
				fprintf(stderr,
					"wol-proxy: ignored own loopback packet from %s\n",
					srcbuf);
			continue;
		}

		if (!source_allowed(peer.sin_addr.s_addr,
				    allow_addr, allow_mask)) {
			if (cfg->verbose)
				fprintf(stderr,
					"wol-proxy: ignored packet from %s (not in %s)\n",
					srcbuf, cfg->allow_cidr);
			continue;
		}

		if (!parse_magic(buf, (size_t)n, mac)) {
			if (cfg->verbose)
				fprintf(stderr,
					"wol-proxy: ignored non-magic packet from %s:%u (%zd bytes)\n",
					srcbuf,
					(unsigned)ntohs(peer.sin_port), n);
			continue;
		}

		snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
			 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		if (dup_check(dup_tab, DUP_MAX, peer.sin_addr.s_addr,
			      mac, time(NULL))) {
			if (cfg->verbose)
				fprintf(stderr,
					"wol-proxy: ignored duplicate magic packet for %s from %s within %ds\n",
					macbuf, srcbuf, DUP_WINDOW_SECS);
			continue;
		}

		if (get_lan_info(cfg->lan_ifname, &lan) != 0) {
			fprintf(stderr,
				"wol-proxy: no IPv4 address on '%s' yet, will retry\n",
				cfg->lan_ifname);
			continue;
		}
		if (cfg->verbose) {
			char abuf[INET_ADDRSTRLEN];
			char bbuf[INET_ADDRSTRLEN];
			struct in_addr a;
			struct in_addr b;

			a.s_addr = lan.addr;
			b.s_addr = lan.broadcast;
			if (!inet_ntop(AF_INET, &a, abuf, sizeof(abuf)))
				strcpy(abuf, "?");
			if (!inet_ntop(AF_INET, &b, bbuf, sizeof(bbuf)))
				strcpy(bbuf, "?");
			fprintf(stderr,
				"wol-proxy: %s addr=%s broadcast=%s ifindex=%d\n",
				cfg->lan_ifname,
				abuf, bbuf, lan.ifindex);
		}

		/* Prefer the broadcast address from the received packet when
		 * it belongs to the LAN subnet, otherwise use the LAN's own
		 * broadcast address (or the configured override).
		 */
		target = lan.broadcast;
		if (cfg->lan_broadcast[0]) {
			struct in_addr bc;

			if (inet_pton(AF_INET, cfg->lan_broadcast, &bc) == 1)
				target = bc.s_addr;
		} else if (dst_addr &&
			   (dst_addr & lan.netmask) ==
			   (lan.addr & lan.netmask) &&
			   dst_addr != lan.addr) {
			target = dst_addr;
		}

		build_magic(mac, pkt);
		if (send_magic(cfg, &lan, target, pkt) >= 0) {
			target_addr.s_addr = target;
			if (!inet_ntop(AF_INET, &target_addr,
				       dstbuf, sizeof(dstbuf)))
				strcpy(dstbuf, "?");
			fprintf(stderr,
				"wol-proxy: relaying magic packet for %s from %s via %s -> %s:%u\n",
				macbuf, srcbuf, cfg->lan_ifname,
				dstbuf, (unsigned)cfg->port);
		}
	}

	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "bind",      required_argument, NULL, 'b' },
		{ "port",      required_argument, NULL, 'p' },
		{ "ifname",    required_argument, NULL, 'i' },
		{ "broadcast", required_argument, NULL, 'B' },
		{ "allow",     required_argument, NULL, 'a' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct wol_config cfg;
	char *end = NULL;
	long port;
	int opt;

	memset(&cfg, 0, sizeof(cfg));
	copy_str(cfg.bind_addr, sizeof(cfg.bind_addr), "0.0.0.0");
	copy_str(cfg.lan_ifname, sizeof(cfg.lan_ifname), DEFAULT_IFNAME);
	copy_str(cfg.allow_cidr, sizeof(cfg.allow_cidr), "0.0.0.0/0");
	cfg.port = DEFAULT_PORT;

	while ((opt = getopt_long(argc, argv, "b:p:i:B:a:vh",
				  longopts, NULL)) != -1) {
		switch (opt) {
		case 'b':
			copy_str(cfg.bind_addr, sizeof(cfg.bind_addr), optarg);
			break;
		case 'p':
			port = strtol(optarg, &end, 10);
			if (!end || *end != '\0' || port < 1 || port > 65535) {
				fprintf(stderr, "wol-proxy: invalid port '%s'\n",
					optarg);
				return 1;
			}
			cfg.port = (uint16_t)port;
			break;
		case 'i':
			copy_str(cfg.lan_ifname, sizeof(cfg.lan_ifname), optarg);
			break;
		case 'B':
			copy_str(cfg.lan_broadcast, sizeof(cfg.lan_broadcast),
				 optarg);
			break;
		case 'a':
			copy_str(cfg.allow_cidr, sizeof(cfg.allow_cidr), optarg);
			break;
		case 'v':
			cfg.verbose = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return run(&cfg);
}
