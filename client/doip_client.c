#include "client/doip_client.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include "transport/doip_tcp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

/* ===== Internal Client State Machine ===== */

typedef enum {
    DOIP_CLI_STATE_UNINIT      = 0,
    DOIP_CLI_STATE_INITIALIZED = 1,
    DOIP_CLI_STATE_CONNECTED   = 2,
    DOIP_CLI_STATE_ACTIVATED   = 3,
    DOIP_CLI_STATE_ERROR       = 0xFF
} DoIP_ClientState_t;

struct DoIP_ClientContext {
    DoIP_ClientState_t   state;
    int                  tcp_fd;
    DoIP_ClientConfig_t  config;
    uint16_t             tester_la;
    uint16_t             peer_ecu_la;       /* ECU address learned from routing activation response */
    char                 server_ip[INET6_ADDRSTRLEN]; /* Wide enough for IPv4 or IPv6 */
    uint16_t             server_port;
    uint8_t              rx_payload[DOIP_MAX_PAYLOAD_SIZE];  /* scratch recv buffer */
};

/* ===== Static Helpers ===== */

static int client_set_rx_timeout(int fd, uint32_t timeout_ms) {
    struct timeval tv = {
        .tv_sec  = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U)
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Receive one complete DoIP frame: sets socket timeout, reads header, reads payload.
 * Returns DOIP_CLIENT_OK on success, negative error code otherwise. */
static DoIP_ClientStatus_t client_recv_frame(int fd, uint32_t timeout_ms,
                                              doip_header_t *hdr_out,
                                              uint8_t *payload_buf) {
    client_set_rx_timeout(fd, timeout_ms);

    uint8_t raw[DOIP_HEADER_SIZE];
    ssize_t n = doip_recv_n(fd, raw, DOIP_HEADER_SIZE);
    if (n == 0)  return DOIP_CLIENT_ERR_IO;      /* server closed connection */
    if (n == -2) return DOIP_CLIENT_ERR_TIMEOUT;
    if (n < 0)   return DOIP_CLIENT_ERR_IO;

    ssize_t hdr_rc = doip_deserialize_header(raw, DOIP_HEADER_SIZE, hdr_out);
    if (hdr_rc < 0) {
        LOG_WARN(DOIP_LOG_MODULE_CLIENT, "Malformed DoIP header (rc=%zd)", hdr_rc);
        return DOIP_CLIENT_ERR_NACK;
    }

    if (hdr_out->payload_length > 0) {
        if (hdr_out->payload_length > DOIP_MAX_PAYLOAD_SIZE) {
            LOG_WARN(DOIP_LOG_MODULE_CLIENT, "Payload too large (%u)", hdr_out->payload_length);
            return DOIP_CLIENT_ERR_IO;
        }
        n = doip_recv_n(fd, payload_buf, hdr_out->payload_length);
        if (n == -2) return DOIP_CLIENT_ERR_TIMEOUT;
        if (n != (ssize_t)hdr_out->payload_length) return DOIP_CLIENT_ERR_IO;
    }

    return DOIP_CLIENT_OK;
}

/* ===== Lifecycle ===== */

DoIP_Client_t *DoIP_Client_Create(void) {
    DoIP_Client_t *c = (DoIP_Client_t *)calloc(1, sizeof(DoIP_Client_t));
    if (!c) return NULL;
    c->tcp_fd = -1;
    c->state  = DOIP_CLI_STATE_UNINIT;
    return c;
}

void DoIP_Client_Destroy(DoIP_Client_t *client) {
    if (!client) return;
    DoIP_Client_DeInit(client);
    free(client);
}

DoIP_ClientStatus_t DoIP_Client_Init(DoIP_Client_t *client, const DoIP_ClientConfig_t *cfg) {
    if (!client) return DOIP_CLIENT_ERR_PARAM;

    if (cfg) {
        client->config = *cfg;
    } else {
        client->config.tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS;
        client->config.connect_timeout_ms  = DOIP_RX_TIMEOUT_MS;
        client->config.response_timeout_ms = DOIP_RX_TIMEOUT_MS;
        client->config.routing_timeout_ms  = DOIP_ROUTING_ACTIVATION_TIMEOUT_MS;
    }

    /* Apply defaults for any zero fields so partial configs are safe */
    if (client->config.tester_logical_addr == 0)
        client->config.tester_logical_addr = DOIP_TESTER_LOGICAL_ADDRESS;
    if (client->config.connect_timeout_ms  == 0)
        client->config.connect_timeout_ms  = DOIP_RX_TIMEOUT_MS;
    if (client->config.response_timeout_ms == 0)
        client->config.response_timeout_ms = DOIP_RX_TIMEOUT_MS;
    if (client->config.routing_timeout_ms  == 0)
        client->config.routing_timeout_ms  = DOIP_ROUTING_ACTIVATION_TIMEOUT_MS;

    client->tester_la = client->config.tester_logical_addr;
    client->tcp_fd    = -1;
    client->state     = DOIP_CLI_STATE_INITIALIZED;

    LOG_INFO(DOIP_LOG_MODULE_CLIENT, "Client initialized (tester_la=0x%04X)", client->tester_la);
    return DOIP_CLIENT_OK;
}

void DoIP_Client_DeInit(DoIP_Client_t *client) {
    if (!client) return;
    DoIP_Client_Disconnect(client);
    client->state = DOIP_CLI_STATE_UNINIT;
}

/* ===== UDP Discovery ===== */

DoIP_ClientStatus_t DoIP_Client_Discover(const char *target_ip, uint16_t target_port,
                                          uint32_t timeout_ms,
                                          DoIP_DiscoveryResult_t *result_out) {
    if (!result_out) return DOIP_CLIENT_ERR_PARAM;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DOIP_CLIENT_ERR_SOCKET;

    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct timeval tv = {
        .tv_sec  = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U)
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *dest = target_ip ? target_ip : DOIP_BROADCAST_ADDR;
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(target_port),
        .sin_addr.s_addr = inet_addr(dest)
    };

    /* Build and send VIN Request (zero-payload DoIP frame) */
    uint8_t hdr_buf[DOIP_HEADER_SIZE];
    doip_header_t req_hdr = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = DOIP_INVERSE_VERSION,
        .payload_type     = DOIP_PT_VIN_REQ,
        .payload_length   = 0
    };
    doip_serialize_header(&req_hdr, hdr_buf, sizeof(hdr_buf));

    if (sendto(fd, hdr_buf, DOIP_HEADER_SIZE, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        close(fd);
        return DOIP_CLIENT_ERR_IO;
    }

    /* Wait for Vehicle Announcement (0x0003) or VIN Response (0x0002) */
    uint8_t rx_buf[DOIP_HEADER_SIZE + sizeof(doip_vehicle_announce_t) + 16];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t n = recvfrom(fd, rx_buf, sizeof(rx_buf), 0,
                         (struct sockaddr *)&src_addr, &src_len);
    close(fd);

    if (n < (ssize_t)DOIP_HEADER_SIZE) return DOIP_CLIENT_ERR_TIMEOUT;

    doip_header_t rx_hdr;
    if (doip_deserialize_header(rx_buf, (size_t)n, &rx_hdr) < 0)
        return DOIP_CLIENT_ERR_NACK;

    if (rx_hdr.payload_type != DOIP_PT_VIN_RES &&
        rx_hdr.payload_type != DOIP_PT_VEHICLE_ANNOUNCE)
        return DOIP_CLIENT_ERR_IO;

    if (rx_hdr.payload_length < 17U + 2U)  /* VIN(17) + logical_address(2) minimum */
        return DOIP_CLIENT_ERR_IO;
    if ((size_t)n < DOIP_HEADER_SIZE + sizeof(doip_vehicle_announce_t))
        return DOIP_CLIENT_ERR_IO;

    doip_vehicle_announce_t ann_buf;
    memcpy(&ann_buf, rx_buf + DOIP_HEADER_SIZE, sizeof(ann_buf));
    const doip_vehicle_announce_t *ann = &ann_buf;

    inet_ntop(AF_INET, &src_addr.sin_addr, result_out->server_ip, INET_ADDRSTRLEN);
    result_out->logical_address = ntohs(ann->logical_address);
    memcpy(result_out->vin, ann->vin, DOIP_VIN_LENGTH);
    memcpy(result_out->eid, ann->eid, DOIP_EID_LENGTH);
    memcpy(result_out->gid, ann->gid, DOIP_GID_LENGTH);

    LOG_INFO(DOIP_LOG_MODULE_CLIENT, "Discovery: ECU at %s (LA=0x%04X, VIN=%.17s)",
             result_out->server_ip, result_out->logical_address, result_out->vin);
    return DOIP_CLIENT_OK;
}

