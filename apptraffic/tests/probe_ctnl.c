/*
 * probe_ctnl.c - check whether the running kernel supports conntrack netlink.
 *
 * Tries a netlink dump of the IPv4 conntrack table. Prints how many entries
 * were returned (or an error). Used to decide whether apptraffic can switch to
 * a netlink-based conntrack reader vs. keep the /proc fallback.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>

int main(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        return 1;
    }

    char buf[128];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nfgenmsg));
    nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = 1;

    struct nfgenmsg *nfg = (struct nfgenmsg *)NLMSG_DATA(nlh);
    nfg->nfgen_family = AF_INET;
    nfg->version = NFNETLINK_V0;
    nfg->res_id = 0;

    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;
    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&dst,
               sizeof(dst)) < 0) {
        perror("sendto");
        return 1;
    }

    int count = 0;
    char rbuf[65536];
    while (1) {
        int n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n < 0) { perror("recv"); break; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
             NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) {
                printf("done, count=%d\n", count);
                close(fd);
                return count > 0 ? 0 : 2;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
                printf("nlmsg error (%d)\n", e->error);
                close(fd);
                return 2;
            }
            count++;
        }
    }
    close(fd);
    printf("entries=%d\n", count);
    return count > 0 ? 0 : 2;
}
