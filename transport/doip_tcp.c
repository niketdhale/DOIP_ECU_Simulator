/**
 * @file doip_tcp.c
 * @brief DoIP TCP server — routing activation, diagnostic messaging, alive check.
 *
 * Changes from baseline:
 *  - I/O abstraction: all per-client read/write goes through doip_io_t vtable.
 *  - Non-blocking partial-frame reassembly buffer per client; SO_RCVTIMEO removed.
 *  - IPv6 dual-stack socket when DOIP_ENABLE_IPV6=true.
 *  - ISO-TP framing hook when DOIP_ENABLE_ISO_TP=true.
 *  - TLS accept path when DOIP_ENABLE_TLS=true.
 *  - Async event callbacks (connect / disconnect / frame-received).
 */

#include "transport/doip_tcp.h"
#include "transport/doip_io.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#if DOIP_ENABLE_TLS
#include "transport/doip_tls.h"
static doip_tls_server_t g_tls_server;

/* TLS config — set via doip_tcp_tls_config() before doip_tcp_init() */
static const char *g_tls_cert   = NULL;
static const char *g_tls_key    = NULL;
static const char *g_tls_ca     = NULL;
static bool        g_tls_verify = false;

void doip_tcp_tls_config(const char *cert_file, const char *key_file,
                          const char *ca_file,   bool verify_peer)
{
    g_tls_cert   = cert_file;
    g_tls_key    = key_file;
    g_tls_ca     = ca_file;
    g_tls_verify = verify_peer;
}
#endif

static int g_tcp_srv_fd = -1;
static doip_client_t g_clients[DOIP_MAX_TCP_CLIENTS] = {0};

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static uint32_t get_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief Send a DoIP frame through the client's I/O vtable.
 *
 * Builds the wire buffer (header + payload) and writes it via io->write().
 * Returns total bytes sent on success, -1 on error.
 */
static ssize_t client_send_frame(doip_client_t *c, uint16_t ptype,
                                  const void *payload, uint32_t plen)
{
    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr;
    hdr.protocol_version = DOIP_PROTOCOL_VERSION;
    hdr.inverse_version  = DOIP_INVERSE_VERSION;
    hdr.payload_type     = ptype;
    hdr.payload_length   = plen;
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) {
        if (plen > DOIP_MAX_PAYLOAD_SIZE) return -1;
        memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);
    }
    size_t total = DOIP_HEADER_SIZE + plen;
    size_t sent  = 0;
    while (sent < total) {
        ssize_t n = c->io.write(c->io.ctx, tx_buf + sent, total - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static doip_client_t *find_free_client(void) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            memset(&g_clients[i], 0, sizeof(doip_client_t));
            g_clients[i].in_use = true;
            return &g_clients[i];
        }
    }
    return NULL;
}

static void close_client(doip_client_t *client) {
    if (!client || !client->in_use) return;
    int fd = client->fd;
    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client disconnected (fd=%d, ip=%s)",
             fd, client->peer_ip[0] ? client->peer_ip : "?");

    /* Notify application before releasing the fd */
    if (g_doip_client_disconnect_cb)
        g_doip_client_disconnect_cb(fd, g_doip_async_user_ctx);

    client->io.close(client->io.ctx);
    memset(client, 0, sizeof(doip_client_t));
}

/* ---------------------------------------------------------------------------
 * Frame processing — non-blocking partial reassembly
 * -------------------------------------------------------------------------*/

/**
 * Accumulate available bytes into client->rx_buf, then attempt to process
 * a complete frame.  Returns 0 when waiting for more data, -1 on error or
 * when the caller should close the client.
 */
