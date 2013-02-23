#include <apr_general.h>
#include <apr_pools.h>
#include <apr_errno.h>
#include <apr_strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "conn.h"
#include "util.h"
#include "sockstream.h"
#include "server.h"

/* ===================== JRS SERVER ====================== */

static int
conn_cmd_new(jrs_conn_t *conn, char *args, int len)
{
    apr_pool_t *subpool;
    jrs_job_t *job;
    apr_status_t rv;
    pid_t pid;
    char *cwd = args, *cmdline = NULL;
    char *p;
    static const int MAXARGV = 256;
    char *argv[MAXARGV];
    char **argvp;
    char buf[1024];

    /* first arg: CWD; second arg: command line. Returns ID. */

    /* split the args: find the semicolon. */
    for (p = args; p < (args + len); p++) {
        if (*p == ';') {
            *p = 0;
            cmdline = p + 1;
            break;
        }
    }
    if (!cmdline)
        return 1;

    /* cut off the trailing newline(s). */
    p = cmdline + strlen(cmdline) - 1;
    while (p >= cmdline && (*p == '\n' || *p == '\r'))
        *p-- = 0;

    /* create the job record. */
    rv = apr_pool_create(&subpool, conn->server->pool);
    if (rv != APR_SUCCESS)
        return 1;

    job = apr_pcalloc(subpool, sizeof(jrs_job_t));
    if (!job) {
        apr_pool_destroy(subpool);
        return 1;
    }

    job->pool = subpool;
    job->id = ++conn->server->jobid;
    job->server = conn->server;
    job->done = 0;

    /* split the command-line arguments by spaces. */
    for (p = cmdline, argvp = argv;
            *p && argvp < (argv + MAXARGV); ) {
        *argvp++ = p;
        for (; *p && *p != ' '; p++) /* nothing */ ;
        /* found a space? cut the string at this point. */
        if (*p == ' ')
            *p++ = 0;
    }
    *argvp = NULL;

    /* fork and spawn the job. */
    pid = fork();
    if (pid < 0) {
        apr_pool_destroy(subpool);
        return 1;
    }

    if (pid == 0) {
        /* in child */
        int rc;
        if (chdir(cwd)) exit(1);
        int fd = open("/dev/null", O_RDWR, 0644);
        close(0);
        close(1);
        close(2);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);

        rc = execvp(argv[0], argv);
        if (rc != 0) exit(1);
    }

    /* in parent */
    job->pid = pid;

    job->spawned_conn = conn;

    /* add to jobs list */
    DLIST_INSERT(DLIST_TAIL(&conn->server->jobs), job);

    /* finally, write the job id back to the client */
    snprintf(buf, sizeof(buf), "%ld\n", job->id);
    jrs_sockstream_write(conn->sockstream, buf, strlen(buf));

    jrs_log("spawn pid %d: jobid %ld (executable %s)", job->pid, job->id, argv[0]);

    return 0;
}

static int
conn_cmd_list(jrs_conn_t *conn)
{
    /* return list of running job IDs, space-separated. */
    jrs_job_t *job;

    DLIST_FOREACH(&conn->server->jobs, job) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%ld ", job->id);
        jrs_sockstream_write(conn->sockstream, buf, strlen(buf));
    }
    jrs_sockstream_write(conn->sockstream, "\r\n", 2);

    return 0;
}

static int
conn_cmd_kill(jrs_conn_t *conn, char *args, int len)
{
    uint64_t jobid;
    int signal;
    jrs_job_t *job;
    char *endptr;

    /* parse args: one job id, optionally a signal number (only 15 and 9,
     * i.e., SIGTERM and SIGKILL, accepted) */
    jobid = strtoull(args, &endptr, 10);
    signal = strtoull(endptr, NULL, 10);

    if (signal != 9 && signal != 15)
        signal = 15;

    /* find and kill the job */
    DLIST_FOREACH(&conn->server->jobs, job) {
        if (job->id == jobid) {
            kill(job->pid, signal);
            jrs_log("sending signal %d to pid %d (jobid %ld)", signal,
                    job->pid, job->id);
            break;
        }
    }

    jrs_sockstream_write(conn->sockstream, "\r\n", 2);

    return 0;
}

