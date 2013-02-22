#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>

#include "sockstream.h"
#include "crypto.h"

typedef struct jrs_client_t {
    apr_pool_t *pool;
    int sockfd;
    jrs_sockstream_t *sockstream;
    crypto_state_t crypto;
} jrs_client_t;

apr_status_t jrs_client_init(jrs_client_t **client, apr_pool_t *rootpool,
        char *remotehost, int port, char *secretfile);

void jrs_client_run(jrs_client_t *client);

void jrs_client_destroy(jrs_client_t *client);

#endif