static int process_client_frame(doip_client_t *client)
{
    /* --- Step 1: Read whatever is available into the tail of rx_buf --- */
    size_t space = sizeof(client->rx_buf) - client->rx_bytes;
    if (space == 0) {
        /* Buffer full with no complete frame → protocol error */
        LOG_WARN(DOIP_LOG_MODULE_TCP, "Rx buffer overflow for fd=%d", client->fd);
        return -1;
    }

    ssize_t n = client->io.read(client->io.ctx,
                                 client->rx_buf + client->rx_bytes, space);
    if (n == 0) {
        /* Graceful peer close */
        LOG_INFO(DOIP_LOG_MODULE_TCP, "Peer closed connection (fd=%d)", client->fd);
        return -1;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; /* No data yet */
        LOG_WARN(DOIP_LOG_MODULE_TCP, "recv error fd=%d: %s", client->fd, strerror(errno));
        return -1;
    }
    client->rx_bytes += (size_t)n;

    /* --- Step 2: Wait until we have at least a complete header --- */
    if (client->rx_bytes < DOIP_HEADER_SIZE) return 0;

    /* --- Step 3: Parse header and wait for full payload --- */
    doip_header_t hdr;
    if (doip_deserialize_header(client->rx_buf, client->rx_bytes, &hdr) < 0) {
        LOG_WARN(DOIP_LOG_MODULE_TCP,
                 "Invalid header from fd=%d — sending NACK", client->fd);
        uint8_t nack = (uint8_t)DOIP_NACK_INVALID_PATTERN;
        client_send_frame(client, DOIP_PT_GENERIC_NACK, &nack, 1);
        return -1;
    }

    size_t frame_total = DOIP_HEADER_SIZE + hdr.payload_length;
    if (client->rx_bytes < frame_total) return 0; /* Still waiting */

    if (hdr.payload_length > DOIP_MAX_PAYLOAD_SIZE) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "Payload too large from fd=%d", client->fd);
        return -1;
    }

    /* --- Step 4: We have a complete frame — update activity timestamp --- */
    client->last_activity_ms  = get_time_ms();
    client->alive_check_sent_ms = 0; /* Any valid frame resets pending probe */

    uint8_t *pl = client->rx_buf + DOIP_HEADER_SIZE;

    /* --- Step 5: Optional application frame intercept hook --- */
    if (g_doip_frame_received_cb) {
        int rc = g_doip_frame_received_cb(client->fd, hdr.payload_type,
                                           pl, hdr.payload_length,
                                           g_doip_async_user_ctx);
        if (rc == 0) {
            /* Application handled it — consume bytes and return */
            client->rx_bytes = 0;
            return 0;
        }
    }

    /* --- Step 6: Built-in dispatch --- */
    switch (hdr.payload_type) {
        case DOIP_PT_ROUTING_ACT_REQ: {
            if (hdr.payload_length < sizeof(doip_routing_act_req_t)) goto frame_done;
            const doip_routing_act_req_t *req = (const doip_routing_act_req_t *)pl;
            doip_logical_addr_t tester_addr;
#if (DOIP_LOGICAL_ADDR_SIZE == 2)
            tester_addr = ntohs(req->tester_logical_address);
#else
            tester_addr = ntohl(req->tester_logical_address);
#endif
            uint8_t act_type = req->activation_type;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Routing Act Req (fd=%d): Tester=0x%04X, Type=0x%02X",
                      client->fd, tester_addr, act_type);

            uint8_t resp_code = DOIP_ACT_SUCCESS;
            if (client->state == DOIP_TCP_STATE_ACTIVATED)
                resp_code = DOIP_ACT_ALREADY_ACTIVE;
            else if (act_type != 0x00 && act_type != 0x01)
                resp_code = DOIP_ACT_REJECTED_UNKNOWN;

            doip_routing_act_res_t res; memset(&res, 0, sizeof(res));
#if (DOIP_LOGICAL_ADDR_SIZE == 2)
            res.ecu_logical_address    = htons(DOIP_ECU_LOGICAL_ADDRESS);
            res.tester_logical_address = req->tester_logical_address;
#else
            res.ecu_logical_address    = htonl(DOIP_ECU_LOGICAL_ADDRESS);
            res.tester_logical_address = req->tester_logical_address;
#endif
            res.activation_code = resp_code;
            if (client_send_frame(client, DOIP_PT_ROUTING_ACT_RES,
                                   &res, sizeof(res)) < 0) {
                client->rx_bytes = 0;
                return -1;
            }
            if (resp_code == DOIP_ACT_SUCCESS) {
                client->state              = DOIP_TCP_STATE_ACTIVATED;
                client->tester_logical_addr = tester_addr;
                LOG_INFO(DOIP_LOG_MODULE_TCP,
                         "Routing Activated for client fd=%d", client->fd);
            }
            break;
        }

        case DOIP_PT_DIAGNOSTIC_MSG: {
            if (hdr.payload_length < 4) break;
            uint16_t tester_addr = ntohs(*(uint16_t *)pl);
            uint8_t *uds_req     = pl + 4;
            uint16_t uds_req_len = (uint16_t)(hdr.payload_length - 4);

            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Diagnostic Req (fd=%d): Tester=0x%04X, UDS_Len=%u",
                      client->fd, tester_addr, uds_req_len);

