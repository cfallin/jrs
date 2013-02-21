#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_getopt.h>
#include <apr_signal.h>
#include <apr_thread_proc.h>

#include "util.h"
#include "server.h"

/* to support re-opening the logfile on SIGHUP (for log rotation) */
static const char *logfile_name = NULL;

int shutdown_signal = 0;

void
handle_shutdown_signal(int sig)
{
    shutdown_signal = 1;
}

void handle_sighup(int sig)
{
    if (logfile_name) {
        int fd;

        /* re-open the logfile */
        fd = open(logfile_name, O_WRONLY | O_APPEND);
        if (fd != -1) {
            close(1);
            close(2);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
    }
    else
        /* if there is no log file, SIGHUP is a deadly signal */
        handle_shutdown_signal(SIGHUP);
}

static void
usage()
{
    fprintf(stderr, "Usage: jrs-daemon [-f] [-l log] [-p port]\n"
            "    -f      : run in foreground\n"
            "    -l log  : log into specified logfile\n"
            "    -p port : listen on specified port\n"
            "\n"
            "    A port must be specified. A log file must be specified\n"
            "    unless foreground mode is specified.\n");
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
    const char *option_logfile = NULL;
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
            case 'l': /* log file */
                option_logfile = option_arg;
                break;
                break;

            case 'p': /* port */
                option_port = atoi(option_arg);
                break;

            case 'f':
                option_foreground = 1;
                break;
        }
    }

    /* Validate args */
    if ((option_port == 0) ||
            (!option_foreground && !option_logfile)) {
        usage();
        return 1;
    }

    /* Daemonize */
    if (!option_foreground) {
        apr_proc_detach(1);
    }

    /* Open the logfile and replace stdout/stderr */
    if (option_logfile) {
        int fd;
        fd = open(option_logfile, O_WRONLY | O_APPEND);
        if (fd == -1) {
            perror("Error opening log file");
            return 1;
        }
        fflush(stdout);
        fflush(stderr);
        close(1);
        close(2);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);

        logfile_name = strdup(option_logfile);
    }

    /* Set up signal handlers */
    apr_signal(SIGHUP, handle_sighup);
    apr_signal(SIGTERM, handle_shutdown_signal);
    apr_signal(SIGSTOP, handle_shutdown_signal);
    apr_signal(SIGINT,  handle_shutdown_signal);

    jrs_log("starting server");

    /* Start the server! */
    rv = jrs_server_init(&serv, rootpool, option_port);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error initializing server");
        return 1;
    }

    jrs_server_run(serv);

    jrs_server_destroy(serv);

    return 0;
}
