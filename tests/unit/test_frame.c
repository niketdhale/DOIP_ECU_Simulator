/**
 * @file test_frame.c
 * @brief Unit tests for core/doip_frame.c — serialize / deserialize only.
 *
 * No sockets are opened. Every test operates on raw byte buffers.
 * Links: unity.c  doip_frame.c  doip_log.c  doip_det.c
 *
 * Run: ./test-frame
 */
#include "unity.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include "config/doip_config.h"

/* ── Unity lifecycle ───────────────────────────────────────────────── */

void setUp(void)    { /* nothing — frame functions are stateless */ }
void tearDown(void) { /* nothing */ }

/* ── Helpers ───────────────────────────────────────────────────────── */

/** Build a valid wire-format DoIP header in buf[8]. */
static void make_valid_wire(uint8_t *buf, uint16_t pt, uint32_t pl_len)
{
    buf[0] = DOIP_PROTOCOL_VERSION;
    buf[1] = DOIP_INVERSE_VERSION;
    buf[2] = (uint8_t)(pt >> 8);
    buf[3] = (uint8_t)(pt & 0xFF);
    buf[4] = (uint8_t)(pl_len >> 24);
    buf[5] = (uint8_t)(pl_len >> 16);
    buf[6] = (uint8_t)(pl_len >>  8);
    buf[7] = (uint8_t)(pl_len & 0xFF);
}

/** Build a valid host-order doip_header_t. */
static doip_header_t make_valid_hdr(uint16_t pt, uint32_t pl_len)
{
    doip_header_t h;
    h.protocol_version = DOIP_PROTOCOL_VERSION;
    h.inverse_version  = DOIP_INVERSE_VERSION;
    h.payload_type     = pt;
    h.payload_length   = pl_len;
    return h;
}

/* ── Serialize tests ───────────────────────────────────────────────── */

void test_serialize_null_input_returns_neg1(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    doip_header_t hdr = make_valid_hdr(0x8001, 0);

    TEST_ASSERT_EQUAL_INT(-1, doip_serialize_header(NULL, buf,  sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, doip_serialize_header(&hdr, NULL, sizeof(buf)));
}

void test_serialize_buffer_too_small_returns_neg1(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    doip_header_t hdr = make_valid_hdr(0x8001, 0);

    /* One byte short */
    TEST_ASSERT_EQUAL_INT(-1, doip_serialize_header(&hdr, buf, DOIP_HEADER_SIZE - 1));
}

void test_serialize_returns_header_size(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    doip_header_t hdr = make_valid_hdr(0x8001, 42);

    ssize_t ret = doip_serialize_header(&hdr, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE, ret);
}

void test_serialize_byte_order_payload_type(void)
{
    /* payload_type 0x8001 must appear as [0x80][0x01] on wire (big-endian) */
    uint8_t buf[DOIP_HEADER_SIZE] = {0};
    doip_header_t hdr = make_valid_hdr(0x8001, 0);

    doip_serialize_header(&hdr, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_HEX8(0x80, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[3]);
}

void test_serialize_byte_order_payload_length(void)
{
    /* payload_length 0x00001234 → bytes [0x00][0x00][0x12][0x34] */
    uint8_t buf[DOIP_HEADER_SIZE] = {0};
    doip_header_t hdr = make_valid_hdr(0x0000, 0x00001234UL);

    doip_serialize_header(&hdr, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_HEX8(0x00, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[5]);
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[7]);
}

void test_serialize_protocol_version_written(void)
{
    uint8_t buf[DOIP_HEADER_SIZE] = {0};
    doip_header_t hdr = make_valid_hdr(0x0001, 0);

    doip_serialize_header(&hdr, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_HEX8(DOIP_PROTOCOL_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(DOIP_INVERSE_VERSION,  buf[1]);
}

/* ── Deserialize tests ─────────────────────────────────────────────── */

void test_deserialize_null_input_returns_neg1(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    doip_header_t out;
    make_valid_wire(buf, 0x8001, 0);

    TEST_ASSERT_EQUAL_INT(-1, doip_deserialize_header(NULL, DOIP_HEADER_SIZE, &out));
    TEST_ASSERT_EQUAL_INT(-1, doip_deserialize_header(buf,  DOIP_HEADER_SIZE, NULL));
    /* Buffer too short */
    TEST_ASSERT_EQUAL_INT(-1, doip_deserialize_header(buf,  DOIP_HEADER_SIZE - 1, &out));
}

void test_deserialize_bad_version_returns_neg2(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    make_valid_wire(buf, 0x8001, 0);
    buf[0] = 0xFF;  /* invalid protocol version */

    doip_header_t out;
    TEST_ASSERT_EQUAL_INT(-2, doip_deserialize_header(buf, DOIP_HEADER_SIZE, &out));
}

void test_deserialize_bad_inverse_returns_neg2(void)
{
    uint8_t buf[DOIP_HEADER_SIZE];
    make_valid_wire(buf, 0x8001, 0);
    buf[1] = 0x00;  /* inverse must be ~protocol_version = 0xFD */

    doip_header_t out;
    TEST_ASSERT_EQUAL_INT(-2, doip_deserialize_header(buf, DOIP_HEADER_SIZE, &out));
}

void test_deserialize_roundtrip(void)
{
    /* Serialize a header, then deserialize it back and compare all fields. */
    doip_header_t src = make_valid_hdr(0x8001, 4096);
    uint8_t wire[DOIP_HEADER_SIZE];

    ssize_t sret = doip_serialize_header(&src, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE, sret);

    doip_header_t dst;
    ssize_t dret = doip_deserialize_header(wire, sizeof(wire), &dst);
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE, dret);

    TEST_ASSERT_EQUAL_HEX8 (src.protocol_version, dst.protocol_version);
    TEST_ASSERT_EQUAL_HEX8 (src.inverse_version,  dst.inverse_version);
    TEST_ASSERT_EQUAL_HEX16(src.payload_type,     dst.payload_type);
    TEST_ASSERT_EQUAL_UINT32(src.payload_length,  dst.payload_length);
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(void)
{
    DoIP_Log_Init(0, 0, NULL);  /* ERROR level only — suppress test noise */

    UNITY_BEGIN();

    RUN_TEST(test_serialize_null_input_returns_neg1);
    RUN_TEST(test_serialize_buffer_too_small_returns_neg1);
    RUN_TEST(test_serialize_returns_header_size);
    RUN_TEST(test_serialize_byte_order_payload_type);
    RUN_TEST(test_serialize_byte_order_payload_length);
    RUN_TEST(test_serialize_protocol_version_written);

    RUN_TEST(test_deserialize_null_input_returns_neg1);
    RUN_TEST(test_deserialize_bad_version_returns_neg2);
    RUN_TEST(test_deserialize_bad_inverse_returns_neg2);
    RUN_TEST(test_deserialize_roundtrip);

    return UNITY_END();
}
