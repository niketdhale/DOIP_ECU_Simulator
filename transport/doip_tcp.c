/**
 * @file doip_tcp.c
 * @brief DoIP TCP server — routing activation, diagnostic messaging, alive check.
 *
 * Changes from baseline:
 *  - I/O abstraction: all per-client read/write goes through doip_io_t vtable.
 *  - Non-blocking partial-frame reassembly buffer per client; SO_RCVTIMEO removed.
 *  - IPv6 dual-stack socket when DOIP_ENABLE_IPV6=true.
 *  - ISO-TP framing hook when DOIP_ENABLE_ISO_TP=true.
 *  - TLS accept path when DOIP_ENABLE_TLS=true.
 *  - Async event callbacks (connect / disconnect / frame-received).
 */

#include "transport/doip_tcp.h"
#include "core/doip_context.h"
#include "state/doip_fsm.h"
#include "transport/doip_io.h"
#include "core/doip_frame.h"
#include "core/doip_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static uint32_t get_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief Send a DoIP frame through the client's I/O vtable.
 *
 * Builds the wire buffer (header + payload) and writes it via io->write().
 * Returns total bytes sent on success, -1 on error.
 */
static ssize_t client_send_frame(doip_client_t *c, uint16_t ptype,
                                  const void *payload, uint32_t plen)
{
    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr;
    hdr.protocol_version = DOIP_PROTOCOL_VERSION;
    hdr.inverse_version  = DOIP_INVERSE_VERSION;
    hdr.payload_type     = ptype;
    hdr.payload_length   = plen;
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) {
        if (plen > DOIP_MAX_PAYLOAD_SIZE) return -1;
        memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);
    }
    size_t total = DOIP_HEADER_SIZE + plen;
    size_t sent  = 0;
    while (sent < total) {
        ssize_t n = c->io.write(c->io.ctx, tx_buf + sent, total - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/** Send a Diagnostic Message (ECU -> tester) carrying @p data. */
static void client_send_diag(DoIP_Handle_t *h, doip_client_t *c, uint16_t tester_addr,
                              const uint8_t *data, uint16_t len)
{
    uint8_t buf[4 + DOIP_MAX_PAYLOAD_SIZE];
    if ((uint32_t)len + 4U > DOIP_MAX_PAYLOAD_SIZE) return;
    put_be16(&buf[0], h->config.logical_address);
    put_be16(&buf[2], tester_addr);
    memcpy(&buf[4], data, len);
    client_send_frame(c, DOIP_PT_DIAGNOSTIC_MSG, buf, 4U + len);
}

/**
 * Send a UDS response (ECU -> tester). With ISO-TP enabled it is framed into
 * 8-byte N-PDUs, one DoIP diagnostic message each: an SF immediately, or an FF
 * after which the CFs are paced by the tester's Flow Control (see isotp_tx_fc()
 * and isotp_tx_tick()).
 */
static void client_send_uds(DoIP_Handle_t *h, doip_client_t *c, uint16_t tester_addr,
                             const uint8_t *data, uint16_t len)
{
#if DOIP_ENABLE_ISO_TP
    doip_isotp_ctx_t *x = &c->isotp_ctx;
    uint8_t seg[1][8];
    if (len <= DOIP_ISOTP_SF_MAX) {
        if (doip_isotp_tx(x, data, len, seg, 1) == 1) client_send_diag(h, c, tester_addr, seg[0], 8);
        return;
    }
    if (len > DOIP_ISOTP_MAX_PDU || x->tx_active) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "ISO-TP TX: response (%u bytes) dropped", len);
        return;
    }
    memcpy(x->tx_buf, data, len);
    x->tx_total = len; x->tx_sent = 6U; x->tx_sn = 1U;
    x->tx_active = 1U; x->tx_wait_fc = 1U;
    x->tx_deadline_ms = get_time_ms() + DOIP_ISOTP_N_BS_MS;
    memset(seg[0], 0xCC, 8);
    seg[0][0] = (uint8_t)(ISOTP_PCI_FF | (len >> 8)); seg[0][1] = (uint8_t)len;
    memcpy(&seg[0][2], data, 6);
    client_send_diag(h, c, tester_addr, seg[0], 8);
#else
    client_send_diag(h, c, tester_addr, data, len);
#endif
}

