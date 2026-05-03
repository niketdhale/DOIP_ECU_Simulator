#ifndef DOIP_CONFIG_H
#define DOIP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ===== DoIP Version Selection ===== */
#define DOIP_VERSION_2012   0  /* ISO 13400-2:2012 (16-bit addresses) */
#define DOIP_VERSION_2019   1  /* ISO 13400-2:2019 (32-bit addresses) */

/* 🔧 CONFIGURABLE: Set your DoIP version here */
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

/* ===== ECU Logical Addresses (Configurable) ===== */
/* Valid ECU Ranges: 0x0001-0x0DFF, 0x1000-0x7FFF, 0x8000-0xDFFF */
/* 0x0E00 is reserved for External Tester, so we use 0x1003 */
#define DOIP_ECU_LOGICAL_ADDRESS    0x1003U     
#define DOIP_OBD_LOGICAL_ADDRESS    0x1340U

/* ===== Network Configuration ===== */
#define DOIP_UDP_PORT               13400U
#define DOIP_TCP_PORT               13400U
#define DOIP_MULTICAST_ADDR         "224.0.0.1"
#define DOIP_BROADCAST_ADDR         "255.255.255.255"
#define DOIP_ANNOUNCE_INTERVAL_MS   2000U /* (ISO default 2-5s) */

#define DOIP_MAX_PAYLOAD_SIZE       4096U
#define DOIP_RX_TIMEOUT_MS          2000U
#define DOIP_TX_TIMEOUT_MS          2000U
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS 5000U
#define DOIP_ENABLE_PERIODIC_ANNOUNCE  true  /* Set false to disable at compile-time */

/* ===== Feature Toggles ===== */
#define DOIP_DEV_ERROR_DETECT       false
#define DOIP_SUPPORT_VIN_REQUEST    true
#define DOIP_SUPPORT_EID_REQUEST    true
#define DOIP_SUPPORT_ROUTING_ACT    true
#define DOIP_SUPPORT_ALIVE_CHECK    true

/* ===== Callback Hooks ===== */
typedef void (*DoIP_RxIndication)(uint16_t payload_type, const uint8_t *data, uint32_t len);
typedef void (*DoIP_TxConfirmation)(bool success);

extern DoIP_RxIndication g_doip_rx_cb;
extern DoIP_TxConfirmation g_doip_tx_cb;

#endif /* DOIP_CONFIG_H */