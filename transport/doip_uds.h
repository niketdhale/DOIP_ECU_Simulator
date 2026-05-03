#ifndef DOIP_UDS_H
#define DOIP_UDS_H

#include "config/doip_uds_config.h"
#include <stdint.h>
#include <stdbool.h>

/* Session State Enum (Must match config values 0x01, 0x02, 0x03) */
typedef enum {
    UDS_SESSION_DEFAULT_STATE     = 0x01,
    UDS_SESSION_EXTENDED_STATE    = 0x03,
    UDS_SESSION_PROGRAMMING_STATE = 0x02
} UdsSessionState_t;

/* Per-Client UDS Context (Replaces global g_uds_ctx) */
typedef struct {
    UdsSessionState_t current_session;
    uint32_t          last_activity_ms;
    bool              security_unlocked;
    uint8_t           security_attempts;
} UdsClientContext_t;

/* API: Stateless (Context is passed by caller) */
int doip_uds_init(void);
int doip_uds_process_request(UdsClientContext_t *ctx, const uint8_t *req_data, uint16_t req_len, uint8_t *res_data, uint16_t *res_len);
void doip_uds_update_activity(UdsClientContext_t *ctx);
bool doip_uds_is_session_active(const UdsClientContext_t *ctx, uint32_t timeout_ms);
void doip_uds_deinit(void);

#endif /* DOIP_UDS_H */