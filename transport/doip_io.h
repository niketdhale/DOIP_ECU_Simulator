/**
 * @file doip_io.h
 * @brief I/O abstraction vtable for the DoIP transport layer.
 *
 * Decouples the TCP transport from the underlying read/write mechanism so
 * that a plain file descriptor and an SSL/TLS session can be used
 * interchangeably.  Every accepted TCP client gets a doip_io_t populated
 * at accept-time; all subsequent read / write / close operations go through
 * this vtable.
 *
 * Usage — plain socket:
 * @code
 *   client->io = doip_io_plain(accepted_fd);
 * @endcode
 *
 * Usage — TLS (requires DOIP_ENABLE_TLS):
 * @code
 *   client->io = doip_tls_accept(&g_tls_server, accepted_fd);
 * @endcode
 */
#ifndef DOIP_IO_H
#define DOIP_IO_H

#include <sys/types.h>  /* ssize_t */
#include <stddef.h>     /* size_t  */

/**
 * @brief Transport I/O function-pointer table.
 *
 * All function pointers must be non-NULL after construction.
 * The @p ctx field is opaque — for plain-fd mode it carries the integer
 * file descriptor cast to (void *); for TLS mode it points to the SSL
 * session object.
 */
typedef struct {
    /**
     * Read up to @p len bytes into @p buf.
     * @return bytes read, 0 on EOF/peer-close, -1 on error (errno set).
     */
    ssize_t (*read) (void *ctx, void       *buf, size_t len);

    /**
     * Write @p len bytes from @p buf.
     * @return bytes written (may be < len on partial), -1 on error.
     */
    ssize_t (*write)(void *ctx, const void *buf, size_t len);

    /**
     * Return the underlying file descriptor.
     * Used by select()/poll() loops to monitor readability.
     */
    int     (*getfd)(void *ctx);

    /**
     * Close the I/O channel and release any associated resources
     * (e.g. SSL_free + close(fd) for TLS, or just close(fd) for plain).
     */
    void    (*close)(void *ctx);

    /** Opaque context forwarded to every vtable function. */
    void   *ctx;
} doip_io_t;

/**
 * @brief Construct a plain file-descriptor doip_io_t.
 *
 * The returned vtable uses recv()/send() directly on @p fd.
 * The file descriptor is encoded directly inside the ctx pointer
 * (via intptr_t cast) so the returned struct is safe to copy by value.
 *
 * @param fd  An open, connected socket file descriptor.
 * @return    Fully populated doip_io_t ready for use.
 */
doip_io_t doip_io_plain(int fd);

#endif /* DOIP_IO_H */
