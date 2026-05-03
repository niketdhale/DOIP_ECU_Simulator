/**
 * @file consumer.c
 * @brief Minimal standalone example using libdoip as an external library.
 *
 * Demonstrates the complete DoIP Client workflow using only the public API:
 *   1. UDP discovery (optional — skip if server IP is passed as argument)
 *   2. Create + Init client
 *   3. TCP Connect
 *   4. Routing Activation
 *   5. Read VIN via UDS ReadDataByIdentifier (0x22 0xF1 0x90)
 *   6. Disconnect
 *
 * Build in-tree:
 *   make test-lib
 *   LD_LIBRARY_PATH=. ./test_lib_example 127.0.0.1
 *
 * Build against installed library (pkg-config):
 *   gcc $(pkg-config --cflags --libs doip) example/consumer.c -o consumer
 *   ./consumer 127.0.0.1
 */
#include "include/doip.h"   /* umbrella — pulls in entire public API */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Satisfy extern globals declared in config/doip_config.h.
 * The library (doip_api.c) owns these at runtime; the client binary
 * only needs the declarations satisfied for the linker. */
DoIP_RxIndication   g_doip_rx_cb = NULL;
DoIP_TxConfirmation g_doip_tx_cb = NULL;

/* ── helpers ──────────────────────────────────────────────────────── */
static void print_hex(const char *label, const uint8_t *buf, uint16_t len)
{
    printf("  %-24s [%u B]: ", label, len);
    for (uint16_t i = 0; i < len && i < 20; i++) printf("%02X ", buf[i]);
    if (len > 20) printf("...");
    printf("\n");
}

static int must(const char *step, DoIP_ClientStatus_t rc)
{
    if (rc == DOIP_CLIENT_OK) return 1;
    fprintf(stderr, "[consumer] FAIL %s (rc=%d)\n", step, rc);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *server_ip = (argc > 1) ? argv[1] : NULL;

    printf("[consumer] libdoip v%s — minimal consumer example\n",
           DOIP_LIB_VERSION_STR);

    /* ── 1. UDP Discovery (skipped if IP given on command line) ───── */
    DoIP_DiscoveryResult_t disc;
    memset(&disc, 0, sizeof(disc));

    if (!server_ip) {
        printf("[consumer] Broadcasting VIN Request on UDP %u...\n",
               DOIP_UDP_PORT);
        DoIP_ClientStatus_t drc =
            DoIP_Client_Discover(NULL, DOIP_UDP_PORT, 2000U, &disc);
        if (drc == DOIP_CLIENT_OK) {
            server_ip = disc.server_ip;
            printf("[consumer] Found ECU: %s  LA=0x%04X  VIN=%.17s\n",
                   disc.server_ip, disc.logical_address, disc.vin);
        } else {
            printf("[consumer] Discovery timeout — trying 127.0.0.1\n");
            server_ip = "127.0.0.1";
        }
    }

    /* ── 2. Create & Init ─────────────────────────────────────────── */
    DoIP_Client_t *client = DoIP_Client_Create();
    if (!client) { fputs("[consumer] DoIP_Client_Create failed\n", stderr); return EXIT_FAILURE; }

    DoIP_ClientConfig_t cfg = {
        .tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS,
        .connect_timeout_ms  = 3000U,
        .response_timeout_ms = DOIP_RX_TIMEOUT_MS,
        .routing_timeout_ms  = DOIP_ROUTING_ACTIVATION_TIMEOUT_MS
    };
    if (!must("Init", DoIP_Client_Init(client, &cfg))) goto cleanup;

    /* ── 3. TCP Connect ───────────────────────────────────────────── */
    printf("[consumer] Connecting to %s:%u...\n", server_ip, DOIP_TCP_PORT);
    if (!must("Connect", DoIP_Client_Connect(client, server_ip, DOIP_TCP_PORT)))
        goto cleanup;
    printf("[consumer] Connected  (routing=%s)\n",
           DoIP_Client_IsConnected(client) ? "pending" : "n/a");

    /* ── 4. Routing Activation ────────────────────────────────────── */
    uint8_t act_code = 0;
    if (!must("ActivateRouting",
              DoIP_Client_ActivateRouting(client, 0x00U, &act_code)))
        goto cleanup;
    printf("[consumer] Routing active  code=0x%02X  active=%s\n",
           act_code, DoIP_Client_IsRoutingActive(client) ? "yes" : "no");

    /* ── 5. UDS: ReadDataByIdentifier — VIN (0xF190) ─────────────── */
    {
        uint8_t  req[]  = { UDS_SID_READ_DATA_BY_ID,
                            (uint8_t)(UDS_DID_VIN_NUMBER >> 8),
                            (uint8_t)(UDS_DID_VIN_NUMBER & 0xFF) };
        uint8_t  resp[64];
        uint16_t rlen = 0;

        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, (uint16_t)sizeof(req),
            resp, (uint16_t)sizeof(resp), &rlen);

        if (must("ReadVIN", rc)) {
            print_hex("VIN response raw", resp, rlen);
            if (rlen >= 3 && resp[0] == UDS_SID_READ_DATA_BY_ID_RES)
                printf("[consumer] VIN: %.17s\n", &resp[3]);
        }
    }

    /* ── 6. Disconnect ────────────────────────────────────────────── */
    DoIP_Client_Disconnect(client);
    printf("[consumer] Done — disconnected\n");

cleanup:
    DoIP_Client_Destroy(client);
    return EXIT_SUCCESS;
}