#if DOIP_ENABLE_ISO_TP
static void isotp_tx_abort(doip_client_t *c, const char *why)
{
    LOG_WARN(DOIP_LOG_MODULE_TCP, "ISO-TP TX aborted (fd=%d): %s", c->fd, why);
    c->isotp_ctx.tx_active = 0;
}

/** Tester's Flow Control frame for the TX in progress. */
static void isotp_tx_fc(doip_client_t *c, const uint8_t *fc, uint16_t len)
{
    doip_isotp_ctx_t *x = &c->isotp_ctx;
    if (!x->tx_active || !x->tx_wait_fc || len < 3U) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "ISO-TP: unexpected FC ignored (fd=%d)", c->fd);
        return;
    }
    uint32_t now = get_time_ms();
    switch (fc[0] & 0x0FU) {
    case ISOTP_FC_CTS:
        x->tx_bs = fc[1]; x->tx_bs_left = fc[1];
        /* 0x00-0x7F ms; 0xF1-0xF9 = 100-900 us (rounded down: ticks are 1 ms);
         * reserved values mean the longest separation, 127 ms */
        x->tx_stmin_ms = fc[2] <= 0x7FU ? fc[2] : (fc[2] >= 0xF1U && fc[2] <= 0xF9U) ? 0U : 0x7FU;
        x->tx_next_ms  = now;
        x->tx_wait_fc  = 0;
        break;
    case ISOTP_FC_WAIT:
        x->tx_deadline_ms = now + DOIP_ISOTP_N_BS_MS;
        break;
    default:   /* Overflow or reserved */
        isotp_tx_abort(c, "tester sent FC overflow");
    }
}

/** Send the CFs that are due; called every tick. */
static void isotp_tx_tick(DoIP_Handle_t *h, doip_client_t *c, uint32_t now_ms)
{
    doip_isotp_ctx_t *x = &c->isotp_ctx;
    if (!x->tx_active) return;
    if (x->tx_wait_fc) {
        if ((int32_t)(now_ms - x->tx_deadline_ms) >= 0) isotp_tx_abort(c, "N_Bs timeout waiting for FC");
        return;
    }
    while (x->tx_active && (int32_t)(now_ms - x->tx_next_ms) >= 0) {
        uint8_t seg[8];
        uint16_t chunk = (uint16_t)(x->tx_total - x->tx_sent);
        if (chunk > 7U) chunk = 7U;
        memset(seg, 0xCC, 8);
        seg[0] = (uint8_t)(ISOTP_PCI_CF | x->tx_sn);
        memcpy(&seg[1], x->tx_buf + x->tx_sent, chunk);
        client_send_diag(h, c, c->tester_logical_addr, seg, 8);
        x->tx_sent = (uint16_t)(x->tx_sent + chunk);
        x->tx_sn   = (uint8_t)((x->tx_sn + 1U) & 0x0FU);
        x->tx_next_ms = now_ms + x->tx_stmin_ms;
        if (x->tx_sent >= x->tx_total) { x->tx_active = 0; return; }
        if (x->tx_bs && --x->tx_bs_left == 0) {
            x->tx_wait_fc = 1U;
            x->tx_deadline_ms = now_ms + DOIP_ISOTP_N_BS_MS;
            return;
        }
        if (x->tx_stmin_ms) return;
    }
}
#endif

static doip_client_t *find_free_client(DoIP_Handle_t *h) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (!h->clients[i].in_use) {
            memset(&h->clients[i], 0, sizeof(doip_client_t));
            h->clients[i].in_use = true;
            return &h->clients[i];
        }
    }
    return NULL;
}

/**
 * @brief Create the client's I/O vtable.  For TLS only the SSL object is
 * created here; the handshake is driven by doip_tcp_poll() (HANDSHAKE state).
 * Returns false if the client must be dropped.
 */
static bool client_setup_io(DoIP_Handle_t *h, doip_client_t *client, int fd)
{
#if DOIP_ENABLE_TLS
    client->hs_ssl = doip_tls_start(&h->tls, fd);
    if (!client->hs_ssl) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "TLS start failed (fd=%d) — dropping", fd);
        return false;
    }
    client->state = DOIP_TCP_STATE_HANDSHAKE;
#else
    (void)h;
    client->io = doip_io_plain(fd);
