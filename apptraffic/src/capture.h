#ifndef CAPTURE_H
#define CAPTURE_H

#include "apptraffic.h"

int capture_init(const char *iface);
void capture_stop(void);
int capture_is_active(void);
void capture_set_dns_callback(apptraffic_dns_cb cb);
void capture_set_sni_callback(apptraffic_sni_cb cb);
void *capture_run(void *arg);

#endif /* CAPTURE_H */
