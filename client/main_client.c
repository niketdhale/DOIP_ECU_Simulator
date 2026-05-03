#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client/doip_client.h"
#include "config/doip_config.h"
#include "config/doip_uds_config.h"

/* Satisfy extern declarations from config/doip_config.h and core/doip_api.h.
 * The client binary does not use server-side Rx/Tx callbacks. */
DoIP_RxIndication   g_doip_rx_cb = NULL;
DoIP_TxConfirmation g_doip_tx_cb = NULL;

static void print_hex(const char *label, const uint8_t *buf, uint16_t len) {
    printf("  %-22s [%3u B]: ", label, len);
    for (uint16_t i = 0; i < len && i < 24; i++) printf("%02X ", buf[i]);
    if (len > 24) printf("...");
    printf("\n");
}

static int check(const char *step, DoIP_ClientStatus_t rc) {
    if (rc != DOIP_CLIENT_OK) {
        printf("[CLIENT] ❌ %s failed (rc=%d)\n", step, rc);
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    const char *server_ip = NULL;

    /* ── Step 1: UDP Discovery ─────────────────────────────────────────── */
    DoIP_DiscoveryResult_t disc;
    memset(&disc, 0, sizeof(disc));

    if (argc > 1) {
        server_ip = argv[1];
        printf("[CLIENT] Server IP from argument: %s\n", server_ip);
    } else {
        printf("[CLIENT] Broadcasting VIN Request on UDP port %u...\n", DOIP_UDP_PORT);
        DoIP_ClientStatus_t drc =
            DoIP_Client_Discover(NULL, DOIP_UDP_PORT, 2000U, &disc);
        if (drc == DOIP_CLIENT_OK) {
            server_ip = disc.server_ip;
            printf("[CLIENT] Found ECU: IP=%-15s  LA=0x%04X  VIN=%.17s\n",
                   disc.server_ip, disc.logical_address, disc.vin);
        } else {
            printf("[CLIENT] Discovery timeout — falling back to 127.0.0.1\n");
            server_ip = "127.0.0.1";
        }
    }

    /* ── Step 2: Create & Init ─────────────────────────────────────────── */
    DoIP_Client_t *client = DoIP_Client_Create();
    if (!client) {
        fprintf(stderr, "[CLIENT] DoIP_Client_Create failed (out of memory)\n");
        return EXIT_FAILURE;
    }

    DoIP_ClientConfig_t cfg = {
        .tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS,
        .connect_timeout_ms  = 3000U,
        .response_timeout_ms = DOIP_RX_TIMEOUT_MS,
        .routing_timeout_ms  = DOIP_ROUTING_ACTIVATION_TIMEOUT_MS
    };

    if (!check("Init", DoIP_Client_Init(client, &cfg))) {
        DoIP_Client_Destroy(client);
        return EXIT_FAILURE;
    }

    /* ── Step 3: TCP Connect ───────────────────────────────────────────── */
    printf("[CLIENT] Connecting to %s:%u...\n", server_ip, DOIP_TCP_PORT);
    if (!check("Connect", DoIP_Client_Connect(client, server_ip, DOIP_TCP_PORT))) {
        DoIP_Client_Destroy(client);
        return EXIT_FAILURE;
    }
    printf("[CLIENT] TCP connection established\n");

    /* ── Step 4: Routing Activation ────────────────────────────────────── */
    uint8_t act_code = 0;
    if (!check("ActivateRouting",
               DoIP_Client_ActivateRouting(client, 0x00U, &act_code))) {
        DoIP_Client_Destroy(client);
        return EXIT_FAILURE;
    }
    printf("[CLIENT] Routing activated (code=0x%02X)\n", act_code);

    /* ── Step 5: UDS Transactions ──────────────────────────────────────── */
    uint8_t  resp[256];
    uint16_t resp_len;
    int      all_ok = 1;

    /* 5a — Session Control: switch to Extended Diagnostic session */
    {
        uint8_t req[] = { UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED };
        resp_len = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, sizeof(req), resp, sizeof(resp), &resp_len);
        if (check("SessionControl", rc)) {
            print_hex("SessionControl rsp", resp, resp_len);
            if (resp_len > 0 && resp[0] == UDS_SID_SESSION_CONTROL_RES)
                printf("[CLIENT] ✅ Extended session active\n");
            else { printf("[CLIENT] ❌ Unexpected session response\n"); all_ok = 0; }
        } else all_ok = 0;
    }

    /* 5b — Read VIN (DID 0xF190) */
    {
        uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                          (uint8_t)(UDS_DID_VIN_NUMBER >> 8),
                          (uint8_t)(UDS_DID_VIN_NUMBER & 0xFF) };
        resp_len = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, sizeof(req), resp, sizeof(resp), &resp_len);
        if (check("ReadVIN", rc)) {
            print_hex("ReadVIN rsp", resp, resp_len);
            if (resp_len > 3 && resp[0] == UDS_SID_READ_DATA_BY_ID_RES)
                printf("[CLIENT] ✅ VIN: %.17s\n", &resp[3]);
            else { printf("[CLIENT] ❌ Unexpected VIN response\n"); all_ok = 0; }
        } else all_ok = 0;
    }

    /* 5c — Read Software Version (DID 0xF182) */
    {
        uint8_t req[] = { UDS_SID_READ_DATA_BY_ID,
                          (uint8_t)(UDS_DID_SOFTWARE_VERSION >> 8),
                          (uint8_t)(UDS_DID_SOFTWARE_VERSION & 0xFF) };
        resp_len = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, sizeof(req), resp, sizeof(resp), &resp_len);
        if (check("ReadSwVersion", rc)) {
            print_hex("ReadSwVersion rsp", resp, resp_len);
            printf("[CLIENT] ✅ SW Version read OK\n");
        } else all_ok = 0;
    }

    /* 5d — Tester Present (suppress positive response subfunction = 0x00) */
    {
        uint8_t req[] = { UDS_SID_TESTER_PRESENT, 0x00U };
        resp_len = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, sizeof(req), resp, sizeof(resp), &resp_len);
        if (check("TesterPresent", rc)) {
            print_hex("TesterPresent rsp", resp, resp_len);
            if (resp_len > 0 && resp[0] == UDS_SID_TESTER_PRESENT_RES)
                printf("[CLIENT] ✅ Tester Present OK\n");
            else { printf("[CLIENT] ❌ Unexpected TesterPresent response\n"); all_ok = 0; }
        } else all_ok = 0;
    }

    /* 5e — ECU Reset (hard reset) */
    {
        uint8_t req[] = { UDS_SID_ECU_RESET, UDS_RESET_HARD };
        resp_len = 0;
        DoIP_ClientStatus_t rc = DoIP_Client_Transact(
            client, DOIP_ECU_LOGICAL_ADDRESS,
            req, sizeof(req), resp, sizeof(resp), &resp_len);
        if (check("ECUReset", rc)) {
            print_hex("ECUReset rsp", resp, resp_len);
            printf("[CLIENT] ✅ ECU Reset sent OK\n");
        } else all_ok = 0;
    }

    /* ── Step 6: Disconnect ────────────────────────────────────────────── */
    DoIP_Client_Disconnect(client);
    DoIP_Client_Destroy(client);

    printf("\n[CLIENT] Test %s\n", all_ok ? "PASSED ✅" : "FAILED ❌");
    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
