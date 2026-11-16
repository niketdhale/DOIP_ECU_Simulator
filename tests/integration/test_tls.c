/**
 * @file test_tls.c
 * @brief Integration tests for the TLS path of the TCP server.
 *
 * Built with -DDOIP_ENABLE_TLS=true and linked against OpenSSL
 * (make test-tls). The Makefile generates throwaway self-signed
 * certificates into tests/certs/. The tester side is a plain OpenSSL
 * client, because the bundled client library does not speak TLS.
 *
 * Run: ./test-tls   (from the repository root)
 */
#include "unity.h"
#include "fixture.h"
#include "config/doip_uds_config.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>

#if !DOIP_ENABLE_TLS
#error "build with -DDOIP_ENABLE_TLS=true"
#endif

#define SERVER_CERT "tests/certs/server.crt"
#define SERVER_KEY  "tests/certs/server.key"
#define CLIENT_CERT "tests/certs/client.crt"
#define CLIENT_KEY  "tests/certs/client.key"

/* ── Server ────────────────────────────────────────────────────────── */

static void start_server(bool require_client_cert)
{
    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s3_server_timeout_ms = 5000;
    cfg.tls.cert_file = SERVER_CERT;
    cfg.tls.key_file  = SERVER_KEY;
    if (require_client_cert) {
        cfg.tls.ca_file     = CLIENT_CERT;   /* the self-signed client cert is its own CA */
        cfg.tls.verify_peer = true;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fx_start(&cfg), "server start (run from the repo root; certs in tests/certs)");
}

void setUp(void)    { start_server(false); }
void tearDown(void) { fx_stop(); }

/** Open TCP sockets as reported by the (unencrypted) UDP Entity Status. */
static int open_sockets(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    fx_set_timeout(fd, 1000);
    struct sockaddr_in srv = { .sin_family = AF_INET, .sin_port = htons(DOIP_UDP_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    uint8_t frame[DOIP_HEADER_SIZE], resp[64];
    sendto(fd, frame, fx_build_frame(frame, DOIP_PT_ENTITY_STATUS_REQ, NULL, 0), 0,
           (struct sockaddr *)&srv, sizeof(srv));
    ssize_t n = recv(fd, resp, sizeof(resp), 0);
    close(fd);
    return (n >= (ssize_t)DOIP_HEADER_SIZE + 3) ? resp[DOIP_HEADER_SIZE + 2] : -1;
}

/* ── TLS tester ────────────────────────────────────────────────────── */

typedef struct { int fd; SSL_CTX *ctx; SSL *ssl; } tls_tester_t;

/** TCP connect + TLS handshake, trusting only the test server certificate. */
static bool tls_open(tls_tester_t *t, bool with_client_cert)
{
    memset(t, 0, sizeof(*t));
    t->fd = fx_connect();
    if (t->fd < 0) return false;
    t->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_load_verify_locations(t->ctx, SERVER_CERT, NULL);
    if (with_client_cert) {
        SSL_CTX_use_certificate_file(t->ctx, CLIENT_CERT, SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(t->ctx, CLIENT_KEY, SSL_FILETYPE_PEM);
    }
    t->ssl = SSL_new(t->ctx);
    SSL_set_fd(t->ssl, t->fd);
    return SSL_connect(t->ssl) == 1;
}

/** @p clean sends close_notify first; otherwise the TCP connection just drops. */
static void tls_close(tls_tester_t *t, bool clean)
{
    if (t->ssl) { if (clean) SSL_shutdown(t->ssl); SSL_free(t->ssl); }
    if (t->ctx) SSL_CTX_free(t->ctx);
    if (t->fd >= 0) close(t->fd);
    memset(t, 0, sizeof(*t));
    t->fd = -1;
}

static bool tls_send(tls_tester_t *t, const uint8_t *buf, size_t len)
{
    return SSL_write(t->ssl, buf, (int)len) == (int)len;
}

static bool tls_read_exact(tls_tester_t *t, uint8_t *buf, size_t len)
{
    for (size_t got = 0; got < len; ) {
        int n = SSL_read(t->ssl, buf + got, (int)(len - got));
        if (n <= 0) return false;                 /* timeout (SO_RCVTIMEO), close or alert */
        got += (size_t)n;
    }
    return true;
}

/** One DoIP frame. Returns the payload length, or -1. */
static int tls_recv_frame(tls_tester_t *t, uint16_t *pt, uint8_t *buf, size_t buf_size)
{
    uint8_t raw[DOIP_HEADER_SIZE];
    doip_header_t hdr;
    if (!tls_read_exact(t, raw, sizeof(raw))) return -1;
    if (doip_deserialize_header(raw, sizeof(raw), &hdr) < 0 || hdr.payload_length > buf_size) return -1;
    if (hdr.payload_length && !tls_read_exact(t, buf, hdr.payload_length)) return -1;
    *pt = hdr.payload_type;
    return (int)hdr.payload_length;
}

/** Routing activation as tester 0x0E00. Returns the response code, or -1. */
static int tls_activate(tls_tester_t *t)
{
    const uint8_t req[7] = { 0x0E, 0x00, 0x00, 0, 0, 0, 0 };
    uint8_t frame[DOIP_HEADER_SIZE + sizeof(req)], resp[16];
    uint16_t pt = 0;
    if (!tls_send(t, frame, fx_build_frame(frame, DOIP_PT_ROUTING_ACT_REQ, req, sizeof(req)))) return -1;
    if (tls_recv_frame(t, &pt, resp, sizeof(resp)) != 9 || pt != DOIP_PT_ROUTING_ACT_RES) return -1;
    return resp[4];
}

/* ── Tests ─────────────────────────────────────────────────────────── */

void test_uds_over_tls(void)
{
    tls_tester_t t;
    TEST_ASSERT_TRUE_MESSAGE(tls_open(&t, false), "TLS handshake with the configured certificate");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(TLS1_2_VERSION, SSL_version(t.ssl));
    TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));

    const uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t frame[64], pl[64];
    uint16_t pt = 0;
    TEST_ASSERT_TRUE(tls_send(&t, frame, fx_build_diag(frame, DOIP_ECU_LOGICAL_ADDRESS, req, sizeof(req))));
    TEST_ASSERT_EQUAL_INT(5, tls_recv_frame(&t, &pt, pl, sizeof(pl)));
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_DIAGNOSTIC_ACK, pt);
    TEST_ASSERT_EQUAL_INT(4 + 3 + DOIP_VIN_LENGTH, tls_recv_frame(&t, &pt, pl, sizeof(pl)));
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_DIAGNOSTIC_MSG, pt);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, pl[4]);
    TEST_ASSERT_EQUAL_MEMORY("WBAXXXXXXXXXXXXXX", pl + 7, DOIP_VIN_LENGTH);   /* built-in default VIN */
    tls_close(&t, true);
}

