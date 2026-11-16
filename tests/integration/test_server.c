/**
 * @file test_server.c
 * @brief Integration tests — full DoIP server + live TCP/UDP client.
 *
 * Each test spins up a real DoIP server in a background thread on port
 * DOIP_TCP_PORT (23400, overridden by doip_config_test.h), connects a
 * DoIP client over a real socket, and validates responses.
 *
 * Links: unity.c  all production .c files  client/doip_client.c
 * Compile: -include tests/integration/doip_config_test.h
 *
 * Run: ./test-server
 */

/* doip_config_test.h is injected via -include before any other header */
#include "unity.h"

/* Public APIs */
#include "doip_api.h"
#include "client/doip_client.h"
#include "core/doip_frame.h"
#include "core/doip_types.h"
#include "config/doip_uds_config.h"
#include "transport/doip_tcp.h"   /* DOIP_MAX_TCP_CLIENTS */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Server thread ─────────────────────────────────────────────────── */

static DoIP_Handle_t    *g_server        = NULL;
static pthread_t         g_server_thread;
static volatile int      g_server_running = 0;

/* Customisable identity and hook — set in each test before setUp calls
 * DoIP_Init so they take effect from the start of that test. */
static DoIP_EcuIdentity_t g_test_identity;
static int (*g_test_hook)(uint8_t, const uint8_t*, uint16_t,
                           uint8_t*, uint16_t, uint16_t*, void*) = NULL;
static int g_hook_called = 0;

static void *server_thread_fn(void *arg)
{
    (void)arg;
    while (g_server_running)
    {
        DoIP_Tick(g_server);
        usleep(1000);  /* 1 ms tick */
    }
    return NULL;
}

/* ── Unity lifecycle ───────────────────────────────────────────────── */

void setUp(void)
{
    g_hook_called = 0;
    g_test_hook   = NULL;

    /* Default test identity */
    memset(&g_test_identity, 0, sizeof(g_test_identity));
    memcpy(g_test_identity.vin,               "INTTEST1234567AB", DOIP_VIN_LENGTH);
    strncpy(g_test_identity.software_version, "VT.1.0",  sizeof(g_test_identity.software_version) - 1);
    strncpy(g_test_identity.system_name,      "TestNode", sizeof(g_test_identity.system_name) - 1);
    strncpy(g_test_identity.serial_number,    "SN-IT-01", sizeof(g_test_identity.serial_number) - 1);
    memset(g_test_identity.eid, 0x11, DOIP_EID_LENGTH);
    memset(g_test_identity.gid, 0x22, DOIP_GID_LENGTH);

    g_server = DoIP_Create();
    TEST_ASSERT_NOT_NULL(g_server);

    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level            = 0;  /* ERROR only — suppress noise */
    cfg.s3_server_timeout_ms = 5000;
    cfg.ecu_identity         = &g_test_identity;
    cfg.on_uds_request       = g_test_hook;

    int rc = DoIP_Init(g_server, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    g_server_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread_fn, NULL);
    usleep(60000);  /* 60 ms — let server bind + enter poll loop */
}

void tearDown(void)
{
    g_server_running = 0;
    pthread_join(g_server_thread, NULL);
    DoIP_Destroy(g_server);
    g_server = NULL;
    usleep(20000);  /* brief settle before next test re-binds the port */
}

/* ── Client helpers ────────────────────────────────────────────────── */

static DoIP_Client_t *client_connect_and_activate(void)
{
    DoIP_Client_t *c = DoIP_Client_Create();
    if (!c) return NULL;

    DoIP_ClientConfig_t cfg = {
        .tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS,
        .connect_timeout_ms  = 2000,
        .response_timeout_ms = 2000,
        .routing_timeout_ms  = 2000,
    };
    if (DoIP_Client_Init(c, &cfg) != DOIP_CLIENT_OK) { DoIP_Client_Destroy(c); return NULL; }
    if (DoIP_Client_Connect(c, "127.0.0.1", DOIP_TCP_PORT) != DOIP_CLIENT_OK) { DoIP_Client_Destroy(c); return NULL; }

    uint8_t act_code = 0;
    if (DoIP_Client_ActivateRouting(c, 0x00, &act_code) != DOIP_CLIENT_OK) { DoIP_Client_Destroy(c); return NULL; }
    return c;
}

