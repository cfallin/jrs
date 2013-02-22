#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdarg.h>

/* logging */
void jrs_log(const char *fmt, ...);
void jrs_logv(const char *fmt, va_list args);

/* generic perror()-like routine for APR errors */
void apr_perror(apr_status_t rv, const char *msg);

/* global flag set by signal handler for deadly signals */
extern int shutdown_signal;

/* current time since epoch in microseconds */
uint64_t time_usec();

/* generic circular-doubly-linked-list (with sentinel) macros */
#define DLIST_INSERT_EX(after, suffix, node) \
    do { \
        (node) -> suffix ## next = (after) -> suffix ## next; \
        (node) -> suffix ## prev = (after); \
        (node) -> suffix ## next -> suffix ## prev = (node); \
        (node) -> suffix ## prev -> suffix ## next = (node); \
    } while (0)

#define DLIST_REMOVE_EX(suffix, node) \
    do { \
        (node) -> suffix ## next -> suffix ## prev = (node) -> suffix ## prev; \
        (node) -> suffix ## prev -> suffix ## next = (node) -> suffix ## next; \
    } while (0)

#define DLIST_HEAD_EX(suffix, sentinel) \
    (sentinel) -> suffix ## next
#define DLIST_TAIL_EX(suffix, sentinel) \
    (sentinel) -> suffix ## next

#define DLIST_NEXT_EX(suffix, node) \
    (node) -> suffix ## next
#define DLIST_PREV_EX(suffix, node) \
    (node) -> suffix ## prev

#define DLIST_END_EX(suffix, sentinel) \
    (sentinel)

#define DLIST_FOREACH_EX(suffix, sentinel, node) \
    for (   (node)  = DLIST_HEAD_EX(suffix, sentinel); \
            (node) != DLIST_END_EX(suffix, sentinel); \
            (node)  = DLIST_NEXT_EX(suffix, node))

#define DLIST_INIT_EX(suffix, sentinel) \
    do { \
        (sentinel) -> suffix ## next = (sentinel); \
        (sentinel) -> suffix ## prev = (sentinel); \
    } while(0)

#define DLIST_INSERT(after, node) DLIST_INSERT_EX(after, , node)
#define DLIST_REMOVE(node) DLIST_REMOVE_EX( , node)
#define DLIST_HEAD(sentinel) DLIST_HEAD_EX( , sentinel)
#define DLIST_TAIL(sentinel) DLIST_TAIL_EX( , sentinel)
#define DLIST_NEXT(node) DLIST_NEXT_EX( , node)
#define DLIST_PREV(node) DLIST_PREV_EX( , node)
#define DLIST_END(sentinel) DLIST_END_EX( , sentinel)
#define DLIST_FOREACH(sentinel, node) DLIST_FOREACH_EX( , sentinel, node)
#define DLIST_INIT(sentinel) DLIST_INIT_EX( , sentinel)

#endif
