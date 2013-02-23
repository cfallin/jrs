#ifndef _SERVER_H_
#define _SERVER_H_

#include <apr_pools.h>
#include <apr_network_io.h>
#include <apr_poll.h>
#include <apr_hash.h>
#include <unistd.h>

#include "sockstream.h"
#include "crypto.h"

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
struct jrs_metadata_t;
typedef struct jrs_metadata_t jrs_metadata_t;
struct jrs_metadata_user_t;
typedef struct jrs_metadata_user_t jrs_metadata_user_t;
struct jrs_metadata_node_t;
typedef struct jrs_metadata_node_t jrs_metadata_node_t;

struct jrs_conn_t {
    apr_pool_t *pool;
    int socket;
    jrs_sockstream_t *sockstream;
    int linebuf_size;
    uint8_t *linebuf;
    crypto_state_t crypto;
    char clientname[256];
    int inited;

    jrs_metadata_user_t *usermeta; /* for metadata server only */

    jrs_server_t *server;
    jrs_conn_t *next, *prev;
};

struct jrs_job_t {
    apr_pool_t *pool;
    pid_t pid;
    uint64_t id;
    int done;

    jrs_conn_t *spawned_conn;

    jrs_server_t *server;
    jrs_job_t *next, *prev;
};

typedef enum {
    SERVERMODE_JOBS,
    SERVERMODE_MGR,
} ServerMode;

struct jrs_metadata_user_t {
    apr_pool_t *pool;
    char *username;
    int conns; /* how many active connections? */
    int needed; /* how many cores needed (maximally)? */
    int cores; /* how many cores allocated by us? */

    jrs_metadata_user_t *prev, *next;
};

struct jrs_metadata_node_t {
    apr_pool_t *pool;
    char *hostname;
    uint64_t lastseen; /* last seen from an user ping (expire after 20 secs) */
    int cores;

    jrs_metadata_node_t *prev, *next;
};

struct jrs_metadata_t {
    apr_hash_t *userhash; /* hash on client/username */
    apr_hash_t *nodehash; /* hash on node */

    jrs_metadata_user_t users; /* sentinel */
    jrs_metadata_node_t nodes; /* sentinel */

    /* invariants: these are always equal to the number of users and the sum of
     * the `cores` members on nodes, respectively. Updated whenever the lists
     * or list elements are updated. */
    int usercount, corecount;
};

struct jrs_server_t {
    apr_pool_t *pool;
    int port;
    int socket;
    int selfpipe[2];
    char *secretfile;
    ServerMode mode;

    jrs_conn_t conns; /* sentinel */
    jrs_job_t jobs;   /* sentinel */

    jrs_metadata_t mgr;

    uint64_t jobid; /* monotonically increasing */
};

apr_status_t jrs_server_init(jrs_server_t **server, apr_pool_t *pool, int port,
        char *secretfile, ServerMode mode);
void jrs_server_run(jrs_server_t *server);
void jrs_server_destroy(jrs_server_t *server);

#endif
