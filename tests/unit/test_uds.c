/**
 * @file test_uds.c
 * @brief Unit tests for transport/doip_uds.c — UDS request processing.
 *
 * No sockets opened. Calls uds_init() + doip_uds_process_request()
 * directly with in-memory buffers. FSM callbacks are provided by
 * tests/mocks/mock_fsm.c.
 *
 * Links: unity.c  doip_uds.c  mock_fsm.c  doip_log.c
 *
 * Run: ./test-uds
 */
#include "unity.h"
#include "transport/doip_uds.h"
#include "core/doip_context.h"
#include "config/doip_uds_config.h"
#include "core/doip_log.h"
#include "mock_fsm.h"
#include <string.h>
#include <time.h>

/* ── Test identity ─────────────────────────────────────────────────── */
#define TEST_VIN    "TESTVIN12345678AB"
#define TEST_SW     "V9.9.9"
#define TEST_NAME   "TestECU"
#define TEST_SERIAL "SN-UNIT-001"

static DoIP_EcuIdentity_t s_test_identity;

/* The simulator instance under test. Only its UDS-related parts are used:
 * identity, request hook and DTC memory. */
static struct DoIP_Context s_ecu;

typedef int (*uds_hook_t)(uint8_t, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t*, void*);

/** (Re)initialise the instance's UDS state; a NULL identity keeps the current one. */
static void uds_init(const DoIP_EcuIdentity_t *identity, uds_hook_t hook, void *user_ctx)
{
    if (identity) s_ecu.identity = *identity;
    s_ecu.config.on_uds_request = hook;
    s_ecu.config.user_context   = user_ctx;
    doip_uds_init(&s_ecu);
}

/* ── Shared request / response buffers ────────────────────────────── */
static uint8_t  s_resp[DOIP_MAX_PAYLOAD_SIZE];
static uint16_t s_rlen;

/* ── Helpers ───────────────────────────────────────────────────────── */

/** Return a fresh UdsClientContext_t with a recent activity timestamp. */
static UdsClientContext_t make_ctx(void)
{
    UdsClientContext_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ecu             = &s_ecu;
    ctx.current_session = UDS_SESSION_DEFAULT_STATE;
    /* Set last_activity to now so S3 timeout doesn't fire */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx.last_activity_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return ctx;
}

/** Convenience wrapper: process a request and store results in s_resp/s_rlen. */
static int process(UdsClientContext_t *ctx, const uint8_t *req, uint16_t req_len)
{
    s_rlen = 0;
    return doip_uds_process_request(ctx, req, req_len, s_resp, &s_rlen);
}

/* ── Unity lifecycle ───────────────────────────────────────────────── */

void setUp(void)
{
    mock_fsm_reset();
    memset(&s_test_identity, 0, sizeof(s_test_identity));
    memcpy(s_test_identity.vin,              TEST_VIN,    DOIP_VIN_LENGTH);
    strncpy(s_test_identity.software_version, TEST_SW,   sizeof(s_test_identity.software_version) - 1);
    strncpy(s_test_identity.system_name,      TEST_NAME,  sizeof(s_test_identity.system_name) - 1);
    strncpy(s_test_identity.serial_number,    TEST_SERIAL,sizeof(s_test_identity.serial_number) - 1);
    uds_init(&s_test_identity, NULL, NULL);
}

void tearDown(void)
{
    doip_uds_deinit();
}

/* ── Identity tests ────────────────────────────────────────────────── */

void test_init_null_identity_uses_default(void)
{
    uds_init(NULL, NULL, NULL);  /* re-init with no identity */

    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    int rc = process(&ctx, req, sizeof(req));

    /* Should succeed (positive response) — defaults must be non-empty */
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, s_resp[0]);
    TEST_ASSERT_TRUE(s_rlen > 3);
}

void test_identity_vin_returned(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_INT(3 + DOIP_VIN_LENGTH, s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(TEST_VIN, &s_resp[3], DOIP_VIN_LENGTH);
}

void test_identity_sw_version_returned(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                      (uint8_t)(UDS_DID_SOFTWARE_VERSION >> 8),
                      (uint8_t)(UDS_DID_SOFTWARE_VERSION & 0xFF) };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, s_resp[0]);
    /* Response payload starts at [3] */
    size_t sw_len = strlen(TEST_SW);
    TEST_ASSERT_EQUAL_INT((int)(3 + sw_len), s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(TEST_SW, &s_resp[3], sw_len);
}

