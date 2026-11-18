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
#include "core/doip_context.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
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

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

/** Populate a vehicle announcement struct from the instance's identity. */
static void fill_announce(const DoIP_Handle_t *h, doip_vehicle_announce_t *a) {
    memset(a, 0, sizeof(*a));
    memcpy(a->vin, h->identity.vin, DOIP_VIN_LENGTH);
    a->logical_address = htons(h->config.logical_address);
    memcpy(a->eid, h->identity.eid, DOIP_EID_LENGTH);
    memcpy(a->gid, h->identity.gid, DOIP_GID_LENGTH);
    a->further_action  = h->identity.further_action;
    a->vin_sync_status = h->identity.vin_gw_sync_status;
}

/**
 * @brief Send a DoIP UDP frame to an explicit destination address.
 *
 * Accepts a sockaddr_storage so it works for both IPv4 and IPv6.
 */
static int udp_send_to_addr(DoIP_Handle_t *h, uint16_t ptype, const void *payload, uint32_t plen,
                             const struct sockaddr_storage *dst, socklen_t dst_len)
{
    if (h->udp_fd < 0 || (plen > 0 && !payload) ||
        plen > DOIP_MAX_PAYLOAD_SIZE || !dst) return -1;

    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr;
    hdr.protocol_version = DOIP_PROTOCOL_VERSION;
    hdr.inverse_version  = DOIP_INVERSE_VERSION;
    hdr.payload_type     = ptype;
    hdr.payload_length   = plen;
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);

    ssize_t sent = sendto(h->udp_fd, tx_buf, DOIP_HEADER_SIZE + plen, 0,
                          (const struct sockaddr *)dst, dst_len);
    return (sent == (ssize_t)(DOIP_HEADER_SIZE + plen)) ? 0 : -1;
}

/** Send a DoIP UDP frame to the multicast group (for announcements). */
static int udp_send_payload(DoIP_Handle_t *h, uint16_t ptype, const void *payload, uint32_t plen) {
    return udp_send_to_addr(h, ptype, payload, plen,
                             &h->mcast_addr, h->mcast_addr_len);
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int doip_udp_init(DoIP_Handle_t *h) {

#if DOIP_ENABLE_IPV6
    h->udp_fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (h->udp_fd < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "socket (IPv6) failed: %s", strerror(errno));
        return -1;
    }

    /* Dual-stack: also receive IPv4-mapped packets */
    int v6only = 0;
    setsockopt(h->udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse = 1;
    setsockopt(h->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(h->udp_fd, F_SETFL, fcntl(h->udp_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in6 local6;
    memset(&local6, 0, sizeof(local6));
    local6.sin6_family = AF_INET6;
    local6.sin6_addr   = in6addr_any;
    local6.sin6_port   = htons(h->config.port);
    if (bind(h->udp_fd, (struct sockaddr *)&local6, sizeof(local6)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "bind (IPv6) failed: %s", strerror(errno));
        close(h->udp_fd); h->udp_fd = -1; return -2;
    }

    /* Join IPv6 multicast group (DoIP link-local scope) */
    struct ipv6_mreq mreq6;
    memset(&mreq6, 0, sizeof(mreq6));
    if (inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR,
                   &mreq6.ipv6mr_multiaddr) != 1) {
        LOG_WARN(DOIP_LOG_MODULE_UDP, "Invalid IPv6 multicast address");
    } else {
        mreq6.ipv6mr_interface = 0; /* Default interface */
        setsockopt(h->udp_fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                   &mreq6, sizeof(mreq6));
    }

    /* Set multicast destination (IPv6) */
    struct sockaddr_in6 *mcast6 = (struct sockaddr_in6 *)&h->mcast_addr;
    memset(mcast6, 0, sizeof(*mcast6));
    mcast6->sin6_family = AF_INET6;
    mcast6->sin6_port   = htons(h->config.port);
    inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR, &mcast6->sin6_addr);
    h->mcast_addr_len = sizeof(struct sockaddr_in6);

#else  /* IPv4 */
    h->udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->udp_fd < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "socket failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(h->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(h->udp_fd, F_SETFL, fcntl(h->udp_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(h->config.port);
    if (bind(h->udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_UDP, "bind failed: %s", strerror(errno));
        close(h->udp_fd); h->udp_fd = -1; return -2;
    }

    /* Join IPv4 multicast group */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mreq.imr_multiaddr) != 1) {
        LOG_WARN(DOIP_LOG_MODULE_UDP, "Invalid IPv4 multicast address");
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(h->udp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq));
    }

    /* Set multicast destination (IPv4) */
    struct sockaddr_in *mcast4 = (struct sockaddr_in *)&h->mcast_addr;
    memset(mcast4, 0, sizeof(*mcast4));
    mcast4->sin_family = AF_INET;
    mcast4->sin_port   = htons(h->config.port);
    inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mcast4->sin_addr);
    h->mcast_addr_len = sizeof(struct sockaddr_in);

#endif /* DOIP_ENABLE_IPV6 */

    h->last_announce_ms   = 0;
    h->announcement_count = 0;

    LOG_INFO(DOIP_LOG_MODULE_UDP,
             "UDP socket initialized on port %d (IPv6=%s)",
             h->config.port, DOIP_ENABLE_IPV6 ? "yes" : "no");
    return 0;
}

