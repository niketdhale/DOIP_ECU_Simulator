#ifndef DOIP_UDP_H
#define DOIP_UDP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include <stdint.h>
#include <stdbool.h>

int doip_udp_init(void);
int doip_udp_poll(DoIP_RxIndication rx_cb);
void doip_udp_deinit(void);

/* Periodic Announcement */
void doip_udp_tick(uint32_t now_ms);

#endif /* DOIP_UDP_H */