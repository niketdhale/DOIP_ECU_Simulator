#ifndef DOIP_UDS_H
#define DOIP_UDS_H

#include "config/doip_uds_config.h"
#include "core/doip_types.h"
#include "doip_api.h"          /* DoIP_Handle_t */
#include <stdint.h>
#include <stdbool.h>

/* Session State Enum (Must match config values 0x01, 0x02, 0x03) */
typedef enum {
    UDS_SESSION_DEFAULT_STATE     = 0x01,
    UDS_SESSION_EXTENDED_STATE    = 0x03,
    UDS_SESSION_PROGRAMMING_STATE = 0x02
} UdsSessionState_t;

#if UDS_SUPPORT_DTC
/* One entry of the simulated DTC memory (an instance holds UDS_DTC_COUNT of them) */
typedef struct { uint32_t dtc; uint8_t status; } uds_dtc_t;
#define UDS_DTC_COUNT (sizeof((uds_dtc_t[]){ UDS_DTC_TABLE }) / sizeof(uds_dtc_t))
#endif

/* Per-Client UDS Context */
typedef struct {
    DoIP_Handle_t    *ecu;                  /* The simulator instance this client talks to */
    UdsSessionState_t current_session;
    uint32_t          last_activity_ms;
    bool              security_unlocked;
    uint8_t           security_attempts;    /* Consecutive invalid keys                    */
    uint32_t          security_seed;        /* Seed awaiting its key; 0 = none requested   */
    uint32_t          security_lockout_ms;  /* Time of lockout start; 0 = not locked out   */
} UdsClientContext_t;

/* API: no module-level state. ECU-wide data (identity, request hook, DTC
 * memory) lives in the instance, per-tester data in UdsClientContext_t. */

/** Reset the instance's UDS state (DTC memory, seed generator). */
int doip_uds_init(DoIP_Handle_t *ecu);
int doip_uds_process_request(UdsClientContext_t *ctx, const uint8_t *req_data, uint16_t req_len, uint8_t *res_data, uint16_t *res_len);
void doip_uds_update_activity(UdsClientContext_t *ctx);
bool doip_uds_is_session_active(const UdsClientContext_t *ctx, uint32_t timeout_ms);
void doip_uds_deinit(void);

/**
 * Functional addressing rule (ISO 14229-1): negative responses with NRC
 * 0x11, 0x12, 0x31, 0x7E or 0x7F are not sent for functionally addressed
 * requests. Returns true when @p res is such a response and must be dropped.
 */
bool doip_uds_functional_suppresses(const uint8_t *res, uint16_t res_len);

#endif /* DOIP_UDS_H */