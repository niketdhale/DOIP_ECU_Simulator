/**
 * @file fuzz_tcp_stream.c
 * @brief Fuzz target: the TCP server's stream handling, end to end.
 *
 * Drives process_client_frame() — header validation, reassembly across
 * partial reads, routing activation, diagnostic dispatch into the UDS layer
 * (and ISO-TP when built with -DDOIP_ENABLE_ISO_TP=true) — through a fake
 * I/O vtable instead of a socket.
 *
 * Input: one flags byte, then chunks as [length][bytes...]. Each chunk is
 * what one readable event delivers, so frames arrive split and coalesced in
 * arbitrary ways.
 *   flags bit 0: fault injection - hold UDS responses back (pending / busy paths)
 *
 * transport/doip_tcp.c is included directly to reach its static functions.
 */
#include "../../transport/doip_tcp.c"
#include "fuzz.h"

#define HDR(pt, len)  DOIP_PROTOCOL_VERSION, DOIP_INVERSE_VERSION, (pt) >> 8, (pt) & 0xFF, 0, 0, 0, (len)
#define ACTIVATE      HDR(0x0005, 7), 0x0E,0x00, 0x00, 0,0,0,0
#define DIAG(n)       HDR(0x8001, 4 + (n)), 0x0E,0x00, 0x10,0x03

const struct fuzz_seed fuzz_seeds[] = {
    FUZZ_SEED(0, 15, ACTIVATE, 15, DIAG(3), 0x22,0xF1,0x90),
    FUZZ_SEED(0, 15, ACTIVATE, 14, DIAG(2), 0x3E,0x00, 14, DIAG(2), 0x10,0x03, 14, DIAG(2), 0x27,0x01),
    FUZZ_SEED(0, 29, ACTIVATE, DIAG(2), 0x3E,0x80),                      /* two frames, one read   */
    FUZZ_SEED(0, 5, HDR(0x0005, 7), 10, /* rest of the frame */ 0x0E,0x00,0x00,0,0,0,0, 8, HDR(0x0007, 0)),
    FUZZ_SEED(0, 15, ACTIVATE, 8, HDR(0x0008, 0), 8, HDR(0x9999, 0)),
    FUZZ_SEED(0, 15, ACTIVATE, 14, HDR(0x8001, 6), 0x0E,0x00, 0xE4,0x00, 0x3E,0x00),   /* functional */
    FUZZ_SEED(0, 15, ACTIVATE, 15, DIAG(3), 0x02,0x3E,0x00, 20, DIAG(8), 0x10,0x09,0x22,0xF1,0x90,0xF1,0x8C,0xF1,
              16, DIAG(4), 0x21,0x86,0xF1,0x82),                         /* ISO-TP framed          */
    FUZZ_SEED(0, 8, 0xFF,0x00,0x00,0x05,0xFF,0xFF,0xFF,0xFF),             /* absurd length          */
    FUZZ_SEED(1, 15, ACTIVATE, 15, DIAG(3), 0x22,0xF1,0x90, 14, DIAG(2), 0x3E,0x00),   /* delayed + busy */
    FUZZ_SEED(1, 15, ACTIVATE, 15, DIAG(3), 0x02,0x3E,0x00, 15, DIAG(3), 0x02,0x3E,0x00),
};
const size_t fuzz_seed_count = sizeof(fuzz_seeds) / sizeof(fuzz_seeds[0]);

/* ── Fake I/O: reads come from the current chunk, writes are discarded ── */
static const uint8_t *s_chunk;
static size_t         s_chunk_len;

static ssize_t fake_read(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    if (s_chunk_len == 0) { errno = EAGAIN; return -1; }
    size_t n = len < s_chunk_len ? len : s_chunk_len;
    memcpy(buf, s_chunk, n);
    s_chunk += n;
    s_chunk_len -= n;
    return (ssize_t)n;
}
static ssize_t fake_write(void *ctx, const void *buf, size_t len) { (void)ctx; (void)buf; return (ssize_t)len; }
static int     fake_getfd(void *ctx) { (void)ctx; return -1; }
static void    fake_close(void *ctx) { (void)ctx; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static struct DoIP_Context inst;           /* one simulator instance, no sockets */
    DoIP_Log_Init(0, 0, NULL);
    if (size == 0) return 0;

    memset(&inst, 0, sizeof(inst));
    inst.tcp_fd = inst.udp_fd = -1;
    inst.config.logical_address    = DOIP_ECU_LOGICAL_ADDRESS;
    inst.config.functional_address = DOIP_FUNCTIONAL_LOGICAL_ADDRESS;
    inst.config.port               = DOIP_TCP_PORT;
    inst.config.faults.response_delay_ms = (data[0] & 0x01) ? 60000U : 0U;
    memcpy(inst.identity.vin, "FUZZVIN0000000001", DOIP_VIN_LENGTH);
    doip_uds_init(&inst);

    doip_client_t *client = &inst.clients[0];
    client->in_use = true;
    client->fd     = -1;
    client->state  = DOIP_TCP_STATE_CONNECTED;
    client->io     = (doip_io_t){ fake_read, fake_write, fake_getfd, fake_close, NULL };
    client->uds_ctx.ecu              = &inst;
    client->uds_ctx.current_session  = UDS_ECU_DEFAULT_SESSION;
    client->uds_ctx.last_activity_ms = get_time_ms();

    for (size_t pos = 1; pos < size; ) {
        size_t len = data[pos++];
        if (len > size - pos) len = size - pos;
        s_chunk     = data + pos;
        s_chunk_len = len;
        pos += len;
        while (s_chunk_len > 0) {              /* one call per readable event */
            if (process_client_frame(&inst, client) < 0) return 0;   /* server would close here */
            FUZZ_ASSERT(client->rx_bytes <= sizeof(client->rx_buf));
        }
    }
    return 0;
}
