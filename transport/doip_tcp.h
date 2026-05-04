#ifndef DOIP_TCP_H
#define DOIP_TCP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "core/doip_api.h"
#include "transport/doip_uds.h"
#include "transport/doip_io.h"
#if DOIP_ENABLE_ISO_TP
#include "transport/doip_isotp.h"
#endif
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>   /* INET6_ADDRSTRLEN */

#if (DOIP_LOGICAL_ADDR_SIZE == 2)
    typedef uint16_t doip_logical_addr_t;
#else
    typedef uint32_t doip_logical_addr_t;
#endif

typedef struct __attribute__((packed)) {
    doip_logical_addr_t tester_logical_address;
    uint8_t             activation_type;
    uint32_t            reserved;
} doip_routing_act_req_t;

typedef struct __attribute__((packed)) {
    doip_logical_addr_t ecu_logical_address;
    doip_logical_addr_t tester_logical_address;
    uint8_t             activation_code;
    uint8_t             reserved_align;
    uint32_t            reserved;
} doip_routing_act_res_t;

typedef enum { DOIP_TCP_STATE_IDLE, DOIP_TCP_STATE_CONNECTED, DOIP_TCP_STATE_ACTIVATED, DOIP_TCP_STATE_CLOSING } doip_tcp_state_t;

typedef struct {
    int                 fd;
    doip_io_t           io;                   /* I/O vtable — plain fd or TLS              */
    doip_tcp_state_t    state;
    doip_logical_addr_t tester_logical_addr;
    uint32_t            last_activity_ms;
    uint32_t            alive_check_sent_ms;  /* Non-zero while an Alive Check Req is pending */
    UdsClientContext_t  uds_ctx;              /* Per-client UDS state                      */
    bool                in_use;

    /* Partial-frame reassembly buffer — removes need for SO_RCVTIMEO */
    uint8_t             rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    size_t              rx_bytes;             /* Bytes accumulated so far                  */

    /* Peer address string for logging (IPv4 or IPv6) */
    char                peer_ip[INET6_ADDRSTRLEN];

#if DOIP_ENABLE_ISO_TP
    doip_isotp_ctx_t    isotp_ctx;            /* Per-client ISO-TP reassembly/segmentation */
#endif
} doip_client_t;

#define DOIP_MAX_TCP_CLIENTS 5

int doip_tcp_init(void);
int doip_tcp_poll(DoIP_RxIndication rx_cb);
void doip_tcp_tick(uint32_t now_ms);      /* Server-initiated Alive Check + timeout */
uint8_t doip_tcp_get_client_count(void);  /* Count of currently active clients       */
void doip_tcp_deinit(void);

#if DOIP_ENABLE_TLS
/**
 * @brief Configure TLS parameters before calling doip_tcp_init().
 *
 * Must be called before doip_tcp_init() when DOIP_ENABLE_TLS is true.
 * Copies the string pointers — the caller must keep them valid until
 * doip_tcp_deinit() is called.
 */
void doip_tcp_tls_config(const char *cert_file, const char *key_file,
                          const char *ca_file,   bool verify_peer);
#endif

#endif /* DOIP_TCP_H */