#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "core/doip_log.h"
#include "core/doip_api.h"
#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"

static volatile int g_running = 1;
static void sigint_handler(int sig) { 
    (void)sig; 
    g_running = 0; 
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);
    
    /* Parse command line arguments */
    DoIP_LogLevel_t log_level = DOIP_LOG_LEVEL_INFO;
    DoIP_LogModule_t log_modules = DOIP_LOG_MODULE_ALL;
    const char *log_file = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            log_level = DOIP_LOG_LEVEL_VERBOSE;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            log_level = DOIP_LOG_LEVEL_DEBUG;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            log_level = DOIP_LOG_LEVEL_WARN;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_file = argv[++i];
        } else if (strcmp(argv[i], "--tcp-only") == 0) {
            log_modules = DOIP_LOG_MODULE_TCP;
        } else if (strcmp(argv[i], "--uds-only") == 0) {
            log_modules = DOIP_LOG_MODULE_UDS;
        }
    }
    
    /* Initialize logging */
    if (DoIP_Log_Init(log_level, log_modules, log_file) != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return EXIT_FAILURE;
    }
    
    LOG_CORE("=== DoIP ECU Simulator Starting ===");
    LOG_CORE("Log level: %d, Modules: 0x%02X", log_level, log_modules);
    if (log_file) {
        LOG_CORE("Logging to file: %s", log_file);
    }

    /* Initialize transports */
    if (doip_udp_init() != 0) {
        DOIP_LOG_ERROR(DOIP_LOG_MODULE_CORE, "UDP initialization failed");
        DoIP_Log_DeInit();
        return EXIT_FAILURE;
    }
    
    if (doip_tcp_init() != 0) {
        DOIP_LOG_ERROR(DOIP_LOG_MODULE_CORE, "TCP initialization failed");
        doip_udp_deinit();
        DoIP_Log_DeInit();
        return EXIT_FAILURE;
    }

    /* Initialize UDS */
    doip_uds_init();

    LOG_CORE("UDP & TCP ready. Press Ctrl+C to exit.");
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server listening on port %d", DOIP_TCP_PORT);
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP listening on port %d", DOIP_UDP_PORT);

    while (g_running) {
        doip_udp_poll(NULL);
        doip_tcp_poll(NULL);
        usleep(1000);
    }

    LOG_CORE("Shutting down...");
    
    doip_uds_deinit();
    doip_tcp_deinit();
    doip_udp_deinit();
    DoIP_Log_DeInit();
    
    printf("Cleanup complete.\n");
    return EXIT_SUCCESS;
}