#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_getopt.h>
#include <apr_signal.h>
#include <apr_thread_proc.h>

#include "util.h"
#include "server.h"

int shutdown_signal = 0;
int jrs_log_syslog = 0;
char pidfilename[1024] = { 0, };

void
pidfile_remove()
{
    if (pidfilename[0])
        unlink(pidfilename);
}

void
handle_shutdown_signal(int sig)
{
    shutdown_signal = 1;
}

static void
usage()
{
    fprintf(stderr, "Usage: jrs-daemon [-f] [-p port]\n"
            "    -f            : run in foreground\n"
            "    -l port       : listen on specified port\n"
            "    -p pidfile    : write server pid to pidfile\n"
            "    -s secretfile : use the given shared-secret file\n"
            "\n"
            "    A port must be specified.\n");
}

int
main(int argc,
        /* APR harps on const-correctness... */
        const char * const *argv, 
        const char * const *envp)
{
    pid_t pid, sid;
    apr_status_t rv;
    apr_pool_t *rootpool;
    char err[1024];
    apr_getopt_t *opt;
    char option_ch;
    const char *option_arg;
    int option_port = 0;
    int option_foreground = 0;
    jrs_server_t *serv;

    /* Initialize APR */
    rv = apr_app_initialize(&argc, &argv, &envp);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error starting APR");
        return 1;
    }
    atexit(apr_terminate);

    /* Set up a root pool from which all subpools are allocated */
    rv = apr_pool_create(&rootpool, NULL);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error creating root memory pool");
        return 1;
    }

    /* Parse command-line arguments */
    rv = apr_getopt_init(&opt, rootpool, argc, argv);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error initializing getopt");
        return 1;
    }

    while ((rv = apr_getopt(opt, "l:p:f", &option_ch, &option_arg)) ==
            APR_SUCCESS) {
        switch (option_ch) {
            case 'l': /* listen port */
                option_port = atoi(option_arg);
                break;

            case 'p': /* pid file */
                {
                    char cwdbuf[1024];
                    snprintf(pidfilename, sizeof(pidfilename),
                            "%s/%s", getcwd(cwdbuf, sizeof(cwdbuf)),
                            option_arg);
                }
                break;

            case 'f':
                option_foreground = 1;
                break;
        }
    }

    /* Validate args */
    if (option_port == 0) {
        usage();
        return 1;
    }

    /* Daemonize */
    if (!option_foreground) {
        apr_proc_detach(1);
        jrs_log_syslog = 1;
        openlog("jrs", LOG_PID, LOG_USER);
    }

    /* write pid file */
    if (pidfilename[0]) {
        FILE *pidfile_f = fopen(pidfilename, "w");
        if (!pidfile_f)
            jrs_log("could not open pid file '%s'.", pidfilename);
        else {
            fprintf(pidfile_f, "%d\n", getpid());
            fclose(pidfile_f);
            atexit(pidfile_remove);
        }
    }

    /* Init the server (this opens and binds the listener) */
    rv = jrs_server_init(&serv, rootpool, option_port);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error initializing server (binding to socket)");
        return 1;
    }

    /* Set up signal handlers */
    apr_signal(SIGTERM, handle_shutdown_signal);
    apr_signal(SIGSTOP, handle_shutdown_signal);
    apr_signal(SIGINT,  handle_shutdown_signal);

    jrs_log("starting server");

    jrs_server_run(serv);

    jrs_server_destroy(serv);

    jrs_log("shutting down.");

    return 0;
}
