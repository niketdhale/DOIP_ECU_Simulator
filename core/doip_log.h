/**
 * @file doip_log.h
 * @brief DoIP logging subsystem API and convenience macros.
 *
 * Provides thread-safe, level-filtered, module-filtered logging with
 * optional ANSI colours, timestamps, and source-location tags.
 *
 * @defgroup doip_log_api Logging API
 * @{
 */
#ifndef DOIP_LOG_H
#define DOIP_LOG_H

#include "config/doip_log_config.h"
#include "include/doip_version.h"
#include <stdint.h>

/* ===== Init / Control ===== */

/**
 * @brief Initialise the logging subsystem.
 *
 * Must be called once before any LOG_* macro is used.
 *
 * @param[in] level          Initial log level (0=ERROR .. 4=VERBOSE).
 * @param[in] module_filter  Bitmask of enabled modules (see DoIP_LogModule_t).
 * @param[in] file_path      Path to a log file, or NULL for console-only output.
 * @retval  0  Initialisation succeeded.
 * @retval <0  Error (e.g., could not open file).
 */
DOIP_API int  DoIP_Log_Init(uint8_t level, uint32_t module_filter, const char *file_path);

/**
 * @brief Shut down the logging subsystem and release resources.
 */
DOIP_API void DoIP_Log_DeInit(void);

/**
 * @brief Change the runtime log level.
 *
 * Messages above this level are silently discarded.
 *
 * @param[in] level  New log level (0=ERROR .. 4=VERBOSE).
 */
DOIP_API void DoIP_Log_SetLevel(uint8_t level);

/**
 * @brief Internal write function -- prefer the LOG_* convenience macros.
 *
 * @param[in] level   Severity level of this message.
 * @param[in] module  Module bitmask (e.g., DOIP_LOG_MODULE_TCP).
 * @param[in] file    Source file name (__FILE__).
 * @param[in] line    Source line number (__LINE__).
 * @param[in] fmt     printf-style format string.
 * @param[in] ...     Format arguments.
 */
DOIP_API void doip_log_write(uint8_t level, uint32_t module,
                              const char *file, int line,
                              const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* ===== Logging Macros ===== */

/** @brief Log an ERROR-level message.   @param mod Module ID.  @param fmt printf format string. */
#define LOG_ERROR(mod, fmt, ...)   doip_log_write(DOIP_LOG_LEVEL_ERROR,   (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/** @brief Log a WARNING-level message.  @param mod Module ID.  @param fmt printf format string. */
#define LOG_WARN(mod, fmt, ...)    doip_log_write(DOIP_LOG_LEVEL_WARN,    (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/** @brief Log an INFO-level message.    @param mod Module ID.  @param fmt printf format string. */
#define LOG_INFO(mod, fmt, ...)    doip_log_write(DOIP_LOG_LEVEL_INFO,    (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/** @brief Log a DEBUG-level message.    @param mod Module ID.  @param fmt printf format string. */
#define LOG_DEBUG(mod, fmt, ...)   doip_log_write(DOIP_LOG_LEVEL_DEBUG,   (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/** @brief Log a VERBOSE-level message.  @param mod Module ID.  @param fmt printf format string. */
#define LOG_VERBOSE(mod, fmt, ...) doip_log_write(DOIP_LOG_LEVEL_VERBOSE, (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/** @} */ /* end of doip_log_api group */

#endif /* DOIP_LOG_H */
