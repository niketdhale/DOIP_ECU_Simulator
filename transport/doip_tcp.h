#ifndef DOIP_TCP_H
#define DOIP_TCP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "core/doip_api.h"
#include "transport/doip_uds.h" 
#include <stdint.h>
#include <stdbool.h>

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
    doip_tcp_state_t    state;
    doip_logical_addr_t tester_logical_addr;
    uint32_t            last_activity_ms;
    uint32_t            alive_check_sent_ms;  /* Non-zero while an Alive Check Req is pending */
    UdsClientContext_t  uds_ctx;   /* Per-Client UDS State */
    bool                in_use;
} doip_client_t;

#define DOIP_MAX_TCP_CLIENTS 5

int doip_tcp_init(void);
int doip_tcp_poll(DoIP_RxIndication rx_cb);
void doip_tcp_tick(uint32_t now_ms);      /* Server-initiated Alive Check + timeout */
uint8_t doip_tcp_get_client_count(void);  /* Count of currently active clients       */
void doip_tcp_deinit(void);

#endif /* DOIP_TCP_H */