static void client_destroy(DoIP_Client_t *c)
{
    if (c) { DoIP_Client_Disconnect(c); DoIP_Client_Destroy(c); }
}

/* Send a raw DoIP frame directly on a plain socket (for NACK testing). */
static int raw_tcp_connect(void)
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

/* ── Test 1: Lifecycle smoke test ──────────────────────────────────── */

void test_server_starts_and_stops(void)
{
    /* setUp spun the server up, tearDown will shut it down.
     * Just assert DoIP_Init returned 0 (checked in setUp). */
    TEST_PASS_MESSAGE("Server started and will be cleanly stopped in tearDown");
}

/* ── Test 2: Routing activation ────────────────────────────────────── */

void test_tcp_routing_activation(void)
{
    DoIP_Client_t *c = DoIP_Client_Create();
    TEST_ASSERT_NOT_NULL(c);

    DoIP_ClientConfig_t cfg = {
        .tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS,
        .connect_timeout_ms  = 2000,
        .response_timeout_ms = 2000,
        .routing_timeout_ms  = 2000,
    };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Init(c, &cfg));
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Connect(c, "127.0.0.1", DOIP_TCP_PORT));

    uint8_t act_code = 0xFF;
    DoIP_ClientStatus_t rc = DoIP_Client_ActivateRouting(c, 0x00, &act_code);

    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_TRUE(DoIP_Client_IsRoutingActive(c));
    /* ISO 13400-2: 0x10 = routingActivationSuccessful */
    TEST_ASSERT_EQUAL_HEX8(0x10, act_code);

    client_destroy(c);
}

/* ── Test 3: Already active ─────────────────────────────────────────── */

void test_tcp_routing_already_active(void)
{
    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);

    /* Activate a second time on the same connection with the same SA */
    uint8_t act_code = 0xFF;
    DoIP_ClientStatus_t rc = DoIP_Client_ActivateRouting(c, 0x00, &act_code);

    /* ISO 13400-2: re-activation of an already-registered SA succeeds (0x10) */
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_EQUAL_HEX8(0x10, act_code);

    client_destroy(c);
}

/* ── Test 4: VIN from custom identity ──────────────────────────────── */

void test_uds_read_vin_custom_identity(void)
{
    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);

    uint8_t req[]  = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t resp[64];
    uint16_t rlen = 0;

    DoIP_ClientStatus_t rc = DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS,
        req, (uint16_t)sizeof(req),
        resp, (uint16_t)sizeof(resp), &rlen);

    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_TRUE(rlen >= 3 + DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, resp[0]);
    TEST_ASSERT_EQUAL_MEMORY("INTTEST1234567AB", &resp[3], DOIP_VIN_LENGTH);

    client_destroy(c);
}

/* ── Test 5: SW version from custom identity ────────────────────────── */

void test_uds_read_sw_version_custom_identity(void)
{
    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);

    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                      (uint8_t)(UDS_DID_SOFTWARE_VERSION >> 8),
                      (uint8_t)(UDS_DID_SOFTWARE_VERSION & 0xFF) };
    uint8_t  resp[64];
    uint16_t rlen = 0;

    DoIP_ClientStatus_t rc = DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS,
        req, (uint16_t)sizeof(req),
        resp, (uint16_t)sizeof(resp), &rlen);

    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, resp[0]);
    const char *expected_sw = "VT.1.0";
    size_t sw_len = strlen(expected_sw);
    TEST_ASSERT_TRUE(rlen >= (uint16_t)(3 + sw_len));
    TEST_ASSERT_EQUAL_MEMORY(expected_sw, &resp[3], sw_len);

    client_destroy(c);
}

/* ── Test 6 & 7: on_uds_request hook ───────────────────────────────── */

/*
 * The hook is set as a function pointer before setUp builds the config.
 * Because setUp reads g_test_hook, we must set it BEFORE setUp() runs.
 * Unity calls setUp() before each test, so we use a module-level hook
 * and a separate test that re-inits the server with the hook installed.
 *
 * Approach: tearDown / manual server restart within the test.
 */

