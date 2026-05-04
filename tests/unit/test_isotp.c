/**
 * @file test_isotp.c
 * @brief Unit tests for transport/doip_isotp.c — no sockets, pure logic.
 *
 * Tests cover: SF rx/tx, FF+CF rx, multi-CF rx, SN mismatch, overflow,
 * FC generation, TX segmentation, and context reset.
 *
 * Build (ISO-TP must be enabled):
 *   make DOIP_ISOTP=1 test-isotp
 * Run:
 *   ./test-isotp
 */
#define DOIP_ENABLE_ISO_TP true   /* Force-enable for unit testing */

#include "unity.h"
#include "transport/doip_isotp.h"
#include <string.h>
#include <stdint.h>

/* ── Unity lifecycle ────────────────────────────────────────────────────── */

static doip_isotp_ctx_t g_ctx;

void setUp(void)    { doip_isotp_reset(&g_ctx); }
void tearDown(void) { doip_isotp_reset(&g_ctx); }

/* ── Helpers ────────────────────────────────────────────────────────────── */

/** Build a 1+data Single Frame into buf. Returns total length. */
static uint8_t make_sf(uint8_t *buf, const uint8_t *data, uint8_t dl)
{
    buf[0] = (uint8_t)(ISOTP_PCI_SF | (dl & 0x0FU));
    memcpy(buf + 1, data, dl);
    return (uint8_t)(1U + dl);
}

/** Build a First Frame carrying the first 6 bytes of data. */
static uint8_t make_ff(uint8_t *buf, uint16_t total_len,
                        const uint8_t *data)
{
    buf[0] = (uint8_t)(ISOTP_PCI_FF | ((total_len >> 8U) & 0x0FU));
    buf[1] = (uint8_t)(total_len & 0xFFU);
    memcpy(buf + 2, data, 6);
    return 8U;
}

/** Build a Consecutive Frame with up to 7 data bytes. */
static uint8_t make_cf(uint8_t *buf, uint8_t sn,
                        const uint8_t *data, uint8_t dl)
{
    buf[0] = (uint8_t)(ISOTP_PCI_CF | (sn & 0x0FU));
    memcpy(buf + 1, data, dl);
    return (uint8_t)(1U + dl);
}

/* ── RX tests ───────────────────────────────────────────────────────────── */

/** TEST 1: Single Frame with 1 byte of data is reassembled immediately. */
void test_sf_one_byte(void)
{
    uint8_t in[2]  = { 0x01, 0xAA };
    uint8_t out[8] = {0};
    int rc = doip_isotp_rx(&g_ctx, in, 2, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT8(0xAA, out[0]);
}

/** TEST 2: Single Frame with max SF payload (7 bytes). */
void test_sf_max_bytes(void)
{
    uint8_t data[7] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77};
    uint8_t in[8];
    uint8_t len = make_sf(in, data, 7);
    uint8_t out[8] = {0};
    int rc = doip_isotp_rx(&g_ctx, in, len, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(7, rc);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 7);
}

