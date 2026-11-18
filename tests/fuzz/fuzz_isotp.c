/**
 * @file fuzz_isotp.c
 * @brief Fuzz target: ISO-TP reassembly and segmentation (transport/doip_isotp.c).
 *
 * Input: N-PDUs as [length][bytes...], fed to one reassembly context like a
 * tester's diagnostic messages. Whatever the first PDU contains is also
 * segmented and reassembled again, which must give the same bytes back.
 */
#include "fuzz.h"
#include "transport/doip_isotp.h"
#include "core/doip_log.h"
#include <string.h>

const struct fuzz_seed fuzz_seeds[] = {
    FUZZ_SEED(0x03, 0x02,0x3E,0x00),
    FUZZ_SEED(0x08, 0x10,0x09,0x22,0xF1,0x90,0xF1,0x8C,0xF1, 0x04, 0x21,0x86,0xF1,0x82),
    FUZZ_SEED(0x08, 0x10,0x14,1,2,3,4,5,6, 0x08, 0x21,7,8,9,10,11,12,13, 0x08, 0x22,14,15,16,17,18,19,20),
    FUZZ_SEED(0x03, 0x30,0x00,0x00, 0x04, 0x21,0x01,0x02,0x03),
    FUZZ_SEED(0x08, 0x1F,0xFF,1,2,3,4,5,6, 0x08, 0x21,1,2,3,4,5,6,7),
};
const size_t fuzz_seed_count = sizeof(fuzz_seeds) / sizeof(fuzz_seeds[0]);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static doip_isotp_ctx_t rx, loop;          /* ~4 KiB each: keep them off the stack */
    static uint8_t out[DOIP_ISOTP_MAX_PDU], back[DOIP_ISOTP_MAX_PDU];
    static uint8_t segs[1 + DOIP_ISOTP_MAX_PDU / 7][8];
    DoIP_Log_Init(0, 0, NULL);
    doip_isotp_reset(&rx);

    for (size_t pos = 0; pos < size; ) {
        size_t len = data[pos++];
        if (len > size - pos) len = size - pos;
        uint8_t fc[8], fc_len = 0;
        int n = doip_isotp_rx(&rx, data + pos, (uint16_t)len, out, sizeof(out), fc, &fc_len);
        FUZZ_ASSERT(n <= (int)sizeof(out));
        FUZZ_ASSERT(fc_len <= sizeof(fc));
        if (n < 0) doip_isotp_reset(&rx);      /* the server closes the connection here */

        /* Round trip: segment these bytes, reassemble, compare */
        if (pos == 1 && len > 0) {
            int count = doip_isotp_tx(&loop, data + pos, (uint16_t)len, segs, (int)(sizeof(segs) / sizeof(segs[0])));
            FUZZ_ASSERT(count > 0);
            doip_isotp_reset(&loop);
            int total = 0;
            for (int i = 0; i < count; i++) {
                total = doip_isotp_rx(&loop, segs[i], 8, back, sizeof(back), NULL, NULL);
                FUZZ_ASSERT(total >= 0);
                FUZZ_ASSERT(total == 0 || i == count - 1);
            }
            FUZZ_ASSERT(total == (int)len && memcmp(back, data + pos, len) == 0);
        }
        pos += len;
    }
    return 0;
}
