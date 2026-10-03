#include "transport/doip_uds.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Runtime ECU identity and optional UDS request hook */
static DoIP_EcuIdentity_t g_identity;
static int (*g_uds_request_cb)(uint8_t, const uint8_t*, uint16_t,
                                uint8_t*, uint16_t, uint16_t*, void*) = NULL;
static void *g_uds_user_ctx = NULL;

static uint32_t get_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ===== Service Handlers (Stateless: accept ctx pointer) ===== */

static int handle_session_control(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F; res[1] = UDS_SID_SESSION_CONTROL; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    uint8_t session_type = req[1];
    LOG_DEBUG(DOIP_LOG_MODULE_UDS, "Session Control: 0x%02X", session_type);

    bool valid = false;
    switch (session_type) {
        case UDS_SESSION_DEFAULT: ctx->current_session = UDS_SESSION_DEFAULT_STATE; valid = true; break;
        case UDS_SESSION_EXTENDED: if (UDS_ECU_SUPPORTS_EXTENDED) { ctx->current_session = UDS_SESSION_EXTENDED_STATE; valid = true; } break;
        case UDS_SESSION_PROGRAMMING: if (UDS_ECU_SUPPORTS_PROGRAMMING) { ctx->current_session = UDS_SESSION_PROGRAMMING_STATE; valid = true; } break;
    }

    if (!valid) {
        res[0] = 0x7F; res[1] = UDS_SID_SESSION_CONTROL; res[2] = UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED; *res_len = 3; return -1;
    }

    res[0] = UDS_SID_SESSION_CONTROL_RES; res[1] = session_type;
    res[2] = 0x00; res[3] = 0x32; res[4] = 0x01; res[5] = 0xF4;
    *res_len = 6;
    LOG_INFO(DOIP_LOG_MODULE_UDS, "Session changed to: 0x%02X", session_type);
    DoIP_Fsm_OnUdsSessionChange(session_type);
    return 0;
}

