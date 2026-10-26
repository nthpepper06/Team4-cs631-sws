/*
 * HTTP/1.0 protocol implementation (RFC 1945).
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"
#include "http.h"
#include "log.h"
#include "sws.h"

/*
 * Map HTTP status code to RFC 1945 reason phrase.
 */
const char *
http_reason_phrase(int status_code)
{

	switch (status_code) {
	case HTTP_OK:
		return "OK";
	case HTTP_NOT_MODIFIED:
		return "Not Modified";
	case HTTP_BAD_REQUEST:
		return "Bad Request";
	case HTTP_FORBIDDEN:
		return "Forbidden";
	case HTTP_NOT_FOUND:
		return "Not Found";
	case HTTP_INTERNAL_ERROR:
		return "Internal Server Error";
	case HTTP_NOT_IMPLEMENTED:
		return "Not Implemented";
	default:
		return "Unknown";
	}
}

/*
 * Format a time_t value into RFC 1123 GMT string.
 */
void
http_format_date(time_t t, char *buf, size_t buflen)
{
	struct tm tm_buf;

	if (gmtime_r(&t, &tm_buf) == NULL) {
		(void)strlcpy(buf, "Thu, 01 Jan 1970 00:00:00 GMT", buflen);
		return;
	}
	(void)strftime(buf, buflen, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
}

/*
 * Parse an HTTP date conforming to RFC 1123, RFC 850, or asctime formats.
 */
time_t
http_parse_date(const char *str)
{
	struct tm tm_buf;
	const char *ret;

	if (str == NULL || *str == '\0') {
		return 0;
	}

	while (*str == ' ' || *str == '\t') {
		str++;
	}

	(void)memset(&tm_buf, 0, sizeof(tm_buf));
	ret = strptime(str, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
	if (ret != NULL) {
		return timegm(&tm_buf);
	}

	(void)memset(&tm_buf, 0, sizeof(tm_buf));
	ret = strptime(str, "%a, %d-%b-%y %H:%M:%S GMT", &tm_buf);
	if (ret != NULL) {
		return timegm(&tm_buf);
	}

	(void)memset(&tm_buf, 0, sizeof(tm_buf));
	ret = strptime(str, "%a %b %e %H:%M:%S %Y", &tm_buf);
	if (ret != NULL) {
		return timegm(&tm_buf);
	}

	return 0;
}

/*
 * Read an incoming HTTP request and parse the request line and headers.
 */
int
http_read_request(int client_fd, struct http_request *req)
{
	struct pollfd pfd;
	ssize_t nr;
	size_t total;
	char *eol;
	char *hdr;
	char *p;
	char *saveptr;
	int ready;

	(void)memset(req, 0, sizeof(*req));
	req->request_time = time(NULL);
	total = 0;

	pfd.fd = client_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	for (;;) {
		ready = poll(&pfd, 1, READ_TIMEOUT_SEC * 1000);
		if (ready <= 0) {
			return HTTP_BAD_REQUEST;
		}

		nr = read(client_fd, req->raw_request + total,
		    sizeof(req->raw_request) - 1 - total);
		if (nr <= 0) {
			return HTTP_BAD_REQUEST;
		}
		total += (size_t)nr;
		req->raw_request[total] = '\0';

		if (strstr(req->raw_request, "\r\n\r\n") != NULL ||
		    strstr(req->raw_request, "\n\n") != NULL) {
			break;
		}

		if (total >= sizeof(req->raw_request) - 1) {
			return HTTP_BAD_REQUEST;
		}
	}

	/* Extract first line */
	eol = strstr(req->raw_request, "\r\n");
	if (eol == NULL) {
		eol = strchr(req->raw_request, '\n');
	}
	if (eol == NULL) {
		return HTTP_BAD_REQUEST;
	}

	if ((size_t)(eol - req->raw_request) >= sizeof(req->request_line)) {
		return HTTP_BAD_REQUEST;
	}
	(void)memcpy(req->request_line, req->raw_request,
	    (size_t)(eol - req->raw_request));
	req->request_line[eol - req->raw_request] = '\0';

	/* Parse Method */
	p = strtok_r(req->request_line, " \t", &saveptr);
	if (p == NULL) {
		return HTTP_BAD_REQUEST;
	}
	(void)strlcpy(req->method, p, sizeof(req->method));

	if (strcmp(req->method, "GET") == 0) {
		req->is_head = 0;
	} else if (strcmp(req->method, "HEAD") == 0) {
		req->is_head = 1;
	} else {
		return HTTP_NOT_IMPLEMENTED;
	}

	/* Parse Request-URI */
	p = strtok_r(NULL, " \t", &saveptr);
	if (p == NULL || p[0] != '/') {
		return HTTP_BAD_REQUEST;
	}
	(void)strlcpy(req->uri, p, sizeof(req->uri));

	/* Parse HTTP-Version */
	p = strtok_r(NULL, " \t", &saveptr);
	if (p == NULL) {
		req->is_simple = 1;
		(void)strlcpy(req->version, "HTTP/0.9", sizeof(req->version));
	} else {
		req->is_simple = 0;
		(void)strlcpy(req->version, p, sizeof(req->version));
		if (strcmp(req->version, "HTTP/1.0") != 0 &&
		    strcmp(req->version, "HTTP/1.1") != 0) {
			return HTTP_BAD_REQUEST;
		}
	}

	/* Restore request line for logging */
	(void)memcpy(req->request_line, req->raw_request,
	    (size_t)(eol - req->raw_request));
	req->request_line[eol - req->raw_request] = '\0';

	/* Parse headers for If-Modified-Since */
	hdr = eol;
	while (*hdr == '\r' || *hdr == '\n') {
		hdr++;
	}

	while (*hdr != '\0') {
		char line[1024];
		char *val;
		size_t llen;

		eol = strstr(hdr, "\r\n");
		if (eol == NULL) {
			eol = strchr(hdr, '\n');
		}
		if (eol == NULL) {
			break;
		}

		llen = (size_t)(eol - hdr);
		if (llen == 0) {
			break;
		}
		if (llen >= sizeof(line)) {
			llen = sizeof(line) - 1;
		}
		(void)memcpy(line, hdr, llen);
		line[llen] = '\0';

		if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
			val = line + 18;
			while (*val == ' ' || *val == '\t') {
				val++;
			}
			req->if_modified_since = http_parse_date(val);
		}

		hdr = eol;
		while (*hdr == '\r' || *hdr == '\n') {
			hdr++;
		}
	}

	return 0;
}

/*
 * Send HTTP/1.0 response headers to client.
 */
int
http_send_headers(int client_fd, const struct http_request *req, int status,
    const char *content_type, off_t content_length, time_t last_modified)
{
	char date_str[64];
	char lm_str[64];
	char hdr_buf[2048];
	size_t len;
	ssize_t nw;

	if (req != NULL && req->is_simple != 0) {
		return 0;
	}

	http_format_date(time(NULL), date_str, sizeof(date_str));

	len = (size_t)snprintf(hdr_buf, sizeof(hdr_buf),
	    "HTTP/1.0 %d %s\r\n"
	    "Date: %s\r\n"
	    "Server: %s\r\n",
	    status, http_reason_phrase(status),
	    date_str, SERVER_NAME_STRING);

	if (status != HTTP_NOT_MODIFIED) {
		if (last_modified > 0) {
			http_format_date(last_modified, lm_str,
			    sizeof(lm_str));
			len += (size_t)snprintf(hdr_buf + len,
			    sizeof(hdr_buf) - len,
			    "Last-Modified: %s\r\n", lm_str);
		}
		if (content_type != NULL) {
			len += (size_t)snprintf(hdr_buf + len,
			    sizeof(hdr_buf) - len,
			    "Content-Type: %s\r\n", content_type);
		}
		len += (size_t)snprintf(hdr_buf + len, sizeof(hdr_buf) - len,
		    "Content-Length: %" PRIdMAX "\r\n",
		    (intmax_t)content_length);
	}

	len += (size_t)snprintf(hdr_buf + len, sizeof(hdr_buf) - len, "\r\n");

	nw = write(client_fd, hdr_buf, len);
	if (nw < 0) {
		return -1;
	}

	return 0;
}

/*
 * Send standard HTTP error response.
 */
int
http_send_error(int client_fd, const struct http_request *req, int status,
    const char *client_ip)
{
	char body[1024];
	const char *reason;
	size_t body_len;
	ssize_t nw;
	time_t req_time;
	const char *req_line;

	reason = http_reason_phrase(status);
	body_len = (size_t)snprintf(body, sizeof(body),
	    "<!DOCTYPE html>\n"
	    "<html>\n"
	    "<head><title>%d %s</title></head>\n"
	    "<body>\n"
	    "<h1>%d %s</h1>\n"
	    "<p>The requested operation could not be completed.</p>\n"
	    "<hr>\n"
	    "<address>%s</address>\n"
	    "</body>\n"
	    "</html>\n",
	    status, reason, status, reason, SERVER_NAME_STRING);

	(void)http_send_headers(client_fd, req, status, "text/html",
	    (off_t)body_len, 0);

	if (req == NULL || (req->is_head == 0)) {
		nw = write(client_fd, body, body_len);
		(void)nw;
	}

	req_time = (req != NULL) ? req->request_time : time(NULL);
	req_line = (req != NULL && req->request_line[0] != '\0') ?
	    req->request_line : "-";

	log_request(client_ip, req_time, req_line, status,
	    (req != NULL && req->is_head != 0) ? 0 : (off_t)body_len);

	return 0;
}