/* ===== TCP Connection ===== */

DoIP_ClientStatus_t DoIP_Client_Connect(DoIP_Client_t *client,
                                         const char *server_ip, uint16_t server_port) {
    if (!client || !server_ip) return DOIP_CLIENT_ERR_PARAM;
    if (client->state < DOIP_CLI_STATE_INITIALIZED) return DOIP_CLIENT_ERR_NOT_READY;
    if (client->tcp_fd >= 0) DoIP_Client_Disconnect(client);

    /* Use getaddrinfo() so the caller can pass either an IPv4 or IPv6 address */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", server_port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;     /* Accept IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai_rc = getaddrinfo(server_ip, port_str, &hints, &res);
    if (gai_rc != 0 || !res) {
        LOG_ERROR(DOIP_LOG_MODULE_CLIENT, "getaddrinfo(%s): %s",
                  server_ip, gai_strerror(gai_rc));
        return DOIP_CLIENT_ERR_SOCKET;
    }

    int fd = -1;
    struct addrinfo *rp;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        client_set_rx_timeout(fd, client->config.connect_timeout_ms);

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break; /* success */

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CLIENT, "connect(%s:%u) failed: %s",
                  server_ip, server_port, strerror(errno));
        return DOIP_CLIENT_ERR_SOCKET;
    }

    client->tcp_fd      = fd;
    client->server_port = server_port;
    strncpy(client->server_ip, server_ip, sizeof(client->server_ip) - 1);
    client->server_ip[sizeof(client->server_ip) - 1] = '\0';
    client->state = DOIP_CLI_STATE_CONNECTED;

    LOG_INFO(DOIP_LOG_MODULE_CLIENT, "Connected to %s:%u (fd=%d)",
             server_ip, server_port, fd);
    return DOIP_CLIENT_OK;
}

