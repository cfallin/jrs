#include "queue.h"
#include "util.h"
#include "crypto.h"

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

/* policy state */
typedef struct {
    uint64_t last_update;
} policystate_t;

/* internal functions */
static void state_init(policystate_t *st);
static job_t *queue_populate_job(cluster_t *cluster, cluster_ops_t *ops,
        char *iwd, char *exe, char *args);

static void
state_init(policystate_t *st)
{
    st->last_update = 0;
}

static job_t *
queue_populate_job(cluster_t *cluster, cluster_ops_t *ops,
        char *iwd, char *exe, char *args)
{
    char cmdline[8192];
    snprintf(cmdline, sizeof(cmdline), "%s %s", exe, args);
    /*
    jrs_log("new job: iwd = %s cmdline = %s", iwd, cmdline);
    */
    return ops->createjob(cluster, iwd, cmdline);
}

int
queue_populate(cluster_t *cluster, cluster_ops_t *ops, char *jobfile)
{
    char buf[4096], exe[4096], args[4096], iwd[4096];
    struct stat statbuf, statbuf2;
    apr_array_header_t *jobarr;
    char *s;
    FILE *f = fopen(jobfile, "r");

    if (!f) {
        jrs_log("Could not open job file '%s'.", jobfile);
        return 1;
    }

    /* parse a condor-like jobfile */
    exe[0] = 0; args[0] = 0;
    s = getcwd(iwd, sizeof(iwd));
    jobarr = apr_array_make(cluster->pool, 0, sizeof(job_t*));
    while (fgets(buf, sizeof(buf), f)) {
        char *begin, *end, *key, *value;

        /* strip begin and end */
        begin = buf;
        while (*begin && isspace(*begin)) begin++;
        end = begin + strlen(begin) - 1;
        while (end >= begin && isspace(*end)) *end-- = 0;

        /* blank line? continue */
        if (!*begin) continue;

        /* equal to "queue": add a job */
        if (!strncasecmp(begin, "queue", strlen("queue"))) {
            job_t *j = queue_populate_job(cluster, ops, iwd, exe, args);
            *((job_t **)apr_array_push(jobarr)) = j;
            continue;
        }

        /* it's going to be a "key = value" line: find the key and value */
        key = strtok(begin, "=");
        value = strtok(NULL, "=");
        if (!value) continue;
        while (*value && isspace(*value)) value++;

        /* starts with 'Executable =': save executable name */
        if (!strncasecmp(begin, "executable", strlen("executable")))
            strncpy(exe, value, sizeof(exe));
        else if (!strncasecmp(begin, "arguments", strlen("arguments")))
            strncpy(args, value, sizeof(args));
        else if (!strncasecmp(begin, "initialdir", strlen("arguments")))
            strncpy(iwd, value, sizeof(iwd));
    }

    fclose(f);

    return 0;
}

static int
rand_coinflip(float prob)
{
    float x = (float)rand()/(float)INT_MAX;
    return (x <= prob) ? 1 : 0;
}

void
queue_policy(cluster_t *cluster, cluster_ops_t *ops)
{
    node_t *node;
    job_t *job;
    policystate_t *st;
    int running, queued, sent;
    int delta;
    uint64_t now = time_usec();

    /* update our state. */
    if (!cluster->policy_impl) {
        cluster->policy_impl = apr_pcalloc(cluster->pool, sizeof(policystate_t));
        state_init(cluster->policy_impl);
    }
    st = cluster->policy_impl;

    /* request an update from every node twice per second. */
    if ((now - st->last_update) > 500000) {
        DLIST_FOREACH(&cluster->nodes, node) {
            ops->updatenode(node);
        }

        /* do we have any jobs on nodes which are overcommited? probabilistcally
         * remove them once per second. */
        DLIST_FOREACH(&cluster->nodes, node) {
            if (node->running > 0 &&
                    node->all_running > node->cores &&
                    rand_coinflip(0.1)) {
                job_t *j = DLIST_HEAD(&node->jobs);
                /*
                jrs_log("node '%s' is overcommited: running %d of our jobs, %d total, on %d cores",
                        node->hostname, node->running, node->all_running, node->cores);
                jrs_log("removed job %d from node '%s'", j->jobid, node->hostname);
                        */
                ops->removejob(j);
            }
        }

        /* count running jobs, sent jobs, and queued jobs. */
        running = 0;
        sent = 0;
        DLIST_FOREACH(&cluster->nodes, node) {
            node->running = 0;
            if (node->mgr) continue;
            DLIST_FOREACH(&node->jobs, job) {
                /* we are counting only *our* jobs on this node. */
                running++;
                node->running++;
            }
            sent += node->sent;
            /*
            jrs_log("node %s: %d running, %d all_running, %d sent",
                    node->hostname,
                    node->running, node->all_running, node->sent);
                    */
        }

        queued = 0;
        DLIST_FOREACH(&cluster->jobs, job) {
            queued++;
        }

        /* check for stop-state: no queued or running jobs and no queued 'sent'
         * jobs. */
        if (running + queued == 0) {
            jrs_log("All jobs completed.");
            shutdown_signal = 1;
        }


        /* always request exactly the number of running + queued jobs as cores
         * (this is a maximum; we likely won't get it if we have many queued jobs)
         * */
        cluster->req_cores = queued + running;

        /* how many cores should we occupy or vacate? */
        delta = cluster->alloced_cores - running;

        jrs_log("queued: %d; running: %d; req'd cores: %d; alloc'd cores: %d",
                queued, running, cluster->req_cores, cluster->alloced_cores);

        /* should we send jobs to cores? */
        while (delta > 0) {
            /* pick a job off the queue */
            job = DLIST_HEAD(&cluster->jobs);
            if (job == DLIST_END(&cluster->jobs))
                break;

            /* find any node with an open core that we haven't committed yet. */
            int sent = 0;
            DLIST_FOREACH(&cluster->nodes, node) {
                /* pick the higher of all_running (which is the latest job
                 * count at this node reflected by the (L)ist command, but may
                 * not be quite up to date), or running (which is our count of
                 * our own jobs for which we've gotten spawn confirmation, but
                 * may not be after the latest list) to represent the number of
                 * total jobs currently at this machine. Then add the number of
                 * jobs 'sent' (those for which we've enqueued (N)ew commands
                 * but haven't yet gotten spawn confirmation) to represent the
                 * total number of jobs we know about that have been committed
                 * to this machine. */
                int r = node->all_running;
                if (node->running > r) r = node->running;
                if (r + node->sent < node->cores) {
                    ops->sendjob(node, job);
                    sent = 1;
                    break;
                }
            }

            if (sent) {
                /*
                jrs_log("sent job %d to node '%s'.", job->seq, job->node->hostname);
                */
                job->start_timestamp = time_usec();
            }
            else
                break;
        }

        /* should we remove jobs from cores? */
        while (delta < 0) {
            job_t *mostrecent = NULL;

            /* find the most recently started job */
            DLIST_FOREACH(&cluster->nodes, node) {
                DLIST_FOREACH(&node->jobs, job) {
                    if (!mostrecent || job->start_timestamp >
                            mostrecent->start_timestamp)
                        mostrecent = job;
                }
            }

            /* kill it */
            if (mostrecent) {
                /*
                jrs_log("reducing footprint: removed job %ld from node '%s'.",
                        mostrecent->seq, mostrecent->node->hostname);
                        */
                mostrecent->node->running--;
                mostrecent->node->all_running--;
                ops->removejob(mostrecent);
                mostrecent->start_timestamp = 0;
                delta++;
            }
            else
                break;
        }

        st->last_update = now;
    }
}
