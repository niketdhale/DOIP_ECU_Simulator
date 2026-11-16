#include "doip_api.h"
#include "core/doip_context.h"
#include "core/doip_log.h"
#include "state/doip_fsm.h"
#include "transport/doip_udp.h"
#include "transport/doip_tcp.h"
#include "config/doip_config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/*
 * Every instance (handle) owns its sockets, clients, identity and UDS state:
 * see struct DoIP_Context in core/doip_context.h. Only the logger is shared
 * by the whole process.
 */

/* Built-in default ECU identity (mirrors the previously hardcoded values) */
static const DoIP_EcuIdentity_t g_default_identity = {
    .vin              = "WBAXXXXXXXXXXXXXX",
    .software_version = "V1.0.0",
    .system_name      = "DoIP ECU Simulator",
    .serial_number    = "ECU123456789",
    .eid              = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA },
    .gid              = { 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB },
    .further_action   = 0x00,
    .vin_gw_sync_status = 0x00,
};

static const DoIP_Config_t g_default_config = {
    .log_level              = DOIP_LOG_LEVEL_INFO,
    .log_file_path          = NULL,
    .s3_server_timeout_ms   = 5000,
    /* callbacks, addresses, port, faults and .tls default to zero / NULL */
};

/* The logger is process-wide: the first initialised instance opens it with its
 * level and log file, later instances share it, the last one to go closes it. */
static pthread_mutex_t g_log_users_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_log_users       = 0;

static int log_acquire(const DoIP_Config_t *cfg) {
    int rc = 0;
    pthread_mutex_lock(&g_log_users_mutex);
    if (g_log_users == 0)
        rc = DoIP_Log_Init(cfg->log_level, DOIP_LOG_MODULE_ALL, cfg->log_file_path);
    if (rc == 0) g_log_users++;
    pthread_mutex_unlock(&g_log_users_mutex);
    return rc;
}

static void log_release(void) {
    pthread_mutex_lock(&g_log_users_mutex);
    if (--g_log_users == 0) DoIP_Log_DeInit();
    pthread_mutex_unlock(&g_log_users_mutex);
}

static int DoIP_Config_Validate(const DoIP_Config_t *cfg) {
    uint16_t ecu_addr = cfg->logical_address;
    bool addr_valid = ((ecu_addr >= 0x0001 && ecu_addr <= 0x0DFF) ||
                       (ecu_addr >= 0x1000 && ecu_addr <= 0x7FFF) ||
                       (ecu_addr >= 0x8000 && ecu_addr <= 0xDFFF));
    if (!addr_valid) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: ECU logical address 0x%04X is invalid.", ecu_addr); return -1; }
    if (cfg->functional_address == ecu_addr) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: functional address 0x%04X equals the ECU logical address.", ecu_addr); return -1; }
    if (cfg->functional_address < 0xE000 || cfg->functional_address > 0xEFFF)
        LOG_WARN(DOIP_LOG_MODULE_CORE, "Functional address 0x%04X is outside the ISO 13400-2 functional group range (0xE000-0xEFFF).", cfg->functional_address);
    if (DOIP_MAX_PAYLOAD_SIZE < 8) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Payload size too small."); return -1; }
    if (DOIP_ROUTING_ACTIVATION_TIMEOUT_MS < 1000) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Timeout too short."); return -1; }
    LOG_INFO(DOIP_LOG_MODULE_CORE, "Configuration validation passed.");
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
    handle->tcp_fd = -1;
    handle->udp_fd = -1;
    handle->announce_enabled = DOIP_ENABLE_PERIODIC_ANNOUNCE;
    pthread_mutex_init(&handle->lock, NULL);
    return handle;
}

int DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config) {
    if (!handle) return -1;
    pthread_mutex_lock(&handle->lock);
    int ret = 0;
    if (handle->is_initialized) { ret = -5; goto unlock; }

    if (config) {
        if (config->log_level > 0) handle->config.log_level = config->log_level;
        if (config->log_file_path) handle->config.log_file_path = config->log_file_path;
        if (config->s3_server_timeout_ms > 0) handle->config.s3_server_timeout_ms = config->s3_server_timeout_ms;
        if (config->on_state_change) handle->config.on_state_change = config->on_state_change;
        if (config->on_rx_message) handle->config.on_rx_message = config->on_rx_message;
        if (config->user_context) handle->config.user_context = config->user_context;
        if (config->on_uds_request) handle->config.on_uds_request = config->on_uds_request;
        /* Async callbacks */
        if (config->on_client_connect)    handle->config.on_client_connect    = config->on_client_connect;
        if (config->on_client_disconnect) handle->config.on_client_disconnect = config->on_client_disconnect;
        if (config->on_frame_received)    handle->config.on_frame_received    = config->on_frame_received;
        handle->config.logical_address = config->logical_address;
        handle->config.functional_address = config->functional_address;
        handle->config.port            = config->port;
        handle->config.faults          = config->faults;
#if DOIP_ENABLE_TLS
        handle->config.tls             = config->tls;   /* paths are borrowed: keep them valid */
#endif
    }

    /* 0 = compiled-in default */
    if (!handle->config.logical_address)    handle->config.logical_address    = DOIP_ECU_LOGICAL_ADDRESS;
    if (!handle->config.functional_address) handle->config.functional_address = DOIP_FUNCTIONAL_LOGICAL_ADDRESS;
    if (!handle->config.port)               handle->config.port               = DOIP_TCP_PORT;

    /* Resolve ECU identity: use caller-supplied struct or built-in defaults */
    if (config && config->ecu_identity)
        handle->identity = *config->ecu_identity;
    else
        handle->identity = g_default_identity;

    if (log_acquire(&handle->config) != 0) { ret = -2; goto unlock; }
    LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Initializing with S3=%u ms", handle->config.s3_server_timeout_ms);
    if (DoIP_Config_Validate(&handle->config) != 0) { LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config validation failed"); log_release(); ret = -3; goto unlock; }
    if (DoIP_Fsm_Init(handle) != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "FSM init failed"); log_release(); ret = -4; goto unlock;
    }

    handle->is_initialized = true;
unlock:
    pthread_mutex_unlock(&handle->lock);
    return ret;
}

int DoIP_SetFaults(DoIP_Handle_t *handle, const DoIP_FaultConfig_t *faults) {
    if (!handle) return -1;
    int ret = -1;
    pthread_mutex_lock(&handle->lock);
    if (handle->is_initialized) {
        if (faults) handle->config.faults = *faults;
        else        memset(&handle->config.faults, 0, sizeof(handle->config.faults));
        ret = 0;
    }
    pthread_mutex_unlock(&handle->lock);
    return ret;
}

int DoIP_SetDid(DoIP_Handle_t *handle, uint16_t did, const void *data, uint16_t len, bool writable) {
    if (!handle) return -1;
    pthread_mutex_lock(&handle->lock);
    int ret = doip_uds_set_did(handle, did, data, len, writable);
    pthread_mutex_unlock(&handle->lock);
    return ret;
}

void DoIP_Tick(DoIP_Handle_t *handle) {
    if (!handle || !handle->is_initialized) return;
    pthread_mutex_lock(&handle->lock);

    uint32_t now = api_get_ms();
    DoIP_Fsm_MainFunction(handle);
    doip_udp_tick(handle, now); /* Drives periodic announcements */

    pthread_mutex_unlock(&handle->lock);
}

void DoIP_DeInit(DoIP_Handle_t *handle) {
    if (!handle) return;
    pthread_mutex_lock(&handle->lock);
    if (handle->is_initialized) {
        LOG_INFO(DOIP_LOG_MODULE_CORE, "DoIP API: Deinitializing...");
        DoIP_Fsm_DeInit(handle);
        log_release();
        handle->is_initialized = false;
    }
    pthread_mutex_unlock(&handle->lock);
}

void DoIP_Destroy(DoIP_Handle_t *handle) {
    if (!handle) return;
    DoIP_DeInit(handle);
    pthread_mutex_destroy(&handle->lock);
    free(handle);
}

void DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level) {
    if (!handle) return;
    pthread_mutex_lock(&handle->lock);
    handle->config.log_level = level;
    DoIP_Log_SetLevel(level);   /* the logger is shared by all instances */
    pthread_mutex_unlock(&handle->lock);
}

void DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable) {
    if (!handle) return;
    pthread_mutex_lock(&handle->lock);
    handle->announce_enabled = enable;
    if (enable) {                          /* a fresh burst, starting on the next tick */
        handle->announcement_count = 0;
        handle->last_announce_ms   = 0;
    }
    pthread_mutex_unlock(&handle->lock);
}
