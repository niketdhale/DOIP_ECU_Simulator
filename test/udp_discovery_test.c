#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "core/doip_api.h"
#include "transport/doip_udp.h"

static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

static void on_doip_rx(uint16_t ptype, const uint8_t *data, uint32_t len) {
    printf("[RX] PayloadType=0x%04X, Length=%u\n", ptype, len);
    if (len > 0 && len <= 32) {
        printf("      Data: ");
        for (uint32_t i = 0; i < len && i < 32; i++) printf("%02X ", data[i]);
        printf("\n");
    }
}

int main(void) {
    signal(SIGINT, sigint_handler);
    printf("=== DoIP UDP Discovery Test ===\n");
    
    if (doip_udp_init() != 0) {
        fprintf(stderr, "FATAL: Failed to init UDP transport\n");
        return EXIT_FAILURE;
    }
    printf("UDP socket initialized on port %d\n", DOIP_UDP_PORT);
    
    doip_vehicle_announce_t announce = {0};
    memcpy(announce.vin, "TESTVIN1234567890", DOIP_VIN_LENGTH);
    announce.logical_address = htons(0x0E00);
    memset(announce.eid, 0xAA, DOIP_EID_LENGTH);
    memset(announce.gid, 0xBB, DOIP_GID_LENGTH);
    announce.further_action = htonl(0x00000000);
    announce.vin_sync_status = htons(0x0010);
    
    if (doip_send_vehicle_announce(&announce) != 0) {
        fprintf(stderr, "ERROR: Failed to send announcement (errno: %d)\n", errno);
    } else {
        printf("✅ Sent Vehicle Announcement to 224.0.0.1:13400\n");
    }
    
    g_doip_rx_cb = on_doip_rx;
    printf("Polling for messages (Ctrl+C to exit)...\n");
    while (g_running) {
        int ret = doip_udp_poll(g_doip_rx_cb);
        if (ret < 0) { fprintf(stderr, "UDP poll error: %d\n", ret); break; }
        usleep(100000);
    }
    
    doip_udp_deinit();
    printf("Cleanup complete.\n");
    return EXIT_SUCCESS;
}