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
#define DOIP_VERSION_2012   0  /* ISO 13400-2:2012 — protocol version 0x02 */
#define DOIP_VERSION_2019   1  /* ISO 13400-2:2019 — protocol version 0x03 */

/* CONFIGURABLE: Set your DoIP version here (or build with: make DOIP_2019=1) */
#ifndef DOIP_PROTOCOL_VERSION_SELECTED
#define DOIP_PROTOCOL_VERSION_SELECTED  DOIP_VERSION_2012
#endif

/* ===== Protocol Version Constants =====
 * Logical addresses are 16-bit in both editions; only the header
 * version byte (and its bitwise inverse) differs. */
#if (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2012)
    #define DOIP_PROTOCOL_VERSION       0x02U
    #define DOIP_INVERSE_VERSION        0xFDU
#elif (DOIP_PROTOCOL_VERSION_SELECTED == DOIP_VERSION_2019)
    #define DOIP_PROTOCOL_VERSION       0x03U
    #define DOIP_INVERSE_VERSION        0xFCU
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
/* Functional (group) address this ECU also answers to. ISO 13400-2 reserves
 * 0xE400-0xEFFF for vehicle-manufacturer-defined functional group addresses. */
#define DOIP_FUNCTIONAL_LOGICAL_ADDRESS 0xE400U
#define DOIP_TESTER_LOGICAL_ADDRESS 0x0E00U     /* Default tester address used by the client */
/* ISO 13400-2: range reserved for external test equipment; routing
 * activation from any other source address is rejected (code 0x00). */
#define DOIP_TESTER_ADDR_MIN        0x0E00U
#define DOIP_TESTER_ADDR_MAX        0x0FFFU

/* ===== Network Configuration ===== */
#ifndef DOIP_UDP_PORT
#define DOIP_UDP_PORT               13400U
#endif
#ifndef DOIP_TCP_PORT
#define DOIP_TCP_PORT               13400U
#endif
#ifndef DOIP_MULTICAST_ADDR
#define DOIP_MULTICAST_ADDR         "224.0.0.1"   /* IPv4 destination of vehicle announcements */
#endif
#define DOIP_BROADCAST_ADDR         "255.255.255.255"

#define DOIP_MAX_PAYLOAD_SIZE       4096U
#define DOIP_RX_TIMEOUT_MS          2000U
#define DOIP_TX_TIMEOUT_MS          2000U
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS 5000U

/* ===== Periodic Announcement Configuration ===== */
/* Set to false to disable periodic announcements completely */
#define DOIP_ENABLE_PERIODIC_ANNOUNCE  true

/* Interval between announcements (milliseconds) */
#ifndef DOIP_ANNOUNCE_INTERVAL_MS
#define DOIP_ANNOUNCE_INTERVAL_MS   2000U /* (ISO default 2-5s) */
#endif

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

/* ===== Feature Toggles (override with -D, e.g. -DDOIP_SUPPORT_EID_REQUEST=false) =====
 * VIN_REQUEST: answer Vehicle ID requests (PT 0x0001, 0x0003); EID_REQUEST: PT 0x0002;
 * ALIVE_CHECK: server-initiated Alive Check probing (tester-initiated ones are always answered). */
#ifndef DOIP_SUPPORT_VIN_REQUEST
#define DOIP_SUPPORT_VIN_REQUEST    true
#endif
#ifndef DOIP_SUPPORT_EID_REQUEST
#define DOIP_SUPPORT_EID_REQUEST    true
#endif
#ifndef DOIP_SUPPORT_ALIVE_CHECK
#define DOIP_SUPPORT_ALIVE_CHECK    true
#endif

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

/** @} */ /* end of doip_config group */

#endif /* DOIP_CONFIG_H */