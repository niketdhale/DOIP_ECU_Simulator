#ifndef DOIP_UDS_CONFIG_H
#define DOIP_UDS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ===== UDS Service Configuration ===== */
/* Format: Request ID -> Response ID mapping */

/* Diagnostic Session Control */
#define UDS_SID_SESSION_CONTROL         0x10
#define UDS_SID_SESSION_CONTROL_RES     0x50
#define UDS_SESSION_DEFAULT             0x01
#define UDS_SESSION_EXTENDED            0x03
#define UDS_SESSION_PROGRAMMING         0x02

/* ECU Reset */
#define UDS_SID_ECU_RESET               0x11
#define UDS_SID_ECU_RESET_RES           0x51
#define UDS_RESET_HARD                  0x01
#define UDS_RESET_KEY_OFF_ON            0x02
#define UDS_RESET_SOFT                  0x03

/* Read Data By Identifier */
#define UDS_SID_READ_DATA_BY_ID         0x22
#define UDS_SID_READ_DATA_BY_ID_RES     0x62

/* Read DTC Information */
#define UDS_SID_READ_DTC_INFO           0x19
#define UDS_SID_READ_DTC_INFO_RES       0x59

/* Write Data By Identifier */
#define UDS_SID_WRITE_DATA_BY_ID        0x2E
#define UDS_SID_WRITE_DATA_BY_ID_RES    0x6E

/* Routine Control */
#define UDS_SID_ROUTINE_CONTROL         0x31
#define UDS_SID_ROUTINE_CONTROL_RES     0x71
#define UDS_ROUTINE_START               0x01
#define UDS_ROUTINE_STOP                0x02
#define UDS_ROUTINE_REQUEST_RESULTS     0x03

/* Communication Control */
#define UDS_SID_COMMUNICATION_CONTROL   0x28
#define UDS_SID_COMMUNICATION_CONTROL_RES 0x68

/* Tester Present */
#define UDS_SID_TESTER_PRESENT          0x3E
#define UDS_SID_TESTER_PRESENT_RES      0x7E

/* Access Security */
#define UDS_SID_SECURITY_ACCESS         0x27
#define UDS_SID_SECURITY_ACCESS_RES     0x67

/* Negative Response Code (NRC) */
#define UDS_NRC_POSITIVE_RESPONSE       0x00
#define UDS_NRC_GENERAL_REJECT          0x10
#define UDS_NRC_SERVICE_NOT_SUPPORTED   0x11
#define UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED 0x12
#define UDS_NRC_INCORRECT_MESSAGE_LENGTH 0x13
#define UDS_NRC_CONDITIONS_NOT_CORRECT  0x22
#define UDS_NRC_REQUEST_SEQUENCE_ERROR  0x24
#define UDS_NRC_REQUEST_OUT_OF_RANGE    0x31
#define UDS_NRC_SECURITY_ACCESS_DENIED  0x33
#define UDS_NRC_INVALID_KEY             0x35
#define UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS 0x36
#define UDS_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED 0x37

/* ===== Configurable Data Identifiers (DIDs) ===== */
#define UDS_DID_VIN_NUMBER              0xF190
#define UDS_DID_SYSTEM_NAME             0xF186
#define UDS_DID_CALIBRATION_ID          0xF1A0
#define UDS_DID_SOFTWARE_VERSION        0xF182
#define UDS_DID_ECU_SERIAL_NUMBER       0xF18C

/* ===== Feature Toggles ===== */
#define UDS_SUPPORT_SESSION_CONTROL     true
#define UDS_SUPPORT_ECU_RESET           true
#define UDS_SUPPORT_READ_DATA           true
#define UDS_SUPPORT_WRITE_DATA          false  /* Read-only ECU */
#define UDS_SUPPORT_ROUTINE_CONTROL     true
#define UDS_SUPPORT_TESTER_PRESENT      true
#define UDS_SUPPORT_SECURITY_ACCESS     false  /* Enable if needed */

/* ===== Timing Parameters ===== */
#define UDS_P2_SERVER_MS                50      /* Server response time */
#define UDS_P2_STAR_SERVER_MS           5000    /* Extended server response time */
#define UDS_S3_SERVER_MS                5000    /* Session timeout */

/* ===== ECU Configuration ===== */
#define UDS_ECU_DEFAULT_SESSION         UDS_SESSION_DEFAULT
#define UDS_ECU_SUPPORTS_EXTENDED       true
#define UDS_ECU_SUPPORTS_PROGRAMMING    false

#endif /* DOIP_UDS_CONFIG_H */