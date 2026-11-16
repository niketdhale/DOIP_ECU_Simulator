#include "transport/doip_uds.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Write a negative response (7F <sid> <nrc>); returns -1 for handler tail calls. */
static int neg(uint8_t *res, uint16_t *res_len, uint8_t sid, uint8_t nrc) {
    res[0] = 0x7F; res[1] = sid; res[2] = nrc; *res_len = 3;
    return -1;
}

/* Back to the power-on diagnostic state: default session, security locked. */
static void enter_default_session(UdsClientContext_t *ctx) {
    ctx->current_session   = UDS_SESSION_DEFAULT_STATE;
    ctx->security_unlocked = false;
    ctx->security_seed     = 0;
}

/* ===== Service Handlers (Stateless: accept ctx pointer) ===== */

static int handle_session_control(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F; res[1] = UDS_SID_SESSION_CONTROL; res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH; *res_len = 3; return -1;
    }
    uint8_t session_type = req[1] & 0x7F;  /* bit 7 = suppressPosRspMsgIndicationBit */
    LOG_DEBUG(DOIP_LOG_MODULE_UDS, "Session Control: 0x%02X", session_type);

    bool valid = false;
    switch (session_type) {
        case UDS_SESSION_DEFAULT: enter_default_session(ctx); valid = true; break;
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

/* Look up a readable DID; returns its data length or -1 if unsupported. */
static int did_read(uint16_t did, const char **data) {
    switch (did) {
        case UDS_DID_VIN_NUMBER:        *data = g_identity.vin;              return DOIP_VIN_LENGTH;
        case UDS_DID_SYSTEM_NAME:       *data = g_identity.system_name;      break;
        case UDS_DID_SOFTWARE_VERSION:  *data = g_identity.software_version; break;
        case UDS_DID_ECU_SERIAL_NUMBER: *data = g_identity.serial_number;    break;
        default: return -1;
    }
    return (int)strlen(*data);
}

/* 0x22: one or more DIDs per request. Unsupported DIDs are skipped; the
 * request is rejected only if none of them is supported (ISO 14229-1). */
static int handle_read_data_by_id(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    (void)ctx;
    if (req_len < 3 || (req_len - 1) % 2 != 0)
        return neg(res, res_len, UDS_SID_READ_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);

    uint16_t out = 1;
    res[0] = UDS_SID_READ_DATA_BY_ID_RES;
    for (uint16_t i = 1; i < req_len; i += 2) {
        uint16_t did = (uint16_t)((req[i] << 8) | req[i + 1]);
        const char *data = NULL;
        int n = did_read(did, &data);
        LOG_DEBUG(DOIP_LOG_MODULE_UDS, "Read DID: 0x%04X%s", did, n < 0 ? " (unsupported)" : "");
        if (n < 0) continue;
        if (out + 2U + (unsigned)n > UDS_MAX_RESPONSE_LEN)
            return neg(res, res_len, UDS_SID_READ_DATA_BY_ID, UDS_NRC_RESPONSE_TOO_LONG);
        res[out++] = req[i]; res[out++] = req[i + 1];
        memcpy(&res[out], data, (size_t)n);
        out += (uint16_t)n;
    }
    if (out == 1)
        return neg(res, res_len, UDS_SID_READ_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
    *res_len = out;
    return 0;
}

/* DIDs writable through 0x2E, backed by the runtime identity (which UDP
 * vehicle announcements also read, so a written VIN is announced too). */
static int handle_write_data_by_id(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    (void)ctx;
    uint8_t nrc = 0;
    char   *dst = NULL;
    size_t  max = 0;                       /* max data bytes; for VIN also the exact length */
    uint16_t did = (req_len >= 3) ? (uint16_t)((req[1] << 8) | req[2]) : 0;
    size_t   n   = (req_len >= 3) ? (size_t)(req_len - 3) : 0;

    switch (did) {
        case UDS_DID_VIN_NUMBER:        dst = g_identity.vin;           max = DOIP_VIN_LENGTH; break;
        case UDS_DID_SYSTEM_NAME:       dst = g_identity.system_name;   max = sizeof(g_identity.system_name) - 1; break;
        case UDS_DID_ECU_SERIAL_NUMBER: dst = g_identity.serial_number; max = sizeof(g_identity.serial_number) - 1; break;
        default: break;
    }

    if (req_len < 4)                                            nrc = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
    else if (!dst)                                              nrc = UDS_NRC_REQUEST_OUT_OF_RANGE;
    else if (n > max || (did == UDS_DID_VIN_NUMBER && n != max)) nrc = UDS_NRC_INCORRECT_MESSAGE_LENGTH;

    if (nrc) {
        res[0] = 0x7F; res[1] = UDS_SID_WRITE_DATA_BY_ID; res[2] = nrc; *res_len = 3; return -1;
    }
    memcpy(dst, &req[3], n);
    dst[n] = '\0';
    LOG_INFO(DOIP_LOG_MODULE_UDS, "Write DID 0x%04X (%u bytes)", did, (unsigned)n);
    res[0] = UDS_SID_WRITE_DATA_BY_ID_RES; res[1] = req[1]; res[2] = req[2]; *res_len = 3;
    return 0;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* 0x27 level 1. ponytail: rand() seed and XOR key are simulator-grade, not
 * a real seed/key algorithm — replace via the on_uds_request hook if needed. */
static int handle_security_access(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len, uint8_t *res, uint16_t *res_len) {
    const uint8_t sid = UDS_SID_SECURITY_ACCESS;
    if (req_len < 2) return neg(res, res_len, sid, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    uint8_t sub = req[1] & 0x7F;
    if (sub != 0x01 && sub != 0x02) return neg(res, res_len, sid, UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED);
    if (req_len != (sub == 0x01 ? 2 : 6)) return neg(res, res_len, sid, UDS_NRC_INCORRECT_MESSAGE_LENGTH);

    if (ctx->security_lockout_ms != 0) {
        /* signed: the |1 below can put the stamp 1 ms ahead of the clock */
        if ((int32_t)(get_time_ms() - ctx->security_lockout_ms) < (int32_t)UDS_SECURITY_LOCKOUT_MS)
            return neg(res, res_len, sid, UDS_NRC_TIME_DELAY_NOT_EXPIRED);
        ctx->security_lockout_ms = 0;
        ctx->security_attempts   = 0;
    }

    res[0] = UDS_SID_SECURITY_ACCESS_RES; res[1] = sub;
    if (sub == 0x01) {                       /* requestSeed */
        /* An already-unlocked ECU reports an all-zero seed */
        ctx->security_seed = ctx->security_unlocked ? 0 : ((uint32_t)rand() | 1U);
        put_be32(&res[2], ctx->security_seed);
        *res_len = 6;
        return 0;
    }

    /* sendKey */
    if (ctx->security_seed == 0) return neg(res, res_len, sid, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    uint32_t key = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) | ((uint32_t)req[4] << 8) | req[5];
    uint32_t expected = ctx->security_seed ^ (uint32_t)UDS_SECURITY_KEY_MASK;
    ctx->security_seed = 0;                  /* a seed is good for one attempt */
    if (key != expected) {
        if (++ctx->security_attempts >= UDS_SECURITY_MAX_ATTEMPTS) {
            ctx->security_lockout_ms = get_time_ms() | 1U;   /* never 0 = "not locked" */
            LOG_WARN(DOIP_LOG_MODULE_UDS, "Security Access locked out for %u ms", (unsigned)UDS_SECURITY_LOCKOUT_MS);
            return neg(res, res_len, sid, UDS_NRC_EXCEEDED_ATTEMPTS);
        }
        return neg(res, res_len, sid, UDS_NRC_INVALID_KEY);
    }
    ctx->security_unlocked = true;
    ctx->security_attempts = 0;
    LOG_INFO(DOIP_LOG_MODULE_UDS, "Security Access unlocked");
    *res_len = 2;
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
    if (req_len != 2)
        return neg(res, res_len, UDS_SID_ECU_RESET, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    uint8_t type = req[1] & 0x7F;
    if (type < UDS_RESET_HARD || type > UDS_RESET_SOFT)
        return neg(res, res_len, UDS_SID_ECU_RESET, UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED);

    /* Simulated reset: the ECU comes back in the default session, locked */
    enter_default_session(ctx);
    DoIP_Fsm_OnUdsSessionChange(UDS_SESSION_DEFAULT);
    res[0] = UDS_SID_ECU_RESET_RES; res[1] = type; *res_len = 2;
    LOG_WARN(DOIP_LOG_MODULE_UDS, "ECU Reset type 0x%02X requested (simulated)", type);
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
#if UDS_SUPPORT_WRITE_DATA
    { UDS_SID_WRITE_DATA_BY_ID, UDS_SID_WRITE_DATA_BY_ID_RES, handle_write_data_by_id, true },
#endif
#if UDS_SUPPORT_SECURITY_ACCESS
    { UDS_SID_SECURITY_ACCESS, UDS_SID_SECURITY_ACCESS_RES, handle_security_access, true },
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

/* Services whose second byte is a sub-function, where bit 7 is the
 * suppressPosRspMsgIndicationBit (ISO 14229-1). */
static bool sid_has_subfunction(uint8_t sid) {
    switch (sid) {
        case 0x10: case 0x11: case 0x19: case 0x27: case 0x28:
        case 0x31: case 0x3E: case 0x85: return true;
        default: return false;
    }
}

static bool sid_in(uint8_t sid, const uint8_t *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (list[i] == sid) return true;
    return false;
}

/* Services only available in a non-default session (extended / programming). */
static bool sid_needs_non_default_session(uint8_t sid) {
    const uint8_t list[] = { UDS_NON_DEFAULT_SESSION_SIDS };
    return sid_in(sid, list, sizeof(list));
}

/* Services that need Security Access to be unlocked first. */
static bool sid_needs_security(uint8_t sid) {
#if UDS_SUPPORT_SECURITY_ACCESS
    const uint8_t list[] = { UDS_SECURED_SIDS };
    return sid_in(sid, list, sizeof(list));
#else
    (void)sid; return false;
#endif
}

int doip_uds_init(const DoIP_EcuIdentity_t *identity,
                  int (*uds_request_cb)(uint8_t, const uint8_t*, uint16_t,
                                        uint8_t*, uint16_t, uint16_t*, void*),
                  void *user_ctx) {
    if (identity) g_identity = *identity;
    g_uds_request_cb = uds_request_cb;
    g_uds_user_ctx   = user_ctx;
    srand((unsigned)time(NULL));
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
        enter_default_session(ctx);
    }

    /* Session gating: these services exist only outside the default session */
    if (ctx->current_session == UDS_SESSION_DEFAULT_STATE && sid_needs_non_default_session(sid)) {
        doip_uds_update_activity(ctx);
        return neg(res_data, res_len, sid, UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);
    }
    if (!ctx->security_unlocked && sid_needs_security(sid)) {
        doip_uds_update_activity(ctx);
        return neg(res_data, res_len, sid, UDS_NRC_SECURITY_ACCESS_DENIED);
    }

    /* Tester asked for no positive response: the service still runs and
     * negative responses are still sent; *res_len == 0 means "send nothing". */
    bool suppress = sid_has_subfunction(sid) && req_len > 1 && (req_data[1] & 0x80);

    /* Try Custom Handler */
    for (size_t i = 0; g_uds_services[i].handler != NULL; i++) {
        if (g_uds_services[i].sid == sid && g_uds_services[i].enabled) {
            doip_uds_update_activity(ctx);
            int rc = g_uds_services[i].handler(ctx, req_data, req_len, res_data, res_len);
            if (rc == 0 && suppress) *res_len = 0;
            return rc;
        }
    }

    /* Fallback: Auto-Response */
#if UDS_SIMULATOR_AUTO_RESPOND
    res_data[0] = sid | 0x40;
    if (req_len > 1) { memcpy(&res_data[1], &req_data[1], req_len - 1); *res_len = req_len; }
    else { *res_len = 1; }
    if (suppress) *res_len = 0;
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

const DoIP_EcuIdentity_t *doip_uds_identity(void) { return &g_identity; }

void doip_uds_deinit(void) { LOG_INFO(DOIP_LOG_MODULE_UDS, "UDS module deinitialized"); }