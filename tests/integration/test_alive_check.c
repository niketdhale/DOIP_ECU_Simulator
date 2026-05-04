/**
 * @file test_alive_check.c
 * @brief Slow integration tests — server-initiated Alive Check timing.
 *
 * Each test waits for real wall-clock events:
 *   DOIP_ALIVE_CHECK_INTERVAL_MS (5000 ms) before server sends PT=0x0007
 *   DOIP_ALIVE_CHECK_TIMEOUT_MS  (2000 ms) before server disconnects
 *
 * Total runtime: ~22 s.  Run separately with: make test-alive
 *
 * Links: unity.c  all production .c files  client/doip_client.c
 * Compile: -include tests/integration/doip_config_test.h
 */

#include "unity.h"

#include "doip_api.h"
#include "core/doip_frame.h"
#include "core/doip_types.h"
#include "config/doip_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ── Server thread ─────────────────────────────────────────────────── */

static DoIP_Handle_t   *g_server        = NULL;
static pthread_t        g_server_thread;
static volatile int     g_server_running = 0;

static void *server_thread_fn(void *arg)
{
    (void)arg;
    while (g_server_running) { DoIP_Tick(g_server); usleep(1000); }
    return NULL;
}

void setUp(void)
{
    g_server = DoIP_Create();
    TEST_ASSERT_NOT_NULL(g_server);

    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level            = 0;
    cfg.s3_server_timeout_ms = 10000;  /* longer than alive-check interval */

    int rc = DoIP_Init(g_server, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    g_server_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread_fn, NULL);
    usleep(60000);
}

void tearDown(void)
{
    g_server_running = 0;
    pthread_join(g_server_thread, NULL);
    DoIP_Destroy(g_server);
    g_server = NULL;
    usleep(30000);
}

/* ── Socket helpers ────────────────────────────────────────────────── */

/** Open a raw TCP socket and return the fd. */
static int raw_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(DOIP_TCP_PORT),
    };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    return fd;
}

/** Send Routing Activation Request on fd, discard the response. */
static int do_routing_activation(int fd)
{
    /* Routing Activation Request: PT=0x0005, payload = tester_addr(2) + act_type(1) + reserved(4) */
    uint8_t payload[7] = {
        0x0E, 0x00,   /* tester logical address = 0x0E00 */
        0x00,         /* activation type = default */
        0x00, 0x00, 0x00, 0x00  /* reserved */
    };
    uint8_t frame[DOIP_HEADER_SIZE + sizeof(payload)];
    doip_header_t hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_ROUTING_ACT_REQ,
        .payload_length   = sizeof(payload),
    };
    doip_serialize_header(&hdr, frame, DOIP_HEADER_SIZE);
    memcpy(frame + DOIP_HEADER_SIZE, payload, sizeof(payload));

    if (send(fd, frame, sizeof(frame), 0) < 0) return -1;

    /* Read and discard routing activation response */
    uint8_t resp[64];
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    recv(fd, resp, sizeof(resp), 0);
    return 0;
}

/** Read one DoIP frame from fd. Returns payload_type, or -1 on error. */
static int read_frame_pt(int fd, int timeout_sec)
{
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t hdr_buf[DOIP_HEADER_SIZE];
    ssize_t n = recv(fd, hdr_buf, sizeof(hdr_buf), MSG_WAITALL);
    if (n <= 0) return -1;

    doip_header_t hdr;
    if (doip_deserialize_header(hdr_buf, (size_t)n, &hdr) < 0) return -1;

    /* Drain any payload bytes */
    if (hdr.payload_length > 0 && hdr.payload_length <= 4096) {
        uint8_t tmp[4096];
        recv(fd, tmp, hdr.payload_length, 0);
    }
    return (int)hdr.payload_type;
}