#endif
    return true;
}

static void close_client(DoIP_Handle_t *h, doip_client_t *client) {
    if (!client || !client->in_use) return;
    int fd = client->fd;
    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client disconnected (fd=%d, ip=%s)",
             fd, client->peer_ip[0] ? client->peer_ip : "?");

#if DOIP_ENABLE_TLS
    if (client->state == DOIP_TCP_STATE_HANDSHAKE) {
        /* Never announced as connected, so no disconnect callback either */
        doip_tls_abort((SSL *)client->hs_ssl);
        memset(client, 0, sizeof(doip_client_t));
        return;
    }
#endif
    /* Notify application before releasing the fd */
    if (h->config.on_client_disconnect)
        h->config.on_client_disconnect(fd, h->config.user_context);

    client->io.close(client->io.ctx);
    memset(client, 0, sizeof(doip_client_t));

    /* Last activated tester gone: the FSM falls back to IDLE */
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++)
        if (h->clients[i].in_use && h->clients[i].state == DOIP_TCP_STATE_ACTIVATED) return;
    DoIP_Fsm_OnTcpDisconnect(h);
}

/* ---------------------------------------------------------------------------
 * Frame processing — non-blocking partial reassembly
 * -------------------------------------------------------------------------*/

/**
 * Process every complete frame currently in client->rx_buf.  Returns 0 when
 * waiting for more data, -1 on error or when the caller should close the client.
 */