static int hook_custom_response(uint8_t sid,
                                const uint8_t *req, uint16_t req_len,
                                uint8_t *resp, uint16_t resp_size,
                                uint16_t *resp_len_out,
                                void *ctx)
{
    (void)req_len; (void)resp_size; (void)ctx;
    g_hook_called = 1;
    if (sid == UDS_SID_READ_DATA_BY_ID && req[1] == 0xF1 && req[2] == 0x90) {
        resp[0] = UDS_SID_READ_DATA_BY_ID_RES;
        resp[1] = 0xF1; resp[2] = 0x90;
        /* Write "HOOKVIN00000000XX" (17 bytes) */
        memcpy(&resp[3], "HOOKVIN00000000XX", 17);
        *resp_len_out = 20;
        return 0;  /* handled — skip built-in */
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
    g_hook_called = 1;
    return -1;  /* fall through to built-in */
}

/** Helper: restart the server with a new config (installs hook). */
static void restart_server_with_hook(
    int (*hook)(uint8_t, const uint8_t*, uint16_t,
                uint8_t*, uint16_t, uint16_t*, void*))
{
    /* Stop current server */
    g_server_running = 0;
    pthread_join(g_server_thread, NULL);
    DoIP_Destroy(g_server);
    usleep(30000);

    /* Restart with hook */
    g_server = DoIP_Create();
    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level            = 0;
    cfg.s3_server_timeout_ms = 5000;
    cfg.ecu_identity         = &g_test_identity;
    cfg.on_uds_request       = hook;
    DoIP_Init(g_server, &cfg);

    g_server_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread_fn, NULL);
    usleep(60000);
}

void test_uds_hook_intercepts_request(void)
{
    restart_server_with_hook(hook_custom_response);
    g_hook_called = 0;

    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);

    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t  resp[64];
    uint16_t rlen = 0;

    DoIP_ClientStatus_t rc = DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS,
        req, (uint16_t)sizeof(req),
        resp, (uint16_t)sizeof(resp), &rlen);

    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_hook_called);
    TEST_ASSERT_EQUAL_MEMORY("HOOKVIN00000000XX", &resp[3], 17);

    client_destroy(c);
}

void test_uds_hook_fallthrough(void)
{
    restart_server_with_hook(hook_passthrough);
    g_hook_called = 0;

    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);

    uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
    uint8_t  resp[64];
    uint16_t rlen = 0;

    DoIP_ClientStatus_t rc = DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS,
        req, (uint16_t)sizeof(req),
        resp, (uint16_t)sizeof(resp), &rlen);

    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_hook_called);
    /* Built-in responded with the configured VIN */
    TEST_ASSERT_EQUAL_MEMORY("INTTEST1234567AB", &resp[3], DOIP_VIN_LENGTH);

    client_destroy(c);
}

/* ── Test 8: Generic NACK on unknown PT ─────────────────────────────── */

void test_nack_on_unknown_payload_type(void)
{
    int fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);

    /* Send a well-formed DoIP header with unknown PT = 0x9999 */
    uint8_t frame[DOIP_HEADER_SIZE];
    doip_header_t hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = 0x9999,
        .payload_length   = 0,
    };
    doip_serialize_header(&hdr, frame, sizeof(frame));
    send(fd, frame, sizeof(frame), 0);

    /* Read the NACK response */
    uint8_t resp[DOIP_HEADER_SIZE + 4];
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, resp, sizeof(resp), 0);

    TEST_ASSERT_GREATER_THAN_INT(0, n);

    /* Deserialize and check */
    doip_header_t out;
    ssize_t dret = doip_deserialize_header(resp, (size_t)n, &out);
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE, dret);
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_GENERIC_NACK, out.payload_type);
    /* NACK payload: code 0x01 = UNKNOWN_PAYLOAD_TYPE */
    if (n > DOIP_HEADER_SIZE) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)DOIP_NACK_UNKNOWN_PT, resp[DOIP_HEADER_SIZE]);
    }

    close(fd);
}

/* ── Test 9: Entity Status (UDP) ────────────────────────────────────── */

