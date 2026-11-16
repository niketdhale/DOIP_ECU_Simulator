/**
 * @file doip_tls.c
 * @brief OpenSSL TLS/DTLS back-end for the DoIP I/O abstraction layer.
 *
 * Compiled only when DOIP_ENABLE_TLS=true (build with: make DOIP_TLS=1).
 * Requires OpenSSL >= 1.1.1.
 */

#include "config/doip_config.h"

#if DOIP_ENABLE_TLS

#include "transport/doip_tls.h"
#include "core/doip_log.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>   /* intptr_t */
#include <signal.h>

/* ---------------------------------------------------------------------------
 * TLS vtable functions — ctx holds a malloc'd SSL* pointer
 * -------------------------------------------------------------------------*/

/* Map an SSL_read/SSL_write result <= 0 onto the recv()/send() contract the
 * transport expects: 0 = orderly close, -1 with errno EAGAIN = try again,
 * -1 with any other errno = dead connection.  errno is always set, so a stale
 * EAGAIN from an unrelated call can never make a broken session look idle. */
static ssize_t tls_result(SSL *ssl, int n)
{
    switch (SSL_get_error(ssl, n)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:  errno = EAGAIN;     return -1;
        case SSL_ERROR_ZERO_RETURN:                     return 0;   /* close_notify */
        default:                    errno = ECONNRESET; return -1;
    }
}

static ssize_t tls_read(void *ctx, void *buf, size_t len)
{
    SSL *ssl = (SSL *)ctx;
    ERR_clear_error();   /* SSL_get_error() needs an empty error queue */
    int n = SSL_read(ssl, buf, (int)len);
    return (n > 0) ? (ssize_t)n : tls_result(ssl, n);
}

static ssize_t tls_write(void *ctx, const void *buf, size_t len)
{
    SSL *ssl = (SSL *)ctx;
    ERR_clear_error();
    int n = SSL_write(ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;
    ssize_t rc = tls_result(ssl, n);
    if (rc == 0) { errno = ECONNRESET; rc = -1; }   /* peer already closed: not a successful write */
    return rc;
}

static int tls_getfd(void *ctx)
{
    SSL *ssl = (SSL *)ctx;
    return SSL_get_fd(ssl);
}

static void tls_close(void *ctx)
{
    SSL *ssl = (SSL *)ctx;
    if (ssl) {
        int fd = SSL_get_fd(ssl);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        if (fd >= 0) close(fd);
    }
}

/* DTLS shares the server's UDP socket — free the session but leave the fd open */
static void dtls_close(void *ctx)
{
    SSL *ssl = (SSL *)ctx;
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int doip_tls_init(doip_tls_server_t *s,
                   const char *cert_file, const char *key_file,
                   const char *ca_file,   bool verify_peer)
{
    if (!s || !cert_file || !key_file) return -1;

    /* OpenSSL writes with plain write(): without this, a tester that
     * disconnects while a response is being sent kills the process. */
    signal(SIGPIPE, SIG_IGN);

    /* One-time OpenSSL initialisation (idempotent in OpenSSL 1.1+) */
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    s->ctx = SSL_CTX_new(TLS_server_method());
    if (!s->ctx) {
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* Enforce TLS 1.2 minimum */
    SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(s->ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "TLS: Cannot load certificate '%s'", cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(s->ctx); s->ctx = NULL; return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(s->ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "TLS: Cannot load private key '%s'", key_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(s->ctx); s->ctx = NULL; return -1;
    }
    if (SSL_CTX_check_private_key(s->ctx) != 1) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP, "TLS: Certificate/key mismatch");
        SSL_CTX_free(s->ctx); s->ctx = NULL; return -1;
    }

    if (ca_file) {
        if (SSL_CTX_load_verify_locations(s->ctx, ca_file, NULL) != 1) {
            LOG_WARN(DOIP_LOG_MODULE_TCP, "TLS: Cannot load CA bundle '%s'", ca_file);
            if (verify_peer) {
                SSL_CTX_free(s->ctx); s->ctx = NULL; return -1;
            }
        }
    }

    if (verify_peer) {
        SSL_CTX_set_verify(s->ctx,
                            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                            NULL);
    }

    LOG_INFO(DOIP_LOG_MODULE_TCP, "TLS server context initialised (cert=%s)", cert_file);
    return 0;
}

void doip_tls_deinit(doip_tls_server_t *s)
{
    if (s && s->ctx) {
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
        LOG_INFO(DOIP_LOG_MODULE_TCP, "TLS server context released");
    }
}

doip_io_t doip_tls_accept(doip_tls_server_t *s, int client_fd)
{
    doip_io_t io;
    memset(&io, 0, sizeof(io));

    if (!s || !s->ctx || client_fd < 0) return io;

    SSL *ssl = SSL_new(s->ctx);
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        return io;
    }

    if (SSL_set_fd(ssl, client_fd) != 1) {
        SSL_free(ssl);
        return io;
    }

    int rc = SSL_accept(ssl);
    if (rc != 1) {
        int err = SSL_get_error(ssl, rc);
        LOG_ERROR(DOIP_LOG_MODULE_TCP,
                  "TLS handshake failed (fd=%d, err=%d)", client_fd, err);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return io;
    }

    LOG_INFO(DOIP_LOG_MODULE_TCP,
             "TLS handshake OK (fd=%d, cipher=%s)",
             client_fd, SSL_get_cipher(ssl));

    io.ctx   = (void *)ssl;
    io.read  = tls_read;
    io.write = tls_write;
    io.getfd = tls_getfd;
    io.close = tls_close;
    return io;
}

doip_io_t doip_dtls_wrap(doip_tls_server_t *s, int udp_fd,
                           const struct sockaddr *peer, socklen_t plen)
{
    doip_io_t io;
    memset(&io, 0, sizeof(io));

    (void)plen;
    if (!s || !s->ctx || udp_fd < 0 || !peer) return io;

    SSL *ssl = SSL_new(s->ctx);
    if (!ssl) return io;

    /* Associate the UDP socket with this SSL session */
    BIO *bio = BIO_new_dgram(udp_fd, BIO_NOCLOSE);
    if (!bio) { SSL_free(ssl); return io; }

    /* Set the peer address so OpenSSL knows where to send datagrams */
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, (void *)peer);
    SSL_set_bio(ssl, bio, bio);

    int rc = SSL_accept(ssl);
    if (rc != 1) {
        LOG_ERROR(DOIP_LOG_MODULE_TCP,
                  "DTLS handshake failed (udp_fd=%d, err=%d)",
                  udp_fd, SSL_get_error(ssl, rc));
        SSL_free(ssl);
        return io;
    }

    LOG_INFO(DOIP_LOG_MODULE_TCP, "DTLS handshake OK (udp_fd=%d)", udp_fd);

    io.ctx   = (void *)ssl;
    io.read  = tls_read;
    io.write = tls_write;
    io.getfd = tls_getfd;
    io.close = dtls_close;
    return io;
}

#endif /* DOIP_ENABLE_TLS */