static int process_buffered_frames(DoIP_Handle_t *h, doip_client_t *client)
{
    /* --- Step 2: Wait until we have at least a complete header --- */
next_frame:
    if (client->rx_bytes < DOIP_HEADER_SIZE) return 0;

    /* --- Step 3: Parse header and wait for full payload --- */
    doip_header_t hdr;
    if (doip_deserialize_header(client->rx_buf, client->rx_bytes, &hdr) < 0) {
        LOG_WARN(DOIP_LOG_MODULE_TCP,
                 "Invalid header from fd=%d — sending NACK", client->fd);
        uint8_t nack = (uint8_t)DOIP_NACK_INVALID_PATTERN;
        client_send_frame(client, DOIP_PT_GENERIC_NACK, &nack, 1);
        return -1;
    }

    size_t frame_total = DOIP_HEADER_SIZE + hdr.payload_length;
    if (client->rx_bytes < frame_total) return 0; /* Still waiting */

    if (hdr.payload_length > DOIP_MAX_PAYLOAD_SIZE) {
        LOG_WARN(DOIP_LOG_MODULE_TCP, "Payload too large from fd=%d", client->fd);
        return -1;
    }

    /* --- Step 4: We have a complete frame — update activity timestamp --- */
    client->last_activity_ms  = get_time_ms();
    client->alive_check_sent_ms = 0; /* Any valid frame resets pending probe */

    uint8_t *pl = client->rx_buf + DOIP_HEADER_SIZE;

    if (h->config.on_rx_message)
        h->config.on_rx_message(hdr.payload_type, pl, hdr.payload_length,
                                h->config.user_context);

    /* --- Step 5: Optional application frame intercept hook --- */
    if (h->config.on_frame_received) {
        int rc = h->config.on_frame_received(client->fd, hdr.payload_type,
                                             pl, hdr.payload_length,
                                             h->config.user_context);
        if (rc == 0) goto frame_done; /* Application handled it — consume the frame */
    }

    /* --- Step 6: Built-in dispatch --- */
    switch (hdr.payload_type) {
        case DOIP_PT_ROUTING_ACT_REQ: {
            if (hdr.payload_length < sizeof(doip_routing_act_req_t)) goto frame_done;
            const doip_routing_act_req_t *req = (const doip_routing_act_req_t *)pl;
            doip_logical_addr_t tester_addr = ntohs(req->tester_logical_address);
            uint8_t act_type = req->activation_type;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Routing Act Req (fd=%d): Tester=0x%04X, Type=0x%02X",
                      client->fd, tester_addr, act_type);

            /* Re-activation with the same SA on an active socket succeeds again. */
            uint8_t resp_code = DOIP_ACT_SUCCESS;
            if (tester_addr < DOIP_TESTER_ADDR_MIN || tester_addr > DOIP_TESTER_ADDR_MAX)
                resp_code = DOIP_ACT_UNKNOWN_SA;
            else if (client->state == DOIP_TCP_STATE_ACTIVATED &&
                     tester_addr != client->tester_logical_addr)
                resp_code = DOIP_ACT_SA_MISMATCH;
            else if (act_type != 0x00 && act_type != 0x01)
                resp_code = DOIP_ACT_UNSUPPORTED_TYPE;

            doip_routing_act_res_t res; memset(&res, 0, sizeof(res));
            res.tester_logical_address = req->tester_logical_address;
            res.ecu_logical_address    = htons(h->config.logical_address);
            res.activation_code = resp_code;
            if (client_send_frame(client, DOIP_PT_ROUTING_ACT_RES,
                                   &res, sizeof(res)) < 0 ||
                resp_code != DOIP_ACT_SUCCESS) {
                /* ISO 13400-2: a denied activation closes the socket */
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "Routing Activation denied (fd=%d, code=0x%02X)",
                         client->fd, resp_code);
                client->rx_bytes = 0;
                return -1;
            }
            client->state               = DOIP_TCP_STATE_ACTIVATED;
            client->tester_logical_addr = tester_addr;
            LOG_INFO(DOIP_LOG_MODULE_TCP,
                     "Routing Activated for client fd=%d", client->fd);
            DoIP_Fsm_OnRoutingActivation(h, true);
            break;
        }

        case DOIP_PT_DIAGNOSTIC_MSG: {
            if (hdr.payload_length < 4) break;
            uint16_t tester_addr = get_be16(pl);

            uint16_t target_addr = get_be16(pl + 2);
            uint8_t  nack_code   = 0;
            bool     functional  = (target_addr == h->config.functional_address);

            if (client->state != DOIP_TCP_STATE_ACTIVATED ||
                tester_addr != client->tester_logical_addr)
                nack_code = DOIP_DIAG_NACK_INVALID_SA;
            else if (target_addr != h->config.logical_address && !functional)
                nack_code = DOIP_DIAG_NACK_UNKNOWN_TA;

            if (h->config.faults.force_diag_nack) nack_code = h->config.faults.force_diag_nack;

            if (nack_code) {
                uint8_t nack[5];
                put_be16(&nack[0], target_addr);
                put_be16(&nack[2], tester_addr);
                nack[4] = nack_code;
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "Diagnostic Req rejected (fd=%d, code=0x%02X)",
                         client->fd, nack[4]);
                client_send_frame(client, DOIP_PT_DIAGNOSTIC_NACK, nack, sizeof(nack));
                break;
            }
            /* The DoIP ACK confirms receipt of every accepted diagnostic message
             * (with ISO-TP: of every N-PDU) and is sent even when no UDS
             * response follows. Its source is the address the tester targeted;
             * the UDS response itself always comes from the ECU's physical address. */
            uint8_t ack_payload[5];
            put_be16(&ack_payload[0], target_addr);
            put_be16(&ack_payload[2], tester_addr);
            ack_payload[4] = 0x00;
            if (!h->config.faults.drop_diag_ack)
                client_send_frame(client, DOIP_PT_DIAGNOSTIC_ACK,
                                   ack_payload, sizeof(ack_payload));

            uint8_t *uds_req     = pl + 4;
            uint16_t uds_req_len = (uint16_t)(hdr.payload_length - 4);

            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Diagnostic Req (fd=%d): Tester=0x%04X, Target=0x%04X%s, UDS_Len=%u",
                      client->fd, tester_addr, target_addr,
                      functional ? " (functional)" : "", uds_req_len);