void test_udp_entity_status_response(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build Entity Status Request frame (PT=0x4001, no payload) */
    uint8_t req_frame[DOIP_HEADER_SIZE];
    doip_header_t req_hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_ENTITY_STATUS_REQ,
        .payload_length   = 0,
    };
    doip_serialize_header(&req_hdr, req_frame, sizeof(req_frame));

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(DOIP_UDP_PORT),
    };
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    sendto(fd, req_frame, sizeof(req_frame), 0, (struct sockaddr*)&srv, sizeof(srv));

    /* Receive response */
    uint8_t resp[DOIP_HEADER_SIZE + 16];
    ssize_t n = recv(fd, resp, sizeof(resp), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    doip_header_t out;
    ssize_t dret = doip_deserialize_header(resp, (size_t)n, &out);
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE, dret);
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_ENTITY_STATUS_RES, out.payload_type);

    /* Payload: node_type(1) max_sockets(1) curr_sockets(1) max_data_size(4) */
    TEST_ASSERT_TRUE(n >= DOIP_HEADER_SIZE + 7);
    uint8_t *pl = resp + DOIP_HEADER_SIZE;
    TEST_ASSERT_EQUAL_HEX8(0x01, pl[0]);   /* node_type = DoIP node */
    TEST_ASSERT_EQUAL_HEX8(DOIP_MAX_TCP_CLIENTS, pl[1]);  /* max_open_sockets */
    /* pl[2] = curr_open_sockets (0, no TCP clients yet) */
    TEST_ASSERT_EQUAL_HEX8(0x00, pl[2]);

    close(fd);
}

/* ── Test 10: Diagnostic Power Mode (UDP) ───────────────────────────── */

void test_udp_power_mode_response(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t req_frame[DOIP_HEADER_SIZE];
    doip_header_t req_hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_POWER_MODE_REQ,
        .payload_length   = 0,
    };
    doip_serialize_header(&req_hdr, req_frame, sizeof(req_frame));

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(DOIP_UDP_PORT),
    };
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    sendto(fd, req_frame, sizeof(req_frame), 0, (struct sockaddr*)&srv, sizeof(srv));

    uint8_t resp[DOIP_HEADER_SIZE + 4];
    ssize_t n = recv(fd, resp, sizeof(resp), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    doip_header_t out;
    doip_deserialize_header(resp, (size_t)n, &out);
    TEST_ASSERT_EQUAL_HEX16(DOIP_PT_POWER_MODE_RES, out.payload_type);
    TEST_ASSERT_TRUE(n > DOIP_HEADER_SIZE);
    TEST_ASSERT_EQUAL_HEX8(0x01, resp[DOIP_HEADER_SIZE]);  /* power_mode = ready */

    close(fd);
}

/* ── Test 11: Multiple clients — independent UDS sessions ───────────── */

void test_multiple_clients_independent_sessions(void)
{
    DoIP_Client_t *c1 = client_connect_and_activate();
    DoIP_Client_t *c2 = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c1);
    TEST_ASSERT_NOT_NULL(c2);

    /* Switch c1 to Extended session */
    {
        uint8_t req[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED };
        uint8_t resp[16]; uint16_t rlen = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            c1, DOIP_ECU_LOGICAL_ADDRESS, req, sizeof(req), resp, sizeof(resp), &rlen);
        TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
        TEST_ASSERT_EQUAL_HEX8(UDS_SID_SESSION_CONTROL_RES, resp[0]);
        TEST_ASSERT_EQUAL_HEX8(UDS_SESSION_EXTENDED, resp[1]);
    }

    /* c2 should still be in Default — read VIN works regardless of session */
    {
        uint8_t req[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
        uint8_t resp[64]; uint16_t rlen = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            c2, DOIP_ECU_LOGICAL_ADDRESS, req, sizeof(req), resp, sizeof(resp), &rlen);
        TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, rc);
        TEST_ASSERT_EQUAL_HEX8(UDS_SID_READ_DATA_BY_ID_RES, resp[0]);
        /* Should return the configured VIN, not garbage */
        TEST_ASSERT_EQUAL_MEMORY("INTTEST1234567AB", &resp[3], DOIP_VIN_LENGTH);
    }

    client_destroy(c1);
    client_destroy(c2);
}



/* ── Raw-frame helpers for protocol conformance tests ──────────────── */

