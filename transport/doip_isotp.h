/**
 * @file doip_isotp.h
 * @brief ISO 15765-2 (ISO-TP) segmentation / reassembly for DoIP.
 *
 * Compiled only when DOIP_ENABLE_ISO_TP=true (build with: make DOIP_ISOTP=1).
 *
 * ISO-TP sits at the **DoIP diagnostic message boundary**: it segments and
 * reassembles UDS payloads that are too large for a single frame.  It does
 * NOT replace TCP or DoIP framing — those remain intact.
 *
 * Frame types handled (ISO 15765-2 §9):
 *   SF  – Single Frame        (PCI nibble = 0x0)
 *   FF  – First Frame         (PCI nibble = 0x1)
 *   CF  – Consecutive Frame   (PCI nibble = 0x2)
 *   FC  – Flow Control        (PCI nibble = 0x3)
 *
 * One doip_isotp_ctx_t is kept per TCP client (embedded in doip_client_t).
 */
#ifndef DOIP_ISOTP_H
#define DOIP_ISOTP_H

#include "config/doip_config.h"

#if DOIP_ENABLE_ISO_TP

#include <stdint.h>
#include <stddef.h>

/** Maximum reassembled UDS payload (ISO 15765-2 supports up to 4 095 bytes). */
#define DOIP_ISOTP_MAX_PDU  4095U

/* ---------------------------------------------------------------------------
 * Frame-type PCI nibble constants (upper nibble of first byte)
 * -------------------------------------------------------------------------*/
#define ISOTP_PCI_SF  0x00U  /**< Single Frame       */
#define ISOTP_PCI_FF  0x10U  /**< First Frame        */
#define ISOTP_PCI_CF  0x20U  /**< Consecutive Frame  */
#define ISOTP_PCI_FC  0x30U  /**< Flow Control       */

/* Flow Control flag values */
#define ISOTP_FC_CTS  0x00U  /**< Continue To Send   */
#define ISOTP_FC_WAIT 0x01U  /**< Wait               */
#define ISOTP_FC_OVFL 0x02U  /**< Overflow / Abort   */

/**
 * @brief Per-client ISO-TP state for both RX reassembly and TX segmentation.
 */
typedef struct {
    /* --- RX reassembly -------------------------------------------------- */
    uint8_t  rx_buf[DOIP_ISOTP_MAX_PDU]; /**< Accumulated UDS data           */
    uint16_t rx_total;                    /**< Bytes expected (from FF DL)    */
    uint16_t rx_received;                 /**< Bytes accumulated so far       */
    uint8_t  rx_sn;                       /**< Next expected CF sequence num  */
    uint32_t rx_fc_sent_ms;               /**< Timestamp when FC was sent (N_Cr timer) */

    /* --- TX segmentation ------------------------------------------------ */
    const uint8_t *tx_data;              /**< Pointer to UDS response data   */
    uint16_t tx_total;                    /**< Total bytes to send            */
    uint16_t tx_sent;                     /**< Bytes sent so far              */
    uint8_t  tx_sn;                       /**< Next CF sequence number (0-F)  */
    uint8_t  tx_bs;                       /**< Block size from last FC        */
    uint8_t  tx_stmin;                    /**< STmin byte from last FC        */
    uint32_t tx_last_ms;                  /**< Timestamp of last CF sent      */
} doip_isotp_ctx_t;

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

/**
 * @brief Feed an incoming DoIP diagnostic payload into the ISO-TP reassembler.
 *
 * Handles SF / FF / CF detection automatically.  If a FF is received a Flow
 * Control (FC CTS) is written into @p fc_out / @p fc_len_out so the caller
 * can forward it back to the sender.
 *
 * @param[in,out] ctx         Per-client ISO-TP context.
 * @param[in]     in          Raw bytes from the DoIP diagnostic payload.
 * @param[in]     in_len      Length of @p in.
 * @param[out]    out_buf     Buffer to receive the reassembled UDS message.
 * @param[in]     out_size    Size of @p out_buf (must be >= DOIP_ISOTP_MAX_PDU).
 * @param[out]    fc_out      Optional: FC frame to send back (8 bytes max).
 *                            Set to NULL if you don't need FC output.
 * @param[out]    fc_len_out  Length of FC frame written into @p fc_out.
 * @retval  >0   Complete UDS message assembled — byte count returned.
 * @retval   0   Waiting for more consecutive frames (not yet complete).
 * @retval  -1   Protocol error — caller should close the session.
 */
int doip_isotp_rx(doip_isotp_ctx_t *ctx,
                   const uint8_t *in, uint16_t in_len,
                   uint8_t *out_buf, uint16_t out_size,
                   uint8_t *fc_out, uint8_t *fc_len_out);

/**
 * @brief Segment a UDS response into ISO-TP N-PDUs for transmission.
 *
 * For short payloads (≤ DOIP_ISOTP_SF_MAX bytes) a single SF is written.
 * Longer payloads produce one FF followed by as many CFs as needed.
 * Each segment is exactly 8 bytes (padded with 0xCC per ISO 15765-2).
 *
 * @param[in,out] ctx         Per-client ISO-TP context (updated with TX state).
 * @param[in]     uds_data    UDS response bytes to segment.
 * @param[in]     uds_len     Length of @p uds_data.
 * @param[out]    out_segs    Output array; each element holds one 8-byte N-PDU.
 * @param[in]     max_segs    Size of @p out_segs array.
 * @return Number of segments written, or -1 on error.
 */
int doip_isotp_tx(doip_isotp_ctx_t *ctx,
                   const uint8_t *uds_data, uint16_t uds_len,
                   uint8_t out_segs[][8], int max_segs);

/**
 * @brief Reset the ISO-TP context to idle state.
 *
 * Call after a session completes or when an error forces a restart.
 *
 * @param[in,out] ctx  Context to reset.
 */
void doip_isotp_reset(doip_isotp_ctx_t *ctx);

#endif /* DOIP_ENABLE_ISO_TP */
#endif /* DOIP_ISOTP_H */