#if DOIP_ENABLE_ISO_TP
            if (uds_req_len > 0 && (uds_req[0] & 0xF0U) == ISOTP_PCI_FC) {
                isotp_tx_fc(client, uds_req, uds_req_len);   /* drives our segmented TX */
                break;
            }
            if (client->isotp_ctx.tx_active) {               /* still sending the last response */
                const uint8_t nrc[3] = { 0x7F, (uds_req_len > 1 && !(uds_req[0] & 0xF0U)) ? uds_req[1] : 0,
                                         UDS_NRC_BUSY_REPEAT_REQUEST };
                client_send_uds(h, client, tester_addr, nrc, sizeof(nrc));
                break;
            }
            /* ISO-TP reassembly: feed the DoIP payload, get UDS bytes out */
            uint8_t isotp_buf[4095];
            uint8_t fc_buf[8];
            uint8_t fc_len = 0;
            int rlen = doip_isotp_rx(&client->isotp_ctx, uds_req, uds_req_len,
                                      isotp_buf, sizeof(isotp_buf),
                                      fc_buf, &fc_len);
            if (fc_len > 0)
                client_send_diag(h, client, tester_addr, fc_buf, fc_len);
            if (rlen == 0)  break;  /* Waiting for more consecutive frames  */
            if (rlen <  0) { client->rx_bytes = 0; return -1; } /* ISO-TP error */
            uds_req     = isotp_buf;
            uds_req_len = (uint16_t)rlen;
#endif

            uint8_t  uds_response[DOIP_MAX_PAYLOAD_SIZE];
            uint16_t uds_res_len = 0;
            bool     busy = client->pending_len > 0;
            if (busy) {
                /* Still working on the previous (delayed) request */
                uds_response[0] = 0x7F; uds_response[1] = uds_req_len ? uds_req[0] : 0;
                uds_response[2] = UDS_NRC_BUSY_REPEAT_REQUEST; uds_res_len = 3;
            } else {
                (void)doip_uds_process_request(&client->uds_ctx, uds_req, uds_req_len,
                                                uds_response, &uds_res_len);
            }

            if (uds_res_len > DOIP_MAX_PAYLOAD_SIZE - 4U) {
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "UDS response too long (%u) — dropped", uds_res_len);
                uds_res_len = 0;
            }
            /* Functionally addressed: "not supported" style negative responses
             * stay silent so that only the ECUs that implement the request answer. */
            if (functional && doip_uds_functional_suppresses(uds_response, uds_res_len))
                uds_res_len = 0;

            if (uds_res_len == 0 || h->config.faults.drop_response) break;

            if (busy || h->config.faults.response_delay_ms == 0) {
                client_send_uds(h, client, tester_addr, uds_response, uds_res_len);
                break;
            }
            /* Hold the response back; doip_tcp_tick() releases it */
            uint32_t now = get_time_ms();
            memcpy(client->pending_res, uds_response, uds_res_len);
            client->pending_len       = uds_res_len;
            client->pending_due_ms    = now + h->config.faults.response_delay_ms;
            client->pending_next78_ms = client->pending_due_ms;   /* i.e. never */
            if (h->config.faults.response_delay_ms > UDS_P2_SERVER_MS) {
                const uint8_t pending[3] = { 0x7F, uds_req[0], UDS_NRC_RESPONSE_PENDING };
                client_send_uds(h, client, tester_addr, pending, sizeof(pending));
                client->pending_next78_ms = now + UDS_P2_STAR_SERVER_MS / 2;
            }
            break;
        }

        case DOIP_PT_ALIVE_CHECK_REQ:
            /* Client probing us — respond immediately */
            client_send_frame(client, DOIP_PT_ALIVE_CHECK_RES, NULL, 0);
            break;

        case DOIP_PT_ALIVE_CHECK_RES:
            /* Client responded to our server-initiated probe — clear pending flag */
            client->alive_check_sent_ms = 0;
            LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                      "Alive Check response from fd=%d", client->fd);
            break;

        default: {
            uint8_t nack = (uint8_t)DOIP_NACK_UNKNOWN_PT;
            client_send_frame(client, DOIP_PT_GENERIC_NACK, &nack, 1);
            LOG_WARN(DOIP_LOG_MODULE_TCP,
                     "Unknown PT=0x%04X from fd=%d — NACK sent",
                     hdr.payload_type, client->fd);
            break;
        }
    }

frame_done:
    /* Consume the processed frame; shift any trailing bytes to front */
    if (client->rx_bytes > frame_total) {
        memmove(client->rx_buf, client->rx_buf + frame_total,
                client->rx_bytes - frame_total);
        client->rx_bytes -= frame_total;
        /* One read can deliver several frames (TCP coalescing) — process
         * the rest now; no further readable event will announce them. */
        goto next_frame;
    }
    client->rx_bytes = 0;
    return 0;
}

/**
 * Accumulate available bytes into client->rx_buf, then process the complete
 * frames.  Returns 0 when waiting for more data, -1 on error or when the
 * caller should close the client.
 */
