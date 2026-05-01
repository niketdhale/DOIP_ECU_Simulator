#ifndef DOIP_TCP_H
#define DOIP_TCP_H

#include "config/doip_config.h"
#include "core/doip_types.h"
#include "core/doip_api.h"
#include <stdint.h>
#include <stdbool.h>

#if (DOIP_LOGICAL_ADDR_SIZE == 2)
    typedef uint16_t doip_logical_addr_t;
#else
    typedef uint32_t doip_logical_addr_t;
#endif

/* Routing Activation Request Payload */
typedef struct __attribute__((packed)) {
    doip_logical_addr_t tester_logical_address;
    uint8_t             activation_type;
    uint32_t            reserved; /* Fixed 4 bytes (0x00000000) */
} doip_routing_act_req_t;

/* Routing Activation Response Payload */
typedef struct __attribute__((packed)) {
    doip_logical_addr_t ecu_logical_address;
    doip_logical_addr_t tester_logical_address;
    uint8_t             activation_code;
    uint8_t             reserved_align; /* 1-byte alignment padding */
    uint32_t            reserved;       /* Fixed 4 bytes (0x00000000) */
} doip_routing_act_res_t;

/* Client state machine */
typedef enum {
    DOIP_TCP_STATE_IDLE,
    DOIP_TCP_STATE_CONNECTED,
    DOIP_TCP_STATE_ACTIVATED,
    DOIP_TCP_STATE_CLOSING
} doip_tcp_state_t;

typedef struct {
    int                 fd;
    doip_logical_addr_t tester_logical_addr;
    doip_tcp_state_t    state;
    uint32_t            last_activity_ms;
} doip_client_t;

int doip_tcp_init(void);
int doip_tcp_poll(DoIP_RxIndication rx_cb);
void doip_tcp_deinit(void);

#endif /* DOIP_TCP_H */