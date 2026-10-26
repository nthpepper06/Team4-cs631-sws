/*
 * Network socket creation, binding, and connection acceptance.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <err.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"
#include "net.h"

/*
 * Create, configure, bind, and listen on a socket for a given addrinfo.
 */
static int
setup_socket(const struct addrinfo *ai, int v6only)
{
	int on;
	int s;

	on = 1;
	if ((s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
		return -1;
	}

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on,
	    (socklen_t)sizeof(on)) < 0) {
		warn("setsockopt SO_REUSEADDR");
	}

	if (ai->ai_family == AF_INET6 && v6only) {
		if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on,
		    (socklen_t)sizeof(on)) < 0) {
			warn("setsockopt IPV6_V6ONLY");
		}
	}

	if (bind(s, ai->ai_addr, ai->ai_addrlen) < 0) {
		(void)close(s);
		return -1;
	}

	if (listen(s, SOMAXCONN) < 0) {
		warn("listen failed");
		(void)close(s);
		return -1;
	}

	return s;
}

/*
 * Set up listening sockets. If bind_addr is NULL, binds to all IPv4 and
 * IPv6 addresses using dual sockets for maximum portability.
 */
int
net_listen(const char *bind_addr, int port, int *sockets, int max_socks)
{
	struct addrinfo hints, *res, *res0;
	char port_str[16];
	int count;
	int error;
	int s;

	count = 0;
	(void)snprintf(port_str, sizeof(port_str), "%d", port);

	if (bind_addr != NULL) {
		(void)memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;

		error = getaddrinfo(bind_addr, port_str, &hints, &res0);
		if (error != 0) {
			warnx("getaddrinfo(%s, %s): %s", bind_addr, port_str,
			    gai_strerror(error));
			return -1;
		}

		for (res = res0; res != NULL && count < max_socks;
		    res = res->ai_next) {
			s = setup_socket(res, 0);
			if (s >= 0) {
				sockets[count++] = s;
			}
		}
		freeaddrinfo(res0);

		if (count == 0) {
			warnx("Could not bind to specified address %s",
			    bind_addr);
			return -1;
		}
		return count;
	}

	/*
	 * Bind to both IPv6 (::) and IPv4 (0.0.0.0) when no address given.
	 */
	(void)memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	error = getaddrinfo(NULL, port_str, &hints, &res0);
	if (error == 0) {
		for (res = res0; res != NULL && count < max_socks;
		    res = res->ai_next) {
			s = setup_socket(res, 1);
			if (s >= 0) {
				sockets[count++] = s;
				break;
			}
		}
		freeaddrinfo(res0);
	}

	(void)memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	error = getaddrinfo(NULL, port_str, &hints, &res0);
	if (error == 0) {
		for (res = res0; res != NULL && count < max_socks;
		    res = res->ai_next) {
			s = setup_socket(res, 0);
			if (s >= 0) {
				sockets[count++] = s;
				break;
			}
		}
		freeaddrinfo(res0);
	}

	if (count == 0) {
		warnx("Could not bind to any network address on port %d", port);
		return -1;
	}

	return count;
}

/*
 * Wait for an incoming connection on any listening socket and accept it.
 * Populates client_ip with remote IP address string.
 */
int
net_accept(int *sockets, int num_socks, char *client_ip, size_t ip_len)
{
	struct sockaddr_storage addr;
	struct pollfd pfd[MAX_LISTEN_SOCKS];
	socklen_t addrlen;
	char host[NI_MAXHOST];
	int client_fd;
	int i;
	int ready;

	if (num_socks <= 0 || num_socks > MAX_LISTEN_SOCKS) {
		return -1;
	}

	for (i = 0; i < num_socks; i++) {
		pfd[i].fd = sockets[i];
		pfd[i].events = POLLIN;
		pfd[i].revents = 0;
	}

	ready = poll(pfd, (nfds_t)num_socks, -1);
	if (ready < 0) {
		if (errno == EINTR) {
			return -1;
		}
		warn("poll error on listening sockets");
		return -1;
	}

	for (i = 0; i < num_socks; i++) {
		if (pfd[i].revents & POLLIN) {
			addrlen = (socklen_t)sizeof(addr);
			client_fd = accept(sockets[i],
			    (struct sockaddr *)&addr, &addrlen);
			if (client_fd < 0) {
				if (errno == EINTR || errno == ECONNABORTED) {
					return -1;
				}
				warn("accept error");
				return -1;
			}

			if (getnameinfo((struct sockaddr *)&addr, addrlen,
			    host, (socklen_t)sizeof(host), NULL, 0,
			    NI_NUMERICHOST) == 0) {
				/* Strip IPv4-mapped IPv6 prefix if present */
				if (strncmp(host, "::ffff:", 7) == 0) {
					(void)strlcpy(client_ip, host + 7,
					    ip_len);
				} else {
					(void)strlcpy(client_ip, host, ip_len);
				}
			} else {
				(void)strlcpy(client_ip, "-", ip_len);
			}

			return client_fd;
		}
	}

	return -1;
}

/*
 * Close all active listening sockets.
 */
void
net_close_sockets(int *sockets, int num_socks)
{
	int i;

	for (i = 0; i < num_socks; i++) {
		if (sockets[i] >= 0) {
			(void)close(sockets[i]);
			sockets[i] = -1;
		}
	}
}
