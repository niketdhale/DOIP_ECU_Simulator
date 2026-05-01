#ifndef DOIP_UDS_H
#define DOIP_UDS_H

#include "config/doip_uds_config.h"
#include "config/doip_config.h"
#include <stdint.h>
#include <stdbool.h>

/* UDS Service Handler Function Type */
typedef int (*UdsServiceHandler)(const uint8_t *req_data, uint16_t req_len,
                                  uint8_t *res_data, uint16_t *res_len);

/* UDS Service Table Entry */
typedef struct {
    uint8_t             service_id;
    uint8_t             response_id;
    UdsServiceHandler   handler;
    bool                enabled;
} UdsServiceEntry_t;

/* UDS Session State */
typedef enum {
    UDS_SESSION_DEFAULT_STATE = 0x01,
    UDS_SESSION_EXTENDED_STATE = 0x03,
    UDS_SESSION_PROGRAMMING_STATE = 0x02
} UdsSessionState_t;

/* UDS Context */
typedef struct {
    UdsSessionState_t   current_session;
    uint32_t            last_activity_ms;
    bool                security_unlocked;
    uint8_t             security_attempts;
} UdsContext_t;

/* Initialize UDS module */
int doip_uds_init(void);

/* Process UDS request and generate response */
int doip_uds_process_request(const uint8_t *req_data, uint16_t req_len,
                             uint8_t *res_data, uint16_t *res_len);

/* Get current session state */
UdsSessionState_t doip_uds_get_session(void);

/* Update session timeout */
void doip_uds_update_activity(void);

/* Check if session is active */
bool doip_uds_is_session_active(uint32_t timeout_ms);

/* Cleanup */
void doip_uds_deinit(void);

#endif /* DOIP_UDS_H */