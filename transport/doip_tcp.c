#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
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
static doip_client_t g_client = { .fd = -1, .state = DOIP_TCP_STATE_IDLE };

static uint32_t get_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int doip_tcp_init(void) {
    g_tcp_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_srv_fd < 0) { LOG_ERROR(DOIP_LOG_MODULE_TCP, "socket failed"); return -1; }
    int opt = 1;
    setsockopt(g_tcp_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_tcp_srv_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    int flags = fcntl(g_tcp_srv_fd, F_GETFL, 0);
    fcntl(g_tcp_srv_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DOIP_TCP_PORT) };
    if (bind(g_tcp_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind failed"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -2;
    }
    if (listen(g_tcp_srv_fd, 1) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "listen failed"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -3;
    }
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server listening on port %d", DOIP_TCP_PORT);
    return 0;
}

int doip_tcp_poll(DoIP_RxIndication rx_cb) {
    (void)rx_cb;
    if (g_client.fd < 0) {
        fd_set readfds; struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
        FD_ZERO(&readfds); FD_SET(g_tcp_srv_fd, &readfds);
        if (select(g_tcp_srv_fd + 1, &readfds, NULL, NULL, &tv) > 0 && FD_ISSET(g_tcp_srv_fd, &readfds)) {
            struct sockaddr_in ca; socklen_t al = sizeof(ca);
            int new_fd = accept(g_tcp_srv_fd, (struct sockaddr*)&ca, &al);
            if (new_fd >= 0) {
                LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: %s:%d", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
                g_client.fd = new_fd; g_client.state = DOIP_TCP_STATE_CONNECTED; g_client.last_activity_ms = get_time_ms();
                struct timeval tv_c = { .tv_sec = 0, .tv_usec = 20000 };
                setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_c, sizeof(tv_c));
            }
        }
        return 0;
    }

    uint8_t rx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    ssize_t n = doip_recv_n(g_client.fd, rx_buf, DOIP_HEADER_SIZE);
    if (n == -2) return 0;
    if (n != DOIP_HEADER_SIZE) goto close_client;

    doip_header_t hdr;
    if (doip_deserialize_header(rx_buf, n, &hdr) < 0) {
        DoIP_Fsm_OnMalformedPacket(); /* Negative scenario tracking */
        LOG_WARN(DOIP_LOG_MODULE_TCP, "Invalid header"); goto close_client;
    }
    g_client.last_activity_ms = get_time_ms();

    if (hdr.payload_length > 0) {
        if (hdr.payload_length > DOIP_MAX_PAYLOAD_SIZE) {
            LOG_WARN(DOIP_LOG_MODULE_TCP, "Payload too large: %u", hdr.payload_length); goto close_client;
        }
        n = doip_recv_n(g_client.fd, rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);
        if (n == -2) return 0;
        if (n != (ssize_t)hdr.payload_length) {
            LOG_WARN(DOIP_LOG_MODULE_TCP, "Incomplete payload"); goto close_client;
        }
    }

    uint8_t *pl = rx_buf + DOIP_HEADER_SIZE;
    switch (hdr.payload_type) {
        case DOIP_PT_ROUTING_ACT_REQ: {
            if (hdr.payload_length < sizeof(doip_routing_act_req_t)) {
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Routing Act payload too short"); goto close_client;
            }
            const doip_routing_act_req_t *req = (const doip_routing_act_req_t *)pl;
            doip_logical_addr_t tester_addr;
            #if (DOIP_LOGICAL_ADDR_SIZE == 2)
                tester_addr = ntohs(req->tester_logical_address);
            #else
                tester_addr = ntohl(req->tester_logical_address);
            #endif
            uint8_t act_type = req->activation_type;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Routing Act Req: Tester=0x%04X, Type=0x%02X", tester_addr, act_type);

            uint8_t resp_code = DOIP_ACT_SUCCESS;
            if (g_client.state == DOIP_TCP_STATE_ACTIVATED) resp_code = DOIP_ACT_ALREADY_ACTIVE;
            else if (act_type != 0x00 && act_type != 0x01) resp_code = DOIP_ACT_REJECTED_UNKNOWN;

            doip_routing_act_res_t res;
            memset(&res, 0, sizeof(res));
            #if (DOIP_LOGICAL_ADDR_SIZE == 2)
                res.ecu_logical_address = htons(DOIP_ECU_LOGICAL_ADDRESS);
                res.tester_logical_address = req->tester_logical_address;
            #else
                res.ecu_logical_address = htonl(DOIP_ECU_LOGICAL_ADDRESS);
                res.tester_logical_address = req->tester_logical_address;
            #endif
            res.activation_code = resp_code;
            
            if (doip_send_frame(g_client.fd, DOIP_PT_ROUTING_ACT_RES, &res, sizeof(res)) < 0) {
                LOG_ERROR(DOIP_LOG_MODULE_TCP, "Failed to send Routing Act Response");
                goto close_client;
            }

            if (resp_code == DOIP_ACT_SUCCESS) {
                g_client.state = DOIP_TCP_STATE_ACTIVATED;
                g_client.tester_logical_addr = tester_addr;
                DoIP_Fsm_OnRoutingActivation(true); /* Hook FSM */
                LOG_INFO(DOIP_LOG_MODULE_TCP, "✅ Routing Activated.");
            } else {
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Routing Act Rejected: Code 0x%02X", resp_code);
            }
            break;
        }
        case DOIP_PT_DIAGNOSTIC_MSG: {
            if (hdr.payload_length < 4) {
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Diagnostic request too short"); break;
            }
            uint16_t tester_addr = ntohs(*(uint16_t*)pl);
            uint16_t ecu_addr = ntohs(*(uint16_t*)(pl + 2));
            uint8_t *uds_req = pl + 4;
            uint16_t uds_req_len = hdr.payload_length - 4;
            
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Diagnostic Req: Tester=0x%04X, ECU=0x%04X, UDS_Len=%u", tester_addr, ecu_addr, uds_req_len);
            DoIP_Fsm_OnDiagnosticActivity(); /* Refresh S3 timer */
            
            uint8_t uds_response[DOIP_MAX_PAYLOAD_SIZE];
            uint16_t uds_res_len = 0;
            
            (void)doip_uds_process_request(uds_req, uds_req_len, uds_response, &uds_res_len);
            
            if (uds_res_len > 0) {
                /* Step 1: Send ACK */
                uint8_t ack_payload[5];
                *(uint16_t*)&ack_payload[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t*)&ack_payload[2] = htons(tester_addr);
                ack_payload[4] = 0x00;
                
                doip_send_frame(g_client.fd, DOIP_PT_DIAGNOSTIC_ACK, ack_payload, sizeof(ack_payload));
                LOG_DEBUG(DOIP_LOG_MODULE_TCP, ">>> Sent Diagnostic ACK (Code: 0x00)");
                
                /* Step 2: Send Response */
                uint8_t diag_resp[4 + DOIP_MAX_PAYLOAD_SIZE];
                *(uint16_t*)&diag_resp[0] = htons(DOIP_ECU_LOGICAL_ADDRESS);
                *(uint16_t*)&diag_resp[2] = htons(tester_addr);
                memcpy(&diag_resp[4], uds_response, uds_res_len);
                
                uint16_t total_len = 4 + uds_res_len;
                doip_send_frame(g_client.fd, DOIP_PT_DIAGNOSTIC_MSG, diag_resp, total_len);
                LOG_DEBUG(DOIP_LOG_MODULE_TCP, ">>> Sent Diagnostic Response (Len=%u)", total_len);
            } else {
                LOG_WARN(DOIP_LOG_MODULE_TCP, "UDS processing failed or no response");
            }
            break;
        }
        case DOIP_PT_ALIVE_CHECK_REQ:
            LOG_DEBUG(DOIP_LOG_MODULE_TCP, "Alive Check Received");
            doip_send_frame(g_client.fd, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
            break;
        default:
            LOG_WARN(DOIP_LOG_MODULE_TCP, "Unknown payload type: 0x%04X", hdr.payload_type);
            break;
    }
    return 0;

close_client:
    DoIP_Fsm_OnTcpDisconnect(); /* Notify FSM */
    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client disconnected");
    close(g_client.fd); g_client.fd = -1; g_client.state = DOIP_TCP_STATE_IDLE;
    return 0;
}

void doip_tcp_deinit(void) {
    if (g_client.fd >= 0) close(g_client.fd);
    if (g_tcp_srv_fd >= 0) close(g_tcp_srv_fd);
    g_tcp_srv_fd = -1; g_client.fd = -1; g_client.state = DOIP_TCP_STATE_IDLE;
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server stopped");
}