static int
conn_cmd_stats(jrs_conn_t *conn)
{
    /* return system load, core count, and memory in KiB */

    char line[1024];
    int len;
    FILE *f;
    float loadavg;
    int cores;
    uint64_t memory;
    char *p;

    cores = 0;
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "processor\t", strlen("processor\t")))
                cores++;
        }
        fclose(f);
    }
    else
        jrs_log("could not open /proc/cpuinfo");

    loadavg = 0.00;
    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            for (p = line; *p && *p != ' '; p++) ;
            *p = 0;
            loadavg = strtof(line, NULL);
        }
        fclose(f);
    }
    else
        jrs_log("could not open /proc/loadavg");

    memory = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "MemTotal:", strlen("MemTotal:"))) {
                char *memtotal = line;
                while (*memtotal && !isdigit(*memtotal)) memtotal++;
                memory = strtoull(memtotal, NULL, 10);
                break;
            }
        }
        fclose(f);
    }
    else
        jrs_log("could not open /proc/meminfo");

    len = snprintf(line, sizeof(line), "%d %0.2f %ld\r\n", cores, loadavg, memory);
    jrs_sockstream_write(conn->sockstream, line, len);

    return 0;
}

/* ===================== MGR SERVER ====================== */

static void
age_out_nodes(jrs_server_t *server)
{
    jrs_metadata_node_t *node, *nnode;

    uint64_t now = time_usec();

    for (node = DLIST_HEAD(&server->mgr.nodes);
            node != DLIST_END(&server->mgr.nodes);
            node = nnode) {
        nnode = DLIST_NEXT(node);

        if ((now - node->lastseen) > 20*1000000) {
            DLIST_REMOVE(node);
            server->mgr.corecount -= node->cores;
            jrs_log("aging out old node '%s'.", node->hostname);
            apr_pool_destroy(node->pool);
        }
    }
}

static void
assign_cores(jrs_server_t *server)
{
    int cores_per_user;
    int alloced_cores;
    int remainder;
    jrs_metadata_user_t *user;

    if (server->mgr.usercount == 0)
        return;

    age_out_nodes(server);

    /* cores are divided evenly per user. */
    cores_per_user = server->mgr.corecount / server->mgr.usercount;
    /* a few users get lucky with the remainder cores */
    remainder = server->mgr.corecount % server->mgr.usercount;

    jrs_log("assigning cores: %d cores total, %d users -> %d cores/user, %d remainder",
            server->mgr.corecount, server->mgr.usercount, cores_per_user,
            remainder);

    DLIST_FOREACH(&server->mgr.users, user) {
        user->cores = cores_per_user;
        if (remainder > 0) {
            user->cores++;
            remainder--;
        }
    }
}

static int
conn_cmd_ident(jrs_conn_t *conn, char *args, int len)
{
    apr_status_t rv;
    apr_pool_t *pool = NULL;
    jrs_metadata_user_t *user;

    /* does the username already exist in the userhash? */
    user = apr_hash_get(conn->server->mgr.userhash, args, len);
    if (!user) {
        /* create a new node. */
        rv = apr_pool_create(&pool, conn->server->pool);
        if (rv != APR_SUCCESS)
            goto out;

        rv = APR_ENOMEM;
        user = apr_pcalloc(pool, sizeof(jrs_metadata_user_t));
        if (!user)
            goto out;

        user->pool = pool;
        user->username = apr_pstrdup(pool, args);
        user->conns = 0;
        user->needed = 0;
        user->cores = 0;

        apr_hash_set(conn->server->mgr.userhash, args, len, user);
        DLIST_INSERT(DLIST_HEAD(&conn->server->mgr.users), user);
        conn->server->mgr.usercount++;
    }

    /* bump the connection count. */
    user->conns++;

    jrs_log("ident from user: '%s' (%d connections)", args, user->conns);

    conn->usermeta = user;

    /* acknowledge */
    jrs_sockstream_write(conn->sockstream, "\r\n", 2);

    return 0;

out:
    if (pool)
        apr_pool_destroy(pool);
    return 1;
}

