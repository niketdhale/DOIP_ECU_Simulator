/**
 * @file doip_io.c
 * @brief Plain file-descriptor implementation of the doip_io_t vtable.
 *
 * Encodes the integer file descriptor inside the opaque @p ctx pointer
 * via an intptr_t cast so that the returned doip_io_t is safe to copy
 * by value — no heap allocation is required.
 */

#include "transport/doip_io.h"
#include <unistd.h>     /* close()  */
#include <sys/socket.h> /* recv(), send() */
#include <stdint.h>     /* intptr_t */

/* ---------------------------------------------------------------------------
 * Static vtable implementations — plain recv/send on the raw fd
 * -------------------------------------------------------------------------*/

static ssize_t plain_read(void *ctx, void *buf, size_t len)
{
    return recv((int)(intptr_t)ctx, buf, len, 0);
}

static ssize_t plain_write(void *ctx, const void *buf, size_t len)
{
    return send((int)(intptr_t)ctx, buf, len, 0);
}

static int plain_getfd(void *ctx)
{
    return (int)(intptr_t)ctx;
}

static void plain_close(void *ctx)
{
    close((int)(intptr_t)ctx);
}

/* ---------------------------------------------------------------------------
 * Public constructor
 * -------------------------------------------------------------------------*/

doip_io_t doip_io_plain(int fd)
{
    doip_io_t io;
    io.ctx   = (void *)(intptr_t)fd;
    io.read  = plain_read;
    io.write = plain_write;
    io.getfd = plain_getfd;
    io.close = plain_close;
    return io;
}
