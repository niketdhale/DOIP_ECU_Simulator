/**
 * @file doip_client.h
 * @brief DoIP Client (Tester) public API.
 *
 * Provides functions to discover ECUs via UDP, establish a TCP
 * connection, activate routing, and exchange UDS diagnostic messages
 * over DoIP according to ISO 13400-2.
 *
 * @par Typical usage
 * @code
 *   DoIP_DiscoveryResult_t disc;
 *   DoIP_Client_Discover(NULL, 13400, 2000, &disc);
 *
 *   DoIP_Client_t *c = DoIP_Client_Create();
 *   DoIP_ClientConfig_t cfg = { .tester_logical_addr = 0x0E00,
 *                                .connect_timeout_ms  = 3000 };
 *   DoIP_Client_Init(c, &cfg);
 *   DoIP_Client_Connect(c, disc.server_ip, 13400);
 *   DoIP_Client_ActivateRouting(c, 0x00, NULL);
 *
 *   uint8_t req[] = { 0x22, 0xF1, 0x90 };       // ReadDataByID — VIN
 *   uint8_t resp[256]; uint16_t rlen;
 *   DoIP_Client_Transact(c, 0x1003, req, 3, resp, sizeof(resp), &rlen);
 *
 *   DoIP_Client_Disconnect(c);
 *   DoIP_Client_Destroy(c);
 * @endcode
 *
 * @defgroup doip_client_api Client API
 * @{
 */
#ifndef DOIP_CLIENT_H
#define DOIP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include "config/doip_config.h"
#include "config/doip_uds_config.h"
#include "include/doip_version.h"

/**
 * @brief Opaque handle for a DoIP Client session.
 *
 * All client API calls (except DoIP_Client_Discover()) require a valid
 * handle obtained from DoIP_Client_Create().
 */
typedef struct DoIP_ClientContext DoIP_Client_t;

/* ===== Client Configuration ===== */

/**
 * @brief Configuration parameters for a DoIP Client session.
 */
typedef struct {
    uint16_t tester_logical_addr;   /**< Source logical address (default: DOIP_TESTER_LOGICAL_ADDRESS) */
    uint32_t connect_timeout_ms;    /**< TCP connect / socket receive timeout in ms */
    uint32_t response_timeout_ms;   /**< Maximum wait for a UDS response in ms */
    uint32_t routing_timeout_ms;    /**< Maximum wait for routing activation response in ms */
} DoIP_ClientConfig_t;

/* ===== UDP Discovery Result ===== */

/**
 * @brief Result of a UDP vehicle discovery (Vehicle Announcement / VIN Response).
 */
typedef struct {
    char     server_ip[INET_ADDRSTRLEN]; /**< Dotted-decimal IP of the responding ECU */
    uint16_t logical_address;            /**< ECU logical address from the announcement */
    uint8_t  vin[DOIP_VIN_LENGTH];       /**< 17-byte VIN (not NUL-terminated) */
    uint8_t  eid[DOIP_EID_LENGTH];       /**< 6-byte Entity ID (MAC address) */
    uint8_t  gid[DOIP_GID_LENGTH];       /**< 6-byte Group ID */
} DoIP_DiscoveryResult_t;

/* ===== Return Codes ===== */

/** @brief Client function return type (0 = success, negative = error). */
typedef int DoIP_ClientStatus_t;

#define DOIP_CLIENT_OK              0   /**< Success */
#define DOIP_CLIENT_ERR_PARAM      (-1) /**< Invalid parameter (NULL pointer, bad length) */
#define DOIP_CLIENT_ERR_SOCKET     (-2) /**< Socket creation or bind failed */
#define DOIP_CLIENT_ERR_TIMEOUT    (-3) /**< Operation timed out */
#define DOIP_CLIENT_ERR_NACK       (-4) /**< Server sent Generic Header NACK */
#define DOIP_CLIENT_ERR_REJECTED   (-5) /**< Routing activation rejected by server */
#define DOIP_CLIENT_ERR_NOT_READY  (-6) /**< Client not in the required state */
#define DOIP_CLIENT_ERR_IO         (-7) /**< Unexpected I/O error (read/write) */

/* ===== Lifecycle ===== */

/**
 * @brief Allocate a new DoIP Client instance.
 *
 * Returns an opaque handle with default values.  Must be followed by
 * DoIP_Client_Init() before use.
 *
 * @return Handle on success, or NULL if memory allocation fails.
 */
