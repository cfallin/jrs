#ifndef _CONN_H_
#define _CONN_H_

#include "server.h"

/* returns 0 for success or 1 to close connection */
int conn_cmd(jrs_conn_t *conn, char *buf, int len);

#endif
