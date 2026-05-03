#include "doip_api.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
#include "transport/doip_udp.h" /* ⬅️ REQUIRED: To use doip_udp_tick */
#include "config/doip_config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal Context Structure */
struct DoIP_Context {
    DoIP_Config_t config;
    bool          is_initialized;
};

/* Default Configuration */
static const DoIP_Config_t g_default_config = {
    .log_level = DOIP_LOG_LEVEL_INFO,
    .log_file_path = NULL,
    .s3_server_timeout_ms = 5000,
    .on_state_change = NULL,
    .on_rx_message = NULL,
    .user_context = NULL
};

/* Helper for timestamp */
static uint32_t api_get_ms(void) {
    struct timespec ts; 
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Configuration Validation */
static int DoIP_Config_Validate(void) {
    uint16_t ecu_addr = DOIP_ECU_LOGICAL_ADDRESS;
    /* Valid: 0x0001-0x0DFF or 0x1000-0x7FFF or 0x8000-0xDFFF */
    bool addr_valid = ((ecu_addr >= 0x0001 && ecu_addr <= 0x0DFF) ||
                       (ecu_addr >= 0x1000 && ecu_addr <= 0x7FFF) ||
                       (ecu_addr >= 0x8000 && ecu_addr <= 0xDFFF));
    if (!addr_valid) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: DOIP_ECU_LOGICAL_ADDRESS 0x%04X is invalid.", ecu_addr);
        return -1;
    }

    if (DOIP_TCP_PORT == 0 || DOIP_UDP_PORT == 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Port numbers cannot be 0.");
        return -1;
    }

    if (DOIP_MAX_PAYLOAD_SIZE < 8) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: DOIP_MAX_PAYLOAD_SIZE must be > 8.");
        return -1;
    }

    if (DOIP_ROUTING_ACTIVATION_TIMEOUT_MS < 1000) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Timeout too short (< 1000ms).");
        return -1;
    }

    LOG_INFO(DOIP_LOG_MODULE_CORE, " Configuration validation passed.");
    return 0;
}

DoIP_Handle_t* DoIP_Create(void) {
    DoIP_Handle_t *handle = (DoIP_Handle_t*)malloc(sizeof(DoIP_Handle_t));
    if (!handle) return NULL;
    
    memset(handle, 0, sizeof(DoIP_Handle_t));
    handle->config = g_default_config;
    return handle;
}

int DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config) {
    if (!handle) return -1;

    /* Apply configuration */
    if (config) {
        if (config->log_level > 0) handle->config.log_level = config->log_level;
        if (config->log_file_path) handle->config.log_file_path = config->log_file_path;
        if (config->s3_server_timeout_ms > 0) handle->config.s3_server_timeout_ms = config->s3_server_timeout_ms;
        if (config->on_state_change) handle->config.on_state_change = config->on_state_change;
        if (config->on_rx_message) handle->config.on_rx_message = config->on_rx_message;
        if (config->user_context) handle->config.user_context = config->user_context;
    }

    /* 1. Initialize Logging */
    if (DoIP_Log_Init(handle->config.log_level, DOIP_LOG_MODULE_ALL, handle->config.log_file_path) != 0) {
        return -2; 
    }

    LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Initializing with S3=%u ms", handle->config.s3_server_timeout_ms);

    /* 2. Validate Configuration */
    if (DoIP_Config_Validate() != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "DoIP API: Configuration validation failed");
        DoIP_Log_DeInit();
        return -3;
    }

    /* 3. Initialize FSM (which inits Transports & UDS) */
    if (DoIP_Fsm_Init(handle->config.s3_server_timeout_ms) != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "DoIP API: FSM Initialization failed");
        DoIP_Log_DeInit();
        return -4;
    }

    handle->is_initialized = true;
    return 0;
}

/* CORRECTED: Single implementation calling both FSM and UDP Tick */
void DoIP_Tick(DoIP_Handle_t *handle) {
    if (!handle || !handle->is_initialized) return;
    
    uint32_t now = api_get_ms();
    
    /* 1. FSM Processing (Network polling, State machine) */
    DoIP_Fsm_MainFunction();
    
    /* 2. UDP Periodic Announcement (Vehicle Discovery) */
    doip_udp_tick(now); 
}

void DoIP_DeInit(DoIP_Handle_t *handle) {
    if (!handle) return;

    LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Deinitializing...");
    
    /* 1. Deinit FSM & Transports */
    DoIP_Fsm_DeInit();
    
    /* 2. Deinit Logging */
    DoIP_Log_DeInit();
    
    handle->is_initialized = false;
}

void DoIP_Destroy(DoIP_Handle_t *handle) {
    if (!handle) return;
    
    /* Ensure clean shutdown if user forgot to DeInit */
    if (handle->is_initialized) {
        DoIP_DeInit(handle);
    }
    
    free(handle);
}

void DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level) {
    if (!handle) return;
    handle->config.log_level = level;
    DoIP_Log_SetLevel(level);
}

void DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable) {
    if (!handle || !handle->is_initialized) return;
    doip_udp_set_periodic_announce(enable);
}