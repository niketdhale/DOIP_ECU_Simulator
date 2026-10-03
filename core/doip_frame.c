#include "core/doip_frame.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

static int doip_validate_header(const doip_header_t *hdr) {
    return (hdr->protocol_version == DOIP_PROTOCOL_VERSION &&
            hdr->inverse_version == DOIP_INVERSE_VERSION) ? 1 : 0;
}

ssize_t doip_serialize_header(const doip_header_t *in, uint8_t *out, size_t out_len) {
    if (!in || !out || out_len < DOIP_HEADER_SIZE) return -1;
    doip_header_t *wire = (doip_header_t*)out;
    wire->protocol_version = in->protocol_version;
    wire->inverse_version  = in->inverse_version;
    wire->payload_type     = htons(in->payload_type);
    wire->payload_length   = htonl(in->payload_length);
    return DOIP_HEADER_SIZE;
}

ssize_t doip_deserialize_header(const uint8_t *in, size_t in_len, doip_header_t *out) {
    if (!in || !out || in_len < DOIP_HEADER_SIZE) return -1;
    const doip_header_t *wire = (const doip_header_t*)in;
    out->protocol_version = wire->protocol_version;
    out->inverse_version  = wire->inverse_version;
    out->payload_type     = ntohs(wire->payload_type);
    out->payload_length   = ntohl(wire->payload_length);
    if (!doip_validate_header(out)) return -2;
    if (out->payload_length > DOIP_MAX_PAYLOAD_SIZE) return -3;
    return DOIP_HEADER_SIZE;
}

ssize_t doip_send_frame(int sock_fd, uint16_t ptype, const void *payload, uint32_t plen) {
    if (sock_fd < 0 || (plen > 0 && !payload) || plen > DOIP_MAX_PAYLOAD_SIZE) return -1;
    uint8_t tx_buf[DOIP_HEADER_SIZE + DOIP_MAX_PAYLOAD_SIZE];
    doip_header_t hdr = { .protocol_version = DOIP_PROTOCOL_VERSION,
                          .inverse_version  = DOIP_INVERSE_VERSION,
                          .payload_type     = ptype,
                          .payload_length   = plen };
    if (doip_serialize_header(&hdr, tx_buf, sizeof(tx_buf)) < 0) return -1;
    if (plen > 0) memcpy(tx_buf + DOIP_HEADER_SIZE, payload, plen);
    size_t total = DOIP_HEADER_SIZE + plen, sent = 0;
    while (sent < total) {
        ssize_t n = send(sock_fd, tx_buf + sent, total - sent, 0);
        if (n <= 0) return (sent == 0) ? n : (ssize_t)sent;
        sent += n;
    }
    return (ssize_t)total;
}

ssize_t doip_recv_n(int fd, void *buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    size_t total = 0;
    uint8_t *ptr = buf;
    while (total < len) {
        ssize_t n = recv(fd, ptr + total, len - total, 0);
        if (n == 0) return (total == 0) ? 0 : (ssize_t)total; /* Connection closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; /* Timeout */
            return -1; /* Real error */
        }
        total += n;
    }
    return (ssize_t)total;
}