static int process_client_frame(DoIP_Handle_t *h, doip_client_t *client)
{
    for (;;) {
        /* --- Step 1: Read whatever is available into the tail of rx_buf --- */
        size_t space = sizeof(client->rx_buf) - client->rx_bytes;
        if (space == 0) {
            /* Buffer full with no complete frame → protocol error */
            LOG_WARN(DOIP_LOG_MODULE_TCP, "Rx buffer overflow for fd=%d", client->fd);
            return -1;
        }

        ssize_t n = client->io.read(client->io.ctx,
                                     client->rx_buf + client->rx_bytes, space);
        if (n == 0) {
            /* Graceful peer close */
            LOG_INFO(DOIP_LOG_MODULE_TCP, "Peer closed connection (fd=%d)", client->fd);
            return -1;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; /* No data yet */
            LOG_WARN(DOIP_LOG_MODULE_TCP, "recv error fd=%d: %s", client->fd, strerror(errno));
            return -1;
        }
        client->rx_bytes += (size_t)n;

        int rc = process_buffered_frames(h, client);
        /* A read that filled all the free space may have left more behind in
         * the I/O layer (the rest of a TLS record, which select() cannot see),
         * so read again instead of waiting for the next readable event. */
        if (rc != 0 || (size_t)n < space) return rc;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int doip_tcp_init(DoIP_Handle_t *h) {
#if DOIP_ENABLE_IPV6
    h->tcp_fd = socket(AF_INET6, SOCK_STREAM, 0);
#else
    h->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (h->tcp_fd < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "socket failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(h->tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(h->tcp_fd, IPPROTO_TCP, TCP_NODELAY,  &opt, sizeof(opt));
    fcntl(h->tcp_fd, F_SETFL,
          fcntl(h->tcp_fd, F_GETFL, 0) | O_NONBLOCK);

#if DOIP_ENABLE_IPV6
    /* Dual-stack: accept both IPv4-mapped and native IPv6 */
    int v6only = 0;
    setsockopt(h->tcp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr   = in6addr_any;
    addr6.sin6_port   = htons(h->config.port);
    if (bind(h->tcp_fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind (IPv6) failed: %s", strerror(errno));
        close(h->tcp_fd); h->tcp_fd = -1; return -2;
    }
#else
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(h->config.port);
    if (bind(h->tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "bind failed: %s", strerror(errno));
        close(h->tcp_fd); h->tcp_fd = -1; return -2;
    }
#endif

    if (listen(h->tcp_fd, DOIP_MAX_TCP_CLIENTS) < 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "listen failed: %s", strerror(errno));
        close(h->tcp_fd); h->tcp_fd = -1; return -3;
    }

#if DOIP_ENABLE_TLS
    if (!h->config.tls.cert_file || !h->config.tls.key_file) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP,
                  "TLS enabled but DoIP_Config_t::tls cert_file / key_file not set");
        close(h->tcp_fd); h->tcp_fd = -1; return -4;
    }
    if (doip_tls_init(&h->tls, h->config.tls.cert_file, h->config.tls.key_file,
                       h->config.tls.ca_file, h->config.tls.verify_peer) != 0) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "TLS context initialisation failed");
        close(h->tcp_fd); h->tcp_fd = -1; return -5;
    }
#endif

    LOG_INFO(DOIP_LOG_MODULE_TCP,
             "TCP server listening on port %d (max %d clients, IPv6=%s, TLS=%s)",
             h->config.port, DOIP_MAX_TCP_CLIENTS,
             DOIP_ENABLE_IPV6 ? "yes" : "no",
             DOIP_ENABLE_TLS  ? "yes" : "no");
    return 0;
}