void test_identity_system_name_returned(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                      (uint8_t)(UDS_DID_SYSTEM_NAME >> 8),
                      (uint8_t)(UDS_DID_SYSTEM_NAME & 0xFF) };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t n = strlen(TEST_NAME);
    TEST_ASSERT_EQUAL_INT((int)(3 + n), s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(TEST_NAME, &s_resp[3], n);
}

void test_identity_serial_returned(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                      (uint8_t)(UDS_DID_ECU_SERIAL_NUMBER >> 8),
                      (uint8_t)(UDS_DID_ECU_SERIAL_NUMBER & 0xFF) };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    size_t n = strlen(TEST_SERIAL);
    TEST_ASSERT_EQUAL_INT((int)(3 + n), s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(TEST_SERIAL, &s_resp[3], n);
}

/* ── NULL / zero-length guard tests ───────────────────────────────── */

void test_process_request_null_args_returns_neg1(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { 0x3E, 0x00 };

    TEST_ASSERT_EQUAL_INT(-1, doip_uds_process_request(NULL, req,   sizeof(req), s_resp, &s_rlen));
    TEST_ASSERT_EQUAL_INT(-1, doip_uds_process_request(&ctx, NULL,  sizeof(req), s_resp, &s_rlen));
    TEST_ASSERT_EQUAL_INT(-1, doip_uds_process_request(&ctx, req,   sizeof(req), NULL,   &s_rlen));
    TEST_ASSERT_EQUAL_INT(-1, doip_uds_process_request(&ctx, req,   sizeof(req), s_resp, NULL));
    TEST_ASSERT_EQUAL_INT(-1, doip_uds_process_request(&ctx, req,   0,           s_resp, &s_rlen));
}

/* ── Session Control (0x10) ───────────────────────────────────────── */

void test_session_control_default(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_DEFAULT };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_SESSION_CONTROL_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SESSION_DEFAULT,          s_resp[1]);
    /* FSM mock must have been called with default session */
    TEST_ASSERT_EQUAL_INT(1,                  g_mock_session_change_calls);
    TEST_ASSERT_EQUAL_HEX8(UDS_SESSION_DEFAULT, g_mock_last_session_id);
}

void test_session_control_extended(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_SESSION_CONTROL_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SESSION_EXTENDED,         s_resp[1]);
    TEST_ASSERT_EQUAL_INT(1,                   g_mock_session_change_calls);
    TEST_ASSERT_EQUAL_HEX8(UDS_SESSION_EXTENDED, g_mock_last_session_id);
}

void test_session_control_invalid_nrc(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_SESSION_CONTROL, 0xFF };  /* unsupported */
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_HEX8(0x7F,                      s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_SESSION_CONTROL,   s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED, s_resp[2]);
    TEST_ASSERT_EQUAL_INT(0, g_mock_session_change_calls);  /* no FSM call */
}

void test_session_control_too_short_nrc(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_SESSION_CONTROL };  /* missing sub-function */
    int rc = process(&ctx, req, 1);

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_HEX8(0x7F,                           s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);
}

/* ── Read DID (0x22) ──────────────────────────────────────────────── */

void test_read_did_unknown_returns_nrc(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xDE, 0xAD };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_HEX8(0x7F,                    s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID, s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_REQUEST_OUT_OF_RANGE, s_resp[2]);
}

/* ── Tester Present (0x3E) ────────────────────────────────────────── */

void test_tester_present_valid(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x00 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_TESTER_PRESENT_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00,                        s_resp[1]);
    TEST_ASSERT_EQUAL_INT(1, g_mock_diag_activity_calls);
}

void test_tester_present_suppress_bit(void)
{
    /* Bit 7 set = suppress positive response: service runs, nothing is sent */
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x80 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(0, s_rlen);
    TEST_ASSERT_EQUAL_INT(1, g_mock_diag_activity_calls);
}

