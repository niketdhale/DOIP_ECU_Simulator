#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
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

static void usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --vin <17 chars>      Vehicle identification number\n"
           "  --address <addr>      ECU logical address, e.g. 0x1003\n"
           "  --functional-address <addr>  Functional group address (default 0x%04X)\n"
           "  --port <port>         UDP + TCP port (default %u)\n"
           "  --log-level <1-4>     1=WARN 2=INFO 3=DEBUG 4=VERBOSE (default 3)\n"
           "  --log-file <path>     Also write the log to a file\n"
           "Fault injection:\n"
           "  --response-delay <ms> Delay UDS responses (sends 7F xx 78 while waiting)\n"
           "  --drop-ack            Never send the DoIP diagnostic ACK\n"
           "  --drop-response       Never send UDS responses\n"
           "  --force-nack <code>   Reject every diagnostic message with this NACK code\n"
#if DOIP_ENABLE_TLS
           "TLS (required in this build):\n"
           "  --tls-cert <pem>      Server certificate\n"
           "  --tls-key <pem>       Server private key\n"
           "  --tls-ca <pem>        CA bundle used to verify client certificates\n"
           "  --tls-verify-peer     Require a valid client certificate\n"
#endif
           "  --help                Show this help\n",
           prog, (unsigned)DOIP_FUNCTIONAL_LOGICAL_ADDRESS, (unsigned)DOIP_TCP_PORT);
}

/* Parse an unsigned number (decimal or 0x hex) in [1, max]; exits on bad input. */
static unsigned long parse_num(const char *opt, const char *s, unsigned long max) {
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (*s == '\0' || *end != '\0' || v == 0 || v > max) {
        fprintf(stderr, "Invalid value for --%s: '%s' (expected 1..%lu)\n", opt, s, max);
        exit(EXIT_FAILURE);
    }
    return v;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);   /* a tester disconnecting mid-response must not kill us */

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

    /* Command-line overrides */
    static const struct option opts[] = {
        { "vin",       required_argument, NULL, 'v' },
        { "address",   required_argument, NULL, 'a' },
        { "functional-address", required_argument, NULL, 'F' },
        { "port",      required_argument, NULL, 'p' },
        { "log-level", required_argument, NULL, 'l' },
        { "log-file",  required_argument, NULL, 'f' },
        { "response-delay", required_argument, NULL, 'd' },
        { "drop-ack",       no_argument,       NULL, 'A' },
        { "drop-response",  no_argument,       NULL, 'R' },
        { "force-nack",     required_argument, NULL, 'n' },
#if DOIP_ENABLE_TLS
        { "tls-cert",        required_argument, NULL, 'c' },
        { "tls-key",         required_argument, NULL, 'k' },
        { "tls-ca",          required_argument, NULL, 'C' },
        { "tls-verify-peer", no_argument,       NULL, 'V' },
#endif
        { "help",      no_argument,       NULL, 'h' },
        { 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (opt) {
            case 'v':
                if (strlen(optarg) != DOIP_VIN_LENGTH) {
                    fprintf(stderr, "Invalid --vin: must be exactly %u characters\n",
                            (unsigned)DOIP_VIN_LENGTH);
                    DoIP_Destroy(handle);
                    return EXIT_FAILURE;
                }
                memcpy(identity.vin, optarg, DOIP_VIN_LENGTH);
                break;
            case 'a': config.logical_address = (uint16_t)parse_num("address", optarg, 0xFFFF); break;
            case 'F': config.functional_address = (uint16_t)parse_num("functional-address", optarg, 0xFFFF); break;
            case 'p': config.port            = (uint16_t)parse_num("port", optarg, 65535);     break;
            case 'l': config.log_level       = (uint8_t)parse_num("log-level", optarg, 4);     break;
            case 'f': config.log_file_path   = optarg; break;
            case 'd': config.faults.response_delay_ms = (uint32_t)parse_num("response-delay", optarg, 600000); break;
            case 'A': config.faults.drop_diag_ack     = true; break;
            case 'R': config.faults.drop_response     = true; break;
            case 'n': config.faults.force_diag_nack   = (uint8_t)parse_num("force-nack", optarg, 0xFF); break;
#if DOIP_ENABLE_TLS
            case 'c': config.tls.cert_file   = optarg; break;
            case 'k': config.tls.key_file    = optarg; break;
            case 'C': config.tls.ca_file     = optarg; break;
            case 'V': config.tls.verify_peer = true;   break;
#endif
            case 'h': usage(argv[0]); DoIP_Destroy(handle); return EXIT_SUCCESS;
            default:  usage(argv[0]); DoIP_Destroy(handle); return EXIT_FAILURE;
        }
    }

#if DOIP_ENABLE_TLS
    if (!config.tls.cert_file || !config.tls.key_file) {
        fprintf(stderr, "This build has TLS enabled: --tls-cert and --tls-key are required\n");
        DoIP_Destroy(handle);
        return EXIT_FAILURE;
    }
#endif

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