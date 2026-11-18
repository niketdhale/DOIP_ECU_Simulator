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
#include <time.h>
#include <stdbool.h>
#include "unity.h"
#include "fixture.h"
#include "transport/doip_isotp.h"
#include "config/doip_uds_config.h"

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

static void send_fc(uint8_t fs, uint8_t bs, uint8_t stmin)
{
    const uint8_t fc[3] = { (uint8_t)(0x30 | fs), bs, stmin };
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, fc, sizeof(fc));
}

/**
 * Receive N-PDUs until a complete UDS message is reassembled, acting as the
 * tester: FC (CTS, @p bs, @p stmin) after the First Frame and after every
 * @p bs Consecutive Frames. Returns the message length, or -1.
 */
static int recv_uds_fc(uint8_t *out, uint16_t out_size, uint8_t bs, uint8_t stmin)
{
    static doip_isotp_ctx_t rx;        /* tester-side reassembly state */
    uint8_t npdu[64];
    int cfs = 0;
    doip_isotp_reset(&rx);
    for (;;) {
        int n = fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL);
        if (n < 0) return -1;
        TEST_ASSERT_EQUAL_INT_MESSAGE(8, n, "every N-PDU from the ECU is padded to 8 bytes");
        int len = doip_isotp_rx(&rx, npdu, (uint16_t)n, out, out_size, NULL, NULL);
        if (len != 0) return len;
        if ((npdu[0] & 0xF0) == 0x10 || (bs && ++cfs % bs == 0)) send_fc(ISOTP_FC_CTS, bs, stmin);
    }
}

static int recv_uds(uint8_t *out, uint16_t out_size) { return recv_uds_fc(out, out_size, 0, 0); }

/** True if nothing arrives within @p ms. */
static bool silent_for(int ms)
{
    uint8_t npdu[16];
    fx_set_timeout(s_fd, ms);
    bool silent = fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL) < 0;
    fx_set_timeout(s_fd, 2000);
    return silent;
}

static void send_read_vin(void)
{
    const uint8_t sf[] = { 0x03, UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));
}

static void expect_tester_present_works(void)
{
    const uint8_t sf[] = { 0x02, UDS_SID_TESTER_PRESENT, 0x00 };
    uint8_t uds[16];
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));
    TEST_ASSERT_EQUAL_INT(2, recv_uds(uds, sizeof(uds)));
    TEST_ASSERT_EQUAL_HEX8(0x7E, uds[0]);
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
    TEST_ASSERT_MESSAGE(silent_for(200), "no CF before the tester's Flow Control");
    send_fc(ISOTP_FC_CTS, 0, 0);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t cf1[8] = { 0x21, 'T', 'P', 'V', 'I', 'N', '1', '2' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cf1, npdu, 8);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t cf2[8] = { 0x22, '3', '4', '5', '6', '7', '8', '9' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cf2, npdu, 8);
}

/* ── Flow Control handling on the ECU's transmit side ──────────────── */

void test_block_size_needs_fc_per_block(void)
{
    uint8_t npdu[16];
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));   /* FF  */
    send_fc(ISOTP_FC_CTS, 1, 0);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));   /* CF1 */
    TEST_ASSERT_EQUAL_HEX8(0x21, npdu[0]);
    TEST_ASSERT_MESSAGE(silent_for(200), "BS=1: CF2 must wait for another FC");
    send_fc(ISOTP_FC_CTS, 1, 0);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));   /* CF2 */
    TEST_ASSERT_EQUAL_HEX8(0x22, npdu[0]);
    TEST_ASSERT_MESSAGE(silent_for(200), "no more CFs after the last one");
}

void test_stmin_spaces_consecutive_frames(void)
{
    uint8_t npdu[16];
    struct timespec t1, t2;
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    send_fc(ISOTP_FC_CTS, 0, 20);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long ms = (t2.tv_sec - t1.tv_sec) * 1000L + (t2.tv_nsec - t1.tv_nsec) / 1000000L;
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(18, ms, "CFs at least STmin apart");
    TEST_ASSERT_LESS_THAN_INT(200, ms);
}

void test_flow_control_wait_then_cts(void)
{
    uint8_t npdu[16];
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    send_fc(ISOTP_FC_WAIT, 0, 0);
    TEST_ASSERT_MESSAGE(silent_for(300), "FC Wait: no CF");
    send_fc(ISOTP_FC_WAIT, 0, 0);
    send_fc(ISOTP_FC_CTS, 0, 0);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    TEST_ASSERT_EQUAL_HEX8(0x21, npdu[0]);
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    TEST_ASSERT_EQUAL_HEX8(0x22, npdu[0]);
}

void test_flow_control_overflow_aborts_response(void)
{
    uint8_t npdu[16];
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    send_fc(ISOTP_FC_OVFL, 0, 0);
    TEST_ASSERT_MESSAGE(silent_for(300), "FC Overflow: no CFs");
    expect_tester_present_works();                       /* connection still usable, not busy */
}

void test_missing_flow_control_times_out(void)
{
    uint8_t npdu[16];
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    TEST_ASSERT_MESSAGE(silent_for(300), "no FC: no CFs");

    /* meanwhile the response is still in flight: a new request is answered busy */
    const uint8_t sf[] = { 0x02, UDS_SID_TESTER_PRESENT, 0x00 };
    fx_send_diag(s_fd, DOIP_ECU_LOGICAL_ADDRESS, sf, sizeof(sf));
    TEST_ASSERT_EQUAL_INT(8, fx_recv_diag(s_fd, npdu, sizeof(npdu), NULL));
    const uint8_t busy[4] = { 0x03, 0x7F, UDS_SID_TESTER_PRESENT, 0x21 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(busy, npdu, 4);

    usleep((DOIP_ISOTP_N_BS_MS + 200) * 1000U);          /* N_Bs expires */
    TEST_ASSERT_MESSAGE(silent_for(50), "still no CFs after N_Bs");
    expect_tester_present_works();
}

/* ── Block size and STmin with the reassembling helper ─────────────── */

void test_long_response_with_bs2_and_stmin(void)
{
    uint8_t uds[64];
    send_read_vin();
    TEST_ASSERT_EQUAL_INT(20, recv_uds_fc(uds, sizeof(uds), 2, 1));
    TEST_ASSERT_EQUAL_MEMORY("\x62\xF1\x90" TEST_VIN, uds, 20);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_frame_request_and_response);
    RUN_TEST(test_long_response_is_segmented);
    RUN_TEST(test_block_size_needs_fc_per_block);
    RUN_TEST(test_stmin_spaces_consecutive_frames);
    RUN_TEST(test_flow_control_wait_then_cts);
    RUN_TEST(test_flow_control_overflow_aborts_response);
    RUN_TEST(test_missing_flow_control_times_out);
    RUN_TEST(test_long_response_with_bs2_and_stmin);
    RUN_TEST(test_segmented_request_gets_flow_control_and_answer);
    RUN_TEST(test_response_pending_is_framed);
    RUN_TEST(test_unexpected_consecutive_frame_closes_connection);
    return UNITY_END();
}
