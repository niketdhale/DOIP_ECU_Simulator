#ifndef DOIP_LOG_CONFIG_H
#define DOIP_LOG_CONFIG_H

#include <stdbool.h>

/* ===== Log Levels (AUTOSAR-aligned) ===== */
#define DOIP_LOG_LEVEL_ERROR    0
#define DOIP_LOG_LEVEL_WARN     1
#define DOIP_LOG_LEVEL_INFO     2
#define DOIP_LOG_LEVEL_DEBUG    3
#define DOIP_LOG_LEVEL_VERBOSE  4

/* 🔧 CONFIGURABLE: Set default compile-time log level */
#define DOIP_LOG_DEFAULT_LEVEL  DOIP_LOG_LEVEL_INFO

/* 🔧 CONFIGURABLE: Enable/disable features */
#define DOIP_LOG_ENABLE_COLORS      true
#define DOIP_LOG_ENABLE_TIMESTAMPS  true
#define DOIP_LOG_ENABLE_SOURCE_LOC  true
#define DOIP_LOG_ENABLE_THREAD_ID   false

/* ===== Module IDs (for filtering) ===== */
typedef enum {
    DOIP_LOG_MODULE_CORE   = 0x01,
    DOIP_LOG_MODULE_UDP    = 0x02,
    DOIP_LOG_MODULE_TCP    = 0x04,
    DOIP_LOG_MODULE_UDS    = 0x08,
    DOIP_LOG_MODULE_FSM    = 0x10,
    DOIP_LOG_MODULE_ALL    = 0xFF
} DoIP_LogModule_t;

/* 🔧 CONFIGURABLE: Enable modules at compile-time */
#define DOIP_LOG_ENABLED_MODULES  DOIP_LOG_MODULE_ALL

#endif /* DOIP_LOG_CONFIG_H */