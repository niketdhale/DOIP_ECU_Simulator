#ifndef DOIP_UDP_H
#define DOIP_UDP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "core/doip_api.h"

/* Initialize UDP socket for Vehicle Discovery (port 13400) */
int doip_udp_init(void);

/* Send Vehicle Announcement (Payload Type 0x0003) */
int doip_send_vehicle_announce(const doip_vehicle_announce_t *announce);

/* Poll for incoming DoIP UDP messages; calls rx_cb if data received */
int doip_udp_poll(DoIP_RxIndication rx_cb);

/* Handle specific request types internally */
void doip_udp_handle_request(uint16_t ptype, const uint8_t *data, uint32_t len);

/* Cleanup UDP resources */
void doip_udp_deinit(void);

/**
 * @brief Non-blocking tick for periodic Vehicle Announcement
 * @param now_ms Current monotonic time in milliseconds
 */
void doip_udp_tick(uint32_t now_ms);

/**
 * @brief Enable or disable periodic Vehicle Announcement at runtime
 */
void doip_udp_set_periodic_announce(bool enable);

#endif /* DOIP_UDP_H */