/** TEST 3: SF with DL=0 is rejected. */
void test_sf_zero_dl_rejected(void)
{
    uint8_t in[1] = { 0x00 };
    uint8_t out[8];
    int rc = doip_isotp_rx(&g_ctx, in, 1, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/** TEST 4: SF with DL > 7 is rejected (> DOIP_ISOTP_SF_MAX). */
void test_sf_oversized_dl_rejected(void)
{
    uint8_t in[9] = { 0x08 }; /* DL=8 */
    uint8_t out[16];
    int rc = doip_isotp_rx(&g_ctx, in, 9, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/** TEST 5: First Frame starts reassembly; returns 0 (waiting) and emits FC. */
void test_ff_starts_reassembly(void)
{
    uint8_t data[14]; memset(data, 0x55, 14);
    uint8_t in[8];
    make_ff(in, 14, data);

    uint8_t fc[8];
    uint8_t fc_len = 0;
    uint8_t out[32] = {0};
    int rc = doip_isotp_rx(&g_ctx, in, 8, out, sizeof(out), fc, &fc_len);

    TEST_ASSERT_EQUAL_INT(0, rc);           /* Waiting for CFs */
    TEST_ASSERT_EQUAL_UINT8(3, fc_len);     /* FC CTS is 3 bytes */
    TEST_ASSERT_EQUAL_UINT8(ISOTP_PCI_FC | ISOTP_FC_CTS, fc[0]);
    TEST_ASSERT_EQUAL_UINT8(6, g_ctx.rx_received); /* 6 bytes from FF */
    TEST_ASSERT_EQUAL_UINT16(14, g_ctx.rx_total);
}

/** TEST 6: FF + one CF → complete reassembly (14-byte message). */
void test_ff_plus_one_cf_complete(void)
{
    uint8_t data[14];
    for (int i = 0; i < 14; i++) data[i] = (uint8_t)i;

    uint8_t in_ff[8], in_cf[9];
    make_ff(in_ff, 14, data);
    make_cf(in_cf, 1, data + 6, 8); /* last 8 bytes (padded, 14-6=8) */

    uint8_t out[32] = {0};
    uint8_t fc[8]; uint8_t fc_len = 0;

    int rc = doip_isotp_rx(&g_ctx, in_ff, 8, out, sizeof(out), fc, &fc_len);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = doip_isotp_rx(&g_ctx, in_cf, 9, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(14, rc);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 14);
}

/** TEST 7: Multi-CF reassembly — 20 byte message (1 FF + 2 CFs). */
void test_multi_cf_reassembly(void)
{
    uint8_t data[20];
    for (int i = 0; i < 20; i++) data[i] = (uint8_t)(i + 1);

    uint8_t in_ff[8]; make_ff(in_ff, 20, data);
    uint8_t in_cf1[8]; make_cf(in_cf1, 1, data + 6, 7);
    uint8_t in_cf2[8]; make_cf(in_cf2, 2, data + 13, 7);

    uint8_t out[32] = {0};
    uint8_t fc[8]; uint8_t fc_len = 0;

    int rc;
    rc = doip_isotp_rx(&g_ctx, in_ff,  8, out, sizeof(out), fc, &fc_len);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = doip_isotp_rx(&g_ctx, in_cf1, 8, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = doip_isotp_rx(&g_ctx, in_cf2, 8, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(20, rc);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 20);
}

/** TEST 8: Sequence number mismatch in CF causes -1 error. */
void test_cf_sn_mismatch_rejected(void)
{
    uint8_t data[14]; memset(data, 0xBB, 14);
    uint8_t in_ff[8]; make_ff(in_ff, 14, data);
    uint8_t in_cf[9]; make_cf(in_cf, 3 /* wrong SN, expected 1 */, data + 6, 8);

    uint8_t out[32]; uint8_t fc[8]; uint8_t fc_len = 0;
    doip_isotp_rx(&g_ctx, in_ff, 8, out, sizeof(out), fc, &fc_len);
    int rc = doip_isotp_rx(&g_ctx, in_cf, 9, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    /* Context must be reset after error */
    TEST_ASSERT_EQUAL_UINT16(0, g_ctx.rx_total);
}

/** TEST 9: CF without preceding FF is rejected. */
void test_cf_without_ff_rejected(void)
{
    uint8_t in_cf[8]; make_cf(in_cf, 1, (uint8_t[]){0xDE,0xAD}, 2);
    uint8_t out[32];
    int rc = doip_isotp_rx(&g_ctx, in_cf, 3, out, sizeof(out), NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/** TEST 10: NULL pointer arguments are rejected. */
void test_rx_null_args_rejected(void)
{
    uint8_t in[8] = {0x01, 0xAA};
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(-1, doip_isotp_rx(NULL, in,  2, out, 8, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, doip_isotp_rx(&g_ctx, NULL, 2, out, 8, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, doip_isotp_rx(&g_ctx, in,  0, out, 8, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, doip_isotp_rx(&g_ctx, in,  2, NULL, 8, NULL, NULL));
}

/* ── TX tests ───────────────────────────────────────────────────────────── */

/** TEST 11: Short payload (≤7 bytes) produces exactly one SF segment. */
void test_tx_single_frame(void)
{
    uint8_t data[5] = {0x22, 0xF1, 0x90, 0x00, 0x01};
    uint8_t segs[4][8];
    int count = doip_isotp_tx(&g_ctx, data, 5, segs, 4);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT8(ISOTP_PCI_SF | 5, segs[0][0]);
    TEST_ASSERT_EQUAL_MEMORY(data, segs[0] + 1, 5);
}

/** TEST 12: 13-byte payload → 1 FF (6 bytes) + 1 CF (7 bytes). */
void test_tx_ff_plus_cf(void)
{
    uint8_t data[13];
    for (int i = 0; i < 13; i++) data[i] = (uint8_t)i;
    uint8_t segs[4][8];
    int count = doip_isotp_tx(&g_ctx, data, 13, segs, 4);
    TEST_ASSERT_EQUAL_INT(2, count);

    /* FF checks */
    TEST_ASSERT_EQUAL_UINT8(ISOTP_PCI_FF | 0x00, segs[0][0]);
    TEST_ASSERT_EQUAL_UINT8(13, segs[0][1]);
    TEST_ASSERT_EQUAL_MEMORY(data, segs[0] + 2, 6);

    /* CF checks — carries the remaining 7 bytes */
    TEST_ASSERT_EQUAL_UINT8(ISOTP_PCI_CF | 0x01, segs[1][0]);
    TEST_ASSERT_EQUAL_MEMORY(data + 6, segs[1] + 1, 7);
}

/** TEST 13: TX with max_segs too small returns -1. */
void test_tx_max_segs_too_small(void)
{
    uint8_t data[20]; memset(data, 0xAA, 20);
    uint8_t segs[2][8];
    /* 20 bytes needs FF + 2 CFs = 3 segments; only 2 slots provided */
    int count = doip_isotp_tx(&g_ctx, data, 20, segs, 2);
    TEST_ASSERT_EQUAL_INT(-1, count);
}

/** TEST 14: doip_isotp_reset zeroes the context. */
void test_reset_clears_context(void)
{
    /* Partially fill context */
    g_ctx.rx_total    = 100;
    g_ctx.rx_received = 50;
    g_ctx.rx_sn       = 3;
    doip_isotp_reset(&g_ctx);
    TEST_ASSERT_EQUAL_UINT16(0, g_ctx.rx_total);
    TEST_ASSERT_EQUAL_UINT16(0, g_ctx.rx_received);
    TEST_ASSERT_EQUAL_UINT8(0, g_ctx.rx_sn);
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    /* RX tests */
    RUN_TEST(test_sf_one_byte);
    RUN_TEST(test_sf_max_bytes);
    RUN_TEST(test_sf_zero_dl_rejected);
    RUN_TEST(test_sf_oversized_dl_rejected);
    RUN_TEST(test_ff_starts_reassembly);
    RUN_TEST(test_ff_plus_one_cf_complete);
    RUN_TEST(test_multi_cf_reassembly);
    RUN_TEST(test_cf_sn_mismatch_rejected);
    RUN_TEST(test_cf_without_ff_rejected);
    RUN_TEST(test_rx_null_args_rejected);

    /* TX tests */
    RUN_TEST(test_tx_single_frame);
    RUN_TEST(test_tx_ff_plus_cf);
    RUN_TEST(test_tx_max_segs_too_small);
    RUN_TEST(test_reset_clears_context);

    return UNITY_END();
}
