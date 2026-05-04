/**
 * @file consumer.c
 * @brief Minimal standalone example using libdoip as an external library.
 *
 * Demonstrates the complete DoIP Client workflow using only the public API:
 *   1. UDP discovery (optional — skip if server IP is passed as argument)
 *   2. Create + Init client
 *   3. TCP Connect
 *   4. Routing Activation
 *   5. Read VIN, Software Version, and System Name via UDS 0x22
 *   6. Send Tester Present (0x3E)
 *   7. Disconnect
 *
 * Build in-tree:
 *   make test-lib
 *   LD_LIBRARY_PATH=. ./test_lib_example 127.0.0.1
 *
 * Build against installed library (pkg-config):
 *   gcc $(pkg-config --cflags --libs doip) example/consumer.c -o consumer
 *   ./consumer 127.0.0.1
 *
 * --- Server-side ECU identity configuration ---
 * The ECU simulator's response data (VIN, SW version, system name, etc.) can
 * be configured at runtime via DoIP_Config_t without recompiling the library.
 * See the annotated snippet below — this would appear in your server main():
 *
 *   DoIP_EcuIdentity_t identity = {0};
 *   strncpy(identity.vin,              "MYVIN00000000001", DOIP_VIN_LENGTH);
 *   strncpy(identity.software_version, "V2.5.0",          sizeof(identity.software_version) - 1);
 *   strncpy(identity.system_name,      "My ECU",          sizeof(identity.system_name) - 1);
 *   strncpy(identity.serial_number,    "SN-12345678",     sizeof(identity.serial_number) - 1);
 *   memset(identity.eid, 0x11, DOIP_EID_LENGTH);
 *   memset(identity.gid, 0x22, DOIP_GID_LENGTH);
 *
 *   DoIP_Config_t config = {0};
 *   config.ecu_identity     = &identity;
 *   config.on_uds_request   = my_live_data_hook;  // optional: override with NVM data
 *   DoIP_Init(handle, &config);
 *
 * The optional on_uds_request hook is called before the built-in handlers.
 * Return 0 (handled) to supply your own response; return -1 to fall through
 * to the built-in service table.
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
    printf("  %-28s [%u B]: ", label, len);
    for (uint16_t i = 0; i < len && i < 24; i++) printf("%02X ", buf[i]);
    if (len > 24) printf("...");
    printf("\n");
}

static int must(const char *step, DoIP_ClientStatus_t rc)
{
    if (rc == DOIP_CLIENT_OK) {
        printf("[consumer] OK:   %s\n", step);
        return 1;
    }
    fprintf(stderr, "[consumer] FAIL: %s (rc=%d)\n", step, rc);
    return 0;
}

/* ── read_did: send 0x22 <did_hi> <did_lo>, print response ───────── */
static void read_did(DoIP_Client_t *client, uint16_t did, const char *label)
{
    uint8_t req[3] = {
        UDS_SID_READ_DATA_BY_ID,
        (uint8_t)(did >> 8),
        (uint8_t)(did & 0xFF)
    };
    uint8_t  resp[128];
    uint16_t rlen = 0;

    DoIP_ClientStatus_t rc = DoIP_Client_Transact(
        client, DOIP_ECU_LOGICAL_ADDRESS,
        req, (uint16_t)sizeof(req),
        resp, (uint16_t)sizeof(resp), &rlen);

    if (rc != DOIP_CLIENT_OK) {
        fprintf(stderr, "[consumer] FAIL: ReadDID 0x%04X (%s) rc=%d\n", did, label, rc);
        return;
    }
    print_hex(label, resp, rlen);
    if (rlen > 3 && resp[0] == UDS_SID_READ_DATA_BY_ID_RES) {
        /* Print as ASCII string if printable */
        printf("             value: \"");
        for (uint16_t i = 3; i < rlen; i++) {
            char c = (char)resp[i];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("\"\n");
    }
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *server_ip = (argc > 1) ? argv[1] : NULL;

    printf("[consumer] libdoip v%s — consumer example\n",
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

    /* ── 4. Routing Activation ────────────────────────────────────── */
    uint8_t act_code = 0;
    if (!must("ActivateRouting",
              DoIP_Client_ActivateRouting(client, 0x00U, &act_code)))
        goto cleanup;
    printf("[consumer] Routing active  code=0x%02X\n", act_code);

    /* ── 5. UDS: Read identity DIDs ──────────────────────────────── */
    printf("\n[consumer] Reading ECU identity DIDs:\n");
    read_did(client, UDS_DID_VIN_NUMBER,        "VIN (0xF190)");
    read_did(client, UDS_DID_SOFTWARE_VERSION,  "SW Version (0xF189)");
    read_did(client, UDS_DID_SYSTEM_NAME,       "System Name (0xF197)");
    read_did(client, UDS_DID_ECU_SERIAL_NUMBER, "Serial Number (0xF18C)");

    /* ── 6. Tester Present (keep session alive) ──────────────────── */
    {
        uint8_t  req[]  = { UDS_SID_TESTER_PRESENT, 0x00 };
        uint8_t  resp[8];
        uint16_t rlen = 0;
        DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, (uint16_t)sizeof(req),
            resp, (uint16_t)sizeof(resp), &rlen);
        printf("\n[consumer] Tester Present response: %s\n",
               (rlen >= 1 && resp[0] == UDS_SID_TESTER_PRESENT_RES) ? "OK" : "unexpected");
    }

    /* ── 7. Disconnect ────────────────────────────────────────────── */
    DoIP_Client_Disconnect(client);
    printf("[consumer] Done — disconnected\n");

cleanup:
    DoIP_Client_Destroy(client);
    return EXIT_SUCCESS;
}
