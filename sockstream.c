#include "sockstream.h"
#include <sys/socket.h>
#include <assert.h>
#include <stdio.h>

#define DEBUG

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

        if (written <= 0)
            break;
    }

    /* is there anything to receive? */
    while (1) {
        ssize_t readbytes;
        uint8_t buf[1024], buf2[1024], *b;
        int i;

        readbytes = recv(sockstream->sockfd, &buf, sizeof(buf), MSG_DONTWAIT);
        if (readbytes > 0) {
            if (sockstream->crypto) {
                RC4(&sockstream->readkey, readbytes, buf, buf2);
                jrs_fifo_write(sockstream->readfifo, buf2, readbytes);
                b = buf2;
            }
            else {
                jrs_fifo_write(sockstream->readfifo, buf, readbytes);
                b = buf;
            }
        }

        if (readbytes == 0 && rflag) { /* EOF */
            sockstream->err = 1;
        }

        /* update the newline and hash counts based on reader state machine */
        for (i = 0; i < readbytes; i++)
            if (b[i] == '\n')
                sockstream->read_lines++;

        if (readbytes <= 0) {
            if (errno == ECONNREFUSED) {
                sockstream->err = 1;
            }
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
        return 0;

    /* if we're in encrypted mode, we must wait for a whole line
     * and a hash to arrive; otherwise, the data we would have
     * returned (a partial line) would not be verified. */
    if (sockstream->crypto) {
        int s = size;
        int rv = jrs_sockstream_readline(sockstream, buf, &s);
        if (rv) return 0;
        return s;
    }

    while (readbytes < size) {
        uint8_t *fifobuf;
        int i;
        int availlen = jrs_fifo_peek_nocopy(sockstream->readfifo, &fifobuf, size);

        if (availlen == 0)
            break;

        if (availlen > (size - readbytes))
            availlen = (size - readbytes);

        memcpy(buf + readbytes, fifobuf, availlen);
        readbytes += availlen;
        for (i = 0; i < availlen; i++) {
            if (fifobuf[i] == '\n')
                sockstream->read_lines--;
        }
        jrs_fifo_advance(sockstream->readfifo, availlen);
    }

    return readbytes;
}

static int
check_mac(jrs_sockstream_t *sockstream, uint8_t *buf, int size)
{
    /* find the hash and remove it from the line */
    int newline_idx = -1;
    int i;
    uint8_t hash[20], recvhash[20];

    printf("check_mac: buf '%s' size %d\n", buf, size);

    for (i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            newline_idx = i;
            break;
        }
    }

    /* if we didn't get a complete line, error */
    if (newline_idx == -1) {
        printf("no newline\n");
        return 1;
    }

    /* if line is shorter than the hash should be, error */
    if (newline_idx < 20) {
        printf("newline idx = %d, wanted >= 20\n", newline_idx);
        return 1;
    }

    memcpy(recvhash, buf + newline_idx - 20, 20);

    buf[newline_idx - 20] = 0;
    size -= 21;

    /* update the SHA-1 hash up to (but not including) the hash/newline */
    SHA1_Update(&sockstream->recvhash, buf, size);

    /* check the hash */
    SHA1_Final(hash, &sockstream->recvhash);

#ifdef DEBUG
    printf("MAC check: computed ");
    for (i = 0; i < 20; i++)
        printf("%02x ", hash[i]);
    printf("received ");
    for (i = 0; i < 20; i++)
        printf("%02x ", recvhash[i]);
    printf("\n");
#endif

    /* error if no match (this will close the connection */
    if (memcmp(hash, recvhash, 20))
        return 1;

    /* re-init the hash for the next line */
    sockstream->recvlinecount++;
    uint32_t initval = htonl(sockstream->recvlinecount);
    SHA1_Init(&sockstream->recvhash);
    SHA1_Update(&sockstream->recvhash, &initval, sizeof(initval));

    /* everything OK */
    return 0;
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

    /* too short buffer --> abort the connection. (HACK!) */
    if (!got_newline && readbytes > 0) {
        sockstream->err = 1;
        return 1;
    }

    /* check the hash if we're in crypto mode */
    if (readbytes > 0 && check_mac(sockstream, buf, readbytes)) {
        sockstream->err = 1;
        return 1;
    }

    *size = readbytes;
    return 0;
}

static void
add_mac(jrs_sockstream_t *sockstream, uint8_t *buf, int size,
        uint8_t *outbuf, int *outsize)
{
    int i;
    int newlines, newline_idx;
    int outcount = 0;

    /* count newlines. sockstream_write must only be called with one
     * newline at a time. */
    for (i = 0, newlines = 0; i < size && outcount < *outsize; i++) {
        if (buf[i] == '\n') {
            outbuf[outcount++] = buf[i];
            newlines++;
            newline_idx = i;
        }
    }

    printf("add_mac: buf '%s' size %d\n", buf, size);

    assert(newlines <= 1);
    /* if there was a newline, it must come at the end of the buf. */
    if (newlines == 1)
        assert(newline_idx == size - 1);

    /* add the contents up to but not including the newline to the
     * line hash. */
    SHA1_Update(&sockstream->linehash, buf, newline_idx);

    /* if we got a newline, append the hash to the line, add the newline char
     * *after* the hash, and reset the hash state. */
    if (newlines) {
        char buf[20];
        SHA1_Final(buf, &sockstream->linehash);

        assert(outcount + 20 + 1 <= *outsize);
        memcpy(outbuf + outcount, buf, *outsize - outcount);
        outcount += 20;
        outbuf[outcount++] = '\n';

        sockstream->linecount++;
        uint32_t initval = htonl(sockstream->linecount);
        SHA1_Init(&sockstream->linehash);
        SHA1_Update(&sockstream->linehash, &initval, sizeof(initval));
    }

    *outsize = outcount;

    printf("add_mac: returning '%s' size %d\n", outbuf, *outsize);
}

int
jrs_sockstream_write(jrs_sockstream_t *sockstream, uint8_t *buf, int size)
{
    if (sockstream->crypto) {
        char ourbuf[4096];
        int oursize = 4096;
        add_mac(sockstream, buf, size, ourbuf, &oursize);
        jrs_fifo_write(sockstream->writefifo, ourbuf, size);
    }
    else
        jrs_fifo_write(sockstream->writefifo, buf, size);

    return 0;
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

    sockstream->linecount = 0;
    uint32_t initval = 0;
    SHA1_Init(&sockstream->linehash);
    SHA1_Update(&sockstream->linehash, &initval, sizeof(initval));

    sockstream->recvlinecount = 0;
    SHA1_Init(&sockstream->recvhash);
    SHA1_Update(&sockstream->recvhash, &initval, sizeof(initval));
}