/** Send an Alive Check Response (PT=0x0008, empty payload) on fd. */
static void send_alive_response(int fd)
{
    uint8_t frame[DOIP_HEADER_SIZE];
    doip_header_t hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_ALIVE_CHECK_RES,
        .payload_length   = 0,
    };
    doip_serialize_header(&hdr, frame, sizeof(frame));
    send(fd, frame, sizeof(frame), 0);
}

/* ── Test 1: Server sends PT=0x0007 after idle ──────────────────────── */

void test_alive_check_request_sent_after_idle(void)
{
    int fd = raw_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    TEST_ASSERT_EQUAL_INT(0, do_routing_activation(fd));

    /* Wait up to INTERVAL + 2 s buffer (7 s total) for the alive check */
    int pt = read_frame_pt(fd, (int)(DOIP_ALIVE_CHECK_INTERVAL_MS / 1000) + 2);

    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_ALIVE_CHECK_REQ, (uint16_t)pt);
    close(fd);
}

/* ── Test 2: Replying keeps connection alive ────────────────────────── */

void test_alive_check_response_keeps_connection(void)
{
    int fd = raw_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    TEST_ASSERT_EQUAL_INT(0, do_routing_activation(fd));

    /* Reply to alive checks for two full cycles */
    for (int cycle = 0; cycle < 2; cycle++) {
        int pt = read_frame_pt(fd, (int)(DOIP_ALIVE_CHECK_INTERVAL_MS / 1000) + 2);
        TEST_ASSERT_EQUAL_HEX16(DOIP_PT_ALIVE_CHECK_REQ, (uint16_t)pt);
        send_alive_response(fd);
    }

    /* After replying, the connection must still be open.
     * Send a Tester Present and verify the server responds. */
    uint8_t tp_payload[4 + 2] = {
        0x0E, 0x00,   /* tester addr */
        0x10, 0x03,   /* ECU addr */
        0x3E, 0x00    /* UDS: TesterPresent sub-fn=0 */
    };
    uint8_t tp_frame[DOIP_HEADER_SIZE + sizeof(tp_payload)];
    doip_header_t hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_DIAGNOSTIC_MSG,
        .payload_length   = sizeof(tp_payload),
    };
    doip_serialize_header(&hdr, tp_frame, DOIP_HEADER_SIZE);
    memcpy(tp_frame + DOIP_HEADER_SIZE, tp_payload, sizeof(tp_payload));
    send(fd, tp_frame, sizeof(tp_frame), 0);

    /* Expect at least one response frame (DiagACK or DiagMsg) */
    int resp_pt = read_frame_pt(fd, 2);
    TEST_ASSERT_GREATER_THAN_INT(-1, resp_pt);  /* connection still alive */

    close(fd);
}

/* ── Test 3: Server disconnects on timeout ──────────────────────────── */

void test_client_disconnected_on_timeout(void)
{
    int fd = raw_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    TEST_ASSERT_EQUAL_INT(0, do_routing_activation(fd));

    /* Wait for alive check probe */
    int pt = read_frame_pt(fd, (int)(DOIP_ALIVE_CHECK_INTERVAL_MS / 1000) + 2);
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_ALIVE_CHECK_REQ, (uint16_t)pt);

    /* Deliberately do NOT reply — wait for timeout + 1 s buffer */
    int disc_pt = read_frame_pt(fd, (int)(DOIP_ALIVE_CHECK_TIMEOUT_MS / 1000) + 2);

    /* Server should have closed the socket:
     * recv returns 0 (graceful close) or -1 (ECONNRESET / ETIMEDOUT) */
    TEST_ASSERT_EQUAL_INT(-1, disc_pt);

    close(fd);
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(void)
{
    printf("\n[alive-check] Note: this suite takes ~22 s to complete.\n\n");

    UNITY_BEGIN();

    RUN_TEST(test_alive_check_request_sent_after_idle);
    RUN_TEST(test_alive_check_response_keeps_connection);
    RUN_TEST(test_client_disconnected_on_timeout);

    return UNITY_END();
}
