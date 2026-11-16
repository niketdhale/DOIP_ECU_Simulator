/**
 * @file test_multi_instance.c
 * @brief Two simulator instances in one process, each ticked by its own thread.
 *
 * Checks that instances share nothing: identity, addressing, sessions,
 * security state, DTC memory, fault injection and sockets are per handle.
 * Also built with -fsanitize=thread (make test-tsan) to catch data races
 * between instances.
 *
 * Run: ./test-multi
 */
#include "unity.h"
#include "doip_api.h"
#include "client/doip_client.h"
#include "core/doip_frame.h"
#include "config/doip_uds_config.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

enum { A, B, COUNT };
static const uint16_t    k_port[COUNT]    = { DOIP_TCP_PORT + 10, DOIP_TCP_PORT + 11 };
static const uint16_t    k_address[COUNT] = { 0x1001, 0x1002 };
static const char *const k_vin[COUNT]     = { "MULTIECU-AAAAAAAA", "MULTIECU-BBBBBBBB" };

static DoIP_Handle_t *s_ecu[COUNT];
static pthread_t      s_thread[COUNT];
static atomic_int     s_running[COUNT];   /* written by main, read by the tick threads */

static void *tick_loop(void *arg)
{
    int i = (int)(intptr_t)arg;
    while (s_running[i]) { DoIP_Tick(s_ecu[i]); usleep(1000); }
    return NULL;
}

static int start(int i, uint16_t port)
{
    DoIP_EcuIdentity_t id;
    memset(&id, 0, sizeof(id));
    memcpy(id.vin, k_vin[i], DOIP_VIN_LENGTH);
    strcpy(id.serial_number, i == A ? "SN-A" : "SN-B");

    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level            = 1;          /* warnings only */
    cfg.s3_server_timeout_ms = 5000;
    cfg.ecu_identity         = &id;
    cfg.logical_address      = k_address[i];
    cfg.port                 = port;

    s_ecu[i] = DoIP_Create();
    int rc = DoIP_Init(s_ecu[i], &cfg);
    if (rc != 0) { DoIP_Destroy(s_ecu[i]); s_ecu[i] = NULL; return rc; }
    s_running[i] = 1;
    pthread_create(&s_thread[i], NULL, tick_loop, (void *)(intptr_t)i);
    return 0;
}

static void stop(int i)
{
    if (!s_ecu[i]) return;
    s_running[i] = 0;
    pthread_join(s_thread[i], NULL);
    DoIP_Destroy(s_ecu[i]);
    s_ecu[i] = NULL;
}

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(0, start(A, k_port[A]));
    TEST_ASSERT_EQUAL_INT(0, start(B, k_port[B]));
    usleep(60000);
}

void tearDown(void)
{
    stop(A);
    stop(B);
    usleep(20000);
}

/* ── Tester helpers ────────────────────────────────────────────────── */

static DoIP_Client_t *tester(int i)
{
    DoIP_Client_t *c = DoIP_Client_Create();
    DoIP_ClientConfig_t cfg = {
        .tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS,
        .connect_timeout_ms = 2000, .response_timeout_ms = 2000, .routing_timeout_ms = 2000,
    };
    if (DoIP_Client_Init(c, &cfg) != DOIP_CLIENT_OK ||
        DoIP_Client_Connect(c, "127.0.0.1", k_port[i]) != DOIP_CLIENT_OK ||
        DoIP_Client_ActivateRouting(c, 0x00, NULL) != DOIP_CLIENT_OK) {
        DoIP_Client_Destroy(c);
        return NULL;
    }
    return c;
}

static uint8_t  s_resp[128];
static uint16_t s_rlen;

/** One UDS exchange with instance @p i's ECU address; returns the client status. */
static DoIP_ClientStatus_t uds(DoIP_Client_t *c, int i, const uint8_t *req, uint16_t len)
{
    s_rlen = 0;
    return DoIP_Client_Transact(c, k_address[i], req, len, s_resp, sizeof(s_resp), &s_rlen);
}