#if DOIP_ENABLE_ISO_TP
            /* ISO-TP reassembly: feed the DoIP payload, get UDS bytes out */
            uint8_t isotp_buf[4095];
            int rlen = doip_isotp_rx(&client->isotp_ctx, uds_req, uds_req_len,
                                      isotp_buf, sizeof(isotp_buf));
            if (rlen == 0)  break;  /* Waiting for more consecutive frames  */
            if (rlen <  0) { client->rx_bytes = 0; return -1; } /* ISO-TP error */
            uds_req     = isotp_buf;
            uds_req_len = (uint16_t)rlen;
#endif

            uint8_t  uds_response[DOIP_MAX_PAYLOAD_SIZE];
            uint16_t uds_res_len = 0;
            (void)doip_uds_process_request(&client->uds_ctx, uds_req, uds_req_len,
                                            uds_response, &uds_res_len);

            if (uds_res_len > 0) {
                uint8_t ack_payload[5];
                *(uint16_t *)&ack_payload[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t *)&ack_payload[2] = htons(tester_addr);
                ack_payload[4] = 0x00;
                client_send_frame(client, DOIP_PT_DIAGNOSTIC_ACK,
                                   ack_payload, sizeof(ack_payload));

                uint8_t diag_resp[4 + DOIP_MAX_PAYLOAD_SIZE];
                *(uint16_t *)&diag_resp[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t *)&diag_resp[2] = htons(tester_addr);
                memcpy(&diag_resp[4], uds_response, uds_res_len);
                client_send_frame(client, DOIP_PT_DIAGNOSTIC_MSG,
                                   diag_resp, 4 + uds_res_len);
            }
            break;
        }

        case DOIP_PT_ALIVE_CHECK_REQ:
            /* Client probing us — respond immediately */
            client_send_frame(client, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
            break;

        case DOIP_PT_ALIVE_CHECK_RES:
            /* Client responded to our server-initiated probe — clear pending flag */
            client->alive_check_sent_ms = 0;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Alive Check response from fd=%d", client->fd);
            break;

        default: {
            uint8_t nack = (uint8_t)DOIP_NACK_UNKNOWN_PT;
            client_send_frame(client, DOIP_PT_GENERIC_NACK, &nack, 1);
            LOG_WARN(DOIP_LOG_MODULE_TCP,
                     "Unknown PT=0x%04X from fd=%d — NACK sent",
                     hdr.payload_type, client->fd);
            break;
        }
    }

