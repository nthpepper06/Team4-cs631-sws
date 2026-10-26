/*
 * Network socket management header for sws.
 */

#ifndef _NET_H_
#define _NET_H_

#include <stddef.h>

#define MAX_LISTEN_SOCKS	4

int net_listen(const char *, int, int *, int);
int net_accept(int *, int, char *, size_t);
void net_close_sockets(int *, int);

#endif /* !_NET_H_ */
