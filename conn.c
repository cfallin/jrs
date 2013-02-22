#include <apr_general.h>
#include <apr_pools.h>
#include <apr_errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "conn.h"
#include "util.h"
#include "sockstream.h"
#include "server.h"

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
        close(0);
        close(1);
        close(2);

        rc = execvp(argv[0], argv);
        if (rc != 0) exit(1);
    }

    /* in parent */
    job->pid = pid;

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
            break;
        }
    }

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
            int scanned = fscanf(f, "%f", &loadavg);
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

int
conn_cmd(jrs_conn_t *conn, char *buf, int len)
{

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

    /* unknown command */
    return 1;
}
