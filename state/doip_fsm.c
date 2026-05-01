#include "state/doip_fsm.h"
#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
#include <time.h>

static DoIP_FsmContext_t g_fsm_ctx = {0};

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DoIP_Fsm_Init(uint32_t s3_timeout_ms) {
    g_fsm_ctx.current_state = DOIP_STATE_UNINIT;
    g_fsm_ctx.previous_state = DOIP_STATE_UNINIT;
    g_fsm_ctx.state_entry_time_ms = get_time_ms();
    g_fsm_ctx.s3_server_timeout_ms = s3_timeout_ms;
    g_fsm_ctx.routing_activated = false;
    g_fsm_ctx.current_uds_session = 0x01;
    g_fsm_ctx.malformed_packet_count = 0;
    g_fsm_ctx.timeout_violation_count = 0;

    /* Initialize underlying transports */
    if (doip_udp_init() != 0 || doip_tcp_init() != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_FSM, "Transport initialization failed");
        g_fsm_ctx.current_state = DOIP_STATE_ERROR;
        return -1;
    }
    doip_uds_init();

    g_fsm_ctx.current_state = DOIP_STATE_IDLE;
    LOG_INFO(DOIP_LOG_MODULE_FSM, "FSM initialized. S3=%u ms", s3_timeout_ms);
    return 0;
}

void DoIP_Fsm_DeInit(void) {
    doip_uds_deinit();
    doip_tcp_deinit();
    doip_udp_deinit();
    g_fsm_ctx.current_state = DOIP_STATE_UNINIT;
    LOG_INFO(DOIP_LOG_MODULE_FSM, "FSM deinitialized");
}

static bool fsm_transition(DoIP_State_t new_state) {
    if (g_fsm_ctx.current_state == new_state) return true;
    g_fsm_ctx.previous_state = g_fsm_ctx.current_state;
    g_fsm_ctx.current_state = new_state;
    g_fsm_ctx.state_entry_time_ms = get_time_ms();
    LOG_INFO(DOIP_LOG_MODULE_FSM, "State: %d -> %d", g_fsm_ctx.previous_state, new_state);
    return true;
}

void DoIP_Fsm_OnRoutingActivation(bool activated) {
    g_fsm_ctx.routing_activated = activated;
    if (activated) {
        fsm_transition(DOIP_STATE_ROUTING_ACTIVE);
    } else {
        fsm_transition(DOIP_STATE_IDLE);
    }
}

void DoIP_Fsm_OnUdsSessionChange(uint8_t session_id) {
    g_fsm_ctx.current_uds_session = session_id;
    if (session_id != 0x01) {
        fsm_transition(DOIP_STATE_DIAG_SESSION);
    } else {
        fsm_transition(DOIP_STATE_ROUTING_ACTIVE);
    }
}

void DoIP_Fsm_OnDiagnosticActivity(void) {
    g_fsm_ctx.state_entry_time_ms = get_time_ms(); /* Refresh S3 */
}

void DoIP_Fsm_OnTcpDisconnect(void) {
    LOG_WARN(DOIP_LOG_MODULE_FSM, "TCP disconnected. Forcing state reset.");
    g_fsm_ctx.routing_activated = false;
    g_fsm_ctx.current_uds_session = 0x01;
    fsm_transition(DOIP_STATE_IDLE);
}

void DoIP_Fsm_OnMalformedPacket(void) {
    g_fsm_ctx.malformed_packet_count++;
    LOG_WARN(DOIP_LOG_MODULE_FSM, "Malformed packet detected (Count: %u)", g_fsm_ctx.malformed_packet_count);
    
    /* Negative Scenario: Drop connection after threshold */
    if (g_fsm_ctx.malformed_packet_count > 3) {
        LOG_ERROR(DOIP_LOG_MODULE_FSM, "Malformed packet threshold exceeded. Closing client.");
        /* TCP layer will handle close on next poll cycle */
    }
}

void DoIP_Fsm_MainFunction(void) {
    if (g_fsm_ctx.current_state == DOIP_STATE_UNINIT || 
        g_fsm_ctx.current_state == DOIP_STATE_ERROR) {
        return;
    }

    /* 1. Non-blocking transport polling */
    doip_udp_poll(NULL);
    doip_tcp_poll(NULL);

    /* 2. S3 Server Timeout Enforcement (Negative Scenario: Session expiration) */
    if (g_fsm_ctx.current_state == DOIP_STATE_DIAG_SESSION || 
        g_fsm_ctx.current_state == DOIP_STATE_ROUTING_ACTIVE) {
        
        uint32_t now = get_time_ms();
        if ((now - g_fsm_ctx.state_entry_time_ms) > g_fsm_ctx.s3_server_timeout_ms) {
            g_fsm_ctx.timeout_violation_count++;
            LOG_WARN(DOIP_LOG_MODULE_FSM, "️ S3 Server timeout expired (Violations: %u)", 
                     g_fsm_ctx.timeout_violation_count);
            
            /* Force reset to default session per ISO 14229-1 */
            g_fsm_ctx.current_uds_session = 0x01;
            fsm_transition(DOIP_STATE_IDLE);
            
            /* In production: notify tester or close TCP socket */
        }
    }
}

DoIP_State_t DoIP_Fsm_GetState(void) {
    return g_fsm_ctx.current_state;
}