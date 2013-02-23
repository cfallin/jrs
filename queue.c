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
#include <fcntl.h>
#include <stdlib.h>

static job_t *cluster_ops_createjob(cluster_t *cluster, char *cwd, char *cmdline);
static void cluster_ops_updatenode(node_t *node);
static void cluster_ops_sendjob(node_t *node, job_t *job);
static void cluster_ops_removejob(job_t *job);
static apr_status_t  enqueue_req(node_t *node, char type, char *cmdline,
        char *cwd, uint64_t jobid, job_t *job, int cores, char *username);
static void req_done(req_t *req, uint8_t *buf, int len);
static void handle_response(node_t *node, uint8_t *buf, int len);
static void start_connection(node_t *node);
static void wait_connection(node_t *node);
static int  handle_conn(node_t *node, int rflag, int eflag, int wflag);
static void killall_jobs(cluster_t *cluster, cluster_ops_t *ops);

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

    /* init the mgr node */
    rv = node_create(&cluster->mgrnode, cluster, config->mgrnode, subpool);
    if (rv != APR_SUCCESS)
        goto out;

    cluster->mgrnode->mgr = 1;

    DLIST_INSERT(DLIST_TAIL(&cluster->nodes), cluster->mgrnode);

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
    job->seq = cluster->seq++;

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
    if (node->mgr) {
        if (!node->idented) {
            enqueue_req(node, 'I', NULL, NULL, 0, NULL, 0, node->cluster->config->username);
            node->idented = 1;
        }
        enqueue_req(node, 'R', NULL, NULL, 0, NULL, node->cluster->req_cores, NULL);
        enqueue_req(node, 'N', NULL, NULL, 0, NULL, 0, NULL);
    }
    else {
        enqueue_req(node, 'L', NULL, NULL, 0, NULL, 0, NULL);
        enqueue_req(node, 'S', NULL, NULL, 0, NULL, 0, NULL);
        enqueue_req(node, 'F', NULL, NULL, 0, NULL, 0, NULL);
    }
}

static void
cluster_ops_sendjob(node_t *node, job_t *job)
{
    assert(job->state == JOBSTATE_QUEUED);

    DLIST_REMOVE(job);
    job->state = JOBSTATE_SENT;
    job->node = node;
    job->jobid = 0; /* don't have a jobid until response comes back */
    node->sent++;

    enqueue_req(node, 'N', job->cmdline, job->cwd, 0, job, 0, NULL);
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
            /* remove the job from the running list immediately. */
            DLIST_REMOVE(job);
            /* re-enqueue the job. */
            DLIST_INSERT(DLIST_TAIL(&job->node->cluster->jobs), job);
            /* send a kill command. */
            enqueue_req(job->node, 'K', NULL, NULL, job->jobid, job, 0, NULL);
            job->node = NULL;
            job->state = JOBSTATE_QUEUED;
            break;
    }
}

cluster_ops_t cluster_ops = {
    .updatenode = &cluster_ops_updatenode,
    .createjob = &cluster_ops_createjob,
    .sendjob = &cluster_ops_sendjob,
    .removejob = &cluster_ops_removejob,
};

