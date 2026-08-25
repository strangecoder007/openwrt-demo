#include "wifi-probe.h"
#include <pcap.h>
#include <stdio.h>
#include <string.h>

static pcap_t *g_pcap = NULL;

static void on_frame(u_char *user, const struct pcap_pkthdr *hdr, const u_char *bytes)
{
    (void)user;
    wp_capture_process_frame(bytes, (int)hdr->caplen, time(NULL));
}

int wp_capture_init(const char *iface, const char *capture_filter)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    const char *dev = (iface && iface[0]) ? iface : "any";
    g_pcap = pcap_open_live(dev, 4096, 1, 500, errbuf);
    if (!g_pcap) { fprintf(stderr, "pcap_open_live: %s\n", errbuf); return -1; }

    const char *filter = (capture_filter && strcmp(capture_filter, "probe") == 0)
        ? "type mgt subtype probe-req"
        : "type mgt subtype probe-req or type mgt subtype beacon";

    struct bpf_program fp;
    if (pcap_compile(g_pcap, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "pcap_compile: %s\n", pcap_geterr(g_pcap));
        pcap_close(g_pcap); g_pcap = NULL; return -1;
    }
    if (pcap_setfilter(g_pcap, &fp) == -1) {
        fprintf(stderr, "pcap_setfilter: %s\n", pcap_geterr(g_pcap));
        pcap_freecode(&fp); pcap_close(g_pcap); g_pcap = NULL; return -1;
    }
    pcap_freecode(&fp);
    return 0;
}

void *wp_capture_run(void *arg)
{
    (void)arg;
    if (!g_pcap) return NULL;
    while (g_running) pcap_dispatch(g_pcap, 10, on_frame, NULL);
    return NULL;
}

void wp_capture_stop(void)
{
    g_running = 0;
    if (g_pcap) { pcap_breakloop(g_pcap); pcap_close(g_pcap); g_pcap = NULL; }
}

int wp_capture_active(void){ return g_pcap != NULL; }
