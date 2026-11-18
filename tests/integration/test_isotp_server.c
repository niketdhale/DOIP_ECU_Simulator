/**
 * @file test_isotp_server.c
 * @brief Integration tests for the ISO-TP path of the TCP server.
 *
 * Built with -DDOIP_ENABLE_ISO_TP=true (make test-isotp-server). With ISO-TP
 * enabled every DoIP diagnostic message carries one ISO 15765-2 N-PDU in
 * both directions: the tester sends SF / FF / CF frames, the ECU answers
 * with a Flow Control after a First Frame and segments its UDS responses.
 *
 * Run: ./test-isotp-server
 */
#include "unity.h"
#include "fixture.h"
#include "transport/doip_isotp.h"
#include "config/doip_uds_config.h"
#include "client/doip_client.h"

#if !DOIP_ENABLE_ISO_TP
#error "build with -DDOIP_ENABLE_ISO_TP=true"
#endif

#define TEST_VIN    "ISOTPVIN123456789"
#define TEST_SERIAL "SN-ISOTP"
#define TEST_NAME   "IsoTpNode"
#define TEST_SW     "V7.7"

static DoIP_EcuIdentity_t s_identity;
static int s_fd = -1;

void setUp(void)
{
    memset(&s_identity, 0, sizeof(s_identity));
    memcpy(s_identity.vin, TEST_VIN, DOIP_VIN_LENGTH);
    strcpy(s_identity.serial_number,    TEST_SERIAL);
    strcpy(s_identity.system_name,      TEST_NAME);
    strcpy(s_identity.software_version, TEST_SW);

    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s3_server_timeout_ms = 5000;
    cfg.ecu_identity         = &s_identity;
    TEST_ASSERT_EQUAL_INT(0, fx_start(&cfg));

    s_fd = fx_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, s_fd);
    TEST_ASSERT_EQUAL_INT(0x10, fx_activate(s_fd));
}

void tearDown(void)
{
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    fx_stop();
}

/** Receive N-PDUs until a complete UDS message is reassembled. Returns its length, or -1. */
static int recv_uds(uint8_t *out, uint16_t out_size)
{
    static doip_isotp_ctx_t rx;        /* tester-side reassembly state */
    uint8_t npdu[64];
    doip_isotp_reset(&rx);
    for (;;) {
        int n = fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL);
        if (n < 0) return -1;
        TEST_ASSERT_EQUAL_INT_MESSAGE(8, n, "every N-PDU from the ECU is padded to 8 bytes");
        int len = doip_isotp_rx(&rx, npdu, (uint16_t)n, out, out_size, NULL, NULL);
        if (len != 0) return len;
    }
}

/* ── Single frame in, single frame out ─────────────────────────────── */

void test_single_frame_request_and_response(void)
{
    const uint8_t sf[] = { 0x02, UDS_SID_TESTER_PRESENT, 0x00 };
    uint8_t npdu[16];
    int acks = 0;
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));

    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), &acks));
    const uint8_t expected[8] = { 0x02, 0x7E, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, npdu, 8);
    TEST_ASSERT_EQUAL_INT(1, acks);
}

/* ── Response longer than 7 bytes is segmented: FF + CFs ───────────── */

void test_long_response_is_segmented(void)
{
    const uint8_t sf[] = { 0x03, UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t npdu[16];
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));

    /* 62 F1 90 + 17-byte VIN = 20 bytes: FF (6 data) + CF1 (7) + CF2 (7) */
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t ff[8] = { 0x10, 0x14, 0x62, 0xF1, 0x90, 'I', 'S', 'O' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ff, npdu, 8);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t cf1[8] = { 0x21, 'T', 'P', 'V', 'I', 'N', '1', '2' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cf1, npdu, 8);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t cf2[8] = { 0x22, '3', '4', '5', '6', '7', '8', '9' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cf2, npdu, 8);
}

/* ── Request longer than 7 bytes: FF -> Flow Control -> CF ──────────── */

