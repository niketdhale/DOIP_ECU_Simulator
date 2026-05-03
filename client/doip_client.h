#ifndef DOIP_CLIENT_H
#define DOIP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include "config/doip_config.h"
#include "config/doip_uds_config.h"

/* Forward declaration — internals live in doip_client.c */
typedef struct DoIP_ClientContext DoIP_Client_t;

/* ===== Client Configuration ===== */
typedef struct {
    uint16_t tester_logical_addr;   /* Our source address.  Default: DOIP_TESTER_LOGICAL_ADDRESS */
    uint32_t connect_timeout_ms;    /* TCP connect / socket rx timeout. Default: DOIP_RX_TIMEOUT_MS */
    uint32_t response_timeout_ms;   /* Wait for UDS response.          Default: DOIP_RX_TIMEOUT_MS */
    uint32_t routing_timeout_ms;    /* Wait for routing activation.    Default: DOIP_ROUTING_ACTIVATION_TIMEOUT_MS */
} DoIP_ClientConfig_t;

/* ===== UDP Discovery Result ===== */
typedef struct {
    char     server_ip[INET_ADDRSTRLEN];
    uint16_t logical_address;
    uint8_t  vin[DOIP_VIN_LENGTH];
    uint8_t  eid[DOIP_EID_LENGTH];
    uint8_t  gid[DOIP_GID_LENGTH];
} DoIP_DiscoveryResult_t;

/* ===== Return Codes ===== */
typedef int DoIP_ClientStatus_t;
#define DOIP_CLIENT_OK              0
#define DOIP_CLIENT_ERR_PARAM      (-1)
#define DOIP_CLIENT_ERR_SOCKET     (-2)
#define DOIP_CLIENT_ERR_TIMEOUT    (-3)
#define DOIP_CLIENT_ERR_NACK       (-4)
#define DOIP_CLIENT_ERR_REJECTED   (-5)
#define DOIP_CLIENT_ERR_NOT_READY  (-6)
#define DOIP_CLIENT_ERR_IO         (-7)

/* ===== Lifecycle ===== */
DoIP_Client_t*      DoIP_Client_Create(void);
void                DoIP_Client_Destroy(DoIP_Client_t *client);
DoIP_ClientStatus_t DoIP_Client_Init(DoIP_Client_t *client, const DoIP_ClientConfig_t *cfg);
void                DoIP_Client_DeInit(DoIP_Client_t *client);

/* ===== UDP Discovery (stateless — no handle required) ===== */
/*
 * Sends a VIN Request to target_ip (or broadcast if NULL) on target_port.
 * Waits up to timeout_ms for a Vehicle Announcement / VIN Response.
 * Returns DOIP_CLIENT_OK and fills result_out on success.
 */
DoIP_ClientStatus_t DoIP_Client_Discover(const char            *target_ip,
                                          uint16_t               target_port,
                                          uint32_t               timeout_ms,
                                          DoIP_DiscoveryResult_t *result_out);

/* ===== TCP Connection ===== */
DoIP_ClientStatus_t DoIP_Client_Connect(DoIP_Client_t *client,
                                         const char    *server_ip,
                                         uint16_t       server_port);
DoIP_ClientStatus_t DoIP_Client_Disconnect(DoIP_Client_t *client);

/* ===== Routing Activation ===== */
/*
 * activation_type: 0x00 = default, 0x01 = WWH-OBD.
 * out_activation_code: optional, receives the raw activation code byte.
 */
DoIP_ClientStatus_t DoIP_Client_ActivateRouting(DoIP_Client_t *client,
                                                  uint8_t        activation_type,
                                                  uint8_t       *out_activation_code);

/* ===== Diagnostic Messaging ===== */

/* Send a UDS request wrapped in a DoIP Diagnostic Message frame. */
DoIP_ClientStatus_t DoIP_Client_SendDiagnostic(DoIP_Client_t *client,
                                                uint16_t       target_ecu_addr,
                                                const uint8_t *uds_data,
                                                uint16_t       uds_len);

/*
 * Receive a UDS response.  Silently consumes any preceding DiagACK (0x8002)
 * and transparently responds to Alive Check Requests (0x0007).
 * src_addr_out: optional, receives the ECU's logical address from the frame header.
 */
DoIP_ClientStatus_t DoIP_Client_RecvDiagnostic(DoIP_Client_t *client,
                                                uint8_t       *resp_buf,
                                                uint16_t       resp_buf_size,
                                                uint16_t      *resp_len_out,
                                                uint16_t      *src_addr_out);

/* Convenience: SendDiagnostic + RecvDiagnostic in one call. */
DoIP_ClientStatus_t DoIP_Client_Transact(DoIP_Client_t *client,
                                          uint16_t       target_ecu_addr,
                                          const uint8_t *req_data,
                                          uint16_t       req_len,
                                          uint8_t       *resp_buf,
                                          uint16_t       resp_buf_size,
                                          uint16_t      *resp_len_out);

/* ===== State Query ===== */
bool DoIP_Client_IsConnected(const DoIP_Client_t *client);
bool DoIP_Client_IsRoutingActive(const DoIP_Client_t *client);

#endif /* DOIP_CLIENT_H */