void test_plaintext_client_is_rejected(void)
{
    int fd = fx_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, fx_activate(fd), "cleartext routing activation must not be answered");
    close(fd);

    /* ...and the server keeps serving proper TLS clients */
    tls_tester_t t;
    TEST_ASSERT_TRUE(tls_open(&t, false));
    TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));
    tls_close(&t, true);
}

void test_peer_close_is_detected(void)
{
    for (int clean = 0; clean <= 1; clean++) {
        tls_tester_t t;
        TEST_ASSERT_TRUE(tls_open(&t, false));
        TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));
        TEST_ASSERT_EQUAL_INT(1, open_sockets());
        tls_close(&t, clean);
        usleep(200000);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, open_sockets(),
                                      clean ? "after close_notify" : "after an abrupt TCP close");
    }
}

void test_pipelined_frames_in_one_tls_record(void)
{
    /* 400 TesterPresent requests (suppress bit set: ACK only) = 5600 bytes in ONE
     * TLS record. That is more than the server takes in one read; the rest stays
     * inside OpenSSL, where select() on the socket cannot see it. */
    enum { N = 400 };
    tls_tester_t t;
    TEST_ASSERT_TRUE(tls_open(&t, false));
    TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));

    static uint8_t burst[N * (DOIP_HEADER_SIZE + 4 + 2)];
    const uint8_t tp[] = { UDS_SID_TESTER_PRESENT, 0x80 };
    size_t len = 0;
    for (int i = 0; i < N; i++)
        len += fx_build_diag(burst + len, DOIP_ECU_LOGICAL_ADDRESS, tp, sizeof(tp));
    TEST_ASSERT_TRUE(tls_send(&t, burst, len));

    int acks = 0;
    uint16_t pt = 0;
    uint8_t pl[16];
    while (acks < N && tls_recv_frame(&t, &pt, pl, sizeof(pl)) == 5 && pt == DOIP_PT_DIAGNOSTIC_ACK)
        acks++;
    TEST_ASSERT_EQUAL_INT(N, acks);
    tls_close(&t, true);
}

void test_server_survives_disconnect_during_responses(void)
{
    /* 300 unknown-payload-type frames -> 300 NACK writes through OpenSSL to a
     * tester that is already gone. Must not raise SIGPIPE. */
    tls_tester_t t;
    TEST_ASSERT_TRUE(tls_open(&t, false));
    static uint8_t burst[300 * DOIP_HEADER_SIZE];
    size_t len = 0;
    for (int i = 0; i < 300; i++)
        len += fx_build_frame(burst + len, 0x9999, NULL, 0);
    TEST_ASSERT_TRUE(tls_send(&t, burst, len));
    tls_close(&t, false);
    usleep(150000);

    TEST_ASSERT_TRUE(tls_open(&t, false));
    TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));
    tls_close(&t, true);
}

void test_mutual_tls_requires_client_certificate(void)
{
    fx_stop();
    start_server(true);

    /* Without a certificate the session never becomes usable. With TLS 1.3 the
     * client's side of the handshake can finish before the server's rejection
     * arrives, so the first exchange is checked as well. */
    tls_tester_t t;
    bool usable = tls_open(&t, false) && tls_activate(&t) == 0x10;
    TEST_ASSERT_FALSE(usable);
    tls_close(&t, false);

    TEST_ASSERT_TRUE_MESSAGE(tls_open(&t, true), "handshake with the trusted client certificate");
    TEST_ASSERT_EQUAL_INT(0x10, tls_activate(&t));
    tls_close(&t, true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_uds_over_tls);
    RUN_TEST(test_plaintext_client_is_rejected);
    RUN_TEST(test_peer_close_is_detected);
    RUN_TEST(test_pipelined_frames_in_one_tls_record);
    RUN_TEST(test_server_survives_disconnect_during_responses);
    RUN_TEST(test_mutual_tls_requires_client_certificate);
    return UNITY_END();
}