frame_done:
    /* Consume the processed frame; shift any trailing bytes to front */
    if (client->rx_bytes > frame_total) {
        memmove(client->rx_buf, client->rx_buf + frame_total,
                client->rx_bytes - frame_total);
        client->rx_bytes -= frame_total;
    } else {
        client->rx_bytes = 0;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int doip_tcp_init(void) {
#if DOIP_ENABLE_IPV6
    g_tcp_srv_fd = socket(AF_INET6, SOCK_STREAM, 0);
#else
    g_tcp_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (g_tcp_srv_fd < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "socket failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(g_tcp_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_tcp_srv_fd, IPPROTO_TCP, TCP_NODELAY,  &opt, sizeof(opt));
    fcntl(g_tcp_srv_fd, F_SETFL,
          fcntl(g_tcp_srv_fd, F_GETFL, 0) | O_NONBLOCK);

#if DOIP_ENABLE_IPV6
    /* Dual-stack: accept both IPv4-mapped and native IPv6 */
    int v6only = 0;
    setsockopt(g_tcp_srv_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr   = in6addr_any;
    addr6.sin6_port   = htons(DOIP_TCP_PORT);
    if (bind(g_tcp_srv_fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind (IPv6) failed: %s", strerror(errno));
        close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -2;
    }
#else
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(DOIP_TCP_PORT);
    if (bind(g_tcp_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind failed: %s", strerror(errno));
        close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -2;
    }
#endif

    if (listen(g_tcp_srv_fd, DOIP_MAX_TCP_CLIENTS) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "listen failed: %s", strerror(errno));
        close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -3;
    }

#if DOIP_ENABLE_TLS
    if (!g_tls_cert || !g_tls_key) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP,
                  "TLS enabled but cert/key not set — call doip_tcp_tls_config() first");
        close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -4;
    }
    if (doip_tls_init(&g_tls_server, g_tls_cert, g_tls_key,
                       g_tls_ca, g_tls_verify) != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "TLS context initialisation failed");
        close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -5;
    }
#endif

    LOG_INFO(DOIP_LOG_MODULE_TCP,
             "TCP server listening on port %d (max %d clients, IPv6=%s, TLS=%s)",
             DOIP_TCP_PORT, DOIP_MAX_TCP_CLIENTS,
             DOIP_ENABLE_IPV6 ? "yes" : "no",
             DOIP_ENABLE_TLS  ? "yes" : "no");
    return 0;
}

int doip_tcp_poll(DoIP_RxIndication rx_cb) {
    (void)rx_cb;
    if (g_tcp_srv_fd < 0) return -1;

    fd_set readfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    FD_ZERO(&readfds);
    FD_SET(g_tcp_srv_fd, &readfds);
    int max_fd = g_tcp_srv_fd;

    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (g_clients[i].in_use) {
            int fd = g_clients[i].io.getfd(g_clients[i].io.ctx);
            FD_SET(fd, &readfds);
            if (fd > max_fd) max_fd = fd;
        }
    }

    if (select(max_fd + 1, &readfds, NULL, NULL, &tv) < 0) return -1;

    /* --- Accept new connections --- */
    if (FD_ISSET(g_tcp_srv_fd, &readfds)) {
#if DOIP_ENABLE_IPV6
        struct sockaddr_in6 ca;  socklen_t al = sizeof(ca);
        int new_fd = accept(g_tcp_srv_fd, (struct sockaddr *)&ca, &al);
        if (new_fd >= 0) {
            doip_client_t *client = find_free_client();
            if (client) {
                /* Set non-blocking so partial reads return immediately */
                fcntl(new_fd, F_SETFL,
                      fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);
                client->fd    = new_fd;
                client->state = DOIP_TCP_STATE_CONNECTED;
                client->last_activity_ms = get_time_ms();
                client->uds_ctx.current_session  = UDS_ECU_DEFAULT_SESSION;
                client->uds_ctx.last_activity_ms = get_time_ms();
                client->rx_bytes = 0;
#if DOIP_ENABLE_TLS
                client->io = doip_tls_accept(&g_tls_server, new_fd);
#else
                client->io = doip_io_plain(new_fd);
#endif
                inet_ntop(AF_INET6, &ca.sin6_addr,
                          client->peer_ip, sizeof(client->peer_ip));
                LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: [%s]:%d (fd=%d)",
                         client->peer_ip, ntohs(ca.sin6_port), new_fd);
                if (g_doip_client_connect_cb)
                    g_doip_client_connect_cb(new_fd, g_doip_async_user_ctx);
            } else {
                close(new_fd);
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Max clients reached");
            }
        }
#else  /* IPv4 */
        struct sockaddr_in ca;  socklen_t al = sizeof(ca);
        int new_fd = accept(g_tcp_srv_fd, (struct sockaddr *)&ca, &al);
        if (new_fd >= 0) {
            doip_client_t *client = find_free_client();
            if (client) {
                fcntl(new_fd, F_SETFL,
                      fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);
                client->fd    = new_fd;
                client->state = DOIP_TCP_STATE_CONNECTED;
                client->last_activity_ms = get_time_ms();
                client->uds_ctx.current_session  = UDS_ECU_DEFAULT_SESSION;
                client->uds_ctx.last_activity_ms = get_time_ms();
                client->rx_bytes = 0;
#if DOIP_ENABLE_TLS
                client->io = doip_tls_accept(&g_tls_server, new_fd);
#else
                client->io = doip_io_plain(new_fd);
#endif
                inet_ntop(AF_INET, &ca.sin_addr,
                          client->peer_ip, sizeof(client->peer_ip));
                LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: %s:%d (fd=%d)",
                         client->peer_ip, ntohs(ca.sin_port), new_fd);
                if (g_doip_client_connect_cb)
                    g_doip_client_connect_cb(new_fd, g_doip_async_user_ctx);
            } else {
                close(new_fd);
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Max clients reached");
            }
        }
