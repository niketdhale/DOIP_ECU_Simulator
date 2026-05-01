#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
#include "core/doip_frame.h"
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
    if (g_tcp_srv_fd < 0) { perror("tcp socket"); return -1; }
    int opt = 1;
    setsockopt(g_tcp_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_tcp_srv_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    int flags = fcntl(g_tcp_srv_fd, F_GETFL, 0);
    fcntl(g_tcp_srv_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DOIP_TCP_PORT) };
    if (bind(g_tcp_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("tcp bind"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -2; }
    if (listen(g_tcp_srv_fd, 1) < 0) { perror("tcp listen"); close(g_tcp_srv_fd); g_tcp_srv_fd = -1; return -3; }
    printf("TCP DoIP server listening on port %d\n", DOIP_TCP_PORT);
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
                printf("[TCP] Client connected: %s:%d\n", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
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
    if (doip_deserialize_header(rx_buf, n, &hdr) < 0) goto close_client;
    g_client.last_activity_ms = get_time_ms();

    if (hdr.payload_length > 0) {
        if (hdr.payload_length > DOIP_MAX_PAYLOAD_SIZE) goto close_client;
        n = doip_recv_n(g_client.fd, rx_buf + DOIP_HEADER_SIZE, hdr.payload_length);
        if (n == -2) return 0;
        if (n != (ssize_t)hdr.payload_length) goto close_client;
    }

    uint8_t *pl = rx_buf + DOIP_HEADER_SIZE;
    switch (hdr.payload_type) {
        case DOIP_PT_ROUTING_ACT_REQ: {
            if (hdr.payload_length < sizeof(doip_routing_act_req_t)) goto close_client;
            const doip_routing_act_req_t *req = (const doip_routing_act_req_t *)pl;
            
            doip_logical_addr_t tester_addr;
            #if (DOIP_LOGICAL_ADDR_SIZE == 2)
                tester_addr = ntohs(req->tester_logical_address);
            #else
                tester_addr = ntohl(req->tester_logical_address);
            #endif
            
            uint8_t act_type = req->activation_type;
            printf("[TCP] Routing Act Req: Tester=0x%04X, Type=0x%02X\n", tester_addr, act_type);

            uint8_t resp_code = DOIP_ACT_SUCCESS;
            if (g_client.state == DOIP_TCP_STATE_ACTIVATED) resp_code = DOIP_ACT_ALREADY_ACTIVE;
            else if (act_type != 0x00 && act_type != 0x01) resp_code = DOIP_ACT_REJECTED_UNKNOWN;

            /* Build Response with 4-byte reserved field */
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
            res.reserved = 0x00000000; /* Explicit 4-byte reserved */
            
            doip_send_frame(g_client.fd, DOIP_PT_ROUTING_ACT_RES, &res, sizeof(res));

            if (resp_code == DOIP_ACT_SUCCESS) {
                g_client.state = DOIP_TCP_STATE_ACTIVATED;
                g_client.tester_logical_addr = tester_addr;
                printf("[TCP] ✅ Routing Activated.\n");
            }
            break;
        }
        case DOIP_PT_DIAGNOSTIC_MSG: {
            /* Diagnostic Request payload structure:
             * [Source LA: 2B] [Target LA: 2B] [UDS Data: N bytes]
             */
            if (hdr.payload_length < 4) {
                fprintf(stderr, "[TCP] Diagnostic request too short\n");
                break;
            }
            
            /* Extract addresses */
            uint16_t tester_addr = ntohs(*(uint16_t*)pl);
            uint16_t ecu_addr = ntohs(*(uint16_t*)(pl + 2));
            
            /* UDS payload starts after 4-byte address header */
            uint8_t *uds_req = pl + 4;
            uint16_t uds_req_len = hdr.payload_length - 4;
            
            printf("[TCP] Diagnostic Req: Tester=0x%04X, ECU=0x%04X, UDS_Len=%u\n", 
                   tester_addr, ecu_addr, uds_req_len);
            
            /* Process UDS request */
            uint8_t uds_response[DOIP_MAX_PAYLOAD_SIZE];
            uint16_t uds_res_len = 0;
            
            /* Call UDS processor */
            int ret = doip_uds_process_request(uds_req, uds_req_len, 
                                               uds_response, &uds_res_len);
            
            if (ret == 0 && uds_res_len > 0) {
                /* Step 1: Send Diagnostic ACK (0x8002) */
                /* Format: [Src LA: 2B] [Dst LA: 2B] [ACK Code: 1B] */
                uint8_t ack_payload[5];  
                *(uint16_t*)&ack_payload[0] = htons(DOIP_ECU_LOGICAL_ADDRESS); /* Source = ECU */
                *(uint16_t*)&ack_payload[2] = htons(tester_addr);              /* Target = Tester */
                ack_payload[4] = 0x00;  /* ACK Code: 0x00 (Positive ACK) */
                
                doip_send_frame(g_client.fd, DOIP_PT_DIAGNOSTIC_ACK, 
                               ack_payload, sizeof(ack_payload));
                printf("[TCP] >>> Sent Diagnostic ACK (Code: 0x00)\n");
                
                /* Step 2: Send Diagnostic Response (0x8001) */
                /* Format: [Src LA: 2B] [Dst LA: 2B] [UDS Data: N bytes] */
                uint8_t diag_resp[4 + DOIP_MAX_PAYLOAD_SIZE];
                
                *(uint16_t*)&diag_resp[0] = htons(DOIP_ECU_LOGICAL_ADDRESS); /* Source = ECU */
                *(uint16_t*)&diag_resp[2] = htons(tester_addr);              /* Target = Tester */
                memcpy(&diag_resp[4], uds_response, uds_res_len);            /* UDS Payload */
                
                uint16_t total_len = 4 + uds_res_len;
                
                doip_send_frame(g_client.fd, DOIP_PT_DIAGNOSTIC_MSG, 
                               diag_resp, total_len);
                printf("[TCP] >>> Sent Diagnostic Response (Len=%u)\n", total_len);
            } else {
                printf("[TCP] UDS processing failed or no response\n");
            }
            break;
        }
        case DOIP_PT_ALIVE_CHECK_REQ:
            printf("[TCP] Alive Check\n");
            doip_send_frame(g_client.fd, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
            break;
        default:
            fprintf(stderr, "[TCP] Unknown PT: 0x%04X\n", hdr.payload_type);
            break;
    }
    return 0;

close_client:
    close(g_client.fd); g_client.fd = -1; g_client.state = DOIP_TCP_STATE_IDLE;
    printf("[TCP] Client disconnected\n");
    return 0;
}

void doip_tcp_deinit(void) {
    if (g_client.fd >= 0) close(g_client.fd);
    if (g_tcp_srv_fd >= 0) close(g_tcp_srv_fd);
    g_tcp_srv_fd = -1; g_client.fd = -1; g_client.state = DOIP_TCP_STATE_IDLE;
}