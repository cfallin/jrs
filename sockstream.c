#include "sockstream.h"
#include <sys/socket.h>
#include <assert.h>

apr_status_t
jrs_fifo_create(jrs_fifo_t **retfifo, int initsize, apr_pool_t *pool)
{
    apr_pool_t *subpool;
    apr_status_t rv;
    jrs_fifo_t *fifo;

    rv = apr_pool_create(&subpool, pool);
    if (rv != APR_SUCCESS)
        return rv;

    rv = APR_ENOMEM;
    fifo = apr_pcalloc(subpool, sizeof(jrs_fifo_t));
    if (!fifo) {
        apr_pool_destroy(subpool);
        return rv;
    }

    rv = APR_ENOMEM;
    fifo->buf = apr_pcalloc(subpool, initsize);
    if (!fifo->buf) {
        apr_pool_destroy(subpool);
        return rv;
    }

    fifo->pool = subpool;
    fifo->alloc = initsize;
    fifo->load = 0;
    fifo->head = fifo->tail = 0;

    *retfifo = fifo;
    
    return APR_SUCCESS;
}

void
jrs_fifo_destroy(jrs_fifo_t *fifo)
{
    apr_pool_destroy(fifo->pool);
}

int
jrs_fifo_avail(jrs_fifo_t *fifo)
{
    return fifo->load;
}

int
jrs_fifo_read(jrs_fifo_t *fifo, uint8_t *buf, int size)
{
    int readlen = size;
    if (readlen > fifo->load)
        readlen = fifo->load;

    /* wraparound? */
    if ((fifo->tail + readlen) > fifo->alloc) {
        memcpy(buf, fifo->buf + fifo->tail, fifo->alloc - fifo->tail);
        memcpy(buf + (fifo->alloc - fifo->tail), fifo->buf, readlen -
                (fifo->alloc - fifo->tail));
    }
    else {
        memcpy(buf, fifo->buf + fifo->tail, readlen);
    }

    fifo->tail += readlen;
    if (fifo->tail > fifo->alloc) fifo->tail -= fifo->alloc;
    fifo->load -= readlen;

    return readlen;
}

int
jrs_fifo_peek_nocopy(jrs_fifo_t *fifo, uint8_t **buf, int size)
{
    int readlen = size;
    if (readlen > fifo->load)
        readlen = fifo->load;
    if (readlen > (fifo->alloc - fifo->tail))
        readlen = (fifo->alloc - fifo->tail);

    if (readlen > 0)
        *buf = fifo->buf + fifo->tail;
    else
        *buf = NULL;

    return readlen;
}

int
jrs_fifo_advance(jrs_fifo_t *fifo, int size)
{
    assert(size <= fifo->load);
    fifo->tail += size;
    if (fifo->tail >= fifo->alloc) fifo->tail -= fifo->alloc;
    fifo->load -= size;
}

int
jrs_fifo_write(jrs_fifo_t *fifo, uint8_t *buf, int size)
{
    if (fifo->load + size > fifo->alloc) { /* grow the buffer */
        int newsize = fifo->alloc;
        while ((fifo->load + size) > newsize) newsize <<= 1;

        uint8_t *newbuf = apr_pcalloc(fifo->pool, newsize);
        if (!newbuf)
            return 0;

        if (fifo->tail + fifo->load > fifo->alloc) {
            memcpy(newbuf, fifo->buf + fifo->tail, (fifo->alloc - fifo->tail));
            memcpy(newbuf + (fifo->alloc - fifo->tail), fifo->buf,
                    fifo->load - (fifo->alloc - fifo->tail));
        }
        else {
            memcpy(newbuf, fifo->buf + fifo->tail, fifo->load);
        }

        fifo->buf = newbuf;
        fifo->alloc = newsize;
        fifo->tail = 0;
        fifo->head = fifo->load;
    }

    if (fifo->head + size > fifo->alloc) {
        memcpy(fifo->buf + fifo->head, buf, fifo->alloc - fifo->head);
        memcpy(fifo->buf, buf + (fifo->alloc - fifo->head),
                size - (fifo->alloc - fifo->head));
    }
    else {
        memcpy(fifo->buf + fifo->head, buf, size);
    }

    fifo->head += size;
    if (fifo->head >= fifo->alloc) fifo->head -= fifo->alloc;
    fifo->load += size;

    return size;
}

apr_status_t
jrs_sockstream_create(jrs_sockstream_t **retsockstream, int sockfd,
        apr_pool_t *pool)
{
    apr_pool_t *subpool;
    jrs_sockstream_t *sockstream;
    apr_status_t rv;

    rv = apr_pool_create(&subpool, pool);
    if (rv != APR_SUCCESS)
        return rv;

    rv = APR_ENOMEM;
    sockstream = apr_pcalloc(subpool, sizeof(jrs_sockstream_t));
    if (!sockstream) {
        apr_pool_destroy(subpool);
        return rv;
    }

    rv = jrs_fifo_create(&sockstream->readfifo, 4096, subpool);
    if (rv != APR_SUCCESS) {
        apr_pool_destroy(subpool);
        return rv;
    }
    rv = jrs_fifo_create(&sockstream->writefifo, 4096, subpool);
    if (rv != APR_SUCCESS) {
        apr_pool_destroy(subpool);
        return rv;
    }

    sockstream->sockfd = sockfd;
    sockstream->pool = subpool;
    sockstream->err = 0;
    sockstream->read_lines = 0;

    *retsockstream = sockstream;

    return APR_SUCCESS;
}

