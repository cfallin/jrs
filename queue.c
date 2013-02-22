#include "queue.h"
#include "util.h"
#include "crypto.h"

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>
#include <apr_strings.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <assert.h>

static job_t *cluster_ops_createjob(cluster_t *cluster, char *cwd, char *cmdline);
static void cluster_ops_updatenode(node_t *node);
static void cluster_ops_sendjob(node_t *node, job_t *job);
static void cluster_ops_removejob(job_t *job);
static apr_status_t  enqueue_req(node_t *node, char type, char *cmdline,
        char *cwd, uint64_t jobid, job_t *job);
static void req_done(req_t *req, uint8_t *buf, int len);
static void handle_response(node_t *node, uint8_t *buf, int len);
static void start_connection(node_t *node);
static int  handle_conn(node_t *node, int rflag, int eflag, int wflag);

apr_status_t
cluster_create(cluster_t **outcluster, config_t *config, apr_pool_t *pool)
{
    apr_pool_t *subpool = NULL;
    apr_status_t rv;
    cluster_t *cluster;
    node_t *node;
    char **nodenamep;

    rv = apr_pool_create(&subpool, pool);
    if (rv != APR_SUCCESS)
        goto out;

    rv = APR_ENOMEM;
    cluster = apr_pcalloc(subpool, sizeof(cluster_t));
    if (!cluster)
        goto out;

    cluster->pool = subpool;
    DLIST_INIT(&cluster->jobs);
    DLIST_INIT(&cluster->nodes);

    cluster->config = config;

    for (nodenamep = config->nodes; *nodenamep; nodenamep++) {
        char *nodename = *nodenamep;

        rv = node_create(&node, cluster, nodename, subpool);
        if (rv != APR_SUCCESS)
            goto out;

        DLIST_INSERT(DLIST_TAIL(&cluster->nodes), node);
    }

    *outcluster = cluster;
    return APR_SUCCESS;

out:
    if (subpool)
        apr_pool_destroy(subpool);
    return rv;
}

void
cluster_destroy(cluster_t *cluster)
{
    node_t *node, *nnode;
    for (node = DLIST_HEAD(&cluster->nodes);
            node != DLIST_END(&cluster->nodes);
            node = nnode) {
        nnode = DLIST_NEXT(node);
        node_destroy(node);
    }

    apr_pool_destroy(cluster->pool);
}

apr_status_t
node_create(node_t **outnode, cluster_t *cluster, char *hostname,
        apr_pool_t *pool)
{
    apr_pool_t *subpool = NULL;
    apr_status_t rv;
    node_t *node;

    rv = apr_pool_create(&subpool, pool);
    if (rv != APR_SUCCESS)
        goto out;

    rv = APR_ENOMEM;
    node = apr_pcalloc(subpool, sizeof(node_t));
    if (!node)
        goto out;

    node->pool = subpool;
    node->hostname = apr_pstrdup(subpool, hostname);
    node->cores = 0;
    node->sent = 0;
    node->loadavg = 0.0;

    node->state = NODESTATE_INIT;
    node->cluster = cluster;

    rv = APR_ENOMEM;
    node->linebuf_size = 4096;
    node->linebuf = apr_pcalloc(subpool, node->linebuf_size);
    if (!node->linebuf)
        goto out;

    DLIST_INIT(&node->jobs);
    DLIST_INIT(&node->reqs);
    node->curreq = NULL;

    node->sockstream = NULL;

    *outnode = node;

    return APR_SUCCESS;

out:
    if (subpool)
        apr_pool_destroy(subpool);
    return rv;
}

void
node_destroy(node_t *node)
{
    if (node->sockstream)
        jrs_sockstream_destroy(node->sockstream);
    apr_pool_destroy(node->pool);
}

