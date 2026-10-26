/*
 * HTTP protocol definitions and prototypes (RFC 1945).
 */

#ifndef _HTTP_H_
#define _HTTP_H_

#include <sys/types.h>
#include <time.h>

#include "sws.h"

struct http_request {
	char raw_request[MAX_REQ_BUF];
	char request_line[1024];
	char method[16];
	char uri[2048];
	char version[16];
	int is_simple;
	int is_head;
	time_t if_modified_since;
	time_t request_time;
};

const char *http_reason_phrase(int);
void http_format_date(time_t, char *, size_t);
time_t http_parse_date(const char *);
int http_read_request(int, struct http_request *);
int http_send_headers(int, const struct http_request *, int,
    const char *, off_t, time_t);
int http_send_error(int, const struct http_request *, int, const char *);

#endif /* !_HTTP_H_ */