static apr_status_t 
enqueue_req(node_t *node, char type, char *cmdline, char *cwd, uint64_t jobid,
        job_t *job, int cores, char *username)
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

    req->type = type;

    char buf[65536];
    if (!node->mgr) {
        switch (type) {
            case 'N':
                snprintf(buf, sizeof(buf), "N %s;%s\r\n", cwd, cmdline);
                req->job = job;
                break;
            case 'K':
                snprintf(buf, sizeof(buf), "K %ld 15\r\n", jobid);
                break;
            case 'L':
                snprintf(buf, sizeof(buf), "L\r\n");
                break;
            case 'S':
                snprintf(buf, sizeof(buf), "S\r\n");
                break;
            case 'F':
                snprintf(buf, sizeof(buf), "F\r\n");
                break;
            default:
                rv = APR_EINVAL;
                goto out;
        }
    }
    else {
        switch (type) {
            case 'I':
                snprintf(buf, sizeof(buf), "I %s\r\n", username);
                break;
            case 'N':
                {
                    char *p = buf;
                    int remaining = sizeof(buf);
                    int len;
                    node_t *n;

                    len = snprintf(p, remaining, "N ");
                    p += len;
                    remaining -= len;

                    DLIST_FOREACH(&node->cluster->nodes, n) {
                        if (n->mgr) continue;
                        if (n->cores > 0) {
                            len = snprintf(p, remaining, "%s %d ", n->hostname, n->cores);
                            p += len;
                            remaining -= len;
                        }
                    }
                    len = snprintf(p, remaining, "\r\n");
                }
                break;
            case 'R':
                snprintf(buf, sizeof(buf), "R %d\r\n", cores);
                break;
            default:
                rv = APR_EINVAL;
                goto out;
        }
    }

    req->pool = subpool;
    req->req = apr_pstrdup(subpool, buf);
    req->len = strlen(req->req);

    req->node = node;

    /*
    jrs_log("ENQUEUE -> node '%s' mgr %d: %s",
            req->node->hostname, req->node->mgr, req->req);
            */

    DLIST_INSERT(DLIST_TAIL(&node->reqs), req);

    if (!node->curreq && node->state == NODESTATE_CONNECTED)
        handle_conn(node, 0, 0, 0);

    return APR_SUCCESS;

out:
    if (subpool)
        apr_pool_destroy(subpool);
    return rv;
}

