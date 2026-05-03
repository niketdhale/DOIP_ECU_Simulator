#ifndef DOIP_LOG_H
#define DOIP_LOG_H

#include "config/doip_log_config.h"
#include <stdint.h>

/* ===== Init / Control ===== */
int  DoIP_Log_Init(uint8_t level, uint32_t module_filter, const char *file_path);
void DoIP_Log_DeInit(void);
void DoIP_Log_SetLevel(uint8_t level);

/* Internal write — use macros below, not this directly */
void doip_log_write(uint8_t level, uint32_t module,
                    const char *file, int line,
                    const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* ===== Logging Macros ===== */
#define LOG_ERROR(mod, fmt, ...)   doip_log_write(DOIP_LOG_LEVEL_ERROR,   (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(mod, fmt, ...)    doip_log_write(DOIP_LOG_LEVEL_WARN,    (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(mod, fmt, ...)    doip_log_write(DOIP_LOG_LEVEL_INFO,    (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(mod, fmt, ...)   doip_log_write(DOIP_LOG_LEVEL_DEBUG,   (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(mod, fmt, ...) doip_log_write(DOIP_LOG_LEVEL_VERBOSE, (uint32_t)(mod), __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* DOIP_LOG_H */
