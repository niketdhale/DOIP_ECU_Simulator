#ifndef DOIP_API_H
#define DOIP_API_H

#include <stdint.h>
#include <stdio.h>
#include "config/doip_config.h"
#include "core/doip_types.h"

/* AUTOSAR-compatible standard return type (portable) */
typedef uint8_t Std_ReturnType;
#define E_OK       0x00U
#define E_NOT_OK   0x01U

/* Global callback pointers (defined in doip_api.c or main) */
extern DoIP_RxIndication g_doip_rx_cb;
extern DoIP_TxConfirmation g_doip_tx_cb;

/* ===== Standard AUTOSAR BSW Module API ===== */
void DoIP_Init(const void* config);
void DoIP_DeInit(void);
void DoIP_MainFunction(void);

/* ===== Service APIs ===== */
Std_ReturnType DoIP_SendVehicleAnnounce(const doip_vehicle_announce_t* announce);
Std_ReturnType DoIP_RegisterRxCallback(DoIP_RxIndication cb);

/* ===== Development Error Detection (portable stub) ===== */
#if DOIP_DEV_ERROR_DETECT
#define DOIP_MODULE_ID  100U
#define Det_ReportError(mod, inst, api, err) \
    fprintf(stderr, "[DoIP DevError] Mod:%u API:%u Code:%u\n", mod, api, err)
#define DOIP_DEV_ERROR_CHECK(_cond, _err) \
    do { if (!(_cond)) { Det_ReportError(DOIP_MODULE_ID, 0, __LINE__, _err); return E_NOT_OK; } } while(0)
#else
#define DOIP_DEV_ERROR_CHECK(_cond, _err) do { if (!(_cond)) return E_NOT_OK; } while(0)
#endif

#endif /* DOIP_API_H */