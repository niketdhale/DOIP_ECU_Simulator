/**
 * @file fuzz_uds.c
 * @brief Fuzz target: the UDS service layer (transport/doip_uds.c).
 *
 * Input: one flags byte, then requests as [length hi][length lo][bytes...].
 * All requests of one input run on the same client context, so multi-step
 * flows (session change -> seed -> key -> write) are reachable.
 *   flags bit 0: start in the extended session
 *   flags bit 1: start with Security Access unlocked
 */
#include "fuzz.h"
#include "transport/doip_uds.h"
#include "core/doip_context.h"
#include "core/doip_log.h"
#include <string.h>
#include <time.h>

#define VIN_DID     0xF1,0x90
#define VIN_DID_4   VIN_DID, VIN_DID, VIN_DID, VIN_DID
#define VIN_DID_16  VIN_DID_4, VIN_DID_4, VIN_DID_4, VIN_DID_4
#define VIN_DID_64  VIN_DID_16, VIN_DID_16, VIN_DID_16, VIN_DID_16
#define SEED_KEY    0x00,0x02, 0x27,0x01, 0x00,0x06, 0x27,0x02,0x00,0x00,0x00,0x00

const struct fuzz_seed fuzz_seeds[] = {
    /* 215 VIN DIDs: the response (4086 bytes) is just under the limit, so one
     * more DID from a mutation must be refused (NRC 0x14), not overflow.
     * Length FFFF = "the rest of the input". */
    FUZZ_SEED(0x00, 0xFF,0xFF, 0x22, VIN_DID_64, VIN_DID_64, VIN_DID_64, VIN_DID_16, VIN_DID_4,
              VIN_DID, VIN_DID, VIN_DID),
    /* three wrong keys -> lockout, then a seed request during the lockout */
    FUZZ_SEED(0x01, SEED_KEY, SEED_KEY, SEED_KEY, 0x00,0x02, 0x27,0x01),
    FUZZ_SEED(0x00, 0x00,0x02, 0x10,0x03, 0x00,0x02, 0x27,0x01, 0x00,0x06, 0x27,0x02,0x00,0x00,0x00,0x00),
    FUZZ_SEED(0x00, 0x00,0x03, 0x22,0xF1,0x90, 0x00,0x07, 0x22,0xF1,0x90,0xF1,0x8C,0xF1,0x86),
    FUZZ_SEED(0x03, 0x00,0x14, 0x2E,0xF1,0x90, 'F','U','Z','Z','V','I','N','0','0','0','0','0','0','0','0','0','1'),
    FUZZ_SEED(0x03, 0x00,0x06, 0x2E,0xF1,0x8C,'A','B','C', 0x00,0x05, 0x2E,0xF1,0x86,'N','M'),
    FUZZ_SEED(0x00, 0x00,0x03, 0x19,0x02,0xFF, 0x00,0x02, 0x19,0x0A, 0x00,0x04, 0x14,0xFF,0xFF,0xFF, 0x00,0x03, 0x19,0x01,0x08),
    FUZZ_SEED(0x01, 0x00,0x04, 0x31,0x01,0xFF,0x00, 0x00,0x02, 0x11,0x01, 0x00,0x02, 0x3E,0x80, 0x00,0x02, 0x3E,0x00),
    FUZZ_SEED(0x01, 0x00,0x03, 0x28,0x00,0x01, 0x00,0x02, 0x85,0x02, 0x00,0x01, 0x99),
};
const size_t fuzz_seed_count = sizeof(fuzz_seeds) / sizeof(fuzz_seeds[0]);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const DoIP_EcuIdentity_t identity = {
        .vin = "FUZZVIN0000000001", .software_version = "V0.0.1",
        .system_name = "Fuzz ECU", .serial_number = "SN-FUZZ",
    };
    static struct DoIP_Context ecu;            /* only its UDS-related parts are used */
    DoIP_Log_Init(0, 0, NULL);                 /* silent */
    ecu.identity = identity;                   /* fresh identity and DTC memory per input */
    doip_uds_init(&ecu);
    if (size == 0) return 0;

    UdsClientContext_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ecu = &ecu;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx.last_activity_ms  = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    ctx.current_session   = (data[0] & 0x01) ? UDS_SESSION_EXTENDED_STATE : UDS_SESSION_DEFAULT_STATE;
    ctx.security_unlocked = (data[0] & 0x02) != 0;

    for (size_t pos = 1; pos + 2 <= size; ) {
        size_t len = ((size_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (len > size - pos) len = size - pos;

        uint8_t  res[DOIP_MAX_PAYLOAD_SIZE];
        uint16_t res_len = 0;
        int rc = doip_uds_process_request(&ctx, data + pos, (uint16_t)len, res, &res_len);
        FUZZ_ASSERT(res_len <= UDS_MAX_RESPONSE_LEN || res_len <= len);  /* echo fallback mirrors the request */
        FUZZ_ASSERT(res_len <= sizeof(res));
        if (rc != 0 && res_len != 0) FUZZ_ASSERT(res_len == 3 && res[0] == 0x7F);
        pos += len;
    }
    return 0;
}
