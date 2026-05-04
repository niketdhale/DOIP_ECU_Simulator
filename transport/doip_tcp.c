#include "transport/doip_tcp.h"
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

static int g_tcp_srv_fd = -1;
static doip_client_t g_clients[DOIP_MAX_TCP_CLIENTS] = {0};

static uint32_t get_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static doip_client_t* find_free_client(void) {
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
    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client disconnected (fd=%d)", client->fd);
    close(client->fd);
    memset(client, 0, sizeof(doip_client_t));
}

static int process_client_frame(doip_client_t *client) {
    uint8_t rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    ssize_t n = doip_recv_n(client->fd, rx_buf, DOIP_HEADER_SIZE);
    if (n == -2) return 0;
    if (n != DOIP_HEADER_SIZE) return -1;

    doip_header_t hdr;
    if (doip_deserialize_header(rx_buf, n, &hdr) < 0) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "Invalid header from client fd=%d — sending NACK", client->fd);
        uint8_t nack = (uint8_t)DOIP_NACK_INVALID_PATTERN;
        doip_send_frame(client->fd, DOIP_PT_GENERIC_NACK, &nack, 1);
        return -1;
    }
    client->last_activity_ms = get_time_ms();
    client->alive_check_sent_ms = 0;  /* Any valid frame resets pending alive check */

    if (hdr.payload_length > 0) {
        if (hdr.payload_length > DOIP_MAX_PAYLOAD_SIZE) { LOG_WARN(DOIP_LOG_MODULE_TCP, "Payload too large"); return -1; }
        n = doip_recv_n(client->fd, rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);
        if (n == -2) return 0;
        if (n != (ssize_t)hdr.payload_length) { LOG_WARN(DOIP_LOG_MODULE_TCP, "Incomplete payload"); return -1; }
    }

    uint8_t *pl = rx_buf + DOIP_HEADER_SIZE;
    switch (hdr.payload_type) {
        case DOIP_PT_ROUTING_ACT_REQ: {
            if (hdr.payload_length < sizeof(doip_routing_act_req_t)) return -1;
            const doip_routing_act_req_t *req = (const doip_routing_act_req_t *)pl;
            doip_logical_addr_t tester_addr;
#if (DOIP_LOGICAL_ADDR_SIZE == 2)
            tester_addr = ntohs(req->tester_logical_address);
#else
            tester_addr = ntohl(req->tester_logical_address);
#endif
            uint8_t act_type = req->activation_type;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Routing Act Req (fd=%d): Tester=0x%04X, Type=0x%02X", client->fd, tester_addr, act_type);

            uint8_t resp_code = DOIP_ACT_SUCCESS;
            if (client->state == DOIP_TCP_STATE_ACTIVATED) resp_code = DOIP_ACT_ALREADY_ACTIVE;
            else if (act_type != 0x00 && act_type != 0x01) resp_code = DOIP_ACT_REJECTED_UNKNOWN;

            doip_routing_act_res_t res; memset(&res, 0, sizeof(res));
#if (DOIP_LOGICAL_ADDR_SIZE == 2)
            res.ecu_logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS);
            res.tester_logical_address = req->tester_logical_address;
#else
            res.ecu_logical_address = htonl(DOIP_ECU_LOGICAL_ADDRESS);
            res.tester_logical_address = req->tester_logical_address;
#endif
            res.activation_code = resp_code;
            
            if (doip_send_frame(client->fd, DOIP_PT_ROUTING_ACT_RES, &res, sizeof(res)) < 0) return -1;

            if (resp_code == DOIP_ACT_SUCCESS) {
                client->state = DOIP_TCP_STATE_ACTIVATED;
                client->tester_logical_addr = tester_addr;
                LOG_INFO(DOIP_LOG_MODULE_TCP, "Routing Activated for client fd=%d", client->fd);
            }
            break;
        }
        case DOIP_PT_DIAGNOSTIC_MSG: {
            if (hdr.payload_length < 4) break;
            uint16_t tester_addr = ntohs(*(uint16_t*)pl);
            uint8_t *uds_req = pl + 4;
            uint16_t uds_req_len = hdr.payload_length - 4;
            
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Diagnostic Req (fd=%d): Tester=0x%04X, UDS_Len=%u", client->fd, tester_addr, uds_req_len);
            
            uint8_t uds_response[DOIP_MAX_PAYLOAD_SIZE];
            uint16_t uds_res_len = 0;
            
            /* Pass per-client UDS context */
            (void)doip_uds_process_request(&client->uds_ctx, uds_req, uds_req_len, uds_response, &uds_res_len);
            
            if (uds_res_len > 0) {
                uint8_t ack_payload[5];
                *(uint16_t*)&ack_payload[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t*)&ack_payload[2] = htons(tester_addr);
                ack_payload[4] = 0x00;
                doip_send_frame(client->fd, DOIP_PT_DIAGNOSTIC_ACK, ack_payload, sizeof(ack_payload));
                
                uint8_t diag_resp[4 + DOIP_MAX_PAYLOAD_SIZE];
                *(uint16_t*)&diag_resp[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t*)&diag_resp[2] = htons(tester_addr);
                memcpy(&diag_resp[4], uds_response, uds_res_len);
                doip_send_frame(client->fd, DOIP_PT_DIAGNOSTIC_MSG, diag_resp, 4 + uds_res_len);
            }
            break;
        }
        case DOIP_PT_ALIVE_CHECK_REQ:
            /* Client probing us — respond immediately */
            doip_send_frame(client->fd, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
            break;
        case DOIP_PT_ALIVE_CHECK_RES:
            /* Client responded to our server-initiated probe — clear pending flag */
            client->alive_check_sent_ms = 0;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Alive Check response from fd=%d", client->fd);
            break;
        default: {
            /* Unknown payload type — send Generic Header NACK (ISO 13400-2 §7.2) */
            uint8_t nack = (uint8_t)DOIP_NACK_UNKNOWN_PT;
            doip_send_frame(client->fd, DOIP_PT_GENERIC_NACK, &nack, 1);
            LOG_WARN(DOIP_LOG_MODULE_TCP, "Unknown PT=0x%04X from fd=%d — NACK sent",
                     hdr.payload_type, client->fd);
            break;
        }
    }
    return 0;
}