void test_suppress_bit_session_control_still_switches(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_SESSION_CONTROL, 0x80 | UDS_SESSION_EXTENDED };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, req, sizeof(req)));
    TEST_ASSERT_EQUAL_UINT16(0, s_rlen);
    TEST_ASSERT_EQUAL_INT(UDS_SESSION_EXTENDED_STATE, ctx.current_session);
}

void test_suppress_bit_does_not_hide_negative_response(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x81 };  /* suppress + invalid sub-fn */
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, req, sizeof(req)));
    TEST_ASSERT_EQUAL_UINT16(3, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_resp[0]);
}

void test_suppress_bit_ignored_for_services_without_subfunction(void)
{
    /* 0x22 has no sub-function: a DID starting with a high bit is not a suppress request */
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, req, sizeof(req)));
    TEST_ASSERT_EQUAL_UINT16(3 + DOIP_VIN_LENGTH, s_rlen);
}

/* ── Write Data By Identifier (0x2E) ───────────────────────────────── */

static UdsClientContext_t make_extended_ctx(void)
{
    UdsClientContext_t ctx = make_ctx();
    ctx.current_session   = UDS_SESSION_EXTENDED_STATE;
    ctx.security_unlocked = true;
    return ctx;
}

void test_write_did_vin_roundtrip(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    uint8_t wr[3 + DOIP_VIN_LENGTH] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x90 };
    memcpy(&wr[3], "NEWVIN00000000001", DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, wr, sizeof(wr)));
    TEST_ASSERT_EQUAL_UINT16(3, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_WRITE_DATA_BY_ID_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF1, s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(0x90, s_resp[2]);

    uint8_t rd[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, rd, sizeof(rd)));
    TEST_ASSERT_EQUAL_MEMORY("NEWVIN00000000001", &s_resp[3], DOIP_VIN_LENGTH);
}

void test_write_did_string_shorter_than_old_value(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    uint8_t wr[] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x8C, 'A', 'B' };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, wr, sizeof(wr)));

    uint8_t rd[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x8C };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, rd, sizeof(rd)));
    TEST_ASSERT_EQUAL_UINT16(3 + 2, s_rlen);
    TEST_ASSERT_EQUAL_MEMORY("AB", &s_resp[3], 2);
}

static void expect_write_nrc(UdsClientContext_t *ctx, const uint8_t *req, uint16_t len, uint8_t nrc)
{
    TEST_ASSERT_EQUAL_INT(-1, process(ctx, req, len));
    TEST_ASSERT_EQUAL_UINT16(3, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_WRITE_DATA_BY_ID, s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(nrc, s_resp[2]);
}

void test_write_did_negative_responses(void)
{
    UdsClientContext_t ext = make_extended_ctx();
    UdsClientContext_t def = make_ctx();

    uint8_t no_data[]   = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t short_vin[] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x90, 'X' };
    uint8_t read_only[] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x82, 'X' };  /* SW version */
    uint8_t ok_serial[] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x8C, 'X' };
    uint8_t too_long[3 + sizeof(((DoIP_EcuIdentity_t*)0)->serial_number)] =
                          { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x8C };

    expect_write_nrc(&ext, no_data,   sizeof(no_data),   UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    expect_write_nrc(&ext, short_vin, sizeof(short_vin), UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    expect_write_nrc(&ext, too_long,  sizeof(too_long),  UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    expect_write_nrc(&ext, read_only, sizeof(read_only), UDS_NRC_REQUEST_OUT_OF_RANGE);
    expect_write_nrc(&def, ok_serial, sizeof(ok_serial), UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);

    /* None of the rejected writes changed the serial number */
    uint8_t rd[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x8C };
    TEST_ASSERT_EQUAL_INT(0, process(&ext, rd, sizeof(rd)));
    TEST_ASSERT_EQUAL_MEMORY(TEST_SERIAL, &s_resp[3], strlen(TEST_SERIAL));
}

void test_tester_present_invalid_subfunc(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x01 };  /* sub-fn != 0x00 */
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_HEX8(0x7F,                    s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED, s_resp[2]);
    TEST_ASSERT_EQUAL_INT(0, g_mock_diag_activity_calls);
}

/* ── ECU Reset (0x11) ─────────────────────────────────────────────── */

void test_ecu_reset_valid(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_ECU_RESET, 0x01 };  /* hard reset */
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_ECU_RESET_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01,                   s_resp[1]);
}

