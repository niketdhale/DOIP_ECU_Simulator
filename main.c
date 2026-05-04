#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "doip_api.h"
#include "config/doip_uds_config.h"

/* Global flag for signal handling */
static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

/* Callback: FSM state change notification */
static void my_state_cb(uint8_t old_st, uint8_t new_st, void *ctx) {
    (void)ctx;
    printf("[APP] State changed: %d -> %d\n", old_st, new_st);
}

/*
 * Optional runtime UDS hook.
 *
 * Return 0 (handled) to override the built-in response with your own data —
 * useful for serving DID values from NVM, live sensors, or DTCs.
 * Return -1 (not handled) to fall through to the built-in service table.
 *
 * This example shows the hook pattern but delegates everything to built-ins.
 * Replace the body with real NVM / sensor reads for production use.
 */
static int my_uds_hook(uint8_t sid,
                       const uint8_t *req, uint16_t req_len,
                       uint8_t *resp, uint16_t resp_size, uint16_t *resp_len_out,
                       void *ctx) {
    (void)ctx; (void)resp; (void)resp_size; (void)resp_len_out;

    /* Example: intercept a custom DID (0xA001) and return live data */
    if (sid == UDS_SID_READ_DATA_BY_ID && req_len >= 3) {
        uint16_t did = ((uint16_t)req[1] << 8) | req[2];
        (void)did;
        /* if (did == 0xA001) { ... populate resp ...; return 0; } */
    }
    return -1;  /* not handled — use built-in handler */
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    /* 1. Create Instance */
    DoIP_Handle_t *handle = DoIP_Create();
    if (!handle) {
        fprintf(stderr, "Failed to create DoIP instance\n");
        return EXIT_FAILURE;
    }

    /*
     * 2. Configure ECU identity (overrides compiled-in defaults).
     *    Change these values to match your target ECU without recompiling.
     */
    DoIP_EcuIdentity_t identity = {0};
    memcpy(identity.vin,               "WBAXXXXXXXXXXXXXX", DOIP_VIN_LENGTH); /* VIN is fixed 17-char; struct is zeroed so NUL is already at [17] */
    strncpy(identity.software_version, "V1.0.0",           sizeof(identity.software_version) - 1);
    strncpy(identity.system_name,      "DoIP ECU Simulator", sizeof(identity.system_name) - 1);
    strncpy(identity.serial_number,    "ECU123456789",     sizeof(identity.serial_number) - 1);
    memset(identity.eid, 0xAA, DOIP_EID_LENGTH);
    memset(identity.gid, 0xBB, DOIP_GID_LENGTH);
    identity.further_action    = 0x00;
    identity.vin_gw_sync_status = 0x00;

    /* 3. Configure */
    DoIP_Config_t config = {0};
    config.log_level             = 3; /* DEBUG */
    config.s3_server_timeout_ms  = 5000;
    config.on_state_change       = my_state_cb;
    config.ecu_identity          = &identity;
    config.on_uds_request        = my_uds_hook;

    /* Optional: override log file via --log-file <path> */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            config.log_file_path = argv[++i];
        }
    }

    /* 4. Initialize */
    if (DoIP_Init(handle, &config) != 0) {
        fprintf(stderr, "DoIP Initialization failed\n");
        DoIP_Destroy(handle);
        return EXIT_FAILURE;
    }

    printf("DoIP Simulator Running. Press Ctrl+C to exit.\n");

    /* 4. Main Loop */
    while (g_running) {
        DoIP_Tick(handle);
        usleep(1000); /* 1ms scheduler tick */
    }

    /* 5. Cleanup */
    DoIP_DeInit(handle);
    DoIP_Destroy(handle);
    
    printf("Shutdown complete.\n");
    return EXIT_SUCCESS;
}