#include <apr_general.h>
#include <apr_errno.h>
#include <stdio.h>
#include "util.h"

void
apr_perror(apr_status_t rv, const char *msg)
{
    char buf[1024];
    fprintf(stderr, "%s: %s\n", msg, apr_strerror(rv, buf, sizeof(buf)));
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
    fprintf(stderr, "JRS: %s\n", buf);
}