/** Extended session + Security Access on instance @p i. */
static void unlock(DoIP_Client_t *c, int i)
{
    const uint8_t session[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED };
    const uint8_t seed[]    = { UDS_SID_SECURITY_ACCESS, 0x01 };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(c, i, session, sizeof(session)));
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(c, i, seed, sizeof(seed)));
    uint32_t key = (((uint32_t)s_resp[2] << 24) | ((uint32_t)s_resp[3] << 16) |
                    ((uint32_t)s_resp[4] << 8) | s_resp[5]) ^ (uint32_t)UDS_SECURITY_KEY_MASK;
    const uint8_t send_key[] = { UDS_SID_SECURITY_ACCESS, 0x02,
                                 (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(c, i, send_key, sizeof(send_key)));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_SECURITY_ACCESS_RES, s_resp[0]);
}

/** UDP request to instance @p i; returns the response length (<= 0 on timeout). */
static ssize_t udp_request(int i, uint16_t pt, uint8_t *resp, size_t resp_size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in srv = { .sin_family = AF_INET, .sin_port = htons(k_port[i]) };
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    uint8_t frame[DOIP_HEADER_SIZE];
    doip_header_t hdr = { DOIP_PROTOCOL_VERSION, DOIP_INVERSE_VERSION, pt, 0 };
    doip_serialize_header(&hdr, frame, sizeof(frame));
    sendto(fd, frame, sizeof(frame), 0, (struct sockaddr *)&srv, sizeof(srv));
    ssize_t n = recv(fd, resp, resp_size, 0);
    close(fd);
    return n;
}

/** VIN announced by instance @p i over UDP (pointer into a static buffer). */
static const uint8_t *announced_vin(int i)
{
    static uint8_t resp[DOIP_HEADER_SIZE + 64];
    TEST_ASSERT_EQUAL_INT(DOIP_HEADER_SIZE + 33, udp_request(i, DOIP_PT_VIN_REQ, resp, sizeof(resp)));
    return resp + DOIP_HEADER_SIZE;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

void test_each_instance_has_its_own_identity_and_address(void)
{
    for (int i = A; i < COUNT; i++) {
        const uint8_t *ann = announced_vin(i);
        TEST_ASSERT_EQUAL_MEMORY(k_vin[i], ann, DOIP_VIN_LENGTH);
        TEST_ASSERT_EQUAL_HEX8(k_address[i] >> 8,   ann[17]);
        TEST_ASSERT_EQUAL_HEX8(k_address[i] & 0xFF, ann[18]);

        DoIP_Client_t *c = tester(i);
        TEST_ASSERT_NOT_NULL(c);
        const uint8_t read_vin[] = { UDS_SID_READ_DATA_BY_ID, 0xF1, 0x90 };
        TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(c, i, read_vin, sizeof(read_vin)));
        TEST_ASSERT_EQUAL_MEMORY(k_vin[i], &s_resp[3], DOIP_VIN_LENGTH);
        /* the other instance's ECU address is unknown here */
        TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_ERR_NACK, uds(c, i == A ? B : A, read_vin, sizeof(read_vin)));
        TEST_ASSERT_EQUAL_HEX8(0x03, DoIP_Client_GetLastNackCode(c));
        DoIP_Client_Destroy(c);
    }
}

void test_sessions_writes_and_dtcs_stay_in_their_instance(void)
{
    DoIP_Client_t *a = tester(A), *b = tester(B);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    /* On A: unlock, rewrite the VIN, clear the DTC memory */
    unlock(a, A);
    uint8_t write_vin[3 + DOIP_VIN_LENGTH] = { UDS_SID_WRITE_DATA_BY_ID, 0xF1, 0x90 };
    memcpy(&write_vin[3], "REWRITTEN-ON-A-01", DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(a, A, write_vin, sizeof(write_vin)));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_WRITE_DATA_BY_ID_RES, s_resp[0]);
    const uint8_t clear_all[] = { UDS_SID_CLEAR_DTC, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(a, A, clear_all, sizeof(clear_all)));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_CLEAR_DTC_RES, s_resp[0]);

    const uint8_t count_dtcs[] = { UDS_SID_READ_DTC_INFO, 0x01, 0xFF };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(a, A, count_dtcs, sizeof(count_dtcs)));
    TEST_ASSERT_EQUAL_HEX8(0, s_resp[5]);
    TEST_ASSERT_EQUAL_MEMORY("REWRITTEN-ON-A-01", announced_vin(A), DOIP_VIN_LENGTH);

    /* B saw none of it: DTCs intact, VIN unchanged, still default session and locked */
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(b, B, count_dtcs, sizeof(count_dtcs)));
    TEST_ASSERT_EQUAL_HEX8(3, s_resp[5]);
    TEST_ASSERT_EQUAL_MEMORY(k_vin[B], announced_vin(B), DOIP_VIN_LENGTH);
    memcpy(&write_vin[3], "MUST-NOT-BE-SET-B", DOIP_VIN_LENGTH);
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(b, B, write_vin, sizeof(write_vin)));
    const uint8_t not_in_session[] = { 0x7F, UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION };
    TEST_ASSERT_EQUAL_MEMORY(not_in_session, s_resp, 3);

    DoIP_Client_Destroy(a);
    DoIP_Client_Destroy(b);
}

