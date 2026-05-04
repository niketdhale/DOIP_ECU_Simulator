/**
 * @file doip_udp.c
 * @brief DoIP UDP transport — vehicle announcements and service discovery.
 *
 * Changes from baseline:
 *  - IPv6 dual-stack socket when DOIP_ENABLE_IPV6=true (ff02::1 multicast).
 *  - All inet_addr() calls replaced with inet_pton() for correctness.
 *  - udp_send_to_addr() accepts sockaddr_storage for v4/v6 polymorphism.
 */

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
static DoIP_RxIndication g_rx_cb = NULL;
static uint32_t g_last_announce_ms   = 0;
static uint32_t g_announcement_count = 0;

/* Multicast destination — populated at init time */
static struct sockaddr_storage g_mcast_addr;
static socklen_t               g_mcast_addr_len = 0;

/* Runtime ECU identity — set once at doip_udp_init() */
static DoIP_EcuIdentity_t g_identity;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

/** Populate a vehicle announcement struct from the runtime identity. */
static void fill_announce(doip_vehicle_announce_t *a) {
    memset(a, 0, sizeof(*a));
    memcpy(a->vin, g_identity.vin, DOIP_VIN_LENGTH);
    a->logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS);
    memcpy(a->eid, g_identity.eid, DOIP_EID_LENGTH);
    memcpy(a->gid, g_identity.gid, DOIP_GID_LENGTH);
    a->further_action  = htonl((uint32_t)g_identity.further_action);
    a->vin_sync_status = htons((uint16_t)g_identity.vin_gw_sync_status);
}

/**
 * @brief Send a DoIP UDP frame to an explicit destination address.
 *
 * Accepts a sockaddr_storage so it works for both IPv4 and IPv6.
 */
static int udp_send_to_addr(uint16_t ptype, const void *payload, uint32_t plen,
                             const struct sockaddr_storage *dst, socklen_t dst_len)
{
    if (g_udp_sock < 0 || (plen > 0 && !payload) ||
        plen > DOIP_MAX_PAYLOAD_SIZE || !dst) return -1;

    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr;
    hdr.protocol_version = DOIP_PROTOCOL_VERSION;
    hdr.inverse_version  = DOIP_INVERSE_VERSION;
    hdr.payload_type     = ptype;
    hdr.payload_length   = plen;
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);

    ssize_t sent = sendto(g_udp_sock, tx_buf, DOIP_HEADER_SIZE + plen, 0,
                          (const struct sockaddr *)dst, dst_len);
    return (sent == (ssize_t)(DOIP_HEADER_SIZE + plen)) ? 0 : -1;
}

/** Send a DoIP UDP frame to the multicast group (for announcements). */
static int udp_send_payload(uint16_t ptype, const void *payload, uint32_t plen) {
    return udp_send_to_addr(ptype, payload, plen,
                             &g_mcast_addr, g_mcast_addr_len);
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int doip_udp_init(const DoIP_EcuIdentity_t *identity) {
    if (identity) g_identity = *identity;

#if DOIP_ENABLE_IPV6
    g_udp_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_sock < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "socket (IPv6) failed: %s", strerror(errno));
        return -1;
    }

    /* Dual-stack: also receive IPv4-mapped packets */
    int v6only = 0;
    setsockopt(g_udp_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse = 1;
    setsockopt(g_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(g_udp_sock, F_SETFL, fcntl(g_udp_sock, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in6 local6;
    memset(&local6, 0, sizeof(local6));
    local6.sin6_family = AF_INET6;
    local6.sin6_addr   = in6addr_any;
    local6.sin6_port   = htons(DOIP_UDP_PORT);
    if (bind(g_udp_sock, (struct sockaddr *)&local6, sizeof(local6)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "bind (IPv6) failed: %s", strerror(errno));
        close(g_udp_sock); g_udp_sock = -1; return -2;
    }

    /* Join IPv6 multicast group (DoIP link-local scope) */
    struct ipv6_mreq mreq6;
    memset(&mreq6, 0, sizeof(mreq6));
    if (inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR,
                   &mreq6.ipv6mr_multiaddr) != 1) {
        LOG_WARN(DOIP_LOG_MODULE_UDP, "Invalid IPv6 multicast address");
    } else {
        mreq6.ipv6mr_interface = 0; /* Default interface */
        setsockopt(g_udp_sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                   &mreq6, sizeof(mreq6));
    }

    /* Set multicast destination (IPv6) */
    struct sockaddr_in6 *mcast6 = (struct sockaddr_in6 *)&g_mcast_addr;
    memset(mcast6, 0, sizeof(*mcast6));
    mcast6->sin6_family = AF_INET6;
    mcast6->sin6_port   = htons(DOIP_UDP_PORT);
    inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR, &mcast6->sin6_addr);
    g_mcast_addr_len = sizeof(struct sockaddr_in6);

#else  /* IPv4 */
    g_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_sock < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "socket failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(g_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(g_udp_sock, F_SETFL, fcntl(g_udp_sock, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(DOIP_UDP_PORT);
    if (bind(g_udp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "bind failed: %s", strerror(errno));
        close(g_udp_sock); g_udp_sock = -1; return -2;
    }

    /* Join IPv4 multicast group */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mreq.imr_multiaddr) != 1) {
        LOG_WARN(DOIP_LOG_MODULE_UDP, "Invalid IPv4 multicast address");
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(g_udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq));
    }

    /* Set multicast destination (IPv4) */
    struct sockaddr_in *mcast4 = (struct sockaddr_in *)&g_mcast_addr;
    memset(mcast4, 0, sizeof(*mcast4));
    mcast4->sin_family = AF_INET;
    mcast4->sin_port   = htons(DOIP_UDP_PORT);
    inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mcast4->sin_addr);
    g_mcast_addr_len = sizeof(struct sockaddr_in);

