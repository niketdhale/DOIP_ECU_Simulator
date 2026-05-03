#include "doip_api.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
#include "transport/doip_udp.h"
#include "config/doip_config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

struct DoIP_Context {
    DoIP_Config_t config;
    bool          is_initialized;
};

/*  Thread Safety: Single API-level mutex */
static pthread_mutex_t g_api_mutex = PTHREAD_MUTEX_INITIALIZER;

static const DoIP_Config_t g_default_config = {
    .log_level = DOIP_LOG_LEVEL_INFO, .log_file_path = NULL, .s3_server_timeout_ms = 5000,
    .on_state_change = NULL, .on_rx_message = NULL, .user_context = NULL
};

static int DoIP_Config_Validate(void) {
    uint16_t ecu_addr = DOIP_ECU_LOGICAL_ADDRESS;
    bool addr_valid = ((ecu_addr >= 0x0001 && ecu_addr <= 0x0DFF) ||
                       (ecu_addr >= 0x1000 && ecu_addr <= 0x7FFF) ||
                       (ecu_addr >= 0x8000 && ecu_addr <= 0xDFFF));
    if (!addr_valid) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: DOIP_ECU_LOGICAL_ADDRESS 0x%04X is invalid.", ecu_addr); return -1; }
    if (DOIP_TCP_PORT == 0 || DOIP_UDP_PORT == 0) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Ports cannot be 0."); return -1; }
    if (DOIP_MAX_PAYLOAD_SIZE < 8) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Payload size too small."); return -1; }
    if (DOIP_ROUTING_ACTIVATION_TIMEOUT_MS < 1000) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Timeout too short."); return -1; }
    LOG_INFO(DOIP_LOG_MODULE_CORE, "✅ Configuration validation passed.");
    return 0;
}

static uint32_t api_get_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

DoIP_Handle_t* DoIP_Create(void) {
    DoIP_Handle_t *handle = (DoIP_Handle_t*)malloc(sizeof(DoIP_Handle_t));
    if (!handle) return NULL;
    memset(handle, 0, sizeof(DoIP_Handle_t));
    handle->config = g_default_config;
    return handle;
}

int DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config) {
    pthread_mutex_lock(&g_api_mutex);
    int ret = 0;
    if (!handle) { ret = -1; goto unlock; }
    if (handle->is_initialized) { ret = -5; goto unlock; }

    if (config) {
        if (config->log_level > 0) handle->config.log_level = config->log_level;
        if (config->log_file_path) handle->config.log_file_path = config->log_file_path;
        if (config->s3_server_timeout_ms > 0) handle->config.s3_server_timeout_ms = config->s3_server_timeout_ms;
        if (config->on_state_change) handle->config.on_state_change = config->on_state_change;
        if (config->on_rx_message) handle->config.on_rx_message = config->on_rx_message;
        if (config->user_context) handle->config.user_context = config->user_context;
    }

    if (DoIP_Log_Init(handle->config.log_level, DOIP_LOG_MODULE_ALL, handle->config.log_file_path) != 0) { ret = -2; goto unlock; }
    LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Initializing with S3=%u ms", handle->config.s3_server_timeout_ms);
    if (DoIP_Config_Validate() != 0) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config validation failed"); DoIP_Log_DeInit(); ret = -3; goto unlock; }
    if (DoIP_Fsm_Init(handle->config.s3_server_timeout_ms) != 0) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "FSM init failed"); DoIP_Log_DeInit(); ret = -4; goto unlock; }

    handle->is_initialized = true;
unlock:
    pthread_mutex_unlock(&g_api_mutex);
    return ret;
}

void DoIP_Tick(DoIP_Handle_t *handle) {
    if (!handle || !handle->is_initialized) return;
    pthread_mutex_lock(&g_api_mutex);
    
    uint32_t now = api_get_ms();
    DoIP_Fsm_MainFunction();
    doip_udp_tick(now); /* Drives periodic announcements */
    
    pthread_mutex_unlock(&g_api_mutex);
}

void DoIP_DeInit(DoIP_Handle_t *handle) {
    pthread_mutex_lock(&g_api_mutex);
    if (handle && handle->is_initialized) {
        LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Deinitializing...");
        DoIP_Fsm_DeInit();
        DoIP_Log_DeInit();
        handle->is_initialized = false;
    }
    pthread_mutex_unlock(&g_api_mutex);
}

void DoIP_Destroy(DoIP_Handle_t *handle) {
    DoIP_DeInit(handle);
    free(handle);
}

void DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level) {
    pthread_mutex_lock(&g_api_mutex);
    if (handle) { handle->config.log_level = level; DoIP_Log_SetLevel(level); }
    pthread_mutex_unlock(&g_api_mutex);
}