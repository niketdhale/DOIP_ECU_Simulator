#ifndef DOIP_TCP_H
#define DOIP_TCP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "transport/doip_uds.h"
#include "transport/doip_io.h"
#if DOIP_ENABLE_ISO_TP
#include "transport/doip_isotp.h"
#endif
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>   /* INET6_ADDRSTRLEN */

typedef uint16_t doip_logical_addr_t;   /* 16-bit in both ISO 13400-2 editions */

/* Routing Activation Request (7 bytes; optional 4-byte OEM field not used) */
typedef struct __attribute__((packed)) {
    doip_logical_addr_t tester_logical_address;
    uint8_t             activation_type;
    uint32_t            reserved;
} doip_routing_act_req_t;

/* Routing Activation Response (9 bytes; optional 4-byte OEM field not used) */
typedef struct __attribute__((packed)) {
    doip_logical_addr_t tester_logical_address;
    doip_logical_addr_t ecu_logical_address;
    uint8_t             activation_code;
    uint32_t            reserved;
} doip_routing_act_res_t;

typedef enum { DOIP_TCP_STATE_IDLE, DOIP_TCP_STATE_CONNECTED, DOIP_TCP_STATE_ACTIVATED, DOIP_TCP_STATE_CLOSING } doip_tcp_state_t;

typedef struct {
    int                 fd;
    doip_io_t           io;                   /* I/O vtable — plain fd or TLS              */
    doip_tcp_state_t    state;
    doip_logical_addr_t tester_logical_addr;
    uint32_t            last_activity_ms;
    uint32_t            connect_ms;           /* Time the connection was accepted          */
    uint32_t            alive_check_sent_ms;  /* Non-zero while an Alive Check Req is pending */
    UdsClientContext_t  uds_ctx;              /* Per-client UDS state                      */
    bool                in_use;

    /* Partial-frame reassembly buffer — removes need for SO_RCVTIMEO */
    uint8_t             rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    size_t              rx_bytes;             /* Bytes accumulated so far                  */

    /* Delayed UDS response (fault injection: response_delay_ms) */
    uint8_t             pending_res[DOIP_MAX_PAYLOAD_SIZE];
    uint16_t            pending_len;          /* > 0 while a response is being held back   */
    uint32_t            pending_due_ms;       /* When the held response is released        */
    uint32_t            pending_next78_ms;    /* When the next 7F xx 78 is due             */

    /* Peer address string for logging (IPv4 or IPv6) */
    char                peer_ip[INET6_ADDRSTRLEN];

#if DOIP_ENABLE_ISO_TP
    doip_isotp_ctx_t    isotp_ctx;            /* Per-client ISO-TP reassembly/segmentation */
#endif
} doip_client_t;

#define DOIP_MAX_TCP_CLIENTS 5

/* All server state (listening socket, client slots, TLS context) lives in the
 * instance; see core/doip_context.h. */
int     doip_tcp_init(DoIP_Handle_t *h);
int     doip_tcp_poll(DoIP_Handle_t *h);
void    doip_tcp_tick(DoIP_Handle_t *h, uint32_t now_ms);     /* Alive Check, timeouts, delayed responses */
uint8_t doip_tcp_get_client_count(const DoIP_Handle_t *h);    /* Currently connected clients              */
void    doip_tcp_deinit(DoIP_Handle_t *h);

#endif /* DOIP_TCP_H */