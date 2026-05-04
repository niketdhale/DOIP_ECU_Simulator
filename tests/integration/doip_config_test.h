/**
 * @file doip_config_test.h
 * @brief Port-override config for integration tests.
 *
 * Injected via -include flag before the real doip_config.h runs.
 * Redirects TCP + UDP traffic to port 23400 so integration tests do
 * not conflict with a running doip_ecu_sim instance on port 13400.
 *
 * All other settings are inherited verbatim from doip_config.h.
 * The #ifndef DOIP_CONFIG_H guard prevents the real file from loading.
 */
#ifndef DOIP_CONFIG_H
#define DOIP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ===== DoIP Version Selection ===== */
#define DOIP_VERSION_2012   0
#define DOIP_VERSION_2019   1
#define DOIP_PROTOCOL_VERSION_SELECTED  DOIP_VERSION_2012

/* ===== Protocol Version Constants ===== */
#if (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2012)
    #define DOIP_PROTOCOL_VERSION       0x02U
    #define DOIP_INVERSE_VERSION        0xFDU
    #define DOIP_LOGICAL_ADDR_SIZE      2U
#elif (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2019)
    #define DOIP_PROTOCOL_VERSION       0x02U
    #define DOIP_INVERSE_VERSION        0xFDU
    #define DOIP_LOGICAL_ADDR_SIZE      4U
#else
    #error "Unsupported DoIP version selected"
#endif

/* ===== Entity Configuration ===== */
#define DOIP_ENTITY_TYPE            0x10U
#define DOIP_VIN_LENGTH             17U
#define DOIP_GID_LENGTH             6U
#define DOIP_EID_LENGTH             6U

/* ===== Logical Addresses ===== */
#define DOIP_ECU_LOGICAL_ADDRESS    0x1003U
#define DOIP_OBD_LOGICAL_ADDRESS    0x1340U
#define DOIP_TESTER_LOGICAL_ADDRESS 0x0E00U

/* ===== Network Configuration — OVERRIDDEN for test isolation ===== */
#define DOIP_UDP_PORT               23400U   /* <-- test port, not 13400 */
#define DOIP_TCP_PORT               23400U   /* <-- test port, not 13400 */
#define DOIP_MULTICAST_ADDR         "224.0.0.1"
#define DOIP_BROADCAST_ADDR         "255.255.255.255"

#define DOIP_MAX_PAYLOAD_SIZE       4096U
#define DOIP_RX_TIMEOUT_MS          2000U
#define DOIP_TX_TIMEOUT_MS          2000U
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS 5000U

/* ===== Periodic Announcement ===== */
#define DOIP_ENABLE_PERIODIC_ANNOUNCE  true
#define DOIP_ANNOUNCE_INTERVAL_MS   2000U
#define DOIP_ANNOUNCE_COUNT_MAX        5

/* ===== Alive Check Timing ===== */
#define DOIP_ALIVE_CHECK_INTERVAL_MS    5000U
#define DOIP_ALIVE_CHECK_TIMEOUT_MS     2000U

/* ===== Feature Toggles ===== */
#define DOIP_DEV_ERROR_DETECT       false
#define DOIP_SUPPORT_VIN_REQUEST    true
#define DOIP_SUPPORT_EID_REQUEST    true
#define DOIP_SUPPORT_ROUTING_ACT    true
#define DOIP_SUPPORT_ALIVE_CHECK    true

/* ===== IPv6 Support ===== */
#ifndef DOIP_ENABLE_IPV6
#define DOIP_ENABLE_IPV6            false
#endif
#define DOIP_IPV6_MULTICAST_ADDR    "ff02::1"

/* ===== TLS/DTLS Transport Security ===== */
#ifndef DOIP_ENABLE_TLS
#define DOIP_ENABLE_TLS             false
#endif

/* ===== ISO-TP Framing (ISO 15765-2) ===== */
#ifndef DOIP_ENABLE_ISO_TP
#define DOIP_ENABLE_ISO_TP          false
#endif
#define DOIP_ISOTP_SF_MAX           7U
#define DOIP_ISOTP_N_BS_MS          1000U
#define DOIP_ISOTP_N_CR_MS          1000U
#define DOIP_ISOTP_STMIN_MS         0U

/* ===== Callback Hooks ===== */
typedef void (*DoIP_RxIndication)(uint16_t payload_type, const uint8_t *data, uint32_t len);
typedef void (*DoIP_TxConfirmation)(bool success);

/* Async TCP-client event hooks (set by doip_api, consumed by doip_tcp) */
typedef void (*DoIP_ClientConnectCb)   (int client_fd, void *user_ctx);
typedef void (*DoIP_ClientDisconnectCb)(int client_fd, void *user_ctx);
typedef int  (*DoIP_FrameReceivedCb)   (int client_fd, uint16_t pt,
                                         const uint8_t *payload, uint32_t plen,
                                         void *user_ctx);

extern DoIP_RxIndication        g_doip_rx_cb;
extern DoIP_TxConfirmation      g_doip_tx_cb;
extern DoIP_ClientConnectCb     g_doip_client_connect_cb;
extern DoIP_ClientDisconnectCb  g_doip_client_disconnect_cb;
extern DoIP_FrameReceivedCb     g_doip_frame_received_cb;
extern void                    *g_doip_async_user_ctx;

#endif /* DOIP_CONFIG_H */
