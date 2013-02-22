#include "client.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>

apr_status_t
jrs_client_init(jrs_client_t **outclient, apr_pool_t *rootpool,
        char *remotehost, int port, char *secretfile)
{
    apr_status_t rv;
    int rc;
    apr_pool_t *subpool;
    jrs_client_t *client;
    char portstr[16];
    struct addrinfo *addrinfo;
    int sockfd;

    /* alloc memory first */
    rv = apr_pool_create(&subpool, rootpool);
    if (rv != APR_SUCCESS)
        return rv;

    rv = APR_ENOMEM;
    client = apr_pcalloc(subpool, sizeof(jrs_client_t));
    if (!client) {
        apr_pool_destroy(subpool);
        return rv;
    }

    client->pool = subpool;

    /* look up the host */
    rv = APR_ENOENT;
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(remotehost, portstr, NULL, &addrinfo);
    if (rc || !addrinfo) {
        apr_pool_destroy(subpool);
        return rv;
    }

    /* create a socket */
    sockfd = socket(addrinfo->ai_family, addrinfo->ai_socktype,
            addrinfo->ai_protocol);
    if (sockfd == -1) {
        apr_pool_destroy(subpool);
        return APR_FROM_OS_ERROR(errno);
    }

    if (connect(sockfd, addrinfo->ai_addr, addrinfo->ai_addrlen) == -1) {
        apr_pool_destroy(subpool);
        close(sockfd);
        return APR_FROM_OS_ERROR(errno);
    }

    /* create a sockstream */
    rv = jrs_sockstream_create(&client->sockstream, sockfd, subpool);
    if (rv != APR_SUCCESS) {
        close(sockfd);
        apr_pool_destroy(subpool);
        return rv;
    }

    /* start encryption */
    if (crypto_start(client->sockstream, secretfile, &client->crypto)) {
        close(sockfd);
        apr_pool_destroy(subpool);
        return APR_ENOMEM;
    }

    client->sockfd = sockfd;

    *outclient = client;
    return APR_SUCCESS;
}

void
jrs_client_run(jrs_client_t *client)
{
    apr_status_t rc;
    int rv;
    fd_set rfds, efds, wfds;

    /* set both socket and stdin to be nonblocking */
    fcntl(client->sockfd, F_SETFL, fcntl(client->sockfd, F_GETFD) | O_NONBLOCK);
    fcntl(0, F_SETFL, fcntl(0, F_GETFD) | O_NONBLOCK);

    while (1) {
        int maxfd, nsig;
        Crypto_State cryptostate;

        /* select()-poll on both stdin and socket */
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_ZERO(&wfds);

        FD_SET(0, &rfds);
        if (jrs_fifo_avail(client->sockstream->readfifo))
            FD_SET(1, &wfds);
        FD_SET(client->sockfd, &rfds);
        FD_SET(client->sockfd, &efds);
        if (jrs_fifo_avail(client->sockstream->writefifo))
            FD_SET(client->sockfd, &wfds);
        maxfd = client->sockfd + 1;

        nsig = select(maxfd, &rfds, &wfds, &efds, NULL);

        /* send and receive whatever is available on the sockstream */
        int rflag = FD_ISSET(client->sockfd, &rfds);
        if (jrs_sockstream_sendrecv(client->sockstream, rflag)) {
            return; /* EOF */
        }

        /* are we still negotiating crypto? */
        cryptostate = crypto_wait(client->sockstream, &client->crypto);
        if (cryptostate == CRYPTO_BAD) {
            jrs_log("bad crypto negotation with server.");
            return;
        }
        if (cryptostate == CRYPTO_WAITING)
            continue; /* just keep waiting */

        assert(cryptostate == CRYPTO_ESTABLISHED);

        /* dump whatever we've received to stdout */
        while (1) {
            uint8_t buf[1024];
            int size = jrs_sockstream_read(client->sockstream, buf, sizeof(buf));
            if (size == 0) break;
            write(1, buf, size);
        }

        /* read whatever is ready on stdin and write to socket */
        while (1) {
            uint8_t buf[1024];
            int size = read(0, buf, sizeof(buf));
            if (size == 0 && FD_ISSET(0, &rfds)) {
                return; /* EOF */
            }
            FD_CLR(0, &rfds);
            if (size <= 0) break;
            jrs_sockstream_write(client->sockstream, buf, size);
        }

        if (jrs_sockstream_sendrecv(client->sockstream, 0))
            return;
    }
}

void
jrs_client_destroy(jrs_client_t *client)
{
    close(client->sockfd);
    apr_pool_destroy(client->pool);
}
