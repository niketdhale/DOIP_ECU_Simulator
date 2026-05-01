#include "core/doip_api.h"
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