#include "queue.h"
#include "util.h"

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>
#include <apr_strings.h>

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

    DLIST_INIT(&node->jobs);
    DLIST_INIT(&node->sent_jobs);

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
}

static void
cluster_ops_sendjob(node_t *node, job_t *job)
{
}

static void
cluster_ops_removejob(job_t *job)
{
}

static cluster_ops_t ops = {
    .createjob = &cluster_ops_createjob,
    .sendjob = &cluster_ops_sendjob,
    .removejob = &cluster_ops_removejob,
};

void
cluster_run(cluster_t *cluster, cluster_policy_func_t policy)
{
}
