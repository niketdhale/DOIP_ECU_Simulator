#include "transport/doip_uds.h"
#include "core/doip_frame.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static UdsContext_t g_uds_ctx = {
    .current_session = UDS_SESSION_DEFAULT_STATE,
    .last_activity_ms = 0,
    .security_unlocked = false,
    .security_attempts = 0
};

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ===== UDS Service Handlers ===== */

/* 0x10 - Diagnostic Session Control */
static int handle_session_control(const uint8_t *req, uint16_t req_len,
                                   uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F;
        res[1] = UDS_SID_SESSION_CONTROL;
        res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
        *res_len = 3;
        return -1;
    }

    uint8_t session_type = req[1];
    printf("[UDS] Session Control: 0x%02X\n", session_type);

    /* Validate session type */
    bool valid = false;
    switch (session_type) {
        case UDS_SESSION_DEFAULT:
            g_uds_ctx.current_session = UDS_SESSION_DEFAULT_STATE;
            valid = true;
            break;
        case UDS_SESSION_EXTENDED:
            if (UDS_ECU_SUPPORTS_EXTENDED) {
                g_uds_ctx.current_session = UDS_SESSION_EXTENDED_STATE;
                valid = true;
            }
            break;
        case UDS_SESSION_PROGRAMMING:
            if (UDS_ECU_SUPPORTS_PROGRAMMING) {
                g_uds_ctx.current_session = UDS_SESSION_PROGRAMMING_STATE;
                valid = true;
            }
            break;
    }

    if (!valid) {
        res[0] = 0x7F;
        res[1] = UDS_SID_SESSION_CONTROL;
        res[2] = UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED;
        *res_len = 3;
        return -1;
    }

    /* Positive Response */
    res[0] = UDS_SID_SESSION_CONTROL_RES;
    res[1] = session_type;
    res[2] = 0x00;  /* P2Server */
    res[3] = 0x32;  /* 50ms */
    res[4] = 0x01;  /* P2*Server */
    res[5] = 0xF4;  /* 5000ms */
    *res_len = 6;

    printf("[UDS] ✅ Session changed to: 0x%02X\n", session_type);
    return 0;
}

/* 0x22 - Read Data By Identifier */
static int handle_read_data_by_id(const uint8_t *req, uint16_t req_len,
                                   uint8_t *res, uint16_t *res_len) {
    if (req_len < 3) {
        res[0] = 0x7F;
        res[1] = UDS_SID_READ_DATA_BY_ID;
        res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
        *res_len = 3;
        return -1;
    }

    uint16_t did = (req[1] << 8) | req[2];
    printf("[UDS] Read DID: 0x%04X\n", did);

    res[0] = UDS_SID_READ_DATA_BY_ID_RES;
    res[1] = req[1];  /* DID MSB */
    res[2] = req[2];  /* DID LSB */

    switch (did) {
        case UDS_DID_VIN_NUMBER: {
            const char *vin = "WBAXXXXXXXXXXXXXX";
            memcpy(&res[3], vin, 17);
            *res_len = 20;
            break;
        }
        case UDS_DID_SYSTEM_NAME: {
            const char *name = "DoIP ECU Simulator";
            memcpy(&res[3], name, strlen(name));
            *res_len = 3 + strlen(name);
            break;
        }
        case UDS_DID_SOFTWARE_VERSION: {
            const char *sw = "V1.0.0";
            memcpy(&res[3], sw, strlen(sw));
            *res_len = 3 + strlen(sw);
            break;
        }
        case UDS_DID_ECU_SERIAL_NUMBER: {
            const char *sn = "ECU123456789";
            memcpy(&res[3], sn, strlen(sn));
            *res_len = 3 + strlen(sn);
            break;
        }
        default:
            res[0] = 0x7F;
            res[1] = UDS_SID_READ_DATA_BY_ID;
            res[2] = UDS_NRC_REQUEST_OUT_OF_RANGE;
            *res_len = 3;
            return -1;
    }

    return 0;
}

/* 0x3E - Tester Present */
static int handle_tester_present(const uint8_t *req, uint16_t req_len,
                                  uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F;
        res[1] = UDS_SID_TESTER_PRESENT;
        res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
        *res_len = 3;
        return -1;
    }

    uint8_t sub_func = req[1] & 0x7F;
    bool keep_alive = (req[1] & 0x80) != 0;

    printf("[UDS] Tester Present (sub=0x%02X, keep_alive=%d)\n", sub_func, keep_alive);

    if (sub_func != 0x00) {
        res[0] = 0x7F;
        res[1] = UDS_SID_TESTER_PRESENT;
        res[2] = UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED;
        *res_len = 3;
        return -1;
    }

    res[0] = UDS_SID_TESTER_PRESENT_RES;
    res[1] = req[1];
    *res_len = 2;

    doip_uds_update_activity();
    return 0;
}