static int handle_read_data_by_id(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    (void)ctx;
    if (req_len < 3) {
        res[0] = 0x7F; res[1] = UDS_SID_READ_DATA_BY_ID; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    uint16_t did = (req[1] << 8) | req[2];
    LOG_DEBUG(DOIP_LOG_MODULE_UDS, "Read DID: 0x%04X", did);

    res[0] = UDS_SID_READ_DATA_BY_ID_RES; res[1] = req[1]; res[2] = req[2];
    switch (did) {
        case UDS_DID_VIN_NUMBER: {
            memcpy(&res[3], g_identity.vin, DOIP_VIN_LENGTH);
            *res_len = 3 + DOIP_VIN_LENGTH;
            break;
        }
        case UDS_DID_SYSTEM_NAME: {
            size_t n = strlen(g_identity.system_name);
            memcpy(&res[3], g_identity.system_name, n);
            *res_len = (uint16_t)(3 + n);
            break;
        }
        case UDS_DID_SOFTWARE_VERSION: {
            size_t n = strlen(g_identity.software_version);
            memcpy(&res[3], g_identity.software_version, n);
            *res_len = (uint16_t)(3 + n);
            break;
        }
        case UDS_DID_ECU_SERIAL_NUMBER: {
            size_t n = strlen(g_identity.serial_number);
            memcpy(&res[3], g_identity.serial_number, n);
            *res_len = (uint16_t)(3 + n);
            break;
        }
        default: res[0] = 0x7F; res[1] = UDS_SID_READ_DATA_BY_ID; res[2] = UDS_NRC_REQUEST_OUT_OF_RANGE; *res_len = 3; return -1;
    }
    return 0;
}

static int handle_tester_present(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F; res[1] = UDS_SID_TESTER_PRESENT; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    if ((req[1] & 0x7F) != 0x00) {
        res[0] = 0x7F; res[1] = UDS_SID_TESTER_PRESENT; res[2] = UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED; *res_len = 3; return -1;
    }
    res[0] = UDS_SID_TESTER_PRESENT_RES; res[1] = req[1]; *res_len = 2;
    doip_uds_update_activity(ctx);
    DoIP_Fsm_OnDiagnosticActivity();
    return 0;
}

static int handle_ecu_reset(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    (void)ctx;
    if (req_len < 2) {
        res[0] = 0x7F; res[1] = UDS_SID_ECU_RESET; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    res[0] = UDS_SID_ECU_RESET_RES; res[1] = req[1]; *res_len = 2;
    LOG_WARN(DOIP_LOG_MODULE_UDS, "ECU Reset requested (simulated)");
    return 0;
}

static int handle_routine_control(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    (void)ctx;
    if (req_len < 4) {
        res[0] = 0x7F; res[1] = UDS_SID_ROUTINE_CONTROL; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    res[0] = UDS_SID_ROUTINE_CONTROL_RES; res[1] = req[1]; res[2] = req[2]; res[3] = req[3]; res[4] = 0x00; *res_len = 5;
    return 0;
}

/* Service Table */
static const struct { uint8_t sid; uint8_t res_id; int (*handler)(UdsClientContext_t*, const uint8_t*, uint16_t, uint8_t*, uint16_t*); bool enabled; } g_uds_services[] = {
#if UDS_SUPPORT_SESSION_CONTROL
    { UDS_SID_SESSION_CONTROL, UDS_SID_SESSION_CONTROL_RES, handle_session_control, true },
#endif
#if UDS_SUPPORT_READ_DATA
    { UDS_SID_READ_DATA_BY_ID, UDS_SID_READ_DATA_BY_ID_RES, handle_read_data_by_id, true },
#endif
#if UDS_SUPPORT_TESTER_PRESENT
    { UDS_SID_TESTER_PRESENT,  UDS_SID_TESTER_PRESENT_RES,  handle_tester_present,  true },
#endif
#if UDS_SUPPORT_ECU_RESET
    { UDS_SID_ECU_RESET, UDS_SID_ECU_RESET_RES, handle_ecu_reset, true },
#endif
#if UDS_SUPPORT_ROUTINE_CONTROL
    { UDS_SID_ROUTINE_CONTROL, UDS_SID_ROUTINE_CONTROL_RES, handle_routine_control, true },
#endif
    { 0x00, 0x00, NULL, false }
};

int doip_uds_init(const DoIP_EcuIdentity_t *identity,
                  int (*uds_request_cb)(uint8_t, const uint8_t*, uint16_t,
                                        uint8_t*, uint16_t, uint16_t*, void*),
                  void *user_ctx) {
    if (identity) g_identity = *identity;
    g_uds_request_cb = uds_request_cb;
    g_uds_user_ctx   = user_ctx;
    LOG_INFO(DOIP_LOG_MODULE_UDS, "UDS module initialized (per-client state)");
    return 0;
}

int doip_uds_process_request(UdsClientContext_t *ctx, const uint8_t *req_data, uint16_t req_len, uint8_t *res_data, uint16_t *res_len) {
    if (!ctx || !req_data || !res_data || !res_len || req_len == 0) return -1;

    uint8_t sid = req_data[0];
    LOG_DEBUG(DOIP_LOG_MODULE_UDS, "<<< SID: 0x%02X (Len=%u)", sid, req_len);

    /* 1. Runtime hook — caller can serve NVM / sensor / DTC data directly */
    if (g_uds_request_cb) {
        uint16_t cb_len = 0;
        if (g_uds_request_cb(sid, req_data, req_len,
                             res_data, DOIP_MAX_PAYLOAD_SIZE, &cb_len,
                             g_uds_user_ctx) == 0) {
            *res_len = cb_len;
            LOG_DEBUG(DOIP_LOG_MODULE_UDS, ">>> SID: 0x%02X handled by runtime hook", sid);
            return 0;
        }
    }

    /* Check Support List */
    bool supported = false;
    const uint8_t supported_list[] = { UDS_SUPPORTED_SIDS };
    for (size_t i = 0; i < sizeof(supported_list); i++) {
        if (supported_list[i] == sid) { supported = true; break; }
    }
    if (!supported) {
        res_data[0] = 0x7F; res_data[1] = sid; res_data[2] = UDS_NRC_SERVICE_NOT_SUPPORTED; *res_len = 3;
        return -1;
    }

    /* S3 Timeout Check */
    if (!doip_uds_is_session_active(ctx, UDS_S3_SERVER_MS)) {
        LOG_WARN(DOIP_LOG_MODULE_UDS, "S3 timeout. Resetting client session to Default.");
        ctx->current_session = UDS_SESSION_DEFAULT_STATE;
    }

    /* Try Custom Handler */
    for (size_t i = 0; g_uds_services[i].handler != NULL; i++) {
        if (g_uds_services[i].sid == sid && g_uds_services[i].enabled) {
            doip_uds_update_activity(ctx);
            return g_uds_services[i].handler(ctx, req_data, req_len, res_data, res_len);
        }
    }

    /* Fallback: Auto-Response */
#if UDS_SIMULATOR_AUTO_RESPOND
    res_data[0] = sid | 0x40;
    if (req_len > 1) { memcpy(&res_data[1], &req_data[1], req_len - 1); *res_len = req_len; }
    else { *res_len = 1; }
    return 0;
#else
    res_data[0] = 0x7F; res_data[1] = sid; res_data[2] = UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED; *res_len = 3;
    return -1;
#endif
}

void doip_uds_update_activity(UdsClientContext_t *ctx) {
    if (ctx) ctx->last_activity_ms = get_time_ms();
}

bool doip_uds_is_session_active(const UdsClientContext_t *ctx, uint32_t timeout_ms) {
    if (!ctx) return false;
    return (get_time_ms() - ctx->last_activity_ms) < timeout_ms;
}

void doip_uds_deinit(void) { LOG_INFO(DOIP_LOG_MODULE_UDS, "UDS module deinitialized"); }