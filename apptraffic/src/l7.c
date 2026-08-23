/*
 * l7.c - lightweight L7 protocol signature matcher.
 *
 * Identifies applications that do not carry a useful SNI / HTTP Host / DNS
 * name (SSH, RDP, QQ/OICQ, P2P, ...) by matching the first payload of a
 * client->server flow against a small rule table. It is deliberately tiny and
 * dependency-free (unlike nDPI) so it fits the constrained board; the rule set
 * is a text file that can be extended.
 *
 * Copyright (C) 2024
 * Licensed under GPL-2.0
 */

#include "l7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>

#define L7_TCP 0
#define L7_UDP 1
#define L7_ANY 2

struct l7_rule {
    int             proto;      /* L7_TCP / L7_UDP / L7_ANY */
    int             port;       /* destination port, 0 = any */
    int             offset;     /* payload offset to match, -1 = no magic */
    uint8_t         magic[16];
    int             magic_len;
    char            app[64];
    char            cat[32];
    struct l7_rule *next;
};

static struct l7_rule *g_rules;
static pthread_mutex_t rule_mutex = PTHREAD_MUTEX_INITIALIZER;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int l7_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: no L7 rules file: %s\n", path);
        return -1;
    }

    pthread_mutex_lock(&rule_mutex);
    while (g_rules) {
        struct l7_rule *n = g_rules->next;
        free(g_rules);
        g_rules = n;
    }

    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        if (!line[0] || line[0] == '#') continue;

        /* proto,dstport,offset,hexmagic,app,category */
        char *proto_s = strtok(line, ",");
        char *port_s  = strtok(NULL, ",");
        char *off_s   = strtok(NULL, ",");
        char *magic_s = strtok(NULL, ",");
        char *app_s   = strtok(NULL, ",");
        char *cat_s   = strtok(NULL, ",");
        if (!proto_s || !port_s || !app_s) continue;

        struct l7_rule *r = calloc(1, sizeof(*r));
        if (!r) continue;

        if (strcmp(proto_s, "tcp") == 0) r->proto = L7_TCP;
        else if (strcmp(proto_s, "udp") == 0) r->proto = L7_UDP;
        else r->proto = L7_ANY;
        r->port = (!port_s || strcmp(port_s, "*") == 0) ? 0 : atoi(port_s);
        r->offset = -1;
        r->magic_len = 0;

        if (off_s && magic_s && strcmp(magic_s, "*") != 0) {
            r->offset = (strcmp(off_s, "*") == 0) ? 0 : atoi(off_s);
            const char *p = magic_s;
            int hi = -1, n = 0;
            for (; *p && n < (int)sizeof(r->magic); p++) {
                if (*p == ' ') continue;
                int v = hexval(*p);
                if (v < 0) { n = 0; break; }
                if (hi < 0) {
                    hi = v;
                } else {
                    r->magic[n++] = (uint8_t)((hi << 4) | v);
                    hi = -1;
                }
            }
            r->magic_len = (hi < 0) ? n : 0;
        }

        strncpy(r->app, app_s, sizeof(r->app) - 1);
        r->app[sizeof(r->app) - 1] = '\0';
        strncpy(r->cat, cat_s ? cat_s : "General", sizeof(r->cat) - 1);
        r->cat[sizeof(r->cat) - 1] = '\0';

        r->next = g_rules;
        g_rules = r;
        count++;
    }

    fclose(fp);
    pthread_mutex_unlock(&rule_mutex);
    return count;
}

int l7_match(uint8_t proto, uint16_t dst_port, const uint8_t *payload, int len,
             const char **app, const char **cat)
{
    if (!payload || len <= 0) return 0;

    pthread_mutex_lock(&rule_mutex);
    for (struct l7_rule *r = g_rules; r; r = r->next) {
        if (r->proto == L7_TCP && proto != IPPROTO_TCP) continue;
        if (r->proto == L7_UDP && proto != IPPROTO_UDP) continue;
        if (r->port && r->port != dst_port) continue;
        if (r->magic_len > 0) {
            if (r->offset < 0 || r->offset + r->magic_len > len)
                continue;
            if (memcmp(payload + r->offset, r->magic, r->magic_len) != 0)
                continue;
        }
        *app = r->app;
        *cat = r->cat;
        pthread_mutex_unlock(&rule_mutex);
        return 1;
    }
    pthread_mutex_unlock(&rule_mutex);
    return 0;
}