int doip_tcp_poll(DoIP_Handle_t *h) {
    if (h->tcp_fd < 0) return -1;

    fd_set readfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    FD_ZERO(&readfds);
    FD_SET(h->tcp_fd, &readfds);
    int max_fd = h->tcp_fd;

    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (h->clients[i].in_use) {
            int fd = h->clients[i].fd;
            FD_SET(fd, &readfds);
            if (fd > max_fd) max_fd = fd;
        }
    }

    if (select(max_fd + 1, &readfds, NULL, NULL, &tv) < 0) return -1;

    /* --- Accept new connections --- */
    if (FD_ISSET(h->tcp_fd, &readfds)) {
#if DOIP_ENABLE_IPV6
        struct sockaddr_in6 ca;  socklen_t al = sizeof(ca);
        int new_fd = accept(h->tcp_fd, (struct sockaddr *)&ca, &al);
        if (new_fd >= 0) {
            doip_client_t *client = find_free_client(h);
            if (client) {
                /* Set non-blocking so partial reads return immediately */
                fcntl(new_fd, F_SETFL,
                      fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);
                client->fd    = new_fd;
                client->state = DOIP_TCP_STATE_CONNECTED;
                client->last_activity_ms = get_time_ms();
                client->uds_ctx.ecu              = h;
                client->uds_ctx.current_session  = UDS_ECU_DEFAULT_SESSION;
                client->uds_ctx.last_activity_ms = get_time_ms();
                client->connect_ms = get_time_ms();
                if (!client_setup_io(h, client, new_fd)) {
                    close(new_fd);
                    memset(client, 0, sizeof(*client));
                } else {
                    inet_ntop(AF_INET6, &ca.sin6_addr,
                              client->peer_ip, sizeof(client->peer_ip));
                    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: [%s]:%d (fd=%d)",
                             client->peer_ip, ntohs(ca.sin6_port), new_fd);
                    if (client->state != DOIP_TCP_STATE_HANDSHAKE && h->config.on_client_connect)
                        h->config.on_client_connect(new_fd, h->config.user_context);
                }
            } else {
                close(new_fd);
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Max clients reached");
            }
        }
#else  /* IPv4 */
        struct sockaddr_in ca;  socklen_t al = sizeof(ca);
        int new_fd = accept(h->tcp_fd, (struct sockaddr *)&ca, &al);
        if (new_fd >= 0) {
            doip_client_t *client = find_free_client(h);
            if (client) {
                fcntl(new_fd, F_SETFL,
                      fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);
                client->fd    = new_fd;
                client->state = DOIP_TCP_STATE_CONNECTED;
                client->last_activity_ms = get_time_ms();
                client->uds_ctx.ecu              = h;
                client->uds_ctx.current_session  = UDS_ECU_DEFAULT_SESSION;
                client->uds_ctx.last_activity_ms = get_time_ms();
                client->connect_ms = get_time_ms();
                if (!client_setup_io(h, client, new_fd)) {
                    close(new_fd);
                    memset(client, 0, sizeof(*client));
                } else {
                    inet_ntop(AF_INET, &ca.sin_addr,
                              client->peer_ip, sizeof(client->peer_ip));
                    LOG_INFO(DOIP_LOG_MODULE_TCP, "Client connected: %s:%d (fd=%d)",
                             client->peer_ip, ntohs(ca.sin_port), new_fd);
                    if (client->state != DOIP_TCP_STATE_HANDSHAKE && h->config.on_client_connect)
                        h->config.on_client_connect(new_fd, h->config.user_context);
                }
            } else {
                close(new_fd);
                LOG_WARN(DOIP_LOG_MODULE_TCP, "Max clients reached");
            }
        }
#endif /* DOIP_ENABLE_IPV6 */
    }

    /* --- Service readable clients --- */
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        if (!h->clients[i].in_use) continue;
        int fd = h->clients[i].fd;
        if (!FD_ISSET(fd, &readfds)) continue;
#if DOIP_ENABLE_TLS
        if (h->clients[i].state == DOIP_TCP_STATE_HANDSHAKE) {
            int rc = doip_tls_continue((SSL *)h->clients[i].hs_ssl);
            if (rc < 0) { close_client(h, &h->clients[i]); continue; }
            if (rc == 0) continue;
            h->clients[i].io = doip_tls_io((SSL *)h->clients[i].hs_ssl);
            h->clients[i].hs_ssl = NULL;
            h->clients[i].state = DOIP_TCP_STATE_CONNECTED;
            h->clients[i].connect_ms = get_time_ms();   /* initial-inactivity window starts after the handshake */
            if (h->config.on_client_connect)
                h->config.on_client_connect(fd, h->config.user_context);
            /* fall through: the handshake may have left application data buffered */
        }