/* ── Routine Control (0x31) ───────────────────────────────────────── */

void test_routine_control_basic(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    uint8_t req[] = { UDS_SID_ROUTINE_CONTROL, 0x01, 0xFF, 0x01 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_ROUTINE_CONTROL_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, s_resp[1]);  /* sub-function echoed */
}

void test_non_default_only_services_rejected_in_default_session(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t routine[] = { UDS_SID_ROUTINE_CONTROL, 0x01, 0xFF, 0x01 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, routine, sizeof(routine)));
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_ROUTINE_CONTROL, s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION, s_resp[2]);

    uint8_t comm_ctrl[] = { UDS_SID_COMMUNICATION_CONTROL, 0x00, 0x01 };  /* auto-responded SID */
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, comm_ctrl, sizeof(comm_ctrl)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION, s_resp[2]);
}

/* ── Security Access (0x27) ───────────────────────────────────────── */

/** Request a seed and send a key; returns the rc of the sendKey request. */
static int seed_and_key(UdsClientContext_t *ctx, uint32_t key_xor)
{
    uint8_t seed_req[] = { UDS_SID_SECURITY_ACCESS, 0x01 };
    TEST_ASSERT_EQUAL_INT(0, process(ctx, seed_req, sizeof(seed_req)));
    TEST_ASSERT_EQUAL_UINT16(6, s_rlen);
    uint32_t seed = ((uint32_t)s_resp[2] << 24) | ((uint32_t)s_resp[3] << 16) |
                    ((uint32_t)s_resp[4] << 8) | s_resp[5];
    TEST_ASSERT_NOT_EQUAL(0, seed);
    uint32_t key = seed ^ key_xor;
    uint8_t key_req[] = { UDS_SID_SECURITY_ACCESS, 0x02,
                          (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key };
    return process(ctx, key_req, sizeof(key_req));
}

static UdsClientContext_t make_locked_extended_ctx(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    ctx.security_unlocked = false;
    return ctx;
}

void test_security_access_unlocks_write(void)
{
    UdsClientContext_t ctx = make_locked_extended_ctx();
    uint8_t wr[] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x8C, 'Z' };

    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, wr, sizeof(wr)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SECURITY_ACCESS_DENIED, s_resp[2]);

    TEST_ASSERT_EQUAL_INT(0, seed_and_key(&ctx, UDS_SECURITY_KEY_MASK));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_SECURITY_ACCESS_RES, s_resp[0]);
    TEST_ASSERT_TRUE(ctx.security_unlocked);
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, wr, sizeof(wr)));

    /* Unlocked ECU reports a zero seed */
    uint8_t seed_req[] = { UDS_SID_SECURITY_ACCESS, 0x01 };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, seed_req, sizeof(seed_req)));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x00, &s_resp[2], 4);
}

void test_security_access_wrong_key_and_lockout(void)
{
    UdsClientContext_t ctx = make_locked_extended_ctx();
    for (int i = 1; i < UDS_SECURITY_MAX_ATTEMPTS; i++) {
        TEST_ASSERT_EQUAL_INT(-1, seed_and_key(&ctx, ~(uint32_t)UDS_SECURITY_KEY_MASK));
        TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INVALID_KEY, s_resp[2]);
    }
    TEST_ASSERT_EQUAL_INT(-1, seed_and_key(&ctx, ~(uint32_t)UDS_SECURITY_KEY_MASK));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_EXCEEDED_ATTEMPTS, s_resp[2]);
    TEST_ASSERT_FALSE(ctx.security_unlocked);

    /* Locked out: even a seed request is refused */
    uint8_t seed_req[] = { UDS_SID_SECURITY_ACCESS, 0x01 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, seed_req, sizeof(seed_req)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_TIME_DELAY_NOT_EXPIRED, s_resp[2]);
}