void test_fault_injection_is_per_instance(void)
{
    const DoIP_FaultConfig_t nack_everything = { .force_diag_nack = 0x05 };
    TEST_ASSERT_EQUAL_INT(0, DoIP_SetFaults(s_ecu[A], &nack_everything));

    DoIP_Client_t *a = tester(A), *b = tester(B);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    const uint8_t tester_present[] = { UDS_SID_TESTER_PRESENT, 0x00 };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_ERR_NACK, uds(a, A, tester_present, sizeof(tester_present)));
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK,       uds(b, B, tester_present, sizeof(tester_present)));
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_TESTER_PRESENT_RES, s_resp[0]);
    DoIP_Client_Destroy(a);
    DoIP_Client_Destroy(b);
}

void test_open_socket_count_is_per_instance(void)
{
    uint8_t resp[DOIP_HEADER_SIZE + 16];
    DoIP_Client_t *a1 = tester(A), *a2 = tester(A);
    TEST_ASSERT_NOT_NULL(a1);
    TEST_ASSERT_NOT_NULL(a2);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)udp_request(A, DOIP_PT_ENTITY_STATUS_REQ, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8(2, resp[DOIP_HEADER_SIZE + 2]);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)udp_request(B, DOIP_PT_ENTITY_STATUS_REQ, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_HEX8(0, resp[DOIP_HEADER_SIZE + 2]);
    DoIP_Client_Destroy(a1);
    DoIP_Client_Destroy(a2);
}

void test_destroying_one_instance_leaves_the_other_serving(void)
{
    DoIP_Client_t *b = tester(B);
    TEST_ASSERT_NOT_NULL(b);
    stop(A);

    const uint8_t tester_present[] = { UDS_SID_TESTER_PRESENT, 0x00 };
    TEST_ASSERT_EQUAL_INT(DOIP_CLIENT_OK, uds(b, B, tester_present, sizeof(tester_present)));
    TEST_ASSERT_EQUAL_MEMORY(k_vin[B], announced_vin(B), DOIP_VIN_LENGTH);
    TEST_ASSERT_NULL_MESSAGE(tester(A), "A's port must be closed");
    DoIP_Client_Destroy(b);

    /* ...and A can come back on the same port */
    TEST_ASSERT_EQUAL_INT(0, start(A, k_port[A]));
    usleep(60000);
    TEST_ASSERT_EQUAL_MEMORY(k_vin[A], announced_vin(A), DOIP_VIN_LENGTH);
}

void test_port_conflict_fails_cleanly(void)
{
    /* A third instance on A's port cannot start. It must not keep a UDP socket
     * on that port either, or it would swallow A's discovery requests. */
    DoIP_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = 1;
    cfg.port      = k_port[A];
    DoIP_Handle_t *dup = DoIP_Create();
    TEST_ASSERT_NOT_EQUAL(0, DoIP_Init(dup, &cfg));
    DoIP_Tick(dup);                              /* no-op on an uninitialised handle */
    TEST_ASSERT_EQUAL_INT(-1, DoIP_SetFaults(dup, NULL));

    for (int n = 0; n < 5; n++)
        TEST_ASSERT_EQUAL_MEMORY(k_vin[A], announced_vin(A), DOIP_VIN_LENGTH);
    DoIP_Client_t *a = tester(A);
    TEST_ASSERT_NOT_NULL(a);
    DoIP_Client_Destroy(a);
    DoIP_Destroy(dup);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_each_instance_has_its_own_identity_and_address);
    RUN_TEST(test_sessions_writes_and_dtcs_stay_in_their_instance);
    RUN_TEST(test_fault_injection_is_per_instance);
    RUN_TEST(test_open_socket_count_is_per_instance);
    RUN_TEST(test_destroying_one_instance_leaves_the_other_serving);
    RUN_TEST(test_port_conflict_fails_cleanly);
    return UNITY_END();
}