/** Build header + payload into @p out with explicit version bytes; returns frame length. */
static size_t raw_frame(uint8_t *out, uint8_t ver, uint16_t pt,
                        const uint8_t *pl, uint32_t plen)
{
    doip_header_t hdr = { ver, (uint8_t)~ver, pt, plen };
    doip_serialize_header(&hdr, out, DOIP_HEADER_SIZE);
    if (plen) memcpy(out + DOIP_HEADER_SIZE, pl, plen);
    return DOIP_HEADER_SIZE + plen;
}

/** Send one UDP request to the server; returns bytes received (<=0 on timeout). */
static ssize_t udp_exchange(uint8_t ver, uint16_t pt, const uint8_t *pl, uint32_t plen,
                            uint8_t *resp, size_t resp_size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in srv = { .sin_family = AF_INET, .sin_port = htons(DOIP_UDP_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    uint8_t frame[DOIP_HEADER_SIZE + 32];
    size_t flen = raw_frame(frame, ver, pt, pl, plen);
    sendto(fd, frame, flen, 0, (struct sockaddr*)&srv, sizeof(srv));
    ssize_t n = recv(fd, resp, resp_size, 0);
    close(fd);
    return n;
}

/** Send one TCP frame and read whatever comes back (<=0 on timeout/close). */
static ssize_t tcp_exchange(int fd, uint16_t pt, const uint8_t *pl, uint32_t plen,
                            uint8_t *resp, size_t resp_size)
{
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t frame[DOIP_HEADER_SIZE + 32];
    size_t flen = raw_frame(frame, DOIP_PROTOCOL_VERSION, pt, pl, plen);
    send(fd, frame, flen, 0);
    return recv(fd, resp, resp_size, 0);
}

/* ── Test 12: Vehicle ID request → 0x0004 response, 33-byte payload ── */

void test_udp_vehicle_id_response_layout(void)
{
    uint8_t resp[DOIP_HEADER_SIZE + 64];
    /* Default version 0xFF/0x00 must be accepted for vehicle ID requests */
    ssize_t n = udp_exchange(0xFF, 0x0001, NULL, 0, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 33, n);

    TEST_ASSERT_EQUAL_HEX8(DOIP_PROTOCOL_VERSION, resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, resp[2]);
    TEST_ASSERT_EQUAL_HEX8(0x04, resp[3]);           /* PT 0x0004 */
    const uint8_t *pl = resp + DOIP_HEADER_SIZE;
    TEST_ASSERT_EQUAL_MEMORY(g_test_identity.vin, pl, 17);
    TEST_ASSERT_EQUAL_HEX8(DOIP_ECU_LOGICAL_ADDRESS >> 8,   pl[17]);
    TEST_ASSERT_EQUAL_HEX8(DOIP_ECU_LOGICAL_ADDRESS & 0xFF, pl[18]);
    TEST_ASSERT_EQUAL_MEMORY(g_test_identity.eid, pl + 19, 6);
    TEST_ASSERT_EQUAL_MEMORY(g_test_identity.gid, pl + 25, 6);
}

/* ── Test 13: EID (0x0002) / VIN (0x0003) filtered requests ─────────── */

void test_udp_vehicle_id_filtered_requests(void)
{
    uint8_t resp[DOIP_HEADER_SIZE + 64];
    uint8_t other[17];
    memset(other, 0x99, sizeof(other));

    /* Matching EID / VIN → answered */
    TEST_ASSERT_GREATER_THAN_INT(0, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0002,
                                 g_test_identity.eid, 6, resp, sizeof(resp)));
    TEST_ASSERT_GREATER_THAN_INT(0, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0003,
                                 (const uint8_t*)g_test_identity.vin, 17, resp, sizeof(resp)));
    /* Non-matching → silently ignored */
    TEST_ASSERT_LESS_THAN_INT(1, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0002,
                              other, 6, resp, sizeof(resp)));
    TEST_ASSERT_LESS_THAN_INT(1, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0003,
                              other, 17, resp, sizeof(resp)));
}

/* ── Test 14: Routing activation response layout + SA validation ────── */