void test_security_access_sequence_and_format_errors(void)
{
    UdsClientContext_t ctx = make_locked_extended_ctx();
    uint8_t key_first[] = { UDS_SID_SECURITY_ACCESS, 0x02, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, key_first, sizeof(key_first)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_REQUEST_SEQUENCE_ERROR, s_resp[2]);

    uint8_t bad_level[] = { UDS_SID_SECURITY_ACCESS, 0x05 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, bad_level, sizeof(bad_level)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED, s_resp[2]);

    uint8_t short_key[] = { UDS_SID_SECURITY_ACCESS, 0x02, 0x00 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, short_key, sizeof(short_key)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);

    UdsClientContext_t def = make_ctx();
    uint8_t seed_req[] = { UDS_SID_SECURITY_ACCESS, 0x01 };
    TEST_ASSERT_EQUAL_INT(-1, process(&def, seed_req, sizeof(seed_req)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION, s_resp[2]);
}

/* ── DTCs (0x19 / 0x14) — default table: 010000/09, 030100/08, C07300/04 ── */

void test_dtc_count_and_list_by_status_mask(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t count_req[] = { UDS_SID_READ_DTC_INFO, 0x01, 0x08 };   /* confirmed */
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, count_req, sizeof(count_req)));
    const uint8_t count_exp[] = { 0x59, 0x01, 0xFF, 0x01, 0x00, 0x02 };
    TEST_ASSERT_EQUAL_UINT16(sizeof(count_exp), s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(count_exp, s_resp, sizeof(count_exp));

    uint8_t list_req[] = { UDS_SID_READ_DTC_INFO, 0x02, 0x08 };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, list_req, sizeof(list_req)));
    const uint8_t list_exp[] = { 0x59, 0x02, 0xFF, 0x01, 0x00, 0x00, 0x09, 0x03, 0x01, 0x00, 0x08 };
    TEST_ASSERT_EQUAL_UINT16(sizeof(list_exp), s_rlen);
    TEST_ASSERT_EQUAL_MEMORY(list_exp, s_resp, sizeof(list_exp));

    uint8_t all_req[] = { UDS_SID_READ_DTC_INFO, 0x0A };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, all_req, sizeof(all_req)));
    TEST_ASSERT_EQUAL_UINT16(3 + 3 * 4, s_rlen);
}

void test_dtc_clear_single_and_all(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t count_any[] = { UDS_SID_READ_DTC_INFO, 0x01, 0xFF };

    uint8_t clear_one[] = { UDS_SID_CLEAR_DTC, 0x03, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, clear_one, sizeof(clear_one)));
    TEST_ASSERT_EQUAL_UINT16(1, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_CLEAR_DTC_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, count_any, sizeof(count_any)));
    TEST_ASSERT_EQUAL_HEX8(2, s_resp[5]);

    uint8_t clear_all[] = { UDS_SID_CLEAR_DTC, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, clear_all, sizeof(clear_all)));
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, count_any, sizeof(count_any)));
    TEST_ASSERT_EQUAL_HEX8(0, s_resp[5]);

    /* Supported DTCs are still listed after a clear, with status 0 */
    uint8_t all_req[] = { UDS_SID_READ_DTC_INFO, 0x0A };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, all_req, sizeof(all_req)));
    TEST_ASSERT_EQUAL_UINT16(3 + 3 * 4, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(0x00, s_resp[6]);
}

void test_dtc_negative_responses(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t bad_sub[]   = { UDS_SID_READ_DTC_INFO, 0x55, 0xFF };
    uint8_t no_mask[]   = { UDS_SID_READ_DTC_INFO, 0x02 };
    uint8_t unknown[]   = { UDS_SID_CLEAR_DTC, 0x12, 0x34, 0x56 };
    uint8_t short_clr[] = { UDS_SID_CLEAR_DTC, 0xFF, 0xFF };

    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, bad_sub, sizeof(bad_sub)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED, s_resp[2]);
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, no_mask, sizeof(no_mask)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, unknown, sizeof(unknown)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_REQUEST_OUT_OF_RANGE, s_resp[2]);
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, short_clr, sizeof(short_clr)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);
}

/* ── Functional addressing rule ───────────────────────────────────── */

