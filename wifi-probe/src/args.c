#include "wifi-probe.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wp_parse_args(int argc, char **argv, struct wp_config *cfg, int *daemon,
                  char *fmt, char *group, char *period)
{
    static struct option long_opts[] = {
        {"daemon",         no_argument,       0, 'd'},
        {"output",         required_argument, 0, 'c'},
        {"group-by",       required_argument, 0, 'g'},
        {"period",         required_argument, 0, 't'},
        {"interface",      required_argument, 0, 'i'},
        {"database",       required_argument, 0, 'D'},
        {"session-gap",    required_argument, 0, 'G'},
        {"commit",         required_argument, 0, 'C'},
        {"retention",      required_argument, 0, 'R'},
        {"anonymize",      required_argument, 0, 'A'},
        {"salt",           required_argument, 0, 'S'},
        {"capture-filter", required_argument, 0, 'f'},
        {"version",        no_argument,       0, 'V'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "dc:g:t:i:D:G:C:R:A:S:f:Vh",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': *daemon = 1; break;
        case 'c': strncpy(fmt, optarg, 15); break;
        case 'g': strncpy(group, optarg, 15); break;
        case 't': strncpy(period, optarg, 15); break;
        case 'i': strncpy(cfg->iface, optarg, sizeof(cfg->iface) - 1); break;
        case 'D': strncpy(cfg->db_path, optarg, sizeof(cfg->db_path) - 1); break;
        case 'G': cfg->session_gap = atoi(optarg); break;
        case 'C': cfg->commit_interval = atoi(optarg); break;
        case 'R': cfg->retention_days = atoi(optarg); break;
        case 'A': cfg->anonymize_mac = atoi(optarg); break;
        case 'S': strncpy(cfg->mac_salt, optarg, sizeof(cfg->mac_salt) - 1); break;
        case 'f': strncpy(cfg->capture_filter, optarg, sizeof(cfg->capture_filter) - 1); break;
        case 'V': printf("wifi-probe %s\n", WP_VERSION); exit(0); break;
        case 'h': return -1;
        default:  return -1;
        }
    }
    return 0;
}
