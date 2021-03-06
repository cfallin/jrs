#include <apr_general.h>
#include <apr_errno.h>
#include <stdio.h>
#include <syslog.h>
#include "util.h"

extern int jrs_log_syslog;

void
apr_perror(apr_status_t rv, const char *msg)
{
    char buf[1024];
    jrs_log("%s: %s", msg, apr_strerror(rv, buf, sizeof(buf)));
}

void
jrs_log(const char *fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    jrs_logv(fmt, list);
    va_end(list);
}

void
jrs_logv(const char *fmt, va_list args)
{
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);

    if (jrs_log_syslog) {
        syslog(LOG_NOTICE, "%s", buf);
    }
    else {
        fprintf(stderr, "JRS: %s\n", buf);
    }
}

uint64_t
time_usec()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return (tv.tv_sec * 1000000) + tv.tv_usec;
}