int doip_udp_poll(DoIP_Handle_t *h) {
    if (h->udp_fd < 0) return -1;

    uint8_t rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    struct sockaddr_storage src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t total = recvfrom(h->udp_fd, rx_buf, sizeof(rx_buf), 0,
                              (struct sockaddr *)&src_addr, &src_len);
    if (total < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (total < DOIP_HEADER_SIZE) return 0;

    doip_header_t hdr;
    if (doip_deserialize_header(rx_buf, (size_t)total, &hdr) < 0) return 0;
    if (total < (ssize_t)(DOIP_HEADER_SIZE + hdr.payload_length)) return 0;

    /* Vehicle announcements are not for us: our own come back through the
     * multicast group, and other entities' are of no interest to an ECU. */
    if (hdr.payload_type == DOIP_PT_VEHICLE_ANNOUNCE) return 0;

    LOG_DEBUG(DOIP_LOG_MODULE_UDP,
              "[RX] PT=0x%04X, Len=%u", hdr.payload_type, hdr.payload_length);
    if (h->config.on_rx_message)
        h->config.on_rx_message(hdr.payload_type, rx_buf + DOIP_HEADER_SIZE,
                                hdr.payload_length, h->config.user_context);

    const uint8_t *pl = rx_buf + DOIP_HEADER_SIZE;
    /* Vehicle ID Request: plain, or filtered by EID / VIN — only answer
     * a filtered request when the filter matches this entity. */
    bool id_req =
        (DOIP_SUPPORT_VIN_REQUEST && hdr.payload_type == DOIP_PT_VIN_REQ) ||
        (DOIP_SUPPORT_EID_REQUEST && hdr.payload_type == DOIP_PT_EID_REQ &&
         hdr.payload_length == DOIP_EID_LENGTH &&
         memcmp(pl, h->identity.eid, DOIP_EID_LENGTH) == 0) ||
        (DOIP_SUPPORT_VIN_REQUEST && hdr.payload_type == DOIP_PT_VIN_REQ_WITH_VIN &&
         hdr.payload_length == DOIP_VIN_LENGTH &&
         memcmp(pl, h->identity.vin, DOIP_VIN_LENGTH) == 0);

    if (id_req) {
        /* Respond unicast to requester with a Vehicle ID Response (0x0004) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Vehicle ID Request (PT=0x%04X)",
                  hdr.payload_type);
        doip_vehicle_announce_t resp;
        fill_announce(h, &resp);
        udp_send_to_addr(h, DOIP_PT_VEHICLE_ANNOUNCE, &resp, sizeof(resp),
                          &src_addr, src_len);

    } else if (hdr.payload_type == DOIP_PT_ENTITY_STATUS_REQ) {
        /* Entity Status Request — respond unicast (ISO 13400-2 §7.6) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Entity Status Request");
        doip_entity_status_res_t res;
        res.node_type         = 0x01;
        res.max_open_sockets  = DOIP_MAX_TCP_CLIENTS;
        res.curr_open_sockets = doip_tcp_get_client_count(h);
        res.max_data_size     = htonl(DOIP_MAX_PAYLOAD_SIZE);
        udp_send_to_addr(h, DOIP_PT_ENTITY_STATUS_RES, &res, sizeof(res),
                          &src_addr, src_len);

    } else if (hdr.payload_type == DOIP_PT_POWER_MODE_REQ) {
        /* Diagnostic Power Mode Request — respond unicast (ISO 13400-2 §7.5) */
        LOG_DEBUG(DOIP_LOG_MODULE_UDP, "Handling Power Mode Request");
        doip_power_mode_res_t res;
        res.power_mode = 0x01;  /* ready for diagnostics */
        udp_send_to_addr(h, DOIP_PT_POWER_MODE_RES, &res, sizeof(res),
                          &src_addr, src_len);
    }
    return 0;
}

void doip_udp_tick(DoIP_Handle_t *h, uint32_t now_ms) {
    if (!h->announce_enabled) return;
    if (h->announcement_count >= DOIP_ANNOUNCE_COUNT_MAX) return;
    if (now_ms - h->last_announce_ms < DOIP_ANNOUNCE_INTERVAL_MS) return;

    h->last_announce_ms = now_ms;
    h->announcement_count++;

    doip_vehicle_announce_t ann;
    fill_announce(h, &ann);
    if (udp_send_payload(h, DOIP_PT_VEHICLE_ANNOUNCE, &ann, sizeof(ann)) == 0) {
        LOG_INFO(DOIP_LOG_MODULE_UDP,
                 "Vehicle Announcement %u/%u sent",
                 h->announcement_count, DOIP_ANNOUNCE_COUNT_MAX);
    }
}

void doip_udp_deinit(DoIP_Handle_t *h) {
    if (h->udp_fd >= 0) {
#if DOIP_ENABLE_IPV6
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        inet_pton(AF_INET6, DOIP_IPV6_MULTICAST_ADDR, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;
        setsockopt(h->udp_fd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
                   &mreq6, sizeof(mreq6));
#else
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, DOIP_MULTICAST_ADDR, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(h->udp_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &mreq, sizeof(mreq));
#endif
        close(h->udp_fd);
        h->udp_fd = -1;
    }
    LOG_INFO(DOIP_LOG_MODULE_UDP, "UDP socket closed");
}
