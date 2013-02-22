#ifndef _QUEUEDRIVER_H_
#define _QUEUEDRIVER_H_

#include <apr_general.h>
#include <apr_errno.h>
#include <apr_pools.h>

#include "queue.h"

int queue_populate(cluster_t *cluster, cluster_ops_t *ops, char *jobfile);
void queue_policy(cluster_t *cluster, cluster_ops_t *ops);

#endif
