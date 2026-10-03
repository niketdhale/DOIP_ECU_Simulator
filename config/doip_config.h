/**
 * @file doip_config.h
 * @brief DoIP protocol and network configuration.
 *
 * Compile-time tunables for the DoIP stack: protocol version selection,
 * logical addresses, network ports, timeouts, periodic announcement
 * parameters, and feature toggles.
 *
 * @defgroup doip_config Configuration
 * @{
 */
#ifndef DOIP_CONFIG_H
#define DOIP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ===== DoIP Version Selection ===== */
#define DOIP_VERSION_2012   0  /* ISO 13400-2:2012 (16-bit addresses) */
#define DOIP_VERSION_2019   1  /* ISO 13400-2:2019 (32-bit addresses) */

/* CONFIGURABLE: Set your DoIP version here */
#define DOIP_PROTOCOL_VERSION_SELECTED  DOIP_VERSION_2012

/* ===== Protocol Version Constants ===== */
#if (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2012)
    #define DOIP_PROTOCOL_VERSION       0x02U
    #define DOIP_INVERSE_VERSION        0xFDU
    #define DOIP_LOGICAL_ADDR_SIZE      2U   /* 16-bit for 2012 */
#elif (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2019)
    #define DOIP_PROTOCOL_VERSION       0x02U
    #define DOIP_INVERSE_VERSION        0xFDU
    #define DOIP_LOGICAL_ADDR_SIZE      4U   /* 32-bit for 2019 */
#else
    #error "Unsupported DoIP version selected"
#endif

/* ===== Entity Configuration ===== */
#define DOIP_ENTITY_TYPE            0x10U       /* 0x10=ECU, 0x00=External Tester */
#define DOIP_VIN_LENGTH             17U
#define DOIP_GID_LENGTH             6U
#define DOIP_EID_LENGTH             6U

/* ===== Logical Addresses (Configurable) ===== */
/* Valid ECU Ranges: 0x0001-0x0DFF, 0x1000-0x7FFF, 0x8000-0xDFFF */
#define DOIP_ECU_LOGICAL_ADDRESS    0x1003U
#define DOIP_OBD_LOGICAL_ADDRESS    0x1340U
#define DOIP_TESTER_LOGICAL_ADDRESS 0x0E00U     /* ISO 13400-2: reserved for external testers */

/* ===== Network Configuration ===== */
#define DOIP_UDP_PORT               13400U
#define DOIP_TCP_PORT               13400U
#define DOIP_MULTICAST_ADDR         "224.0.0.1"
#define DOIP_BROADCAST_ADDR         "255.255.255.255"

#define DOIP_MAX_PAYLOAD_SIZE       4096U
#define DOIP_RX_TIMEOUT_MS          2000U
#define DOIP_TX_TIMEOUT_MS          2000U
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS 5000U

/* ===== Periodic Announcement Configuration ===== */
/* Set to false to disable periodic announcements completely */
#define DOIP_ENABLE_PERIODIC_ANNOUNCE  true

/* Interval between announcements (milliseconds) */
#define DOIP_ANNOUNCE_INTERVAL_MS   2000U /* (ISO default 2-5s) */

/* Maximum number of announcements to send at startup. 
   Set to 0 to disable, or 5 to send exactly 5 times then stop. */
#define DOIP_ANNOUNCE_COUNT_MAX        5

/* ===== Alive Check Timing ===== */
/* How long an ACTIVATED client can be idle before the server sends an Alive Check Request */
#define DOIP_ALIVE_CHECK_INTERVAL_MS    5000U
/* If no Alive Check Response is received within this window the client is disconnected */
#define DOIP_ALIVE_CHECK_TIMEOUT_MS     2000U

/* Max time a TCP client may stay CONNECTED without completing Routing Activation */
#define DOIP_INITIAL_INACTIVITY_TIMEOUT_MS 2000U

/* ===== Feature Toggles ===== */
#define DOIP_DEV_ERROR_DETECT       false
#define DOIP_SUPPORT_VIN_REQUEST    true
#define DOIP_SUPPORT_EID_REQUEST    true
#define DOIP_SUPPORT_ROUTING_ACT    true
#define DOIP_SUPPORT_ALIVE_CHECK    true

/* ===== IPv6 Support =====
 * Dual-stack sockets (AF_INET6 + IPV6_V6ONLY=0).
 * Build with: make DOIP_IPV6=1
 */
#ifndef DOIP_ENABLE_IPV6
#define DOIP_ENABLE_IPV6            false
#endif
#define DOIP_IPV6_MULTICAST_ADDR    "ff02::1"   /* DoIP IPv6 link-local multicast (ISO 13400-2:2019) */

/* ===== TLS/DTLS Transport Security =====
 * Optional OpenSSL encryption for TCP (TLS 1.2+) and UDP (DTLS 1.2).
 * Requires OpenSSL >= 1.1.1.
 * Build with: make DOIP_TLS=1
 */
#ifndef DOIP_ENABLE_TLS
#define DOIP_ENABLE_TLS             false
#endif

/* ===== ISO-TP Framing (ISO 15765-2) =====
 * Transparent segmentation/reassembly of UDS payloads at the DoIP
 * diagnostic message boundary.
 * Build with: make DOIP_ISOTP=1
 */
#ifndef DOIP_ENABLE_ISO_TP
#define DOIP_ENABLE_ISO_TP          false
#endif

/* ISO-TP timing and sizing constants */
#define DOIP_ISOTP_SF_MAX           7U      /* Max data bytes in a Single Frame       */
#define DOIP_ISOTP_N_BS_MS          1000U   /* Sender wait timeout for FC after FF    */
#define DOIP_ISOTP_N_CR_MS          1000U   /* Receiver wait timeout for next CF      */
#define DOIP_ISOTP_STMIN_MS         0U      /* Min separation time between CFs (ms)  */

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

/** @} */ /* end of doip_config group */

#endif /* DOIP_CONFIG_H */