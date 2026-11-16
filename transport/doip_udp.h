#ifndef DOIP_UDP_H
#define DOIP_UDP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "doip_api.h"          /* DoIP_Handle_t */
#include <stdint.h>
#include <stdbool.h>

/* The socket and announcement state live in the instance (core/doip_context.h) */
int  doip_udp_init(DoIP_Handle_t *h);
int  doip_udp_poll(DoIP_Handle_t *h);
void doip_udp_deinit(DoIP_Handle_t *h);

/* Periodic Announcement */
void doip_udp_tick(DoIP_Handle_t *h, uint32_t now_ms);

#endif /* DOIP_UDP_H */