void
jrs_sockstream_destroy(jrs_sockstream_t *sockstream)
{
    apr_pool_destroy(sockstream->pool);
}

int
jrs_sockstream_sendrecv(jrs_sockstream_t *sockstream, int rflag)
{
    if (sockstream->err)
        return 1;

    /* do we have anything to send? */
    while (jrs_fifo_avail(sockstream->writefifo)) {
        ssize_t written;
        uint8_t *data;
        int datalen;
        datalen = jrs_fifo_peek_nocopy(sockstream->writefifo, &data,
                1024);

        if (datalen) {
            if (sockstream->crypto) {
                uint8_t buf[1024];
                RC4(&sockstream->writekey, datalen, data, buf);
                written = send(sockstream->sockfd, buf, datalen, MSG_DONTWAIT);
            }
            else {
                written = send(sockstream->sockfd, data, datalen, MSG_DONTWAIT);
            }
            if (written > 0)
                jrs_fifo_advance(sockstream->writefifo, written);
        }

        if (written < 0 &&
                (errno == ECONNREFUSED ||
                 errno == ENOTCONN))
            sockstream->err = 1;

        if (written <= 0)
            break;
    }

    /* is there anything to receive? */
    while (1) {
        ssize_t readbytes;
        uint8_t buf[1024], buf2[1024];
        int i;

        readbytes = recv(sockstream->sockfd, &buf, sizeof(buf), MSG_DONTWAIT);
        if (readbytes > 0) {
            if (sockstream->crypto) {
                RC4(&sockstream->readkey, readbytes, buf, buf2);
                jrs_fifo_write(sockstream->readfifo, buf2, readbytes);
            }
            else {
                jrs_fifo_write(sockstream->readfifo, buf, readbytes);
            }
        }

        if (readbytes == 0 && rflag) /* EOF */
            sockstream->err = 1;

        for (i = 0; i < readbytes; i++)
            if (buf[i] == '\n')
                sockstream->read_lines++;

        if (readbytes <= 0) {
            if (errno == ECONNREFUSED || errno == ENOTCONN)
                sockstream->err = 1;
            break;
        }
    }

    return sockstream->err;
}

int
jrs_sockstream_closed(jrs_sockstream_t *sockstream)
{
    return sockstream->err;
}

int
jrs_sockstream_hasline(jrs_sockstream_t *sockstream)
{
    return sockstream->read_lines;
}

int
jrs_sockstream_read(jrs_sockstream_t *sockstream, uint8_t *buf, int size)
{
    int readbytes = 0;
    if (sockstream->err)
        return 1;

    while (readbytes < size) {
        uint8_t *fifobuf;
        int availlen = jrs_fifo_peek_nocopy(sockstream->readfifo, &fifobuf, size);

        if (availlen == 0)
            break;

        if (availlen < (size - readbytes))
            availlen = (size - readbytes);

        memcpy(buf + readbytes, fifobuf, availlen);
        readbytes += availlen;
    }

    return readbytes;
}

int
jrs_sockstream_readline(jrs_sockstream_t *sockstream, uint8_t *buf, int *size)
{
    int readbytes = 0;
    int i;
    int got_newline = 0;

    if (sockstream->err)
        return 1;

    buf[0] = 0;
    while (readbytes < *size && !got_newline) {
        uint8_t *fifobuf;
        int availlen = jrs_fifo_peek_nocopy(sockstream->readfifo, &fifobuf, *size);
        for (i = 0; i < availlen; i++) {
            buf[readbytes++] = fifobuf[i];
            jrs_fifo_advance(sockstream->readfifo, 1);
            if (fifobuf[i] == '\n') {
                sockstream->read_lines--;
                got_newline = 1;
                break;
            }
        }
        if (availlen == 0)
            break;
    }

    *size == readbytes;
    return 0;
}

int
jrs_sockstream_write(jrs_sockstream_t *sockstream, uint8_t *buf, int size)
{
    jrs_fifo_write(sockstream->writefifo, buf, size);
}

int
jrs_sockstream_flushed(jrs_sockstream_t *sockstream)
{
    return !jrs_fifo_avail(sockstream->writefifo);
}

void
jrs_sockstream_set_rc4(jrs_sockstream_t *sockstream,
        RC4_KEY *sendkey, RC4_KEY *recvkey)
{
    sockstream->crypto = 1;
    memcpy(&sockstream->writekey, sendkey, sizeof(RC4_KEY));
    memcpy(&sockstream->readkey,  recvkey, sizeof(RC4_KEY));
}
