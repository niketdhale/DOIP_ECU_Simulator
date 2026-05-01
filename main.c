#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "core/doip_log.h"
#include "state/doip_fsm.h"

static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);
    
    DoIP_LogLevel_t log_level = DOIP_LOG_LEVEL_INFO;
    DoIP_LogModule_t log_modules = DOIP_LOG_MODULE_ALL;
    const char *log_file = NULL;
    uint32_t s3_timeout = 5000; /* Default 5s S3 */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) log_level = DOIP_LOG_LEVEL_VERBOSE;
        else if (strcmp(argv[i], "-d") == 0) log_level = DOIP_LOG_LEVEL_DEBUG;
        else if (strcmp(argv[i], "-q") == 0) log_level = DOIP_LOG_LEVEL_WARN;
        else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) log_file = argv[++i];
        else if (strcmp(argv[i], "--tcp-only") == 0) log_modules = DOIP_LOG_MODULE_TCP;
        else if (strcmp(argv[i], "--uds-only") == 0) log_modules = DOIP_LOG_MODULE_UDS;
        else if (strcmp(argv[i], "--s3") == 0 && i + 1 < argc) s3_timeout = (uint32_t)atoi(argv[++i]);
    }
    
    if (DoIP_Log_Init(log_level, log_modules, log_file) != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return EXIT_FAILURE;
    }
    
    LOG_CORE("=== DoIP ECU Simulator Starting ===");
    LOG_CORE("Log level: %d, Modules: 0x%02X, S3: %u ms", log_level, log_modules, s3_timeout);
    if (log_file) LOG_CORE("Logging to file: %s", log_file);

    /* Initialize FSM (handles transports internally) */
    if (DoIP_Fsm_Init(s3_timeout) != 0) {
        DOIP_LOG_ERROR(DOIP_LOG_MODULE_CORE, "FSM initialization failed");
        DoIP_Log_DeInit();
        return EXIT_FAILURE;
    }

    LOG_CORE("System ready. Press Ctrl+C to exit.");

    /* AUTOSAR-style cyclic scheduler */
    while (g_running) {
        DoIP_Fsm_MainFunction();
        usleep(1000); /* 1ms yield */
    }

    LOG_CORE("Shutting down...");
    DoIP_Fsm_DeInit();
    DoIP_Log_DeInit();
    
    printf("Cleanup complete.\n");
    return EXIT_SUCCESS;
}