static int
conn_cmd_nodelist(jrs_conn_t *conn, char *args, int len)
{
    uint64_t now = time_usec();

    /* tokenize the node list and insert/update timestamps/core counts */
    char *tok, *saveptr;
    char *prevtok = NULL;
    for (tok = strtok_r(args, " \r\n", &saveptr);
            tok; tok = strtok_r(NULL, " \r\n", &saveptr)) {

        /* take tokens in pairs */
        if (!prevtok) {
            prevtok = tok;
            continue;
        }
        else {
            jrs_metadata_node_t *node;
            apr_pool_t *pool;
            apr_status_t rv;

            char *hostname = prevtok, *cores_str = tok;
            int cores = atoi(cores_str);
            prevtok = NULL;

            /* does an entry already exist? */
            node = apr_hash_get(conn->server->mgr.nodehash, hostname, strlen(hostname));
            if (!node) {
                rv = apr_pool_create(&pool, conn->server->pool);
                if (rv != APR_SUCCESS)
                    continue;

                node = apr_pcalloc(pool, sizeof(jrs_metadata_node_t));
                if (!node) {
                    apr_pool_destroy(pool);
                    continue;
                }

                node->pool = pool;
                node->hostname = apr_pstrdup(pool, hostname);
                apr_hash_set(conn->server->mgr.nodehash, hostname,
                        strlen(hostname), node);
                DLIST_INSERT(DLIST_HEAD(&conn->server->mgr.nodes), node);
                jrs_log("new node: '%s'", hostname);
            }
            
            /* update total core count */
            conn->server->mgr.corecount += (cores - node->cores);

            /* update last-seen timestamp and number of cores */
            node->lastseen = now;
            node->cores = cores;
        }
    }

    /* ack */
    jrs_sockstream_write(conn->sockstream, "\r\n", 2);

    return 0;
}

static int
conn_cmd_requestcores(jrs_conn_t *conn, char *args, int len)
{
    jrs_metadata_node_t *node;
    jrs_metadata_user_t *user;
    int users, nodes;
    char buf[64];

    /* must ident first */
    if (!conn->usermeta)
        return 1;

    /* update the requested core count */
    conn->usermeta->needed = atoi(args);

    /* recompute assignments */
    assign_cores(conn->server);
    
    /* response: allocated core count (for this connection) */
    snprintf(buf, sizeof(buf), "%d\r\n", conn->usermeta->cores / conn->usermeta->conns);
    jrs_sockstream_write(conn->sockstream, buf, strlen(buf));

    return 0;
}

/* ===================== COMMON STUFF ====================== */

int
conn_cmd_open(jrs_conn_t *conn)
{
    conn->usermeta = NULL;
    return 0;
}

void
conn_cmd_close(jrs_conn_t *conn)
{
    /* dec connection count and remove user if it reaches 0 */
    if (conn->usermeta) {
        conn->usermeta->conns--;
        if (conn->usermeta->conns <= 0) {
            DLIST_REMOVE(conn->usermeta);
            /* remove from hash */
            apr_hash_set(conn->server->mgr.userhash,
                    conn->usermeta->username,
                    strlen(conn->usermeta->username), NULL);
            conn->server->mgr.usercount--;
            jrs_log("disconnect from user '%s'.", conn->usermeta->username);
            apr_pool_destroy(conn->usermeta->pool);
        }
        conn->usermeta = NULL;
    }

    /* if we're a jobs server, kill all jobs spawned by this connection. */
    if (conn->server->mode == SERVERMODE_JOBS) {
        jrs_job_t *job;
        DLIST_FOREACH(&conn->server->jobs, job) {
            if (job->spawned_conn == conn) {
                kill(job->pid, 9);
                job->spawned_conn = NULL;
            }
        }
    }
}

int
conn_cmd(jrs_conn_t *conn, char *buf, int len)
{
    if (conn->server->mode == SERVERMODE_JOBS) {
        /* command (opcode) is first character/byte of line */
        switch (conn->linebuf[0]) {
            case 'N': /* new job */
                if (len < 2) return 1;
                return conn_cmd_new(conn, conn->linebuf + 2, len - 2);

            case 'K': /* kill a job */
                if (len < 2) return 1;
                return conn_cmd_kill(conn, conn->linebuf + 2, len - 2);

            case 'L': /* list jobs */
                return conn_cmd_list(conn);

            case 'S': /* report system stats */
                return conn_cmd_stats(conn);
        }
    }
    else if (conn->server->mode == SERVERMODE_MGR) {
        switch (conn->linebuf[0]) {
            case 'I': /* identify */
                if (len < 2) return 1;
                return conn_cmd_ident(conn, conn->linebuf + 2, len - 2);

            case 'N': /* node-list */
                if (len < 2) return 1;
                return conn_cmd_nodelist(conn, conn->linebuf + 2, len - 2);

            case 'R': /* request cores */
                if (len < 2) return 1;
                return conn_cmd_requestcores(conn, conn->linebuf + 2, len - 2);
        }
    }

    /* unknown command */
    return 1;
}
