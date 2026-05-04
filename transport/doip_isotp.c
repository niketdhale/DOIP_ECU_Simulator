/**
 * @file doip_isotp.c
 * @brief ISO 15765-2 (ISO-TP) segmentation / reassembly implementation.
 *
 * Implements the SF / FF / CF / FC frame state machine.
 * Compiled only when DOIP_ENABLE_ISO_TP=true.
 */

#include "config/doip_config.h"

#if DOIP_ENABLE_ISO_TP

#include "transport/doip_isotp.h"
#include "core/doip_log.h"
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

static uint32_t isotp_get_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

/** Build a 3-byte Flow Control frame into @p out.  Returns byte count (3). */
static uint8_t isotp_build_fc(uint8_t *out, uint8_t flag,
                                uint8_t block_size, uint8_t stmin)
{
    out[0] = (uint8_t)(ISOTP_PCI_FC | (flag & 0x0FU));
    out[1] = block_size;
    out[2] = stmin;
    return 3U;
}

/* ---------------------------------------------------------------------------
 * doip_isotp_rx
 * -------------------------------------------------------------------------*/

int doip_isotp_rx(doip_isotp_ctx_t *ctx,
                   const uint8_t *in, uint16_t in_len,
                   uint8_t *out_buf, uint16_t out_size,
                   uint8_t *fc_out, uint8_t *fc_len_out)
{
    if (!ctx || !in || in_len == 0 || !out_buf || out_size == 0) return -1;

    /* Clear optional FC output */
    if (fc_out && fc_len_out) *fc_len_out = 0;

    uint8_t pci_type = (uint8_t)(in[0] & 0xF0U);

    /* ------------------------------------------------------------------ */
    /* Single Frame (SF)                                                   */
    /* ------------------------------------------------------------------ */
    if (pci_type == ISOTP_PCI_SF) {
        uint8_t dl = (uint8_t)(in[0] & 0x0FU);
        if (dl == 0 || dl > DOIP_ISOTP_SF_MAX || dl > (in_len - 1U)) {
            LOG_WARN(DOIP_LOG_MODULE_TCP,
                     "ISO-TP: SF invalid DL=%u (in_len=%u)", dl, in_len);
            return -1;
        }
        if (dl > out_size) return -1;
        memcpy(out_buf, in + 1U, dl);
        doip_isotp_reset(ctx);
        return (int)dl;

    /* ------------------------------------------------------------------ */
    /* First Frame (FF)                                                    */
    /* ------------------------------------------------------------------ */
    } else if (pci_type == ISOTP_PCI_FF) {
        if (in_len < 6U) return -1; /* FF minimum: 2 PCI + at least 1 data byte */

        uint16_t dl = (uint16_t)(((uint16_t)(in[0] & 0x0FU) << 8U) | in[1]);
        if (dl < 8U || dl > DOIP_ISOTP_MAX_PDU) {
            LOG_WARN(DOIP_LOG_MODULE_TCP,
                     "ISO-TP: FF invalid DL=%u", dl);
            return -1;
        }
        if (dl > out_size) {
            /* Overflow — send FC OVFL */
            if (fc_out && fc_len_out)
                *fc_len_out = isotp_build_fc(fc_out, ISOTP_FC_OVFL, 0, 0);
            return -1;
        }

        /* Copy the data bytes carried in the FF (bytes 2..in_len-1) */
        uint16_t data_in_ff = (uint16_t)(in_len - 2U);
        if (data_in_ff > dl) data_in_ff = dl;

        doip_isotp_reset(ctx);
        ctx->rx_total    = dl;
        ctx->rx_received = data_in_ff;
        ctx->rx_sn       = 1U;
        memcpy(ctx->rx_buf, in + 2U, data_in_ff);

        /* Send FC CTS back to sender */
        if (fc_out && fc_len_out) {
            *fc_len_out = isotp_build_fc(fc_out, ISOTP_FC_CTS,
                                          0 /* no block limit */,
                                          (uint8_t)DOIP_ISOTP_STMIN_MS);
        }
        ctx->rx_fc_sent_ms = isotp_get_ms();
        return 0; /* Not yet complete */

    /* ------------------------------------------------------------------ */
    /* Consecutive Frame (CF)                                              */
    /* ------------------------------------------------------------------ */
    } else if (pci_type == ISOTP_PCI_CF) {
        if (ctx->rx_total == 0) {
            /* CF without a preceding FF */
            LOG_WARN(DOIP_LOG_MODULE_TCP, "ISO-TP: unexpected CF (no active FF)");
            return -1;
        }

        uint8_t sn = (uint8_t)(in[0] & 0x0FU);
        if (sn != (ctx->rx_sn & 0x0FU)) {
            LOG_WARN(DOIP_LOG_MODULE_TCP,
                     "ISO-TP: CF SN mismatch (got=%u expected=%u)",
                     sn, ctx->rx_sn & 0x0FU);
            doip_isotp_reset(ctx);
            return -1;
        }
        ctx->rx_sn++;

        uint16_t remaining = ctx->rx_total - ctx->rx_received;
        uint16_t data_in_cf = (uint16_t)(in_len - 1U);
        if (data_in_cf > remaining) data_in_cf = remaining;

        if (ctx->rx_received + data_in_cf > DOIP_ISOTP_MAX_PDU) {
            doip_isotp_reset(ctx);
            return -1;
        }

        memcpy(ctx->rx_buf + ctx->rx_received, in + 1U, data_in_cf);
        ctx->rx_received += data_in_cf;

        if (ctx->rx_received >= ctx->rx_total) {
            /* Assembly complete */
            uint16_t total = ctx->rx_total;
            if (total > out_size) { doip_isotp_reset(ctx); return -1; }
            memcpy(out_buf, ctx->rx_buf, total);
            doip_isotp_reset(ctx);
            return (int)total;
        }
        return 0; /* More CFs expected */

    /* ------------------------------------------------------------------ */
    /* Flow Control (FC) — received by a sender; not expected here        */
    /* ------------------------------------------------------------------ */
    } else if (pci_type == ISOTP_PCI_FC) {
        /* FC frames are sent by the receiver to the sender.
         * In server-only mode we're the receiver, so this shouldn't arrive. */
        LOG_WARN(DOIP_LOG_MODULE_TCP, "ISO-TP: unexpected FC received");
        return 0;
    }

    LOG_WARN(DOIP_LOG_MODULE_TCP,
             "ISO-TP: unknown PCI type 0x%02X", pci_type);
    return -1;
}

