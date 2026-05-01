#ifndef DOIP_FRAME_H
#define DOIP_FRAME_H

#include <stdint.h>
#include <sys/types.h>
#include "core/doip_types.h"

/* Serialize DoIP header to wire format (host → network byte order) */
ssize_t doip_serialize_header(const doip_header_t *in, uint8_t *out, size_t out_len);

/* Deserialize + validate DoIP header from wire (network → host) */
ssize_t doip_deserialize_header(const uint8_t *in, size_t in_len, doip_header_t *out);

/* Send complete DoIP frame: header + payload */
ssize_t doip_send_frame(int sock_fd, uint16_t ptype, const void *payload, uint32_t plen);

/* Receive exactly N bytes.
 * Returns: >0 (bytes read), 0 (graceful close), -1 (socket error), -2 (timeout/EAGAIN) */
ssize_t doip_recv_n(int fd, void *buf, size_t len);

#endif /* DOIP_FRAME_H */