/**
 * @file doip_tls.h
 * @brief TLS/DTLS transport security for the DoIP stack (OpenSSL back-end).
 *
 * Compiled only when DOIP_ENABLE_TLS=true (build with: make DOIP_TLS=1).
 * The doip_tls_server_t context wraps an SSL_CTX and is shared across all
 * accepted client connections.  Each accepted TCP client gets its own SSL
 * object wrapped in a doip_io_t vtable, making TLS transparent to the
 * frame-processing layer.
 *
 * Usage:
 * @code
 *   doip_tls_server_t srv;
 *   doip_tls_init(&srv, "server.crt", "server.key", NULL, false);
 *   ...
 *   doip_io_t io = doip_tls_accept(&srv, accepted_fd);
 *   client->io = io;
 * @endcode
 */
#ifndef DOIP_TLS_H
#define DOIP_TLS_H

#include "config/doip_config.h"

#if DOIP_ENABLE_TLS

#include "transport/doip_io.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>
#include <sys/socket.h>   /* struct sockaddr / socklen_t for DTLS */

/**
 * @brief Server-side TLS context.
 *
 * Holds the SSL_CTX shared by all accepted connections.
 * Initialise once with doip_tls_init(); release with doip_tls_deinit().
 */
typedef struct {
    SSL_CTX *ctx;  /**< OpenSSL context (TLS_server_method or DTLS_server_method) */
} doip_tls_server_t;

/**
 * @brief Initialise the TLS server context.
 *
 * Loads the server certificate and private key, and optionally configures
 * mutual authentication using a CA bundle.
 *
 * @param s           Output — populated server context.
 * @param cert_file   Path to PEM server certificate.
 * @param key_file    Path to PEM private key.
 * @param ca_file     CA bundle for peer verification, or NULL to skip.
 * @param verify_peer If true, require a valid client certificate.
 * @return 0 on success, -1 on error (OpenSSL error on stderr).
 */
int doip_tls_init(doip_tls_server_t *s,
                   const char *cert_file, const char *key_file,
                   const char *ca_file,   bool verify_peer);

/**
 * @brief Release the TLS server context and free all OpenSSL resources.
 *
 * @param s  Server context initialised by doip_tls_init().
 */
void doip_tls_deinit(doip_tls_server_t *s);

/**
 * @brief Wrap an accepted TCP socket in a TLS session.
 *
 * Performs the TLS handshake synchronously.  Returns a doip_io_t whose
 * read/write/close vtable functions call SSL_read / SSL_write / SSL_free.
 * On handshake failure the returned ctx will be NULL — callers must check
 * @c io.ctx != NULL before using the result.
 *
 * @param s         Server context from doip_tls_init().
 * @param client_fd Accepted, connected TCP socket file descriptor.
 * @return Fully populated doip_io_t, or a zeroed struct on failure.
 */
doip_io_t doip_tls_accept(doip_tls_server_t *s, int client_fd);

/**
 * @brief Wrap a UDP socket in a DTLS session (one SSL per peer).
 *
 * Creates a DTLS_server_method context, performs the cookie-based handshake,
 * and returns a doip_io_t that wraps DTLSv1_listen + SSL_read/SSL_write.
 *
 * @param s         Server context from doip_tls_init() (must use DTLS method).
 * @param udp_fd    Bound UDP socket file descriptor.
 * @param peer      Peer address obtained from recvfrom().
 * @param plen      Length of @p peer.
 * @return Fully populated doip_io_t, or a zeroed struct on failure.
 */
doip_io_t doip_dtls_wrap(doip_tls_server_t *s, int udp_fd,
                           const struct sockaddr *peer, socklen_t plen);

#endif /* DOIP_ENABLE_TLS */
#endif /* DOIP_TLS_H */