void test_functional_addressing_suppression_rule(void)
{
    const uint8_t silent[] = { 0x11, 0x12, 0x31, 0x7E, 0x7F };
    for (size_t i = 0; i < sizeof(silent); i++) {
        const uint8_t res[3] = { 0x7F, 0x22, silent[i] };
        TEST_ASSERT_TRUE_MESSAGE(doip_uds_functional_suppresses(res, 3), "NRC must stay silent");
    }
    const uint8_t sent[] = { 0x13, 0x21, 0x22, 0x33, 0x35, 0x78 };
    for (size_t i = 0; i < sizeof(sent); i++) {
        const uint8_t res[3] = { 0x7F, 0x22, sent[i] };
        TEST_ASSERT_FALSE_MESSAGE(doip_uds_functional_suppresses(res, 3), "NRC must be sent");
    }
    const uint8_t positive[3] = { 0x62, 0xF1, 0x11 };   /* positive response, not an NRC */
    TEST_ASSERT_FALSE(doip_uds_functional_suppresses(positive, 3));
    TEST_ASSERT_FALSE(doip_uds_functional_suppresses(positive, 0));
    TEST_ASSERT_FALSE(doip_uds_functional_suppresses(NULL, 3));
}

/* ── ECU Reset details ────────────────────────────────────────────── */

void test_ecu_reset_returns_to_default_session(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    ctx.security_unlocked = true;
    uint8_t req[] = { UDS_SID_ECU_RESET, UDS_RESET_SOFT };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, req, sizeof(req)));
    TEST_ASSERT_EQUAL_INT(UDS_SESSION_DEFAULT_STATE, ctx.current_session);
    TEST_ASSERT_FALSE(ctx.security_unlocked);
}

void test_ecu_reset_invalid_requests(void)
{
    UdsClientContext_t ctx = make_extended_ctx();
    uint8_t bad_type[] = { UDS_SID_ECU_RESET, 0x7E };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, bad_type, sizeof(bad_type)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED, s_resp[2]);
    uint8_t bad_len[] = { UDS_SID_ECU_RESET, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, bad_len, sizeof(bad_len)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);
    /* Rejected requests did not reset the session */
    TEST_ASSERT_EQUAL_INT(UDS_SESSION_EXTENDED_STATE, ctx.current_session);
}

/* ── Read DID: multiple identifiers ───────────────────────────────── */

void test_read_multiple_dids(void)
{
    UdsClientContext_t ctx = make_ctx();
    /* VIN, an unsupported DID (skipped), then serial number */
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90, 0x12, 0x34, 0xF1, 0x8C };
    TEST_ASSERT_EQUAL_INT(0, process(&ctx, req, sizeof(req)));

    size_t sn = strlen(TEST_SERIAL);
    TEST_ASSERT_EQUAL_UINT16(1 + 2 + DOIP_VIN_LENGTH + 2 + sn, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF1, s_resp[1]);
    TEST_ASSERT_EQUAL_HEX8(0x90, s_resp[2]);
    TEST_ASSERT_EQUAL_MEMORY(TEST_VIN, &s_resp[3], DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_HEX8(0xF1, s_resp[3 + DOIP_VIN_LENGTH]);
    TEST_ASSERT_EQUAL_HEX8(0x8C, s_resp[4 + DOIP_VIN_LENGTH]);
    TEST_ASSERT_EQUAL_MEMORY(TEST_SERIAL, &s_resp[5 + DOIP_VIN_LENGTH], sn);
}

void test_read_did_bad_length_and_too_long(void)
{
    UdsClientContext_t ctx = make_ctx();
    uint8_t odd[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90, 0xF1 };
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, odd, sizeof(odd)));
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_INCORRECT_MESSAGE_LENGTH, s_resp[2]);

    /* 300 x VIN would need 5700 bytes: must be refused, not overflow the buffer */
    static uint8_t many[1 + 2 * 300];
    many[0] = UDS_SID_READ_DATA_BY_ID;
    for (int i = 0; i < 300; i++) { many[1 + 2 * i] = 0xF1; many[2 + 2 * i] = 0x90; }
    TEST_ASSERT_EQUAL_INT(-1, process(&ctx, many, sizeof(many)));
    TEST_ASSERT_EQUAL_UINT16(3, s_rlen);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_RESPONSE_TOO_LONG, s_resp[2]);
}

/* ── Runtime hook tests ───────────────────────────────────────────── */

static uint8_t s_hook_called = 0;

