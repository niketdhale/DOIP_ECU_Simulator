#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "core/doip_api.h"
#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"

static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

static void on_doip_rx(uint16_t ptype, const uint8_t *data, uint32_t len) {
    (void)data;
    printf("[RX] PT=0x%04X, Len=%u\n", ptype, len);
}

int main(void) {
    signal(SIGINT, sigint_handler);
    printf("=== DoIP ECU Simulator ===\n");

    if (doip_udp_init() != 0 || doip_tcp_init() != 0) {
        fprintf(stderr, "FATAL: Transport init failed\n");
        return EXIT_FAILURE;
    }

    /* Initialize UDS module */
    doip_uds_init();

    g_doip_rx_cb = on_doip_rx;
    printf("UDP & TCP ready. Press Ctrl+C to exit.\n");

    /* Poll loop relies on socket/select timeouts internally */
    while (g_running) {
        doip_udp_poll(g_doip_rx_cb);
        doip_tcp_poll(g_doip_rx_cb);
        /* No usleep needed: doip_tcp_poll blocks up to 50ms via select() */
    }

    doip_tcp_deinit();
    doip_udp_deinit();
    printf("Cleanup complete.\n");
    return EXIT_SUCCESS;
}