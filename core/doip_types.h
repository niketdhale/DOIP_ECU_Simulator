#ifndef DOIP_TYPES_H
#define DOIP_TYPES_H

#include <stdint.h>
#include "config/doip_config.h"

/* ===== Generic DoIP Header (ISO 13400-2 Table 11) [[69]] ===== */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;    /* Byte 0: 0x02 */
    uint8_t  inverse_version;     /* Byte 1: 0xFD (bitwise NOT) */
    uint16_t payload_type;        /* Bytes 2-3: Big-endian */
    uint32_t payload_length;      /* Bytes 4-7: Big-endian */
} doip_header_t;

#define DOIP_HEADER_SIZE 8U

/* ===== Payload Types (ISO 13400-2 Table 17) [[63]] ===== */
typedef enum {
    DOIP_PT_GENERIC_NACK          = 0x0000,
    DOIP_PT_VIN_REQ               = 0x0001,  /* UDP: Vehicle ID Request */
    DOIP_PT_VIN_RES               = 0x0002,  /* UDP: Vehicle ID Response */
    DOIP_PT_VEHICLE_ANNOUNCE      = 0x0003,  /* UDP: Periodic Announcement */
    DOIP_PT_EID_REQ               = 0x0004,  /* UDP: Entity ID Request */
    DOIP_PT_ROUTING_ACT_REQ       = 0x0005,  /* TCP: Routing Activation Request */
    DOIP_PT_ROUTING_ACT_RES       = 0x0006,  /* TCP: Routing Activation Response */
    DOIP_PT_ALIVE_CHECK_REQ       = 0x0007,  /* TCP: Alive Check */
    DOIP_PT_ALIVE_CHECK_RES       = 0x0008,  /* TCP: Alive Check Response */
    DOIP_PT_DIAGNOSTIC_MSG        = 0x8001,  /* Diagnostic Message (Req & Res) */
    DOIP_PT_DIAGNOSTIC_ACK        = 0x8002,  /* Diagnostic Message ACK */
} doip_payload_type_t;

/* ===== Generic Header NACK Codes (ISO 13400-2 Table 19) ===== */
typedef enum {
    DOIP_NACK_NONE                = 0x00,
    DOIP_NACK_INVALID_PATTERN     = 0x01,
    DOIP_NACK_UNKNOWN_PT          = 0x02,
    DOIP_NACK_INVALID_LENGTH      = 0x03,
    DOIP_NACK_OUT_OF_MEMORY       = 0x04,
    DOIP_NACK_INVALID_PAYLOAD     = 0x05,
} doip_nack_code_t;

/* ===== Routing Activation Codes (ISO 13400-2 Table 25) ===== */
typedef enum {
    DOIP_ACT_SUCCESS              = 0x10,
    DOIP_ACT_ALREADY_ACTIVE       = 0x11,
    DOIP_ACT_AUTH_REQUIRED        = 0x12,
    DOIP_ACT_REJECTED_UNKNOWN     = 0x20,
    DOIP_ACT_REJECTED_SECURITY    = 0x21,
} doip_activation_code_t;

/* ===== Vehicle Announcement Payload (UDP) ===== */
typedef struct __attribute__((packed)) {
    uint8_t  vin[DOIP_VIN_LENGTH];
    uint16_t logical_address;    /* ISO 13400-2: 0x0E00=Default, 0x1340=OBD */
    uint8_t  eid[DOIP_EID_LENGTH];
    uint8_t  gid[DOIP_GID_LENGTH];
    uint32_t further_action;     /* Bitmask: 0x01=Routing activation required */
    uint16_t vin_sync_status;    /* 0x0010=VIN/GID/EID synchronized */
} doip_vehicle_announce_t;

#endif /* DOIP_TYPES_H */