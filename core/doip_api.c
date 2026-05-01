#include "core/doip_api.h"
#include "core/doip_log.h"       /* Required for LOG_ERROR/INFO */
#include "transport/doip_udp.h"

/* Global callbacks */
DoIP_RxIndication g_doip_rx_cb = NULL;
DoIP_TxConfirmation g_doip_tx_cb = NULL;

void DoIP_Init(const void* config) {
    (void)config;
    doip_udp_init();
}

void DoIP_DeInit(void) {
    doip_udp_deinit();
}

void DoIP_MainFunction(void) {
    /* Cyclic poll - should be called from main loop or scheduler */
    doip_udp_poll(g_doip_rx_cb);
}

Std_ReturnType DoIP_SendVehicleAnnounce(const doip_vehicle_announce_t* announce) {
    DOIP_DEV_ERROR_CHECK(announce != NULL, 0x01);
    return (doip_send_vehicle_announce(announce) == 0) ? E_OK : E_NOT_OK;
}

Std_ReturnType DoIP_RegisterRxCallback(DoIP_RxIndication cb) {
    DOIP_DEV_ERROR_CHECK(cb != NULL, 0x02);
    g_doip_rx_cb = cb;
    return E_OK;
}

/* ===== Configuration Validation Implementation ===== */
int DoIP_Config_Validate(void) {
    /* 1. Validate Logical Addresses (ISO 13400-2 Table 16) */
    /* Valid: 0x0001-0x0DFF or 0x8000-0xDFFF. 0x0000, 0x0F00-0x7FFF, 0xE000-0xFFFF are reserved. */
    uint16_t ecu_addr = DOIP_ECU_LOGICAL_ADDRESS;
    
    /* ISO 13400-2 Table 16 Valid Ranges */
    bool addr_valid = ((ecu_addr >= 0x0001 && ecu_addr <= 0x0DFF) ||  /* OEM Specific */
                       (ecu_addr >= 0x1000 && ecu_addr <= 0x7FFF) || /* Manufacturer Specific */
                       (ecu_addr >= 0x8000 && ecu_addr <= 0xDFFF));  /* Functional */
                       
    if (!addr_valid) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: DOIP_ECU_LOGICAL_ADDRESS 0x%04X is invalid.", ecu_addr);
        return -1;
    }

    /* 2. Validate Ports */
    if (DOIP_TCP_PORT == 0 || DOIP_UDP_PORT == 0) {
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Port numbers cannot be 0.");
        return -1;
    }

    /* 3. Validate Payload Size */
    if (DOIP_MAX_PAYLOAD_SIZE < 8) { /* Must be larger than header */
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: DOIP_MAX_PAYLOAD_SIZE must be > 8.");
        return -1;
    }

    /* 4. Validate Timeouts */
    if (DOIP_ROUTING_ACTIVATION_TIMEOUT_MS < 1000) { /* Min 1s per spec */
        LOG_ERROR(DOIP_LOG_MODULE_CORE, "Config Error: Timeout too short (< 1000ms).");
        return -1;
    }

    LOG_INFO(DOIP_LOG_MODULE_CORE, "Configuration validation passed.");
    return 0;
}