#endif
        {
            if (process_client_frame(h, &h->clients[i]) < 0)
                close_client(h, &h->clients[i]);
        }
    }

    /* NOTE: Per-client idle timeout / alive-check disconnection is handled entirely
     * by doip_tcp_tick(), called from DoIP_Fsm_MainFunction() after this poll.
     * Do not add a second S3 timeout here — it races with the alive-check probe. */
    return 0;
}

void doip_tcp_tick(DoIP_Handle_t *h, uint32_t now_ms) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++) {
        doip_client_t *c = &h->clients[i];
        if (!c->in_use) continue;
#if DOIP_ENABLE_TLS
        if (c->state == DOIP_TCP_STATE_HANDSHAKE) {
            if ((int32_t)(now_ms - c->connect_ms) > (int32_t)(DOIP_TLS_HANDSHAKE_TIMEOUT_S * 1000)) {
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "TLS handshake timeout for fd=%d — disconnecting", c->fd);
                close_client(h, c);
            }
            continue;
        }
#endif
        if (c->state == DOIP_TCP_STATE_CONNECTED) {
            if ((int32_t)(now_ms - c->connect_ms) > (int32_t)DOIP_INITIAL_INACTIVITY_TIMEOUT_MS) {
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "Initial inactivity timeout for fd=%d — disconnecting", c->fd);
                close_client(h, c);
            }
            continue;
        }
        if (c->state != DOIP_TCP_STATE_ACTIVATED) continue;

#if DOIP_ENABLE_ISO_TP
        isotp_tx_tick(h, c, now_ms);
#endif
        if (c->pending_len > 0) {
            if ((int32_t)(now_ms - c->pending_due_ms) >= 0) {
                client_send_uds(h, c, c->tester_logical_addr, c->pending_res, c->pending_len);
                c->pending_len = 0;
            } else if ((int32_t)(now_ms - c->pending_next78_ms) >= 0) {
                const uint8_t pending[3] = { 0x7F, c->pending_res[0] == 0x7F ? c->pending_res[1]
                                                   : (uint8_t)(c->pending_res[0] - 0x40),
                                             UDS_NRC_RESPONSE_PENDING };
                client_send_uds(h, c, c->tester_logical_addr, pending, sizeof(pending));
                c->pending_next78_ms = now_ms + UDS_P2_STAR_SERVER_MS / 2;
            }
        }

        if (!DOIP_SUPPORT_ALIVE_CHECK) continue;

        if (c->alive_check_sent_ms != 0) {
            /* Alive Check already pending — check for timeout */
            if ((int32_t)(now_ms - c->alive_check_sent_ms) > (int32_t)DOIP_ALIVE_CHECK_TIMEOUT_MS) {
                LOG_WARN(DOIP_LOG_MODULE_TCP,
                         "Alive Check timeout for fd=%d — disconnecting", c->fd);
                close_client(h, c);
            }
        /* Signed difference: last_activity_ms is stamped while polling and can
         * be a millisecond newer than now_ms; unsigned math would wrap and
         * fire a spurious probe. */
        } else if ((int32_t)(now_ms - c->last_activity_ms) > (int32_t)DOIP_ALIVE_CHECK_INTERVAL_MS) {
            /* Client idle too long — probe with Alive Check Request */
            if (client_send_frame(c, DOIP_PT_ALIVE_CHECK_REQ, NULL, 0) > 0) {
                c->alive_check_sent_ms = now_ms;
                LOG_DEBUG(DOIP_LOG_MODULE_TCP,
                          "Alive Check Request sent to fd=%d", c->fd);
            }
        }
    }
}

uint8_t doip_tcp_get_client_count(const DoIP_Handle_t *h) {
    uint8_t count = 0;
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++)
        if (h->clients[i].in_use) count++;
    return count;
}

void doip_tcp_deinit(DoIP_Handle_t *h) {
    for (int i = 0; i < DOIP_MAX_TCP_CLIENTS; i++)
        if (h->clients[i].in_use) close_client(h, &h->clients[i]);
    if (h->tcp_fd >= 0) { close(h->tcp_fd); h->tcp_fd = -1; }
#if DOIP_ENABLE_TLS
    doip_tls_deinit(&h->tls);
#endif
    LOG_INFO(DOIP_LOG_MODULE_TCP, "TCP server stopped");
}
