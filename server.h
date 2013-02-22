#ifndef _SERVER_H_
#define _SERVER_H_

#include <apr_pools.h>
#include <apr_network_io.h>
#include <apr_poll.h>
#include <unistd.h>

#include "sockstream.h"

enum JRS_CONN_STATE {
    JRS_CONN_INIT, /* initially connected, not authenticated */
    JRS_CONN_OPEN, /* authenticated and ready to serve */
};

/* forward decls */
struct jrs_conn_t;
typedef struct jrs_conn_t jrs_conn_t;
struct jrs_job_t;
typedef struct jrs_job_t jrs_job_t;
struct jrs_server_t;
typedef struct jrs_server_t jrs_server_t;

struct jrs_conn_t {
    apr_pool_t *pool;
    int socket;
    jrs_sockstream_t *sockstream;
    int linebuf_size;
    uint8_t *linebuf;

    jrs_server_t *server;
    jrs_conn_t *next, *prev;
};

struct jrs_job_t {
    apr_pool_t *pool;
    pid_t pid;
    uint64_t id;
    int done;

    jrs_server_t *server;
    jrs_job_t *next, *prev;
};

struct jrs_server_t {
    apr_pool_t *pool;
    int port;
    int socket;
    int selfpipe[2];

    jrs_conn_t conns; /* sentinel */
    jrs_job_t jobs;   /* sentinel */

    uint64_t jobid; /* monotonically increasing */
};

apr_status_t jrs_server_init(jrs_server_t **server, apr_pool_t *pool, int port);
void jrs_server_run(jrs_server_t *server);
void jrs_server_destroy(jrs_server_t *server);

#endif