static job_t *
cluster_ops_createjob(cluster_t *cluster, char *cwd, char *cmdline)
{
    job_t *job;
    apr_pool_t *pool = NULL;
    apr_status_t rv;

    rv = apr_pool_create(&pool, cluster->pool);
    if (rv != APR_SUCCESS)
        goto out;

    job = apr_pcalloc(pool, sizeof(job_t));
    if (!job)
        goto out;

    job->pool = pool;
    job->state = JOBSTATE_QUEUED;
    job->node = NULL;
    job->jobid = 0;
    job->killed = 0;
    job->mark = 0;

    job->cwd = apr_pstrdup(pool, cwd);
    job->cmdline = apr_pstrdup(pool, cmdline);

    job->cluster = cluster;

    DLIST_INSERT(DLIST_TAIL(&cluster->jobs), job);

    return job;

out:
    if (pool)
        apr_pool_destroy(pool);
    return NULL;
}

static void
cluster_ops_updatenode(node_t *node)
{
    enqueue_req(node, 'L', NULL, NULL, 0, NULL);
    enqueue_req(node, 'S', NULL, NULL, 0, NULL);
}

static void
cluster_ops_sendjob(node_t *node, job_t *job)
{
    assert(job->state == JOBSTATE_QUEUED);

    DLIST_REMOVE(job);
    job->state = JOBSTATE_SENT;
    job->node = node;
    job->jobid = 0; /* don't have a jobid until response comes back */

    enqueue_req(node, 'N', job->cmdline, job->cwd, 0, job);
}

static void
cluster_ops_removejob(job_t *job)
{
    switch (job->state) {
        case JOBSTATE_QUEUED:
            /* simply blow away the job */
            DLIST_REMOVE(job);
            apr_pool_destroy(job->pool);
            break;

        case JOBSTATE_SENT:
            /* set the killed-flag; job will be killed once we know
             * its jobid. */
            job->killed = 1;
            break;

        case JOBSTATE_RUNNING:
            /* send a kill command. */
            enqueue_req(job->node, 'K', NULL, NULL, job->jobid, job);
            /* remove the job from the running list immediately. */
            DLIST_REMOVE(job);
            break;
    }
}

static cluster_ops_t ops = {
    .updatenode = &cluster_ops_updatenode,
    .createjob = &cluster_ops_createjob,
    .sendjob = &cluster_ops_sendjob,
    .removejob = &cluster_ops_removejob,
};

static apr_status_t 
enqueue_req(node_t *node, char type, char *cmdline, char *cwd, uint64_t jobid, job_t *job)
{
    apr_pool_t *subpool = NULL;
    req_t *req;
    apr_status_t rv;

    rv = apr_pool_create(&subpool, node->pool);
    if (rv != APR_SUCCESS)
        goto out;

    rv = APR_ENOMEM;
    req = apr_pcalloc(subpool, sizeof(req_t));
    if (!req)
        goto out;

    char buf[4096];
    switch (type) {
        case 'N':
            snprintf(buf, sizeof(buf), "N %s;%s\r\n", cwd, cmdline);
            req->job = job;
            break;
        case 'K':
            snprintf(buf, sizeof(buf), "K %ld 9\r\n", jobid);
            break;
        case 'L':
            snprintf(buf, sizeof(buf), "L\r\n");
            break;
        case 'S':
            snprintf(buf, sizeof(buf), "S\r\n");
            break;
        default:
            rv = APR_EINVAL;
            goto out;
    }

    req->pool = subpool;
    req->req = apr_pstrdup(subpool, buf);
    req->len = strlen(req->req);

    req->node = node;

    DLIST_INSERT(DLIST_TAIL(&node->reqs), req);

    if (!node->curreq)
        handle_response(node, NULL, 0);

    return APR_SUCCESS;

out:
    if (subpool)
        apr_pool_destroy(subpool);
    return rv;
}