#endif /* DOIP_ENABLE_IPV6 */
    }

    /* --- Service readable clients --- */
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (!g_clients[i].in_use) continue;
        int fd = g_clients[i].io.getfd(g_clients[i].io.ctx);
        if (FD_ISSET(fd, &readfds)) {
            if (process_client_frame(&g_clients[i]) < 0)
                close_client(&g_clients[i]);
        }
    }

    /* NOTE: Per-client idle timeout / alive-check disconnection is handled entirely
     * by doip_tcp_tick(), called from DoIP_Fsm_MainFunction() after this poll.
     * Do not add a second S3 timeout here — it races with the alive-check probe. */
    return 0;
}

void doip_tcp_tick(uint32_t now_ms) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        doip_client_t *c = &g_clients[i];
        if (!c->in_use || c->state != DOIP_TCP_STATE_ACTIVATED) continue;

        if (c->alive_check_sent_ms != 0) {
            /* Alive Check already pending — check for timeout */
            if ((now_ms - c->alive_check_sent_ms) > DOIP_ALIVE_CHECK_TIMEOUT_MS) {
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "Alive Check timeout for fd=%d — disconnecting", c->fd);
                close_client(c);
            }
        } else if ((now_ms - c->last_activity_ms) > DOIP_ALIVE_CHECK_INTERVAL_MS) {
            /* Client idle too long — probe with Alive Check Request */
            if (client_send_frame(c, DOIP_PT_ALIVE_CHECK_REQ, NULL, 0) > 0) {
                c->alive_check_sent_ms = now_ms;
                LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                          "Alive Check Request sent to fd=%d", c->fd);
            }
        }
    }
}

uint8_t doip_tcp_get_client_count(void) {
    uint8_t count = 0;
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++)
        if (g_clients[i].in_use) count++;
    return count;
}

void doip_tcp_deinit(void) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++)
        if (g_clients[i].in_use) close_client(&g_clients[i]);
    if (g_tcp_srv_fd >= 0) { close(g_tcp_srv_fd); g_tcp_srv_fd = -1; }
#if DOIP_ENABLE_TLS
    doip_tls_deinit(&g_tls_server);
#endif
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server stopped");
}
