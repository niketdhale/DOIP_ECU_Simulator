/**
 * @file doip_types.h
 * @brief DoIP protocol structures and payload type constants.
 *
 * Contains the generic DoIP header, all payload type enumerations
 * (ISO 13400-2 Table 17), NACK codes, routing activation codes,
 * and the Vehicle Announcement payload layout.
 *
 * @defgroup doip_types Protocol Types
 * @{
 */
#ifndef DOIP_TYPES_H
#define DOIP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "config/doip_config.h"

/* ===== Generic DoIP Header (ISO 13400-2 Table 11) [[69]] ===== */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;    /* Byte 0: 0x02 (2012) / 0x03 (2019) */
    uint8_t  inverse_version;     /* Byte 1: bitwise NOT of byte 0 */
    uint16_t payload_type;        /* Bytes 2-3: Big-endian */
    uint32_t payload_length;      /* Bytes 4-7: Big-endian */
} doip_header_t;

#define DOIP_HEADER_SIZE 8U

/* ===== Payload Types (ISO 13400-2 Table 17) [[63]] ===== */
typedef enum {
    DOIP_PT_GENERIC_NACK          = 0x0000,
    DOIP_PT_VIN_REQ               = 0x0001,  /* UDP: Vehicle ID Request (no payload) */
    DOIP_PT_EID_REQ               = 0x0002,  /* UDP: Vehicle ID Request with EID (6 bytes) */
    DOIP_PT_VIN_REQ_WITH_VIN      = 0x0003,  /* UDP: Vehicle ID Request with VIN (17 bytes) */
    DOIP_PT_VEHICLE_ANNOUNCE      = 0x0004,  /* UDP: Vehicle Announcement / Vehicle ID Response */
    DOIP_PT_ROUTING_ACT_REQ       = 0x0005,  /* TCP: Routing Activation Request */
    DOIP_PT_ROUTING_ACT_RES       = 0x0006,  /* TCP: Routing Activation Response */
    DOIP_PT_ALIVE_CHECK_REQ       = 0x0007,  /* TCP: Alive Check */
    DOIP_PT_ALIVE_CHECK_RES       = 0x0008,  /* TCP: Alive Check Response */
    DOIP_PT_ENTITY_STATUS_REQ     = 0x4001,  /* UDP: Entity Status Request */
    DOIP_PT_ENTITY_STATUS_RES     = 0x4002,  /* UDP: Entity Status Response */
    DOIP_PT_POWER_MODE_REQ        = 0x4003,  /* UDP: Diagnostic Power Mode Request */
    DOIP_PT_POWER_MODE_RES        = 0x4004,  /* UDP: Diagnostic Power Mode Response */
    DOIP_PT_DIAGNOSTIC_MSG        = 0x8001,  /* Diagnostic Message (Req & Res) */
    DOIP_PT_DIAGNOSTIC_ACK        = 0x8002,  /* Diagnostic Message ACK */
    DOIP_PT_DIAGNOSTIC_NACK       = 0x8003,  /* Diagnostic Message NACK */
} doip_payload_type_t;

/* ===== Generic Header NACK Codes (ISO 13400-2) ===== */
typedef enum {
    DOIP_NACK_INVALID_PATTERN     = 0x00,  /* Incorrect pattern format — close socket */
    DOIP_NACK_UNKNOWN_PT          = 0x01,  /* Unknown payload type */
    DOIP_NACK_MSG_TOO_LARGE       = 0x02,  /* Message too large */
    DOIP_NACK_OUT_OF_MEMORY       = 0x03,
    DOIP_NACK_INVALID_LENGTH      = 0x04,  /* Invalid payload length — close socket */
} doip_nack_code_t;

/* ===== Diagnostic Message NACK Codes (ISO 13400-2) ===== */
#define DOIP_DIAG_NACK_INVALID_SA       0x02U  /* SA not activated on this socket (also used before routing activation) */
#define DOIP_DIAG_NACK_UNKNOWN_TA       0x03U  /* Target address is not this ECU */