DOIP_API DoIP_Client_t* DoIP_Client_Create(void);

/**
 * @brief Free a DoIP Client instance and all associated resources.
 *
 * If the client is still connected, the TCP socket is closed first.
 *
 * @param[in,out] client  Handle returned by DoIP_Client_Create().
 */
DOIP_API void DoIP_Client_Destroy(DoIP_Client_t *client);

/**
 * @brief Initialise the client with the given configuration.
 *
 * Copies configuration parameters into the handle.  The handle
 * transitions from UNINIT to INITIALIZED.
 *
 * @param[in,out] client  Handle returned by DoIP_Client_Create().
 * @param[in]     cfg     Configuration, or NULL for compiled-in defaults.
 * @retval DOIP_CLIENT_OK         Initialisation succeeded.
 * @retval DOIP_CLIENT_ERR_PARAM  @p client is NULL.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_Init(DoIP_Client_t *client,
                                               const DoIP_ClientConfig_t *cfg);

/**
 * @brief De-initialise the client (close sockets, reset state to UNINIT).
 *
 * The handle remains allocated and may be re-initialised.
 *
 * @param[in,out] client  Handle returned by DoIP_Client_Create().
 */
DOIP_API void DoIP_Client_DeInit(DoIP_Client_t *client);

/* ===== UDP Discovery (stateless -- no handle required) ===== */

/**
 * @brief Discover DoIP entities on the network via UDP broadcast.
 *
 * Sends a VIN Request to @p target_ip (or @c 255.255.255.255 if NULL)
 * on @p target_port and waits up to @p timeout_ms for a Vehicle
 * Announcement / VIN Response.
 *
 * This function is stateless: it creates and destroys its own UDP
 * socket internally.
 *
 * @param[in]  target_ip   Unicast/broadcast IP, or NULL for broadcast.
 * @param[in]  target_port UDP port (typically 13400).
 * @param[in]  timeout_ms  Maximum wait time in milliseconds.
 * @param[out] result_out  Filled on success with the discovered ECU info.
 * @retval DOIP_CLIENT_OK          Discovery succeeded.
 * @retval DOIP_CLIENT_ERR_SOCKET  Could not create UDP socket.
 * @retval DOIP_CLIENT_ERR_TIMEOUT No response within @p timeout_ms.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_Discover(const char            *target_ip,
                                                   uint16_t               target_port,
                                                   uint32_t               timeout_ms,
                                                   DoIP_DiscoveryResult_t *result_out);

/* ===== TCP Connection ===== */

/**
 * @brief Establish a TCP connection to a DoIP entity.
 *
 * @param[in,out] client      Initialised client handle.
 * @param[in]     server_ip   Dotted-decimal IP of the target ECU.
 * @param[in]     server_port TCP port (typically 13400).
 * @retval DOIP_CLIENT_OK           Connection established.
 * @retval DOIP_CLIENT_ERR_SOCKET   Socket creation failed.
 * @retval DOIP_CLIENT_ERR_TIMEOUT  connect() timed out.
 * @retval DOIP_CLIENT_ERR_NOT_READY Client not initialised.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_Connect(DoIP_Client_t *client,
                                                  const char    *server_ip,
                                                  uint16_t       server_port);

/**
 * @brief Close the TCP connection to the DoIP entity.
 *
 * @param[in,out] client  Connected client handle.
 * @retval DOIP_CLIENT_OK           Disconnected successfully.
 * @retval DOIP_CLIENT_ERR_NOT_READY Client was not connected.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_Disconnect(DoIP_Client_t *client);

/* ===== Routing Activation ===== */

/**
 * @brief Request routing activation from the connected DoIP entity.
 *
 * Sends a Routing Activation Request (payload type 0x0005) and waits
 * for the response.  Activation codes 0x10 (success) and 0x11
 * (already active) are treated as success.
 *
 * @param[in,out] client              Connected client handle.
 * @param[in]     activation_type     0x00 = default, 0x01 = WWH-OBD.
 * @param[out]    out_activation_code Optional; receives the raw activation code byte.
 * @retval DOIP_CLIENT_OK            Routing activated.
 * @retval DOIP_CLIENT_ERR_REJECTED  Server rejected the request.
 * @retval DOIP_CLIENT_ERR_TIMEOUT   No response within routing_timeout_ms.
 * @retval DOIP_CLIENT_ERR_NOT_READY Client not connected.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_ActivateRouting(DoIP_Client_t *client,
                                                          uint8_t        activation_type,
                                                          uint8_t       *out_activation_code);

/* ===== Diagnostic Messaging ===== */

