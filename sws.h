/*
 * Main header file for sws (Simple Web Server).
 * Advanced Programming in the UNIX Environment - Stevens Institute of Tech.
 */

#ifndef _SWS_H_
#define _SWS_H_

#include <sys/types.h>
#include <sys/stat.h>

#include <limits.h>
#include <stdio.h>
#include <time.h>

#include "compat.h"

#define DEFAULT_PORT		8080
#define SERVER_NAME_STRING	"sws/1.0"
#define MAX_REQ_BUF		8192
#define MAX_HEADER_SIZE		8192
#define READ_TIMEOUT_SEC	30

/*
 * HTTP Status Codes (RFC 1945).
 */
#define HTTP_OK			200
#define HTTP_NOT_MODIFIED	304
#define HTTP_BAD_REQUEST	400
#define HTTP_FORBIDDEN		403
#define HTTP_NOT_FOUND		404
#define HTTP_INTERNAL_ERROR	500
#define HTTP_NOT_IMPLEMENTED	501

/*
 * Server runtime configuration.
 */
struct config {
	char *bind_addr;
	char *cgi_dir;
	char *docroot;
	char *log_file;
	int port;
	int debug_mode;
	int log_fd;
};

extern struct config g_cfg;

#endif /* !_SWS_H_ */