DoIP_ClientStatus_t DoIP_Client_Disconnect(DoIP_Client_t *client) {
    if (!client) return DOIP_CLIENT_ERR_PARAM;
    if (client->tcp_fd >= 0) {
        close(client->tcp_fd);
        client->tcp_fd = -1;
        LOG_INFO(DOIP_LOG_MODULE_CLIENT, "Disconnected from %s:%u",
                 client->server_ip, client->server_port);
    }
    if (client->state >= DOIP_CLI_STATE_CONNECTED)
        client->state = DOIP_CLI_STATE_INITIALIZED;
    return DOIP_CLIENT_OK;
}

/* ===== Routing Activation ===== */

DoIP_ClientStatus_t DoIP_Client_ActivateRouting(DoIP_Client_t *client,
                                                  uint8_t activation_type,
                                                  uint8_t *out_activation_code) {
    if (!client) return DOIP_CLIENT_ERR_PARAM;
    if (client->state != DOIP_CLI_STATE_CONNECTED &&
        client->state != DOIP_CLI_STATE_ACTIVATED)
        return DOIP_CLIENT_ERR_NOT_READY;

    doip_routing_act_req_t req;
    memset(&req, 0, sizeof(req));
#if (DOIP_LOGICAL_ADDR_SIZE == 2)
    req.tester_logical_address = htons(client->tester_la);
#else
    req.tester_logical_address = htonl(client->tester_la);
#endif
    req.activation_type = activation_type;

    if (doip_send_frame(client->tcp_fd, DOIP_PT_ROUTING_ACT_REQ, &req, sizeof(req)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CLIENT, "Failed to send routing activation request");
        return DOIP_CLIENT_ERR_IO;
    }

    doip_header_t rx_hdr;
    DoIP_ClientStatus_t rc = client_recv_frame(client->tcp_fd,
                                                client->config.routing_timeout_ms,
                                                &rx_hdr, client->rx_payload);
    if (rc != DOIP_CLIENT_OK) return rc;

    if (rx_hdr.payload_type != DOIP_PT_ROUTING_ACT_RES) {
        LOG_WARN(DOIP_LOG_MODULE_CLIENT, "Expected ROUTING_ACT_RES (0x%04X), got 0x%04X",
                 DOIP_PT_ROUTING_ACT_RES, rx_hdr.payload_type);
        return DOIP_CLIENT_ERR_IO;
    }
    if (rx_hdr.payload_length < sizeof(doip_routing_act_res_t))
        return DOIP_CLIENT_ERR_IO;

    const doip_routing_act_res_t *res =
        (const doip_routing_act_res_t *)client->rx_payload;
    uint8_t act_code = res->activation_code;

    if (out_activation_code) *out_activation_code = act_code;

#if (DOIP_LOGICAL_ADDR_SIZE == 2)
    client->peer_ecu_la = ntohs(res->ecu_logical_address);
#else
    client->peer_ecu_la = ntohl(res->ecu_logical_address);
#endif

    if (act_code == DOIP_ACT_SUCCESS || act_code == DOIP_ACT_ALREADY_ACTIVE) {
        client->state = DOIP_CLI_STATE_ACTIVATED;
        LOG_INFO(DOIP_LOG_MODULE_CLIENT,
                 "Routing activated (code=0x%02X, ecu_la=0x%04X)",
                 act_code, client->peer_ecu_la);
        return DOIP_CLIENT_OK;
    }

    LOG_WARN(DOIP_LOG_MODULE_CLIENT, "Routing activation rejected (code=0x%02X)", act_code);
    return DOIP_CLIENT_ERR_REJECTED;
}