int doip_tcp_init(void) {
    g_tcp_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_srv_fd < 0) { LOG_ERROR(DOIP_LOG_MODULE_TCP, "socket failed"); return -1; }
    int opt = 1;
    setsockopt(g_tcp_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_tcp_srv_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    fcntl(g_tcp_srv_fd, F_SETFL, fcntl(g_tcp_srv_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DOIP_TCP_PORT) };
    if (bind(g_tcp_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind failed"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -2; }
    if (listen(g_tcp_srv_fd, DOIP_MAX_TCP_CLIENTS) < 0) { LOG_ERROR(DOIP_LOG_MODULE_TCP, "listen failed"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -3; }
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server listening on port %d (max %d clients)", DOIP_TCP_PORT, DOIP_MAX_TCP_CLIENTS);
    return 0;
}

int doip_tcp_poll(DoIP_RxIndication rx_cb) {
    (void)rx_cb;
    if (g_tcp_srv_fd < 0) return -1;

    fd_set readfds; struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    FD_ZERO(&readfds); FD_SET(g_tcp_srv_fd, &readfds);
    int max_fd = g_tcp_srv_fd;
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (g_clients[i].in_use) { FD_SET(g_clients[i].fd, &readfds); if (g_clients[i].fd > max_fd) max_fd = g_clients[i].fd; }
    }

    if (select(max_fd + 1, &readfds, NULL, NULL, &tv) < 0) return -1;

    if (FD_ISSET(g_tcp_srv_fd, &readfds)) {
        struct sockaddr_in ca; socklen_t al = sizeof(ca);
        int new_fd = accept(g_tcp_srv_fd, (struct sockaddr*)&ca, &al);
        if (new_fd >= 0) {
            doip_client_t *client = find_free_client();
            if (client) {
                client->fd = new_fd; client->state = DOIP_TCP_STATE_CONNECTED;
                client->last_activity_ms = get_time_ms();
                /* Initialize client UDS context */
                client->uds_ctx.current_session = UDS_ECU_DEFAULT_SESSION;
                client->uds_ctx.last_activity_ms = get_time_ms();
                
                setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &(struct timeval){.tv_usec=20000}, sizeof(struct timeval));
                LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: %s:%d (fd=%d)", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port), new_fd);
            } else { close(new_fd); LOG_WARN(DOIP_LOG_MODULE_TCP, "Max clients reached"); }
        }
    }

    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (g_clients[i].in_use && FD_ISSET(g_clients[i].fd, &readfds)) {
            if (process_client_frame(&g_clients[i]) < 0) close_client(&g_clients[i]);
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
            /* Client idle too long — send an Alive Check Request */
            if (doip_send_frame(c->fd, DOIP_PT_ALIVE_CHECK_REQ, NULL, 0) == 0) {
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
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) if (g_clients[i].in_use) close_client(&g_clients[i]);
    if (g_tcp_srv_fd >= 0) { close(g_tcp_srv_fd); g_tcp_srv_fd = -1; }
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server stopped");
}