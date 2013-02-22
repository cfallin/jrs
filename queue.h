#ifndef _QUEUE_H_
#define _QUEUE_H_

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>

#include "sockstream.h"
#include "crypto.h"

struct job_t;
typedef struct job_t job_t;
struct node_t;
typedef struct node_t node_t;
struct req_t;
typedef struct req_t req_t;
struct cluster_t;
typedef struct cluster_t cluster_t;
struct config_t;
typedef struct config_t config_t;

typedef enum JobState {
    JOBSTATE_QUEUED,   /* on a queue */
    JOBSTATE_SENT,     /* sent to a node */
    JOBSTATE_RUNNING,  /* running on a node */
} JobState;

struct job_t {
    apr_pool_t *pool;

    JobState state;
    node_t *node; /* currently executing, or sent to, this node */
    uint64_t jobid;

    /* kill-flag is set if job is removed but the job is pending on the sent
     * queue; once the job id comes back from the node, the job will be killed
     * immediately. */
    int killed; 

    /* used when job-list is received from each node */
    int mark;

    char *cmdline, *cwd;

    cluster_t *cluster;

    job_t *next, *prev;
};

struct req_t {
    apr_pool_t *pool;

    uint8_t *req;
    int len;

    char type;
    job_t *job; /* associated job (for newjob reqs) */

    node_t *node;
    req_t *next, *prev;
};

typedef enum NodeState {
    NODESTATE_INIT,
    NODESTATE_CONNECTED,
} NodeState;

struct node_t {
    apr_pool_t *pool;
    jrs_sockstream_t *sockstream;
    char *hostname;
    crypto_state_t crypto;

    int cores;
    int sent; /* cores which are committed to sent jobs */
    double loadavg;
    int mem;

    NodeState state;

    uint8_t *linebuf;
    int linebuf_size;

    req_t *curreq;
    req_t reqs;

    cluster_t *cluster;

    job_t jobs;      /* jobs which are confirmed-running */
    node_t *next, *prev;
};

struct cluster_t {
    apr_pool_t *pool;

    job_t jobs; /* all jobs not sent or running to a given node */
    node_t nodes;

    config_t *config;
};

struct config_t {
    char **nodes; /* null-terminated list of char* */
    char *secretfile;
    int port;
};

typedef struct {
    /* request an update from the given node */
    void (*updatenode)(node_t *node);
    /* create a new job and add it to the global cluster queue */
    job_t *(*createjob)(cluster_t *cluster, char *cwd, char *cmdline);
    /* send a job from the main cluster queue to a node's sent-queue */
    void (*sendjob)(node_t *node, job_t *job);
    /* remove a job from wherever it is; job is killed if running */
    void (*removejob)(job_t *job);
} cluster_ops_t;

apr_status_t node_create(node_t **outnode, cluster_t *cluster, char *nodename,
        apr_pool_t *pool);
void node_destroy(node_t *node);

apr_status_t cluster_create(cluster_t **outcluster, config_t *config, apr_pool_t *pool);
void cluster_destroy(cluster_t *cluster);

typedef void (*cluster_policy_func_t)(cluster_t *cluster, cluster_ops_t *ops);

void cluster_run(cluster_t *cluster, cluster_policy_func_t policy);

#endif
