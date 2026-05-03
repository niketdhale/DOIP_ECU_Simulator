#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>      
#include "doip_api.h"

/* Global flag for signal handling */
static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

/* Callback Example: State Change */
void my_state_cb(uint8_t old_st, uint8_t new_st, void *ctx) {
    (void)ctx; /* FIX: Suppress unused parameter warning */
    printf("[APP] State changed: %d -> %d\n", old_st, new_st);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    /* 1. Create Instance */
    DoIP_Handle_t *handle = DoIP_Create();
    if (!handle) {
        fprintf(stderr, "Failed to create DoIP instance\n");
        return EXIT_FAILURE;
    }

    /* 2. Configure */
    DoIP_Config_t config = {0};
    config.log_level = 3; /* DEBUG */
    config.s3_server_timeout_ms = 5000;
    config.on_state_change = my_state_cb;
    
    /* Check for command line args to override log file */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            config.log_file_path = argv[++i];
        }
    }

    /* 3. Initialize */
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