/**
 * @brief Send a UDS request wrapped in a DoIP Diagnostic Message frame.
 *
 * @param[in,out] client          Activated client handle.
 * @param[in]     target_ecu_addr Logical address of the target ECU.
 * @param[in]     uds_data        UDS request bytes (SID + sub-function + data).
 * @param[in]     uds_len         Length of @p uds_data in bytes.
 * @retval DOIP_CLIENT_OK           Message sent.
 * @retval DOIP_CLIENT_ERR_IO       Socket write error.
 * @retval DOIP_CLIENT_ERR_NOT_READY Routing not activated.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_SendDiagnostic(DoIP_Client_t *client,
                                                         uint16_t       target_ecu_addr,
                                                         const uint8_t *uds_data,
                                                         uint16_t       uds_len);

/**
 * @brief Receive a UDS response from the DoIP entity.
 *
 * Silently consumes any preceding Diagnostic ACK (0x8002) frames and
 * transparently responds to Alive Check Requests (0x0007).  Returns
 * when a Diagnostic Message (0x8001) carrying the UDS response is
 * received.
 *
 * @param[in,out] client        Activated client handle.
 * @param[out]    resp_buf      Buffer for the UDS response payload.
 * @param[in]     resp_buf_size Size of @p resp_buf in bytes.
 * @param[out]    resp_len_out  Number of UDS bytes written to @p resp_buf.
 * @param[out]    src_addr_out  Optional; receives the ECU logical address from the frame.
 * @retval DOIP_CLIENT_OK           Response received.
 * @retval DOIP_CLIENT_ERR_TIMEOUT  No response within response_timeout_ms.
 * @retval DOIP_CLIENT_ERR_IO       Socket read error or unexpected frame type.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_RecvDiagnostic(DoIP_Client_t *client,
                                                         uint8_t       *resp_buf,
                                                         uint16_t       resp_buf_size,
                                                         uint16_t      *resp_len_out,
                                                         uint16_t      *src_addr_out);

/**
 * @brief Perform a complete UDS request/response transaction.
 *
 * Convenience wrapper that calls DoIP_Client_SendDiagnostic() followed
 * by DoIP_Client_RecvDiagnostic().
 *
 * @param[in,out] client          Activated client handle.
 * @param[in]     target_ecu_addr Logical address of the target ECU.
 * @param[in]     req_data        UDS request bytes.
 * @param[in]     req_len         Length of @p req_data in bytes.
 * @param[out]    resp_buf        Buffer for the UDS response payload.
 * @param[in]     resp_buf_size   Size of @p resp_buf in bytes.
 * @param[out]    resp_len_out    Number of UDS response bytes written.
 * @retval DOIP_CLIENT_OK           Transaction completed.
 * @retval DOIP_CLIENT_ERR_TIMEOUT  Response timed out.
 * @retval DOIP_CLIENT_ERR_IO       Socket I/O error.
 * @retval DOIP_CLIENT_ERR_NOT_READY Routing not activated.
 */
DOIP_API DoIP_ClientStatus_t DoIP_Client_Transact(DoIP_Client_t *client,
                                                   uint16_t       target_ecu_addr,
                                                   const uint8_t *req_data,
                                                   uint16_t       req_len,
                                                   uint8_t       *resp_buf,
                                                   uint16_t       resp_buf_size,
                                                   uint16_t      *resp_len_out);

/* ===== State Query ===== */

/**
 * @brief Check whether the client has an active TCP connection.
 * @param[in] client  Client handle.
 * @return @c true if connected, @c false otherwise.
 */
DOIP_API bool DoIP_Client_IsConnected(const DoIP_Client_t *client);

/**
 * @brief Check whether routing has been activated on the connection.
 * @param[in] client  Client handle.
 * @return @c true if routing is active, @c false otherwise.
 */
DOIP_API bool DoIP_Client_IsRoutingActive(const DoIP_Client_t *client);

/** @} */ /* end of doip_client_api group */

#endif /* DOIP_CLIENT_H */