/* ===== Routing Activation Response Codes (ISO 13400-2) ===== */
typedef enum {
    DOIP_ACT_UNKNOWN_SA           = 0x00,  /* Source address not a valid tester address */
    DOIP_ACT_NO_FREE_SOCKET       = 0x01,
    DOIP_ACT_SA_MISMATCH          = 0x02,  /* SA differs from the one already activated on this socket */
    DOIP_ACT_SA_IN_USE            = 0x03,  /* SA already active on another socket */
    DOIP_ACT_AUTH_MISSING         = 0x04,
    DOIP_ACT_CONFIRM_REJECTED     = 0x05,
    DOIP_ACT_UNSUPPORTED_TYPE     = 0x06,  /* Unsupported activation type */
    DOIP_ACT_SUCCESS              = 0x10,
    DOIP_ACT_CONFIRM_REQUIRED     = 0x11,
} doip_activation_code_t;

/* ===== Vehicle Announcement / Vehicle ID Response Payload (UDP, 33 bytes) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  vin[DOIP_VIN_LENGTH];
    uint16_t logical_address;    /* Logical address of this DoIP entity (big-endian) */
    uint8_t  eid[DOIP_EID_LENGTH];
    uint8_t  gid[DOIP_GID_LENGTH];
    uint8_t  further_action;     /* 0x00=none, 0x10=routing activation required (central security) */
    uint8_t  vin_sync_status;    /* 0x00=synchronized, 0x10=incomplete */
} doip_vehicle_announce_t;

/* ===== Entity Status Response (ISO 13400-2 Table 38) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  node_type;           /* 0x00=DoIP gateway, 0x01=DoIP node */
    uint8_t  max_open_sockets;    /* Maximum concurrent TCP sockets     */
    uint8_t  curr_open_sockets;   /* Currently active TCP sockets       */
    uint32_t max_data_size;       /* Max diagnostic payload (big-endian) */
} doip_entity_status_res_t;

/* ===== Diagnostic Power Mode Response (ISO 13400-2 Table 40) ===== */
typedef struct __attribute__((packed)) {
    uint8_t power_mode;           /* 0x00=not ready, 0x01=ready, 0x02=not supported */
} doip_power_mode_res_t;

/* ===== Runtime ECU Identity ===== */
/**
 * @brief Configurable ECU identity data.
 *
 * All fields that were previously hardcoded in the transport layer.
 * Populate and pass via DoIP_Config_t::ecu_identity at DoIP_Init() time.
 * NULL fields or a NULL pointer causes built-in defaults to be used.
 */
typedef struct {
    char    vin[DOIP_VIN_LENGTH + 1]; /**< 17-char VIN, NUL-terminated (default "WBAXXXXXXXXXXXXXX") */
    char    software_version[32];      /**< SW version string (default "V1.0.0")                      */
    char    system_name[64];           /**< ECU name string (default "DoIP ECU Simulator")             */
    char    serial_number[32];         /**< Serial number string (default "ECU123456789")              */
    uint8_t eid[DOIP_EID_LENGTH];     /**< Entity ID, 6 bytes (default 0xAA…)                        */
    uint8_t gid[DOIP_GID_LENGTH];     /**< Group ID, 6 bytes (default 0xBB…)                         */
    uint8_t further_action;            /**< Further action byte (default 0x00 = none required)        */
    uint8_t vin_gw_sync_status;       /**< VIN/GW sync status (default 0x00 = synchronized)          */
} DoIP_EcuIdentity_t;

/* ===== Fault Injection ===== */
/**
 * @brief Deliberate misbehaviour for testing tester robustness.
 *
 * All-zero means a well-behaved ECU. Set at init through
 * DoIP_Config_t::faults or at any time with DoIP_SetFaults().
 */
typedef struct {
    /** Delay every UDS response by this many ms. When longer than
     *  UDS_P2_SERVER_MS the ECU first answers 7F <sid> 78 (response pending)
     *  and repeats it every UDS_P2_STAR_SERVER_MS / 2 until the real response;
     *  further requests from that tester meanwhile get NRC 0x21 (busy). */
    uint32_t response_delay_ms;
    bool     drop_diag_ack;      /**< Do not send the DoIP Diagnostic ACK (0x8002).       */
    bool     drop_response;      /**< Swallow every UDS response (tester sees a timeout). */
    uint8_t  force_diag_nack;    /**< Non-zero: reject every diagnostic message with a
                                      Diagnostic NACK (0x8003) carrying this code.        */
} DoIP_FaultConfig_t;

/** @} */ /* end of doip_types group */

#endif /* DOIP_TYPES_H */