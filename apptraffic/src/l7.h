#ifndef L7_H
#define L7_H

#include <stdint.h>
#include <stddef.h>

/* Load the L7 signature rule file. Returns the number of rules loaded. */
int l7_load(const char *path);

/*
 * Match the first payload of a client->server flow (protocol + destination
 * port) against the rule table. On a match fills app/cat and returns 1.
 * proto is IPPROTO_TCP / IPPROTO_UDP.
 */
int l7_match(uint8_t proto, uint16_t dst_port, const uint8_t *payload, int len,
             const char **app, const char **cat);

#endif /* L7_H */
