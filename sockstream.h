#ifndef _SOCKSTREAM_H_
#define _SOCKSTREAM_H_

#include <apr_general.h>
#include <apr_pools.h>

typedef struct jrs_fifo_t {
    apr_pool_t *pool;
    uint8_t *buf;
    int alloc, load;
    int head, tail;
} jrs_fifo_t;

apr_status_t jrs_fifo_create(jrs_fifo_t **fifo, int initsize, apr_pool_t *pool);
void jrs_fifo_destroy(jrs_fifo_t *fifo);

int jrs_fifo_avail(jrs_fifo_t *fifo);
int jrs_fifo_read(jrs_fifo_t *fifo, uint8_t *buf, int size);
int jrs_fifo_peek_nocopy(jrs_fifo_t *fifo, uint8_t **buf, int size);
int jrs_fifo_consume(jrs_fifo_t *fifo, int size);
int jrs_fifo_write(jrs_fifo_t *fifo, uint8_t *buf, int size);

typedef struct jrs_sockstream_t {
    apr_pool_t *pool;

    int sockfd;
    int err; /* in an error state */

    jrs_fifo_t *readfifo, *writefifo;
    int read_lines; /* how many newline chars in read fifo? */

} jrs_sockstream_t;

apr_status_t jrs_sockstream_create(jrs_sockstream_t **sockstream, int sockfd,
        apr_pool_t *pool);
void jrs_sockstream_destroy(jrs_sockstream_t *sockstream);

int jrs_sockstream_sendrecv(jrs_sockstream_t *sockstream, int select_read_wakeup);
int jrs_sockstream_closed(jrs_sockstream_t *sockstream);
int jrs_sockstream_hasline(jrs_sockstream_t *sockstream);
int jrs_sockstream_readline(jrs_sockstream_t *sockstream,
        uint8_t *buf, int *size);
int jrs_sockstream_write(jrs_sockstream_t *sockstream,
        uint8_t *buf, int size);
int jrs_sockstream_flushed(jrs_sockstream_t *sockstream);

#endif