static int hook_intercept(uint8_t sid,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_size,
                          uint16_t *resp_len_out,
                          void *ctx)
{
    (void)req; (void)req_len; (void)resp_size; (void)ctx;
    s_hook_called = 1;
    if (sid == UDS_SID_READ_DATA_BY_ID) {
        resp[0] = UDS_SID_READ_DATA_BY_ID_RES;
        resp[1] = 0xF1; resp[2] = 0x90;
        resp[3] = 'H';  resp[4] = 'O'; resp[5] = 'O'; resp[6] = 'K';
        *resp_len_out = 7;
        return 0;  /* handled */
    }
    return -1;
}

static int hook_passthrough(uint8_t sid,
                            const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t resp_size,
                            uint16_t *resp_len_out,
                            void *ctx)
{
    (void)sid; (void)req; (void)req_len; (void)resp; (void)resp_size;
    (void)resp_len_out; (void)ctx;
    s_hook_called = 1;
    return -1;  /* not handled — fall through */
}

void test_runtime_hook_intercepts_request(void)
{
    s_hook_called = 0;
    uds_init(&s_test_identity, hook_intercept, NULL);

    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, s_hook_called);
    TEST_ASSERT_EQUAL_INT(7, s_rlen);
    /* Hook wrote 'H','O','O','K' at [3..6] — NOT the real VIN */
    TEST_ASSERT_EQUAL_HEX8('H', s_resp[3]);
    TEST_ASSERT_EQUAL_HEX8('O', s_resp[4]);
}

void test_runtime_hook_fallthrough(void)
{
    s_hook_called = 0;
    uds_init(&s_test_identity, hook_passthrough, NULL);

    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, s_hook_called);  /* hook was called */
    /* But built-in handler responded with the real VIN */
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_MEMORY(TEST_VIN, &s_resp[3], DOIP_VIN_LENGTH);
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(void)
{
    DoIP_Log_Init(0, 0, NULL);

    UNITY_BEGIN();

    /* Identity */
    RUN_TEST(test_init_null_identity_uses_default);
    RUN_TEST(test_identity_vin_returned);
    RUN_TEST(test_identity_sw_version_returned);
    RUN_TEST(test_identity_system_name_returned);
    RUN_TEST(test_identity_serial_returned);

    /* Guard / NULL */
    RUN_TEST(test_process_request_null_args_returns_neg1);

    /* Session Control */
    RUN_TEST(test_session_control_default);
    RUN_TEST(test_session_control_extended);
    RUN_TEST(test_session_control_invalid_nrc);
    RUN_TEST(test_session_control_too_short_nrc);

    /* Read DID */
    RUN_TEST(test_read_did_unknown_returns_nrc);

    /* Tester Present */
    RUN_TEST(test_tester_present_valid);
    RUN_TEST(test_tester_present_suppress_bit);
    RUN_TEST(test_tester_present_invalid_subfunc);

    /* Suppress positive response bit */
    RUN_TEST(test_suppress_bit_session_control_still_switches);
    RUN_TEST(test_suppress_bit_does_not_hide_negative_response);
    RUN_TEST(test_suppress_bit_ignored_for_services_without_subfunction);

    /* Write DID */
    RUN_TEST(test_write_did_vin_roundtrip);
    RUN_TEST(test_write_did_string_shorter_than_old_value);
    RUN_TEST(test_write_did_negative_responses);

    /* ECU Reset */
    RUN_TEST(test_ecu_reset_valid);

    /* Routine Control */
    RUN_TEST(test_routine_control_basic);
    RUN_TEST(test_non_default_only_services_rejected_in_default_session);
    RUN_TEST(test_security_access_unlocks_write);
    RUN_TEST(test_security_access_wrong_key_and_lockout);
    RUN_TEST(test_security_access_sequence_and_format_errors);
    RUN_TEST(test_dtc_count_and_list_by_status_mask);
    RUN_TEST(test_dtc_clear_single_and_all);
    RUN_TEST(test_dtc_negative_responses);
    RUN_TEST(test_functional_addressing_suppression_rule);
    RUN_TEST(test_ecu_reset_returns_to_default_session);
    RUN_TEST(test_ecu_reset_invalid_requests);
    RUN_TEST(test_read_multiple_dids);
    RUN_TEST(test_read_did_bad_length_and_too_long);

    /* Runtime hook */
    RUN_TEST(test_runtime_hook_intercepts_request);
    RUN_TEST(test_runtime_hook_fallthrough);

    return UNITY_END();
}