static void
req_done(req_t *req, uint8_t *buf, int len)
{
    /*
    jrs_log("REQDONE (node '%s' mgr %d type '%c'):\n\t%s ->\n\t%s",
            req->node->hostname, req->node->mgr,
            req->type,
            req->req,
            buf);
            */

    if (req->node->mgr) {
        switch (req->type) {
            case 'I':
                break;
            case 'N':
                break;
            case 'R':
                /* don't accept an allocation of zero cores if we requested
                 * some. */
                if (atoi(buf) > 0 || req->node->cluster->req_cores == 0)
                    req->node->cluster->alloced_cores = atoi(buf);
                break;
        }
    }
    else {
        switch(req->type) {
            case 'N':
                /* we now know the job's jobid, so it goes from SENT to RUNNING
                 * state. */
                req->job->jobid = strtoull(buf, NULL, 10);
                req->job->state = JOBSTATE_RUNNING;
                req->job->node = req->node;
                DLIST_INSERT(DLIST_TAIL(&req->job->node->jobs), req->job);
                req->node->sent--;

                /* is kill flag set? if so, this is a delayed kill.
                 * queue up a kill request. else, leave it in the
                 * running list. */
                if (req->job->killed)
                    cluster_ops_removejob(req->job);

                break;
            case 'L':
                {
                    job_t *job, *njob;
                    req_t *r;
                    char *tok, *saveptr;
                    uint64_t now = time_usec();

                    req->node->all_running = 0;

                    /* tokenize the list and examine each current jobid */
                    for (tok = strtok_r(buf, " ", &saveptr);
                            tok;
                            tok = strtok_r(NULL, " ", &saveptr)) {
                        uint64_t jobid = strtoull(tok, NULL, 10);

                        /* count all jobs on this node */
                        if (jobid > 0)
                            req->node->all_running++;

                        /* is job currently running? */
                        int found = 0;
                        DLIST_FOREACH(&req->node->jobs, job) {
                            if (job->jobid == jobid) {
                                /* update its seen-on-compute-node timestamp. */
                                job->seen_timestamp = now;
                                break;
                            }
                        }
                    }

                    /* purge jobs older than threshold seen-timestamp */
                    for (job = DLIST_HEAD(&req->node->jobs);
                            job != DLIST_END(&req->node->jobs);
                            job = njob) {
                        njob = DLIST_NEXT(job);

                        /* take either the start-timestamp or seen-timestamp: if job
                         * hasn't been started yet (ie its spawn is enqueued), we may
                         * not have seen it in the list *yet* but we still have a recent
                         * timestamp for it. */
                        uint64_t last_ts = job->start_timestamp;
                        if (job->seen_timestamp > 0) last_ts = job->seen_timestamp;
                        if ((now - last_ts) > 20*1000*1000) {
                            jrs_log("job %d disappeared from %s; moving back to queue.",
                                    job->seq, job->node->hostname);
                            DLIST_REMOVE(job);
                            DLIST_INSERT(DLIST_HEAD(&job->node->cluster->jobs), job);
                            job->state = JOBSTATE_QUEUED;
                            job->node = NULL;
                        }
                    }
                }
                break;
            case 'F':
                /* finished job ids */
                {
                    job_t *job, *njob;
                    req_t *r;
                    char *tok, *saveptr;

                    /* tokenize the list and examine each current jobid */
                    for (tok = strtok_r(buf, " ", &saveptr);
                            tok;
                            tok = strtok_r(NULL, " ", &saveptr)) {
                        uint64_t jobid = strtoull(tok, NULL, 10);

                        /* is job currently running? */
                        int found = 0;
                        DLIST_FOREACH(&req->node->jobs, job) {
                            if (job->jobid == jobid) {
                                /* found it -- remove it from the list. */
                                jrs_log("job %d completed.", job->seq);
                                DLIST_REMOVE(job);
                                apr_pool_destroy(job->pool);
                                break;
                            }
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
                    /*
                    jrs_log("status update from '%s': %d cores", req->node->hostname,
                            req->node->cores);
                            */
                }
                break;
            case 'K':
                break;
        }
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

        /*
        jrs_log("sending next req to node %s mgr %d: %s", node->hostname, node->mgr,
                node->curreq->req);
                */

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
    int portdelta;

    node->last_connect_retry = time_usec();

    if (node->sockstream)
        jrs_sockstream_destroy(node->sockstream);

    node->sockstream = NULL;
    node->state = NODESTATE_INIT;
    node->idented = 0;

    /* look up the host */
    rv = APR_ENOENT;
    portdelta = node->mgr ? 1 : 0;
    snprintf(portstr, sizeof(portstr), "%d", node->cluster->config->port + portdelta);
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

        fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);
        if (connect(sockfd, addrinfo->ai_addr, addrinfo->ai_addrlen) == -1) {
            if (errno != EINPROGRESS) {
                close(sockfd);
                continue;
            }
        }

        connected = 1;
        break;
    }

    if (!connected)
        return;

    node->state = NODESTATE_CONNECTING;
    node->sockfd = sockfd;
}

static void
wait_connection(node_t *node)
{
    apr_status_t rv;

    /* create a sockstream */
    rv = jrs_sockstream_create(&node->sockstream, node->sockfd, node->pool);
    if (rv != APR_SUCCESS) {
        close(node->sockfd);
        return;
    }

    /* start encryption */
    if (crypto_start(node->sockstream, node->cluster->config->secretfile,
                &node->crypto)) {
        close(node->sockfd);
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

    /* if any reqs are ready and there is no current req, send it. */
    if (!node->curreq && (DLIST_HEAD(&node->reqs) != DLIST_END(&node->reqs)))
        handle_response(node, NULL, 0);

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
    int killing = 0;

    while (1) {
        struct timeval tv;

        /* if we're in shutdown killing mode, and all request queues are empty
         * and all sockstreams have empty write FIFOs, we're done. */
        if (killing) {
            int ready_to_quit = 1;
            DLIST_FOREACH(&cluster->nodes, node) {
                if (node->sockstream &&
                        jrs_fifo_avail(node->sockstream->writefifo)) {
                    ready_to_quit = 0;
                    break;
                }
                if (node->curreq || (DLIST_HEAD(&node->reqs) != DLIST_END(&node->reqs))) {
                    ready_to_quit = 0;
                    break;
                }
            }

            if (ready_to_quit)
                break;
        }

        /* for each node, check connection state. Build fd sets for select(). */
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_ZERO(&wfds);
        maxfd = 0;
        DLIST_FOREACH(&cluster->nodes, node) {
            if (node->state == NODESTATE_INIT) {
                if ((time_usec() - node->last_connect_retry > 1*1000*1000))
                    start_connection(node);
            }
            else if (node->state == NODESTATE_CONNECTING) {
                FD_SET(node->sockfd, &wfds);
                if (node->sockfd >= maxfd)
                    maxfd = node->sockfd + 1;
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

        tv.tv_sec = 0;
        tv.tv_usec = 50*1000;
        nsig = select(maxfd, &rfds, &wfds, &efds, &tv);

        if (shutdown_signal) {
            /* second Ctrl-C kills us. */
            if (killing)
                break;
            else {
                killall_jobs(cluster, &cluster_ops);
                killing = 1;
                shutdown_signal = 0;
            }
        }

        if (nsig < 0)
            continue;

        /* if any connection is ready, do a receive on it. */
        DLIST_FOREACH(&cluster->nodes, node) {
            int fd, rflag, eflag, wflag;

            fd = node->sockfd;
            rflag = FD_ISSET(fd, &rfds);
            eflag = FD_ISSET(fd, &efds);
            wflag = FD_ISSET(fd, &wfds);

            if (wflag && node->state == NODESTATE_CONNECTING) {
                wait_connection(node);
            }

            else if ((rflag || eflag || wflag) && node->state == NODESTATE_CONNECTED) {
                if (handle_conn(node, rflag, eflag, wflag)) {
                    req_t *req, *nreq;
                    job_t *job, *njob;

                    /* connection failure of some sort. Destroy and recreate the
                     * connection. Clear out any jobs running or sent and place
                     * back on general queue. */
                    jrs_sockstream_destroy(node->sockstream);
                    close(node->sockfd);
                    node->sockstream = NULL;
                    node->state = NODESTATE_INIT;
                    /*
                    jrs_log("connection to node '%s' failed or reset; will retry.",
                            node->hostname);
                            */

                    /* clear out the request queue */
                    if (node->curreq) {
                        DLIST_INSERT(DLIST_HEAD(&node->reqs), node->curreq);
                        node->curreq = NULL;
                    }
                    for (req = DLIST_HEAD(&node->reqs); req != DLIST_END(&node->reqs);
                            req = nreq) {
                        nreq = DLIST_NEXT(req);
                        if (req->job) {
                            DLIST_INSERT(DLIST_TAIL(&cluster->jobs), req->job);
                        }
                        DLIST_REMOVE(req);
                        apr_pool_destroy(req->pool);
                    }

                    /* put any jobs that were running here back on the general
                     * queue */
                    for (job = DLIST_HEAD(&node->jobs); job != DLIST_END(&node->jobs);
                            job = njob) {
                        njob = DLIST_NEXT(job);
                        DLIST_REMOVE(job);
                        DLIST_INSERT(DLIST_TAIL(&cluster->jobs), job);
                    }
                }
            }
        }

        if (!killing) {
            /* now invoke the policy. it will examine the current cluster state and
             * potentially invoke commands which modify state and enqueue node
             * requests. */
            policy(cluster, &cluster_ops);
        }
    }
}

static void
killall_jobs(cluster_t *cluster, cluster_ops_t *ops)
{
    job_t *job, *njob;
    node_t *node;

    DLIST_FOREACH(&cluster->nodes, node) {
        if (!node->sockstream) continue;
        for (job = DLIST_HEAD(&node->jobs); job != DLIST_END(&node->jobs);
                job = njob) {
            njob = DLIST_NEXT(job);
            ops->removejob(job);
        }
        jrs_sockstream_sendrecv(node->sockstream, 0);
    }

}