/* ===== Diagnostics ===== */

DoIP_ClientStatus_t DoIP_Client_SendDiagnostic(DoIP_Client_t *client,
                                                uint16_t target_ecu_addr,
                                                const uint8_t *uds_data, uint16_t uds_len) {
    if (!client || !uds_data || uds_len == 0) return DOIP_CLIENT_ERR_PARAM;
    if (client->state != DOIP_CLI_STATE_ACTIVATED)  return DOIP_CLIENT_ERR_NOT_READY;
    if ((uint32_t)uds_len + 4U > DOIP_MAX_PAYLOAD_SIZE) return DOIP_CLIENT_ERR_PARAM;

    /* Payload layout: tester_la(2) | target_la(2) | uds_data */
    uint8_t payload[4U + DOIP_MAX_PAYLOAD_SIZE];
    payload[0] = (uint8_t)(client->tester_la >> 8);
    payload[1] = (uint8_t)client->tester_la;
    payload[2] = (uint8_t)(target_ecu_addr >> 8);
    payload[3] = (uint8_t)target_ecu_addr;
    memcpy(&payload[4], uds_data, uds_len);

    if (doip_send_frame(client->tcp_fd, DOIP_PT_DIAGNOSTIC_MSG,
                        payload, 4U + uds_len) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CLIENT, "Failed to send diagnostic message");
        return DOIP_CLIENT_ERR_IO;
    }

    LOG_DEBUG(DOIP_LOG_MODULE_CLIENT, "Sent UDS request (target=0x%04X, len=%u)",
              target_ecu_addr, uds_len);
    return DOIP_CLIENT_OK;
}

