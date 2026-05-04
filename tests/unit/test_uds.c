/**
 * @file test_uds.c
 * @brief Unit tests for transport/doip_uds.c — UDS request processing.
 *
 * No sockets opened. Calls doip_uds_init() + doip_uds_process_request()
 * directly with in-memory buffers. FSM callbacks are provided by
 * tests/mocks/mock_fsm.c.
 *
 * Links: unity.c  doip_uds.c  mock_fsm.c  doip_log.c  doip_det.c
 *
 * Run: ./test-uds
 */
#include "unity.h"
#include "transport/doip_uds.h"
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

/* ── Shared request / response buffers ────────────────────────────── */
static uint8_t  s_resp[DOIP_MAX_PAYLOAD_SIZE];
static uint16_t s_rlen;

/* ── Helpers ───────────────────────────────────────────────────────── */

/** Return a fresh UdsClientContext_t with a recent activity timestamp. */
static UdsClientContext_t make_ctx(void)
{
    UdsClientContext_t ctx;
    memset(&ctx, 0, sizeof(ctx));
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
    doip_uds_init(&s_test_identity, NULL, NULL);
}

void tearDown(void)
{
    doip_uds_deinit();
}

/* ── Identity tests ────────────────────────────────────────────────── */

void test_init_null_identity_uses_default(void)
{
    doip_uds_init(NULL, NULL, NULL);  /* re-init with no identity */

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
    /* Bit 7 set = suppress positive response — server still replies in our impl */
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x80 };
    int rc = process(&ctx, req, sizeof(req));

    /* The handler masks req[1] & 0x7F == 0x00, so it is valid */
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_TESTER_PRESENT_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_INT(1, g_mock_diag_activity_calls);
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
    UdsClientContext_t ctx = make_ctx();
    uint8_t req[] = { UDS_SID_ROUTINE_CONTROL, 0x01, 0xFF, 0x01 };
    int rc = process(&ctx, req, sizeof(req));

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_ROUTINE_CONTROL_RES, s_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, s_resp[1]);  /* sub-function echoed */
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
    doip_uds_init(&s_test_identity, hook_intercept, NULL);

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
    doip_uds_init(&s_test_identity, hook_passthrough, NULL);

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

    /* ECU Reset */
    RUN_TEST(test_ecu_reset_valid);

    /* Routine Control */
    RUN_TEST(test_routine_control_basic);

    /* Runtime hook */
    RUN_TEST(test_runtime_hook_intercepts_request);
    RUN_TEST(test_runtime_hook_fallthrough);

    return UNITY_END();
}
