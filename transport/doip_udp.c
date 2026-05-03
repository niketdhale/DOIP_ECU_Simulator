#include "transport/doip_udp.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

static int g_udp_sock = -1;
static struct sockaddr_in g_multicast_addr;
static DoIP_RxIndication g_rx_cb = NULL;
static uint32_t g_last_announce_ms = 0;
static bool g_periodic_announce_enabled = DOIP_ENABLE_PERIODIC_ANNOUNCE;

static int udp_send_payload(uint16_t ptype, const void *payload, uint32_t plen) {
    if (g_udp_sock < 0 || (plen > 0 && !payload) || plen > DOIP_MAX_PAYLOAD_SIZE) return -1;
    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr = { .protocol_version = DOIP_PROTOCOL_VERSION, .inverse_version = DOIP_INVERSE_VERSION, .payload_type = ptype, .payload_length = plen };
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);
    ssize_t sent = sendto(g_udp_sock, tx_buf, DOIP_HEADER_SIZE + plen, 0, (struct sockaddr*)&g_multicast_addr, sizeof(g_multicast_addr));
    return (sent == (ssize_t)(DOIP_HEADER_SIZE + plen)) ? 0 : -1;
}

int doip_udp_init(void) {
    g_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_sock < 0) { LOG_ERROR(DOIP_LOG_MODULE_UDP, "socket failed"); return -1; }
    int reuse = 1; setsockopt(g_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int flags = fcntl(g_udp_sock, F_GETFL, 0);
    fcntl(g_udp_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DOIP_UDP_PORT) };
    if (bind(g_udp_sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "bind failed"); close(g_udp_sock); g_udp_sock = -1; return -2;
    }
    
    struct ip_mreq mreq = { .imr_multiaddr.s_addr = inet_addr(DOIP_MULTICAST_ADDR), .imr_interface.s_addr = INADDR_ANY };
    setsockopt(g_udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    
    memset(&g_multicast_addr, 0, sizeof(g_multicast_addr));
    g_multicast_addr.sin_family = AF_INET;
    g_multicast_addr.sin_port = htons(DOIP_UDP_PORT);
    g_multicast_addr.sin_addr.s_addr = inet_addr(DOIP_MULTICAST_ADDR);
    
    /* Initialize broadcast timer */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_last_announce_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP socket initialized on port %d", DOIP_UDP_PORT);
    return 0;
}

int doip_send_vehicle_announce(const doip_vehicle_announce_t *announce) {
    if (!announce || g_udp_sock < 0) return -1;
    int ret = udp_send_payload(DOIP_PT_VEHICLE_ANNOUNCE, announce, sizeof(doip_vehicle_announce_t));
    if (ret == 0) LOG_INFO(DOIP_LOG_MODULE_UDP, "✅ Sent Vehicle Announcement");
    return ret;
}

int doip_udp_poll(DoIP_RxIndication rx_cb) {
    if (g_udp_sock < 0) return -1;
    uint8_t rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t total = recvfrom(g_udp_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&src_addr, &src_len);
    if (total < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (total < DOIP_HEADER_SIZE) return 0;

    doip_header_t hdr;
    if (doip_deserialize_header(rx_buf, total, &hdr) < 0) return 0;
    if (total < (ssize_t)(DOIP_HEADER_SIZE + hdr.payload_length)) return 0;

    LOG_DEBUG(DOIP_LOG_MODULE_UDP, "[RX] PT=0x%04X, Len=%u", hdr.payload_type, hdr.payload_length);
    if (rx_cb) rx_cb(hdr.payload_type, rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);
    doip_udp_handle_request(hdr.payload_type, rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);
    return 0;
}

void doip_udp_handle_request(uint16_t ptype, const uint8_t *data, uint32_t len) {
    (void)data; (void)len;
    if (ptype == DOIP_PT_VIN_REQ || ptype == DOIP_PT_EID_REQ) {
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling VIN/EID Request");
        doip_vehicle_announce_t resp = {0};
        memcpy(resp.vin, "WBAXXXXXXXXXXXXXX", DOIP_VIN_LENGTH);
        /* FIX: Use config macro instead of hardcoded 0x0E00 */
        resp.logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS); 
        memset(resp.eid, 0xAA, DOIP_EID_LENGTH);
        memset(resp.gid, 0xBB, DOIP_GID_LENGTH);
        resp.further_action = htonl(0x00000000);
        resp.vin_sync_status = htons(0x0010);
        udp_send_payload(DOIP_PT_VIN_RES, &resp, sizeof(resp));
    }
}

void doip_udp_set_periodic_announce(bool enable) {
    g_periodic_announce_enabled = enable;
    LOG_INFO(DOIP_LOG_MODULE_UDP, "Periodic Announcement: %s", enable ? "ENABLED" : "DISABLED");
}

void doip_udp_tick(uint32_t now_ms) {
    if (g_udp_sock < 0 || !g_periodic_announce_enabled) return;
    
    if (now_ms - g_last_announce_ms >= DOIP_ANNOUNCE_INTERVAL_MS) {
        g_last_announce_ms = now_ms;
        doip_vehicle_announce_t ann = {0};
        memcpy(ann.vin, "WBAXXXXXXXXXXXXXX", DOIP_VIN_LENGTH);
        ann.logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS);
        memset(ann.eid, 0xAA, DOIP_EID_LENGTH);
        memset(ann.gid, 0xBB, DOIP_GID_LENGTH);
        ann.further_action = htonl(0x00000000);
        ann.vin_sync_status = htons(0x0010);
        
        if (udp_send_payload(DOIP_PT_VEHICLE_ANNOUNCE, &ann, sizeof(ann)) == 0) {
            LOG_DEBUG(DOIP_LOG_MODULE_UDP, "📡 Periodic Vehicle Announcement sent");
        }
    }
}

void doip_udp_deinit(void) {
    if (g_udp_sock >= 0) {
        struct ip_mreq mreq = { .imr_multiaddr.s_addr = inet_addr(DOIP_MULTICAST_ADDR), .imr_interface.s_addr = INADDR_ANY };
        setsockopt(g_udp_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        close(g_udp_sock); g_udp_sock = -1;
    }
    g_rx_cb = NULL;
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP socket closed");
}