DoIP_ClientStatus_t DoIP_Client_RecvDiagnostic(DoIP_Client_t *client,
                                                uint8_t *resp_buf, uint16_t resp_buf_size,
                                                uint16_t *resp_len_out,
                                                uint16_t *src_addr_out) {
    if (!client || !resp_buf || !resp_len_out) return DOIP_CLIENT_ERR_PARAM;
    if (client->state != DOIP_CLI_STATE_ACTIVATED)  return DOIP_CLIENT_ERR_NOT_READY;

    /* Loop: consume DiagACK and Alive Checks transparently, return on DiagMsg */
    for (int i = 0; i < 3; i++) {
        doip_header_t rx_hdr;
        DoIP_ClientStatus_t rc = client_recv_frame(client->tcp_fd,
                                                    client->config.response_timeout_ms,
                                                    &rx_hdr, client->rx_payload);
        if (rc != DOIP_CLIENT_OK) return rc;

        switch (rx_hdr.payload_type) {
            case DOIP_PT_DIAGNOSTIC_ACK:
                LOG_DEBUG(DOIP_LOG_MODULE_CLIENT, "Received DiagACK — waiting for response");
                continue;

            case DOIP_PT_ALIVE_CHECK_REQ:
                doip_send_frame(client->tcp_fd, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
                LOG_DEBUG(DOIP_LOG_MODULE_CLIENT, "Alive Check — responded");
                continue;

            case DOIP_PT_DIAGNOSTIC_MSG: {
                if (rx_hdr.payload_length < 4U) return DOIP_CLIENT_ERR_IO;
                uint16_t uds_len = (uint16_t)(rx_hdr.payload_length - 4U);
                if (uds_len > resp_buf_size) return DOIP_CLIENT_ERR_IO;
                if (src_addr_out)
                    *src_addr_out = (uint16_t)((client->rx_payload[0] << 8) | client->rx_payload[1]);
                memcpy(resp_buf, &client->rx_payload[4], uds_len);
                *resp_len_out = uds_len;
                LOG_DEBUG(DOIP_LOG_MODULE_CLIENT, "Received UDS response (len=%u)", uds_len);
                return DOIP_CLIENT_OK;
            }

            default:
                LOG_WARN(DOIP_LOG_MODULE_CLIENT,
                         "Unexpected frame type 0x%04X during recv", rx_hdr.payload_type);
                return DOIP_CLIENT_ERR_IO;
        }
    }

    return DOIP_CLIENT_ERR_TIMEOUT;
}

DoIP_ClientStatus_t DoIP_Client_Transact(DoIP_Client_t *client,
                                          uint16_t target_ecu_addr,
                                          const uint8_t *req_data, uint16_t req_len,
                                          uint8_t *resp_buf, uint16_t resp_buf_size,
                                          uint16_t *resp_len_out) {
    DoIP_ClientStatus_t rc =
        DoIP_Client_SendDiagnostic(client, target_ecu_addr, req_data, req_len);
    if (rc != DOIP_CLIENT_OK) return rc;
    return DoIP_Client_RecvDiagnostic(client, resp_buf, resp_buf_size,
                                       resp_len_out, NULL);
}

/* ===== State Query ===== */

bool DoIP_Client_IsConnected(const DoIP_Client_t *client) {
    return client && client->tcp_fd >= 0 &&
           client->state >= DOIP_CLI_STATE_CONNECTED;
}

bool DoIP_Client_IsRoutingActive(const DoIP_Client_t *client) {
    return client && client->state == DOIP_CLI_STATE_ACTIVATED;
}