void test_segmented_request_gets_flow_control_and_answer(void)
{
    /* 22 F190 F18C F186 F182 = 9 bytes */
    const uint8_t ff[] = { 0x10, 0x09, UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90, 0xF1, 0x8C, 0xF1 };
    const uint8_t cf[] = { 0x21, 0x86, 0xF1, 0x82 };
    uint8_t npdu[16];
    int acks = 0;

    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, ff, sizeof(ff));
    TEST_ASSERT_EQUAL_INT(3, fx_recv_diag(s_fd, npdu, sizeof(npdu), &acks));
    const uint8_t fc[3] = { 0x30, 0x00, 0x00 };          /* continue to send, no block limit */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fc, npdu, 3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, acks, "the First Frame itself is acknowledged");

    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, cf, sizeof(cf));
    uint8_t uds[256];
    char expected[256];
    int elen = snprintf(expected, sizeof(expected), "\x62\xF1\x90%s\xF1\x8C%s\xF1\x86%s\xF1\x82%s",
                        TEST_VIN, TEST_SERIAL, TEST_NAME, TEST_SW);
    TEST_ASSERT_EQUAL_INT(elen, recv_uds(uds, sizeof(uds)));
    TEST_ASSERT_EQUAL_MEMORY(expected, uds, (size_t)elen);
}

/* ── Delayed response: the "response pending" frames are ISO-TP framed too ── */

void test_response_pending_is_framed(void)
{
    DoIP_FaultConfig_t f = { .response_delay_ms = 200 };
    TEST_ASSERT_EQUAL_INT(0, DoIP_SetFaults(fx_server, &f));

    const uint8_t sf[] = { 0x02, UDS_SID_TESTER_PRESENT, 0x00 };
    uint8_t uds[16];
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));

    const uint8_t pending[3] = { 0x7F, UDS_SID_TESTER_PRESENT, 0x78 }, final[2] = { 0x7E, 0x00 };
    TEST_ASSERT_EQUAL_INT(3, recv_uds(uds, sizeof(uds)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pending, uds, 3);
    TEST_ASSERT_EQUAL_INT(2, recv_uds(uds, sizeof(uds)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(final, uds, 2);
}

/* ── Protocol error: a Consecutive Frame without a First Frame closes the socket ── */

void test_unexpected_consecutive_frame_closes_connection(void)
{
    const uint8_t cf[] = { 0x21, 0x01, 0x02, 0x03 };
    uint8_t npdu[16];
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, cf, sizeof(cf));
    TEST_ASSERT_EQUAL_INT(-1, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    /* really closed, not just silent */
    fx_set_timeout(s_fd, 500);
    TEST_ASSERT_EQUAL_INT(0, (int)recv(s_fd, npdu, sizeof(npdu), 0));
}

/* ── Client library: ISO-TP in both directions ─────────────────────── */

static uint16_t lib_transact(const uint8_t *req, uint16_t rlen, uint8_t *resp, uint16_t size)
{
    DoIP_Client_t *c = DoIP_Client_Create();
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Init(c, NULL));
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Connect(c, "127.0.0.1", DOIP_TCP_PORT));
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_ActivateRouting(c, 0x00, NULL));
    uint16_t n = 0;
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK,
        DoIP_Client_Transact(c, DOIP_ECU_LOGICAL_ADDRESS, req, rlen, resp, size, &n));
    DoIP_Client_Destroy(c);
    return n;
}

void test_client_library_short_request_and_response(void)
{
    const uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x00 }, exp[] = { 0x7E, 0x00 };
    uint8_t resp[16];
    TEST_ASSERT_EQUAL_UINT16(2, lib_transact(req, sizeof(req), resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, resp, 2);
}

void test_client_library_long_response(void)
{
    const uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t resp[64];
    TEST_ASSERT_EQUAL_UINT16(3 + DOIP_VIN_LENGTH, lib_transact(req, sizeof(req), resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, resp[0]);
    TEST_ASSERT_EQUAL_MEMORY(TEST_VIN, resp + 3, DOIP_VIN_LENGTH);
}

void test_client_library_long_request(void)
{
    const uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90, 0xF1, 0x8C, 0xF1, 0x86, 0xF1, 0x82 };
    uint8_t resp[256];
    char expected[256];
    int elen = snprintf(expected, sizeof(expected), "\x62\xF1\x90%s\xF1\x8C%s\xF1\x86%s\xF1\x82%s",
                        TEST_VIN, TEST_SERIAL, TEST_NAME, TEST_SW);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)elen, lib_transact(req, sizeof(req), resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_MEMORY(expected, resp, (size_t)elen);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_frame_request_and_response);
    RUN_TEST(test_long_response_is_segmented);
    RUN_TEST(test_segmented_request_gets_flow_control_and_answer);
    RUN_TEST(test_response_pending_is_framed);
    RUN_TEST(test_unexpected_consecutive_frame_closes_connection);
    RUN_TEST(test_client_library_short_request_and_response);
    RUN_TEST(test_client_library_long_response);
    RUN_TEST(test_client_library_long_request);
    return UNITY_END();
}
