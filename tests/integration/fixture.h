/**
 * @file fixture.h
 * @brief Shared helpers for the feature integration tests: an in-process
 *        simulator on a background thread, and raw DoIP frame I/O on a plain
 *        TCP socket (tester address 0x0E00).
 *
 * Everything is `static inline` so each test binary only pays for what it uses.
 */
#ifndef TEST_FIXTURE_H
#define TEST_FIXTURE_H

#include "doip_api.h"
#include "core/doip_frame.h"
#include "core/doip_types.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Server thread ─────────────────────────────────────────────────── */

static DoIP_Handle_t *fx_server;
static pthread_t      fx_thread;
static volatile int   fx_running;

static inline void *fx_tick_loop(void *arg)
{
    (void)arg;
    while (fx_running) { DoIP_Tick(fx_server); usleep(1000); }
    return NULL;
}

/** Start a simulator with @p cfg. Returns DoIP_Init()'s result (0 = running). */
static inline int fx_start(const DoIP_Config_t *cfg)
{
    fx_server = DoIP_Create();
    int rc = DoIP_Init(fx_server, cfg);
    if (rc != 0) { DoIP_Destroy(fx_server); fx_server = NULL; return rc; }
    fx_running = 1;
    pthread_create(&fx_thread, NULL, fx_tick_loop, NULL);
    usleep(60000);                     /* let it bind and enter its poll loop */
    return 0;
}

static inline void fx_stop(void)
{
    if (!fx_server) return;
    fx_running = 0;
    pthread_join(fx_thread, NULL);
    DoIP_Destroy(fx_server);
    fx_server = NULL;
    usleep(20000);                     /* brief settle before the port is re-bound */
}

/* ── Frames ────────────────────────────────────────────────────────── */

/** Build header + payload into @p out; returns the frame length. */
static inline size_t fx_build_frame(uint8_t *out, uint16_t pt, const void *pl, uint32_t plen)
{
    doip_header_t hdr = { DOIP_PROTOCOL_VERSION, DOIP_INVERSE_VERSION, pt, plen };
    doip_serialize_header(&hdr, out, DOIP_HEADER_SIZE);
    if (plen) memcpy(out + DOIP_HEADER_SIZE, pl, plen);
    return DOIP_HEADER_SIZE + plen;
}

/** Build a diagnostic message (tester 0x0E00 -> @p target) into @p out; returns the frame length. */
static inline size_t fx_build_diag(uint8_t *out, uint16_t target, const void *data, uint16_t len)
{
    uint8_t pl[4 + 64] = { 0x0E, 0x00, (uint8_t)(target >> 8), (uint8_t)target };
    memcpy(pl + 4, data, len);
    return fx_build_frame(out, DOIP_PT_DIAGNOSTIC_MSG, pl, 4U + len);
}

/* ── Plain TCP tester ──────────────────────────────────────────────── */

static inline void fx_set_timeout(int fd, int timeout_ms)
{
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static inline int fx_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(DOIP_TCP_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    fx_set_timeout(fd, 2000);
    return fd;
}

/**
 * Read exactly one DoIP frame. Returns the payload length (payload in @p buf,
 * type in @p pt), or -1 on timeout / close / malformed header.
 */
static inline int fx_recv_frame(int fd, uint16_t *pt, uint8_t *buf, size_t buf_size)
{
    uint8_t raw[DOIP_HEADER_SIZE];
    doip_header_t hdr;
    if (recv(fd, raw, sizeof(raw), MSG_WAITALL) != (ssize_t)sizeof(raw)) return -1;
    if (doip_deserialize_header(raw, sizeof(raw), &hdr) < 0 || hdr.payload_length > buf_size) return -1;
    if (hdr.payload_length &&
        recv(fd, buf, hdr.payload_length, MSG_WAITALL) != (ssize_t)hdr.payload_length) return -1;
    *pt = hdr.payload_type;
    return (int)hdr.payload_length;
}

/** Routing activation as tester 0x0E00. Returns the response code, or -1. */
static inline int fx_activate(int fd)
{
    const uint8_t req[7] = { 0x0E, 0x00, 0x00, 0, 0, 0, 0 };
    uint8_t frame[DOIP_HEADER_SIZE + sizeof(req)], resp[16];
    uint16_t pt = 0;
    send(fd, frame, fx_build_frame(frame, DOIP_PT_ROUTING_ACT_REQ, req, sizeof(req)), 0);
    if (fx_recv_frame(fd, &pt, resp, sizeof(resp)) != 9 || pt != DOIP_PT_ROUTING_ACT_RES) return -1;
    return resp[4];
}

static inline void fx_send_diag(int fd, uint16_t target, const void *data, uint16_t len)
{
    uint8_t frame[DOIP_HEADER_SIZE + 4 + 64];
    send(fd, frame, fx_build_diag(frame, target, data, len), 0);
}

/**
 * Next diagnostic message from the ECU, skipping DoIP ACKs (counted in
 * @p acks if non-NULL). Returns the length of the data after the two
 * addresses (copied to @p buf), or -1 on timeout / close / other frame type.
 */
static inline int fx_recv_diag(int fd, uint8_t *buf, size_t buf_size, int *acks)
{
    uint8_t pl[DOIP_MAX_PAYLOAD_SIZE];
    uint16_t pt = 0;
    for (;;) {
        int n = fx_recv_frame(fd, &pt, pl, sizeof(pl));
        if (n < 0) return -1;
        if (pt == DOIP_PT_DIAGNOSTIC_ACK) { if (acks) (*acks)++; continue; }
        if (pt != DOIP_PT_DIAGNOSTIC_MSG || n < 4 || (size_t)(n - 4) > buf_size) return -1;
        memcpy(buf, pl + 4, (size_t)(n - 4));
        return n - 4;
    }
}

#endif /* TEST_FIXTURE_H */
