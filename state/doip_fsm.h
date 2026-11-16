#ifndef DOIP_FSM_H
#define DOIP_FSM_H

#include "config/doip_config.h"
#include "core/doip_log.h"
#include "core/doip_types.h"
#include "doip_api.h"          /* DoIP_Handle_t */
#include <stdint.h>
#include <stdbool.h>

/* ===== DoIP State Enumeration (AUTOSAR-aligned) ===== */
typedef enum {
    DOIP_STATE_UNINIT           = 0x00,
    DOIP_STATE_IDLE             = 0x01,
    DOIP_STATE_DISCOVERY        = 0x02,
    DOIP_STATE_ROUTING_ACTIVE   = 0x03,
    DOIP_STATE_DIAG_SESSION     = 0x04,
    DOIP_STATE_ERROR            = 0xFF
} DoIP_State_t;

/* ===== FSM Context ===== */
typedef struct {
    DoIP_State_t    current_state;
    DoIP_State_t    previous_state;
    uint32_t        state_entry_time_ms;
    uint32_t        s3_server_timeout_ms;
    bool            routing_activated;
    uint8_t         current_uds_session;
    uint32_t        malformed_packet_count;
    uint32_t        timeout_violation_count;
} DoIP_FsmContext_t;

/* ===== API (all state lives in the instance: h->fsm) ===== */

/**
 * @brief Initialize the FSM and, through it, the UDS, UDP and TCP modules.
 * Everything is taken from the instance: h->config, h->identity.
 * @return 0 on success, -1 on fatal error
 */
int DoIP_Fsm_Init(DoIP_Handle_t *h);

/** @brief Deinitialize all modules and reset state */
void DoIP_Fsm_DeInit(DoIP_Handle_t *h);

/**
 * @brief AUTOSAR-style cyclic main function (non-blocking)
 * Handles polling, S3 timeout enforcement, and state validation
 */
void DoIP_Fsm_MainFunction(DoIP_Handle_t *h);

/** @brief Get current FSM state */
DoIP_State_t DoIP_Fsm_GetState(const DoIP_Handle_t *h);

/* ===== Event Callbacks (Called by transport/UDS layers) ===== */

/** @brief Notify FSM of successful routing activation */
void DoIP_Fsm_OnRoutingActivation(DoIP_Handle_t *h, bool activated);

/** @brief Notify FSM of UDS session change */
void DoIP_Fsm_OnUdsSessionChange(DoIP_Handle_t *h, uint8_t session_id);

/** @brief Notify FSM of diagnostic activity (refreshes S3 timer) */
void DoIP_Fsm_OnDiagnosticActivity(DoIP_Handle_t *h);

/** @brief Notify FSM of TCP disconnect (forces state reset) */
void DoIP_Fsm_OnTcpDisconnect(DoIP_Handle_t *h);

/** @brief Notify FSM of malformed/invalid packet */
void DoIP_Fsm_OnMalformedPacket(DoIP_Handle_t *h);

#endif /* DOIP_FSM_H */