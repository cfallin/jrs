#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <pwd.h>

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_getopt.h>
#include <apr_signal.h>
#include <apr_thread_proc.h>

#include "util.h"
#include "server.h"
#include "client.h"
#include "queue.h"
#include "queuedriver.h"

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
    fprintf(stderr, "\n"
            "Usage: jrs [options]\n"
            "    -d             : daemon mode\n"
            "    -q             : queue-sender mode\n"
            "    -n             : metadata manager mode\n"
            "    -c             : raw client mode\n"
            "    -s secretfile  : use the given shared-secret file\n"
            "\n"
            " in daemon and metadata manager mode:\n"
            "    -f             : run in foreground\n"
            "    -l port        : listen on specified port\n"
            "    -p pidfile     : write server pid to pidfile\n"
            "\n"
            "  in client mode:\n"
            "    -r remote-host : connect to remote host\n"
            "    -l port        : connect to the given port on the remote host\n"
            "\n"
            "  in queue-sender mode:\n"
            "    -x jobs.list   : execute the given list of jobs\n"
            "    -e nodes.list  : use the given list of nodes, one per line\n"
            "    -m manager     : use the given metadata manager\n"
            "\n"
            "In all cases, a secret file and a port must be specified. In client\n"
            "mode, a remote host must also be specified.\n"
            "\n");
}

static char *
handle_filepath(const char *option, char **ret, int size)
{
    if (!*ret) {
        *ret = malloc(1024);
        size = 1024;
    }

    if (option[0] != '/') {
        char cwdbuf[1024];
        snprintf(*ret, size,
                "%s/%s", getcwd(cwdbuf, sizeof(cwdbuf)),
                option);
    }
    else
        strncpy(*ret, option, size);
}

