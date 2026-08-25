#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

int wp_parse_args(int argc, char **argv, struct wp_config *cfg, int *daemon,
                  char *fmt, char *group, char *period);

int main(void)
{
    struct wp_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.iface, "mon0");
    strcpy(cfg.db_path, "/var/lib/wifi-probe");
    strcpy(cfg.mac_salt, "x");
    cfg.anonymize_mac = 1;
    cfg.session_gap = 300;
    cfg.retention_days = 30;
    cfg.commit_interval = 60;
    int daemon = 0;
    char fmt[16] = "json", group[16] = "device", period[16] = "today";
    char *argv[] = {"wifi-probe","-d","-i","mon0","-C","120","-R","15",
                    "-A","0","-f","probe",NULL};
    int rc = wp_parse_args(12, argv, &cfg, &daemon, fmt, group, period);
    if (rc != 0 || !daemon) { printf("daemon fail\n"); return 1; }
    if (strcmp(cfg.iface, "mon0") != 0) { printf("iface %s\n", cfg.iface); return 2; }
    if (cfg.commit_interval != 120) { printf("ci %d\n", cfg.commit_interval); return 3; }
    if (cfg.retention_days != 15) { printf("ret %d\n", cfg.retention_days); return 4; }
    if (cfg.anonymize_mac != 0) { printf("anon %d\n", cfg.anonymize_mac); return 5; }
    if (strcmp(cfg.capture_filter, "probe") != 0) { printf("filter %s\n", cfg.capture_filter); return 6; }
    printf("cli OK iface=%s ci=%d ret=%d anon=%d filter=%s\n",
           cfg.iface, cfg.commit_interval, cfg.retention_days, cfg.anonymize_mac, cfg.capture_filter);
    return 0;
}
