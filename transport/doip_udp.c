#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
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

static int g_udp_sock = -1;
static struct sockaddr_in g_multicast_addr;
static DoIP_RxIndication g_rx_cb = NULL;
static uint32_t g_last_announce_ms = 0;
static uint32_t g_announcement_count = 0;

/* Runtime ECU identity — set once at doip_udp_init() */
static DoIP_EcuIdentity_t g_identity;

/* Helper: populate a vehicle announcement struct from g_identity */
static void fill_announce(doip_vehicle_announce_t *a) {
    memset(a, 0, sizeof(*a));
    memcpy(a->vin, g_identity.vin, DOIP_VIN_LENGTH);
    a->logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS);
    memcpy(a->eid, g_identity.eid, DOIP_EID_LENGTH);
    memcpy(a->gid, g_identity.gid, DOIP_GID_LENGTH);
    a->further_action  = htonl((uint32_t)g_identity.further_action);
    a->vin_sync_status = htons((uint16_t)g_identity.vin_gw_sync_status);
}

/** Send a DoIP UDP frame to a specific destination address. */
static int udp_send_to_addr(uint16_t ptype, const void *payload, uint32_t plen,
                             const struct sockaddr_in *dst)
{
    if (g_udp_sock < 0 || (plen > 0 && !payload) || plen > DOIP_MAX_PAYLOAD_SIZE || !dst) return -1;
    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr = { .protocol_version = DOIP_PROTOCOL_VERSION, .inverse_version = DOIP_INVERSE_VERSION, .payload_type = ptype, .payload_length = plen };
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);
    ssize_t sent = sendto(g_udp_sock, tx_buf, DOIP_HEADER_SIZE + plen, 0,
                          (const struct sockaddr*)dst, sizeof(*dst));
    return (sent == (ssize_t)(DOIP_HEADER_SIZE + plen)) ? 0 : -1;
}

/** Send a DoIP UDP frame to the multicast group (for announcements). */
static int udp_send_payload(uint16_t ptype, const void *payload, uint32_t plen) {
    return udp_send_to_addr(ptype, payload, plen, &g_multicast_addr);
}

int doip_udp_init(const DoIP_EcuIdentity_t *identity) {
    if (identity) g_identity = *identity;
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
    
    /* Reset counters on init */
    g_last_announce_ms = 0;
    g_announcement_count = 0;
    
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP socket initialized on port %d", DOIP_UDP_PORT);
    return 0;
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
    
    if (hdr.payload_type == DOIP_PT_VIN_REQ || hdr.payload_type == DOIP_PT_EID_REQ) {
        /* VIN / EID Request — respond unicast to requester (ISO 13400-2 §7.2) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling VIN/EID Request");
        doip_vehicle_announce_t resp;
        fill_announce(&resp);
        udp_send_to_addr(DOIP_PT_VIN_RES, &resp, sizeof(resp), &src_addr);

    } else if (hdr.payload_type == DOIP_PT_ENTITY_STATUS_REQ) {
        /* Entity Status Request — respond unicast (ISO 13400-2 §7.6) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Entity Status Request");
        doip_entity_status_res_t res;
        res.node_type         = 0x01;   /* DoIP node (not gateway) */
        res.max_open_sockets  = DOIP_MAX_TCP_CLIENTS;
        res.curr_open_sockets = doip_tcp_get_client_count();
        res.max_data_size     = htonl(DOIP_MAX_PAYLOAD_SIZE);
        udp_send_to_addr(DOIP_PT_ENTITY_STATUS_RES, &res, sizeof(res), &src_addr);

    } else if (hdr.payload_type == DOIP_PT_POWER_MODE_REQ) {
        /* Diagnostic Power Mode Request — respond unicast (ISO 13400-2 §7.5) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Power Mode Request");
        doip_power_mode_res_t res;
        res.power_mode = 0x01;   /* ready for diagnostics */
        udp_send_to_addr(DOIP_PT_POWER_MODE_RES, &res, sizeof(res), &src_addr);
    }
    return 0;
}

/* Tick function with Counter Logic */
void doip_udp_tick(uint32_t now_ms) {
    /* 1. Check Compile-time toggle (Disable completely if false) */
    if (!DOIP_ENABLE_PERIODIC_ANNOUNCE) return;

    /* 2. Check if max count reached (e.g., 5 times) */
    if (g_announcement_count >= DOIP_ANNOUNCE_COUNT_MAX) return;

    /* 3. Check time interval */
    if (now_ms - g_last_announce_ms < DOIP_ANNOUNCE_INTERVAL_MS) return;

    /* Update timer and counter */
    g_last_announce_ms = now_ms;
    g_announcement_count++;

    /* Build and Send Announcement using runtime identity */
    doip_vehicle_announce_t ann;
    fill_announce(&ann);

    if (udp_send_payload(DOIP_PT_VEHICLE_ANNOUNCE, &ann, sizeof(ann)) == 0) {
        LOG_INFO(DOIP_LOG_MODULE_UDP, "Vehicle Announcement %u/%u sent", g_announcement_count, DOIP_ANNOUNCE_COUNT_MAX);
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