/* 0x11 - ECU Reset */
static int handle_ecu_reset(const uint8_t *req, uint16_t req_len,
                             uint8_t *res, uint16_t *res_len) {
    if (req_len < 2) {
        res[0] = 0x7F;
        res[1] = UDS_SID_ECU_RESET;
        res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
        *res_len = 3;
        return -1;
    }

    uint8_t reset_type = req[1];
    printf("[UDS] ECU Reset: 0x%02X\n", reset_type);

    /* Simulate reset (in real ECU, this would trigger actual reset) */
    res[0] = UDS_SID_ECU_RESET_RES;
    res[1] = reset_type;
    *res_len = 2;

    printf("[UDS] ⚠️ ECU Reset requested (simulated)\n");
    return 0;
}

/* 0x31 - Routine Control */
static int handle_routine_control(const uint8_t *req, uint16_t req_len,
                                   uint8_t *res, uint16_t *res_len) {
    if (req_len < 3) {
        res[0] = 0x7F;
        res[1] = UDS_SID_ROUTINE_CONTROL;
        res[2] = UDS_NRC_INCORRECT_MESSAGE_LENGTH;
        *res_len = 3;
        return -1;
    }

    uint8_t sub_func = req[1] & 0x7F;
    uint16_t routine_id = (req[2] << 8) | req[3];

    printf("[UDS] Routine Control: sub=0x%02X, RID=0x%04X\n", sub_func, routine_id);

    res[0] = UDS_SID_ROUTINE_CONTROL_RES;
    res[1] = req[1];
    res[2] = req[2];
    res[3] = req[3];
    
    /* Add routine status info */
    res[4] = 0x00;  /* Routine completed successfully */
    *res_len = 5;

    return 0;
}

/* Service Table */
static const UdsServiceEntry_t g_uds_services[] = {
#if UDS_SUPPORT_SESSION_CONTROL
    { UDS_SID_SESSION_CONTROL, UDS_SID_SESSION_CONTROL_RES, handle_session_control, true },
#endif
#if UDS_SUPPORT_READ_DATA
    { UDS_SID_READ_DATA_BY_ID, UDS_SID_READ_DATA_BY_ID_RES, handle_read_data_by_id, true },
#endif
#if UDS_SUPPORT_TESTER_PRESENT
    { UDS_SID_TESTER_PRESENT, UDS_SID_TESTER_PRESENT_RES, handle_tester_present, true },
#endif
#if UDS_SUPPORT_ECU_RESET
    { UDS_SID_ECU_RESET, UDS_SID_ECU_RESET_RES, handle_ecu_reset, true },
#endif
#if UDS_SUPPORT_ROUTINE_CONTROL
    { UDS_SID_ROUTINE_CONTROL, UDS_SID_ROUTINE_CONTROL_RES, handle_routine_control, true },
#endif
    { 0x00, 0x00, NULL, false }  /* Sentinel */
};

/* ===== Public API Implementation ===== */

int doip_uds_init(void) {
    g_uds_ctx.current_session = UDS_ECU_DEFAULT_SESSION;
    g_uds_ctx.last_activity_ms = get_time_ms();
    g_uds_ctx.security_unlocked = false;
    g_uds_ctx.security_attempts = 0;
    
    printf("[UDS] Module initialized. Session: 0x%02X\n", g_uds_ctx.current_session);
    return 0;
}

int doip_uds_process_request(const uint8_t *req_data, uint16_t req_len,
                              uint8_t *res_data, uint16_t *res_len) {
    if (req_len == 0 || !req_data || !res_data || !res_len) {
        return -1;
    }

    uint8_t sid = req_data[0];
    printf("[UDS] <<< SID: 0x%02X (Len=%u)\n", sid, req_len);

    /* Find matching service handler */
    for (const UdsServiceEntry_t *svc = g_uds_services; svc->handler != NULL; svc++) {
        if (svc->service_id == sid && svc->enabled) {
            doip_uds_update_activity();
            return svc->handler(req_data, req_len, res_data, res_len);
        }
    }

    /* Service not supported */
    res_data[0] = 0x7F;
    res_data[1] = sid;
    res_data[2] = UDS_NRC_SERVICE_NOT_SUPPORTED;
    *res_len = 3;
    
    printf("[UDS] ❌ Service 0x%02X not supported\n", sid);
    return -1;
}

UdsSessionState_t doip_uds_get_session(void) {
    return g_uds_ctx.current_session;
}

void doip_uds_update_activity(void) {
    g_uds_ctx.last_activity_ms = get_time_ms();
}

bool doip_uds_is_session_active(uint32_t timeout_ms) {
    uint32_t now = get_time_ms();
    return (now - g_uds_ctx.last_activity_ms) < timeout_ms;
}

void doip_uds_deinit(void) {
    g_uds_ctx.current_session = UDS_SESSION_DEFAULT_STATE;
    g_uds_ctx.security_unlocked = false;
    printf("[UDS] Module deinitialized\n");
}