void test_routing_activation_wire_format_and_sa_range(void)
{
    uint8_t resp[64];

    /* Valid tester 0x0E80 → 9-byte response: tester(2) entity(2) code(1) reserved(4) */
    int fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    const uint8_t ok_req[7] = { 0x0E, 0x80, 0x00, 0, 0, 0, 0 };
    ssize_t n = tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, ok_req, sizeof(ok_req), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 9, n);
    const uint8_t *pl = resp + DOIP_HEADER_SIZE;
    TEST_ASSERT_EQUAL_HEX8(0x0E, pl[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, pl[1]);
    TEST_ASSERT_EQUAL_HEX8(DOIP_ECU_LOGICAL_ADDRESS >> 8,   pl[2]);
    TEST_ASSERT_EQUAL_HEX8(DOIP_ECU_LOGICAL_ADDRESS & 0xFF, pl[3]);
    TEST_ASSERT_EQUAL_HEX8(0x10, pl[4]);

    /* Different SA on the same, already-activated socket → 0x02 */
    const uint8_t other_req[7] = { 0x0E, 0x81, 0x00, 0, 0, 0, 0 };
    n = tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, other_req, sizeof(other_req), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 9, n);
    TEST_ASSERT_EQUAL_HEX8(0x02, resp[DOIP_HEADER_SIZE + 4]);
    close(fd);

    /* SA outside 0x0E00–0x0FFF → 0x00 (unknown source address) */
    fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    const uint8_t bad_req[7] = { 0x12, 0x34, 0x00, 0, 0, 0, 0 };
    n = tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, bad_req, sizeof(bad_req), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 9, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, resp[DOIP_HEADER_SIZE + 4]);
    close(fd);

    /* Unsupported activation type → 0x06 */
    fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    const uint8_t type_req[7] = { 0x0E, 0x00, 0x7F, 0, 0, 0, 0 };
    n = tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, type_req, sizeof(type_req), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 9, n);
    TEST_ASSERT_EQUAL_HEX8(0x06, resp[DOIP_HEADER_SIZE + 4]);
    close(fd);
}

/* ── Test 15: Diagnostic message to unknown target → NACK 0x03 ──────── */

void test_diag_unknown_target_address_nack(void)
{
    uint8_t resp[64];
    int fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    const uint8_t act[7] = { 0x0E, 0x00, 0x00, 0, 0, 0, 0 };
    TEST_ASSERT_GREATER_THAN_INT(0, tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, act, sizeof(act),
                                                 resp, sizeof(resp)));

    /* SA=0x0E00, TA=0x2222 (not this ECU), UDS = TesterPresent */
    const uint8_t diag[6] = { 0x0E, 0x00, 0x22, 0x22, 0x3E, 0x00 };
    ssize_t n = tcp_exchange(fd, DOIP_PT_DIAGNOSTIC_MSG, diag, sizeof(diag), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 5, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, resp[2]);
    TEST_ASSERT_EQUAL_HEX8(0x03, resp[3]);                       /* PT 0x8003 */
    TEST_ASSERT_EQUAL_HEX8(0x03, resp[DOIP_HEADER_SIZE + 4]);    /* unknown target address */
    close(fd);
}

/* ── Test 16: Two frames in one TCP segment are both processed ──────── */