/* ---------------------------------------------------------------------------
 * doip_isotp_tx
 * -------------------------------------------------------------------------*/

int doip_isotp_tx(doip_isotp_ctx_t *ctx,
                   const uint8_t *uds_data, uint16_t uds_len,
                   uint8_t out_segs[][8], int max_segs)
{
    if (!ctx || !uds_data || uds_len == 0 || !out_segs || max_segs <= 0) return -1;

    int seg_count = 0;

    /* Single Frame — fits in one N-PDU */
    if (uds_len <= DOIP_ISOTP_SF_MAX) {
        if (max_segs < 1) return -1;
        memset(out_segs[0], 0xCC, 8); /* Padding */
        out_segs[0][0] = (uint8_t)(ISOTP_PCI_SF | (uds_len & 0x0FU));
        memcpy(&out_segs[0][1], uds_data, uds_len);
        return 1;
    }

    /* First Frame */
    if (max_segs < 1) return -1;
    memset(out_segs[0], 0xCC, 8);
    out_segs[0][0] = (uint8_t)(ISOTP_PCI_FF | ((uds_len >> 8U) & 0x0FU));
    out_segs[0][1] = (uint8_t)(uds_len & 0xFFU);
    uint16_t ff_data = (uds_len < 6U) ? uds_len : 6U;
    memcpy(&out_segs[0][2], uds_data, ff_data);
    seg_count = 1;

    /* Consecutive Frames */
    uint16_t sent = ff_data;
    uint8_t  sn   = 1U;
    while (sent < uds_len && seg_count < max_segs) {
        uint16_t chunk = uds_len - sent;
        if (chunk > 7U) chunk = 7U;

        memset(out_segs[seg_count], 0xCC, 8);
        out_segs[seg_count][0] = (uint8_t)(ISOTP_PCI_CF | (sn & 0x0FU));
        memcpy(&out_segs[seg_count][1], uds_data + sent, chunk);
        sent += chunk;
        sn    = (uint8_t)((sn + 1U) & 0x0FU);
        seg_count++;
    }

    if (sent < uds_len) {
        /* max_segs too small to hold the full segmented message */
        LOG_WARN(DOIP_LOG_MODULE_TCP,
                 "ISO-TP TX: max_segs=%d too small for %u bytes", max_segs, uds_len);
        return -1;
    }

    return seg_count;
}

/* ---------------------------------------------------------------------------
 * doip_isotp_reset
 * -------------------------------------------------------------------------*/

void doip_isotp_reset(doip_isotp_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

#endif /* DOIP_ENABLE_ISO_TP */