static void
req_done(req_t *req, uint8_t *buf, int len)
{
    switch(req->type) {
        case 'N':
            /* we now know the job's jobid, so it goes from SENT to RUNNING
             * state. */
            req->job->jobid = strtoull(buf, NULL, 10);
            req->job->state = JOBSTATE_RUNNING;

            /* is kill flag set? if so, this is a delayed kill.
             * queue up a kill request. else, leave it in the
             * running list. */
            if (req->job->killed)
                cluster_ops_removejob(req->job);

            break;
        case 'L':
          {
              job_t *job, *njob;
              char *tok, *saveptr;

              /* clear the marks */
              DLIST_FOREACH(&req->node->jobs, job) {
                  job->mark = 0;
              }

              /* tokenize the list and examine each current jobid */
              for (tok = strtok_r(buf, " ", &saveptr);
                      tok;
                      tok = strtok_r(NULL, " ", &saveptr)) {
                  uint64_t jobid = strtoull(tok, NULL, 10);

                  /* is job currently running? */
                  int found = 0;
                  DLIST_FOREACH(&req->node->jobs, job) {
                      if (job->jobid == jobid) {
                          job->mark = 1;
                          found = 1;
                          break;
                      }
                  }

                  /* if not currently running -- someone else's job? leftover?
                   * don't care, just kill it. */
                  if (!found)
                      enqueue_req(req->node, 'K', NULL, NULL, jobid, NULL);
              }

              /* for all jobs not marked, remove them from the list. */
              for (job = DLIST_HEAD(&req->node->jobs);
                      job != DLIST_END(&req->node->jobs);
                      job = njob) {
                  njob = DLIST_NEXT(job);
                  if (!job->mark) {
                      DLIST_REMOVE(job);
                      apr_pool_destroy(job->pool);
                  }
              }
          }
          break;
        case 'S':
          /* a status update */
          {
              char *saveptr;
              char *cores_str = strtok_r(buf, " ", &saveptr);
              char *loadavg_str = strtok_r(NULL, " ", &saveptr);
              char *mem_str = strtok_r(NULL, " ", &saveptr);

              req->node->cores = atoi(cores_str);
              req->node->loadavg = atof(loadavg_str);
              req->node->mem = atoi(mem_str);
          }
          break;
        case 'K':
          break;
    }
    apr_pool_destroy(req->pool);
}

static void
handle_response(node_t *node, uint8_t *buf, int len)
{
    if (node->curreq) {
        req_done(node->curreq, buf, len);
        node->curreq = NULL;
    }

    /* if there's another request, then start it. */
    if (DLIST_HEAD(&node->reqs) != DLIST_END(&node->reqs)) {
        node->curreq = DLIST_HEAD(&node->reqs);
        DLIST_REMOVE(node->curreq);

        jrs_sockstream_write(node->sockstream, node->curreq->req,
                node->curreq->len);
    }
}

