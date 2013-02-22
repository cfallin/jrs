#ifndef _CONN_H_
#define _CONN_H_

#include "server.h"

/* once at open; returns 0 for success or 1 to close connection */
int conn_cmd_open(jrs_conn_t *conn);

/* once per cmd; returns 0 for success or 1 to close connection */
int conn_cmd(jrs_conn_t *conn, char *buf, int len);

/* once at close */
void conn_cmd_close(jrs_conn_t *conn);

#endif