static void
cluster_basic_policy(cluster_t *cluster, cluster_ops_t *ops)
{
    node_t *node;

    DLIST_FOREACH(&cluster->nodes, node) {
        ops->updatenode(node);

        printf("node %s: %d cores, %f loadavg", node->hostname, node->cores, node->loadavg);
    }
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
    int option_daemonmode = 0;
    int option_clientmode = 0;
    int option_queuemode = 0;
    int option_mgrmode = 0;
    char *option_secretfile = NULL;
    char *option_mgr = NULL;
    char *option_remotehost = NULL;
    char *option_nodelist = NULL;
    char *option_joblist = NULL;

    jrs_server_t *serv;
    jrs_client_t *client;

    /* Initialize APR */
    rv = apr_app_initialize(&argc, &argv, &envp);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error starting APR");
        return 1;
    }

    /* Set up a root pool from which all subpools are allocated */
    rv = apr_pool_create(&rootpool, NULL);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error creating root memory pool");
        return 1; }

    /* Parse command-line arguments */
    rv = apr_getopt_init(&opt, rootpool, argc, argv);
    if (rv != APR_SUCCESS) {
        apr_perror(rv, "Error initializing getopt");
        return 1;
    }

    while ((rv = apr_getopt(opt, "dcnqs:l:p:fr:x:e:m:", &option_ch, &option_arg)) ==
            APR_SUCCESS) {
        char *pidfilep = &pidfilename;
        switch (option_ch) {
            case 'l': /* listen port */
                option_port = atoi(option_arg);
                break;

            case 'p': /* pid file */
                handle_filepath(option_arg, &pidfilep, sizeof(pidfilename));
                break;

            case 'f':
                option_foreground = 1;
                break;

            case 'd':
                option_daemonmode = 1;
                break;

            case 'c':
                option_clientmode = 1;
                break;

            case 'q':
                option_queuemode = 1;
                break;

            case 'n':
                option_mgrmode = 1;
                break;

            case 'm':
                option_mgr = strdup(option_arg);

            case 'x':
                option_joblist = NULL;
                handle_filepath(option_arg, &option_joblist, 0);
                break;

            case 'e':
                option_nodelist = NULL;
                handle_filepath(option_arg, &option_nodelist, 0);
                break;

            case 'r':
                option_remotehost = strdup(option_arg);
                break;

            case 's':
                option_secretfile = NULL;
                handle_filepath(option_arg,
                        &option_secretfile, 0);;
                break;
        }
    }

    /* Validate args */
    if ((!option_clientmode && !option_daemonmode && !option_queuemode &&
                !option_mgrmode) ||
            (option_clientmode && (!option_remotehost || !option_port)) ||
            (option_daemonmode && (!option_port)) ||
            (option_queuemode  && (!option_joblist || !option_nodelist || !option_mgr)) ||
            (!option_secretfile)) {
        usage();
        return 1;
    }

    if (option_daemonmode || option_mgrmode) {

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
        rv = jrs_server_init(&serv, rootpool, option_port, option_secretfile,
                option_mgrmode ? SERVERMODE_MGR : SERVERMODE_JOBS);
        if (rv != APR_SUCCESS) {
            apr_perror(rv, "Error initializing server (binding to socket)");
            return 1;
        }

        /* Set up signal handlers */
        apr_signal(SIGTERM, handle_shutdown_signal);
        apr_signal(SIGSTOP, handle_shutdown_signal);
        apr_signal(SIGINT,  handle_shutdown_signal);

        if (option_mgrmode)
            jrs_log("starting metadata manager server");
        else
            jrs_log("starting job-runner server");

        jrs_server_run(serv);

        jrs_log("shutting down.");

        return 0;
    }
    else if (option_clientmode) {

        /* Init the client */
        rv = jrs_client_init(&client, rootpool, option_remotehost, option_port,
                option_secretfile);
        if (rv != APR_SUCCESS) {
            jrs_log("Error initializing client.");
            return 1;
        }

        /* Run the main loop (echo stdin/stdout <-> encrypted connection) */
        jrs_client_run(client);

        jrs_client_destroy(client);

        return 0;
    }

    else if (option_queuemode) {
        FILE *f;
        struct passwd *pw;
        uid_t uid;
        char buf[1024];
        cluster_t *cluster;

        /* set up the config */
        config_t config;
        config.port = option_port;
        config.secretfile = option_secretfile;
        int nodeN = 4096;
        config.nodes = malloc(sizeof(char *) * nodeN);

        /* get the username */
        uid = geteuid();
        pw = getpwuid(uid);

        if (pw)
            config.username = pw->pw_name;
        else
            config.username = "nobody";

        char **nodelistp = config.nodes, **nodelistend = (config.nodes + nodeN - 1);

        f = fopen(option_nodelist, "r");
        if (!f) {
            fprintf(stderr, "Could not read node-list file '%s'\n", option_nodelist);
            exit(1);
        }
        while (fgets(buf, sizeof(buf), f) &&
                nodelistp < nodelistend) {
            /* chop off newline */
            char *p = buf + strlen(buf) - 1;
            if (p >= buf && *p == '\n')
                *p = 0;

            /* add to nodelist */
            *nodelistp++ = strdup(buf);
        }
        fclose(f);
        *nodelistp = NULL;

        config.mgrnode = option_mgr;

        /* instantiate the cluster structures */
        rv = cluster_create(&cluster, &config, rootpool);

        if (rv != APR_SUCCESS) {
            jrs_log("Error initializing queue manager.");
            return 1;
        }

        /* Set up signal handlers */
        apr_signal(SIGTERM, handle_shutdown_signal);
        apr_signal(SIGSTOP, handle_shutdown_signal);
        apr_signal(SIGINT,  handle_shutdown_signal);

        jrs_log("starting queue manager");

        /* populate the jobs */
        queue_populate(cluster, &cluster_ops, option_joblist);

        /* run the policy */
        cluster_run(cluster, queue_policy);

        jrs_log("shutting down");

        cluster_destroy(cluster);

        return 0;
    }

    return 1;
}