static void
start_connection(node_t *node)
{
    apr_status_t rv;
    int rc;
    int sockfd;
    char portstr[16];
    struct addrinfo *addrinfo, hints;

    if (node->sockstream)
        jrs_sockstream_destroy(node->sockstream);

    node->sockstream = NULL;
    node->state = NODESTATE_INIT;

    /* look up the host */
    rv = APR_ENOENT;
    snprintf(portstr, sizeof(portstr), "%d", node->cluster->config->port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = 0;
    rc = getaddrinfo(node->hostname, portstr, NULL, &addrinfo);
    if (rc || !addrinfo)
        return;

    /* create a socket */
    int connected = 0;
    for (; addrinfo; addrinfo = addrinfo->ai_next) {

        sockfd = socket(addrinfo->ai_family, addrinfo->ai_socktype,
                addrinfo->ai_protocol);

        if (sockfd == -1)
            continue;

        if (connect(sockfd, addrinfo->ai_addr, addrinfo->ai_addrlen) == -1)
            continue;

        connected = 1;
        break;
    }

    if (!connected)
        return;

    /* create a sockstream */
    rv = jrs_sockstream_create(&node->sockstream, sockfd, node->pool);
    if (rv != APR_SUCCESS) {
        close(sockfd);
        return;
    }

    /* start encryption */
    if (crypto_start(node->sockstream, node->cluster->config->secretfile,
                &node->crypto)) {
        close(sockfd);
        return;
    }

    node->state = NODESTATE_CONNECTED;
}

static int
handle_conn(node_t *node, int rflag, int eflag, int wflag)
{
    Crypto_State cryptostate;

    /* handle any pending I/O on the connection in a nonblocking way */
    if (jrs_sockstream_sendrecv(node->sockstream, rflag))
        return 1;
    if (jrs_sockstream_closed(node->sockstream))
        return 1;

    /* are we still negotiating crypto? */
    cryptostate = crypto_wait(node->sockstream, &node->crypto);
    if (cryptostate == CRYPTO_BAD) {
        jrs_log("bad crypto negotation with node '%s'.",
                node->hostname);
        return 1; /* close connection */
    }
    if (cryptostate == CRYPTO_WAITING)
        return 0; /* nothing more we can do yet */

    assert(cryptostate == CRYPTO_ESTABLISHED);

    /* line available? */
    if (!jrs_sockstream_hasline(node->sockstream))
        return 0;

    /* process the line */
    int size = node->linebuf_size - 1;
    if (jrs_sockstream_readline(node->sockstream, node->linebuf, &size))
        return 1;
    node->linebuf[size] = 0;
    if (size < 1) return 1;

    handle_response(node, node->linebuf, size);

    jrs_sockstream_sendrecv(node->sockstream, 0);
}

void
cluster_run(cluster_t *cluster, cluster_policy_func_t policy)
{
    node_t *node;
    job_t *job;
    fd_set rfds, efds, wfds;
    int maxfd, nsig;

    while (!shutdown_signal) {

        /* for each node, check connection state. Build fd sets for select(). */
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_ZERO(&wfds);
        maxfd = 0;
        DLIST_FOREACH(&cluster->nodes, node) {
            if (node->state == NODESTATE_INIT) {
                start_connection(node);
                node->state = NODESTATE_CONNECTED;
            }

            if (node->sockstream) {
                int fd = node->sockstream->sockfd;
                FD_SET(fd, &rfds);
                FD_SET(fd, &efds);
                if (jrs_fifo_avail(node->sockstream->writefifo))
                    FD_SET(fd, &wfds);

                if (fd >= maxfd)
                    maxfd = fd + 1;
            }
        }

        nsig = select(maxfd, &rfds, &wfds, &efds, NULL);

        if (shutdown_signal)
            break;

        if (nsig < 0)
            continue;

        /* if any connection is ready, do a receive on it. */
        DLIST_FOREACH(&cluster->nodes, node) {
            int fd, rflag, eflag, wflag;

            if (!node->sockstream) continue;
            fd = node->sockstream->sockfd;
            rflag = FD_ISSET(fd, &rfds);
            eflag = FD_ISSET(fd, &efds);
            wflag = FD_ISSET(fd, &wfds);

            if (handle_conn(node, rflag, eflag, wflag)) {
                /* connection failure of some sort. Destroy and recreate the
                 * connection. If there was a current request outstanding,
                 * reinsert it at the head of the queue. */
                jrs_sockstream_destroy(node->sockstream);
                node->sockstream = NULL;
                node->state = NODESTATE_INIT;
                if (node->curreq) {
                    DLIST_INSERT(DLIST_HEAD(&node->reqs), node->curreq);
                    node->curreq = NULL;
                }
            }
        }

        /* now invoke the policy. it will examine the current cluster state and
         * potentially invoke commands which modify state and enqueue node
         * requests. */
        policy(cluster, &ops);
    }
}

int
cluster_populatejobs(cluster_t *cluster, char *jobfile)
{
    return 0;
}