void test_coalesced_frames_all_processed(void)
{
    int fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Routing activation + TesterPresent sent with a single send() */
    const uint8_t act[7]  = { 0x0E, 0x00, 0x00, 0, 0, 0, 0 };
    const uint8_t diag[6] = { 0x0E, 0x00, DOIP_ECU_LOGICAL_ADDRESS >> 8,
                              DOIP_ECU_LOGICAL_ADDRESS & 0xFF, 0x3E, 0x00 };
    uint8_t buf[64];
    size_t len = raw_frame(buf, DOIP_PROTOCOL_VERSION, DOIP_PT_ROUTING_ACT_REQ, act, sizeof(act));
    len += raw_frame(buf + len, DOIP_PROTOCOL_VERSION, DOIP_PT_DIAGNOSTIC_MSG, diag, sizeof(diag));
    send(fd, buf, len, 0);

    /* Expect: activation response (8+9) + DiagACK (8+5) + TesterPresent response (8+4+2) */
    const size_t expected = 17 + 13 + 14;
    uint8_t resp[128];
    size_t got = 0;
    while (got < expected) {
        ssize_t n = recv(fd, resp + got, sizeof(resp) - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    TEST_ASSERT_EQUAL_UINT(expected, got);
    TEST_ASSERT_EQUAL_HEX8(0x7E, resp[expected - 2]);   /* TesterPresent positive response */
    close(fd);
}

/* ── Test 17: TesterPresent with suppress bit → DoIP ACK, no UDS reply ─ */

void test_suppressed_response_sends_ack_only(void)
{
    uint8_t resp[64];
    int fd = raw_tcp_connect();
    TEST_ASSERT_GREATER_THAN_INT(-1, fd);
    const uint8_t act[7] = { 0x0E, 0x00, 0x00, 0, 0, 0, 0 };
    TEST_ASSERT_GREATER_THAN_INT(0, tcp_exchange(fd, DOIP_PT_ROUTING_ACT_REQ, act, sizeof(act),
                                                 resp, sizeof(resp)));

    const uint8_t diag[6] = { 0x0E, 0x00, DOIP_ECU_LOGICAL_ADDRESS >> 8,
                              DOIP_ECU_LOGICAL_ADDRESS & 0xFF, 0x3E, 0x80 };
    ssize_t n = tcp_exchange(fd, DOIP_PT_DIAGNOSTIC_MSG, diag, sizeof(diag), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 5, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, resp[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, resp[3]);            /* PT 0x8002 Diagnostic ACK */

    /* Nothing else follows */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    TEST_ASSERT_LESS_THAN_INT(1, recv(fd, resp, sizeof(resp), 0));
    close(fd);
}

/* ── Test 18: VIN written over UDS is used by UDP vehicle ID responses ─ */

void test_written_vin_appears_in_udp_response(void)
{
    DoIP_Client_t *c = client_connect_and_activate();
    TEST_ASSERT_NOT_NULL(c);
    uint8_t resp[DOIP_HEADER_SIZE + 64];
    uint16_t rlen = 0;

    uint8_t session[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS, session, sizeof(session), resp, sizeof(resp), &rlen));

    uint8_t wr[3 + DOIP_VIN_LENGTH] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x90 };
    memcpy(&wr[3], "WRITTENVIN0000001", DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, DoIP_Client_Transact(
        c, DOIP_ECU_LOGICAL_ADDRESS, wr, sizeof(wr), resp, sizeof(resp), &rlen));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_WRITE_DATA_BY_ID_RES, resp[0]);
    client_destroy(c);

    ssize_t n = udp_exchange(DOIP_PROTOCOL_VERSION, 0x0001, NULL, 0, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 33, n);
    TEST_ASSERT_EQUAL_MEMORY("WRITTENVIN0000001", resp + DOIP_HEADER_SIZE, DOIP_VIN_LENGTH);

    /* VIN-filtered requests follow the new VIN, not the old one */
    TEST_ASSERT_GREATER_THAN_INT(0, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0003,
                                 (const uint8_t*)"WRITTENVIN0000001", 17, resp, sizeof(resp)));
    TEST_ASSERT_LESS_THAN_INT(1, udp_exchange(DOIP_PROTOCOL_VERSION, 0x0003,
                              (const uint8_t*)g_test_identity.vin, 17, resp, sizeof(resp)));
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_server_starts_and_stops);
    RUN_TEST(test_tcp_routing_activation);
    RUN_TEST(test_tcp_routing_already_active);
    RUN_TEST(test_uds_read_vin_custom_identity);
    RUN_TEST(test_uds_read_sw_version_custom_identity);
    RUN_TEST(test_uds_hook_intercepts_request);
    RUN_TEST(test_uds_hook_fallthrough);
    RUN_TEST(test_nack_on_unknown_payload_type);
    RUN_TEST(test_udp_entity_status_response);
    RUN_TEST(test_udp_power_mode_response);
    RUN_TEST(test_multiple_clients_independent_sessions);
    RUN_TEST(test_udp_vehicle_id_response_layout);
    RUN_TEST(test_udp_vehicle_id_filtered_requests);
    RUN_TEST(test_routing_activation_wire_format_and_sa_range);
    RUN_TEST(test_diag_unknown_target_address_nack);
    RUN_TEST(test_coalesced_frames_all_processed);
    RUN_TEST(test_suppressed_response_sends_ack_only);
    RUN_TEST(test_written_vin_appears_in_udp_response);

    return UNITY_END();
}
