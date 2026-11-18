#include "state/doip_fsm.h"
#include "core/doip_context.h"
#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
#include <time.h>

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DoIP_Fsm_Init(DoIP_Handle_t *h) {
    h->fsm.current_state = DOIP_STATE_UNINIT;
    h->fsm.previous_state = DOIP_STATE_UNINIT;
    h->fsm.state_entry_time_ms = get_time_ms();
    h->fsm.s3_server_timeout_ms = h->config.s3_server_timeout_ms;
    h->fsm.routing_activated = false;
    h->fsm.current_uds_session = 0x01;
    h->fsm.malformed_packet_count = 0;
    h->fsm.timeout_violation_count = 0;

    /* Initialize the UDS layer and the transports of this instance */
    doip_uds_init(h);
    if (doip_udp_init(h) != 0 || doip_tcp_init(h) != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_FSM, "Transport initialization failed");
        doip_tcp_deinit(h);   /* release whichever socket did open */
        doip_udp_deinit(h);
        h->fsm.current_state = DOIP_STATE_ERROR;
        return -1;
    }

    h->fsm.current_state = DOIP_STATE_IDLE;
    LOG_INFO(DOIP_LOG_MODULE_FSM, "FSM initialized. S3=%u ms", h->fsm.s3_server_timeout_ms);
    return 0;
}

void DoIP_Fsm_DeInit(DoIP_Handle_t *h) {
    doip_uds_deinit();
    doip_tcp_deinit(h);
    doip_udp_deinit(h);
    h->fsm.current_state = DOIP_STATE_UNINIT;
    LOG_INFO(DOIP_LOG_MODULE_FSM, "FSM deinitialized");
}

static bool fsm_transition(DoIP_Handle_t *h, DoIP_State_t new_state) {
    if (h->fsm.current_state == new_state) return true;
    h->fsm.previous_state = h->fsm.current_state;
    h->fsm.current_state = new_state;
    h->fsm.state_entry_time_ms = get_time_ms();
    LOG_INFO(DOIP_LOG_MODULE_FSM, "State: %d -> %d", h->fsm.previous_state, new_state);
    if (h->config.on_state_change)
        h->config.on_state_change((uint8_t)h->fsm.previous_state, (uint8_t)new_state,
                                  h->config.user_context);
    return true;
}

void DoIP_Fsm_OnRoutingActivation(DoIP_Handle_t *h, bool activated) {
    h->fsm.routing_activated = activated;
    if (activated) {
        fsm_transition(h, DOIP_STATE_ROUTING_ACTIVE);
    } else {
        fsm_transition(h, DOIP_STATE_IDLE);
    }
}

void DoIP_Fsm_OnUdsSessionChange(DoIP_Handle_t *h, uint8_t session_id) {
    h->fsm.current_uds_session = session_id;
    if (session_id != 0x01) {
        fsm_transition(h, DOIP_STATE_DIAG_SESSION);
    } else {
        fsm_transition(h, DOIP_STATE_ROUTING_ACTIVE);
    }
}

void DoIP_Fsm_OnDiagnosticActivity(DoIP_Handle_t *h) {
    h->fsm.state_entry_time_ms = get_time_ms(); /* Refresh S3 */
}

void DoIP_Fsm_OnTcpDisconnect(DoIP_Handle_t *h) {
    LOG_WARN(DOIP_LOG_MODULE_FSM, "TCP disconnected. Forcing state reset.");
    h->fsm.routing_activated = false;
    h->fsm.current_uds_session = 0x01;
    fsm_transition(h, DOIP_STATE_IDLE);
}

void DoIP_Fsm_OnMalformedPacket(DoIP_Handle_t *h) {
    h->fsm.malformed_packet_count++;
    LOG_WARN(DOIP_LOG_MODULE_FSM, "Malformed packet detected (Count: %u)", h->fsm.malformed_packet_count);
    
    /* Negative Scenario: Drop connection after threshold */
    if (h->fsm.malformed_packet_count > 3) {
        LOG_ERROR(DOIP_LOG_MODULE_FSM, "Malformed packet threshold exceeded. Closing client.");
        /* TCP layer will handle close on next poll cycle */
    }
}

void DoIP_Fsm_MainFunction(DoIP_Handle_t *h) {
    if (h->fsm.current_state == DOIP_STATE_UNINIT || 
        h->fsm.current_state == DOIP_STATE_ERROR) {
        return;
    }

    /* 1. Non-blocking transport polling + alive check tick */
    doip_udp_poll(h);
    doip_tcp_poll(h);
    uint32_t now = get_time_ms();   /* after polling, so it is never older than rx timestamps */
    doip_tcp_tick(h, now);

    /* 2. S3 Server Timeout Enforcement (Negative Scenario: Session expiration) */
    if (h->fsm.current_state == DOIP_STATE_DIAG_SESSION ||
        h->fsm.current_state == DOIP_STATE_ROUTING_ACTIVE) {

        if ((now - h->fsm.state_entry_time_ms) > h->fsm.s3_server_timeout_ms) {
            h->fsm.timeout_violation_count++;
            LOG_WARN(DOIP_LOG_MODULE_FSM, "S3 Server timeout expired (Violations: %u)",
                     h->fsm.timeout_violation_count);
            
            /* Force reset to default session per ISO 14229-1 */
            h->fsm.current_uds_session = 0x01;
            fsm_transition(h, DOIP_STATE_IDLE);
            
            /* In production: notify tester or close TCP socket */
        }
    }
}

DoIP_State_t DoIP_Fsm_GetState(const DoIP_Handle_t *h) {
    return h->fsm.current_state;
}