#endif /* DOIP_ENABLE_IPV6 */

    g_last_announce_ms   = 0;
    g_announcement_count = 0;

    LOG_INFO(DOIP_LOG_MODULE_UDP,
             "UDP socket initialized on port %d (IPv6=%s)",
             DOIP_UDP_PORT, DOIP_ENABLE_IPV6 ? "yes" : "no");
    return 0;
}

int doip_udp_poll(DoIP_RxIndication rx_cb) {
    if (g_udp_sock < 0) return -1;

    uint8_t rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    struct sockaddr_storage src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t total = recvfrom(g_udp_sock, rx_buf, sizeof(rx_buf), 0,
                              (struct sockaddr *)&src_addr, &src_len);
    if (total < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (total < DOIP_HEADER_SIZE) return 0;

    doip_header_t hdr;
    if (doip_deserialize_header(rx_buf, (size_t)total, &hdr) < 0) return 0;
    if (total < (ssize_t)(DOIP_HEADER_SIZE + hdr.payload_length)) return 0;

    LOG_DEBUG(DOIP_LOG_MODULE_UDP,
              "[RX] PT=0x%04X, Len=%u", hdr.payload_type, hdr.payload_length);
    if (rx_cb) rx_cb(hdr.payload_type,
                     rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);

    if (hdr.payload_type == DOIP_PT_VIN_REQ ||
        hdr.payload_type == DOIP_PT_EID_REQ) {
        /* VIN / EID Request — respond unicast to requester (ISO 13400-2 §7.2) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling VIN/EID Request");
        doip_vehicle_announce_t resp;
        fill_announce(&resp);
        udp_send_to_addr(DOIP_PT_VIN_RES, &resp, sizeof(resp),
                          &src_addr, src_len);

    } else if (hdr.payload_type == DOIP_PT_ENTITY_STATUS_REQ) {
        /* Entity Status Request — respond unicast (ISO 13400-2 §7.6) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Entity Status Request");
        doip_entity_status_res_t res;
        res.node_type         = 0x01;
        res.max_open_sockets  = DOIP_MAX_TCP_CLIENTS;
        res.curr_open_sockets = doip_tcp_get_client_count();
        res.max_data_size     = htonl(DOIP_MAX_PAYLOAD_SIZE);
        udp_send_to_addr(DOIP_PT_ENTITY_STATUS_RES, &res, sizeof(res),
                          &src_addr, src_len);

    } else if (hdr.payload_type == DOIP_PT_POWER_MODE_REQ) {
        /* Diagnostic Power Mode Request — respond unicast (ISO 13400-2 §7.5) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Power Mode Request");
        doip_power_mode_res_t res;
        res.power_mode = 0x01;  /* ready for diagnostics */
        udp_send_to_addr(DOIP_PT_POWER_MODE_RES, &res, sizeof(res),
                          &src_addr, src_len);
    }
    return 0;
}

void doip_udp_tick(uint32_t now_ms) {
    if (!DOIP_ENABLE_PERIODIC_ANNOUNCE) return;
    if (g_announcement_count >= DOIP_ANNOUNCE_COUNT_MAX) return;
    if (now_ms - g_last_announce_ms < DOIP_ANNOUNCE_INTERVAL_MS) return;

    g_last_announce_ms = now_ms;
    g_announcement_count++;

    doip_vehicle_announce_t ann;
    fill_announce(&ann);
    if (udp_send_payload(DOIP_PT_VEHICLE_ANNOUNCE, &ann, sizeof(ann)) == 0) {
        LOG_INFO(DOIP_LOG_MODULE_UDP,
                 "Vehicle Announcement %u/%u sent",
                 g_announcement_count, DOIP_ANNOUNCE_COUNT_MAX);
    }
}

void doip_udp_deinit(void) {
    if (g_udp_sock >= 0) {
#if DOIP_ENABLE_IPV6
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;
        setsockopt(g_udp_sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
                   &mreq6, sizeof(mreq6));
#else
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(g_udp_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &mreq, sizeof(mreq));
#endif
        close(g_udp_sock);
        g_udp_sock = -1;
    }
    g_rx_cb = NULL;
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP socket closed");
}
