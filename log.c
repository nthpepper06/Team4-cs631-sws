/*
 * Request logging subsystem for sws.
 * Formats: '%a %t "%r" %>s %b'
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"
#include "log.h"

static int g_log_fd = -1;
static int g_debug_active = 0;

/*
 * Initialize logging system. If log_path is provided, open the file
 * in append mode. If debug_mode is active, logging goes to stdout.
 */
int
log_init(const char *log_path, int debug_mode)
{
	int fd;

	g_debug_active = debug_mode;

	if (debug_mode != 0) {
		return 0;
	}

	if (log_path == NULL) {
		g_log_fd = -1;
		return 0;
	}

	fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		warn("Unable to open log file %s", log_path);
		return -1;
	}

	g_log_fd = fd;
	return 0;
}

/*
 * Log an HTTP request using Apache Common format variation:
 * '%a %t "%r" %>s %b'
 */
void
log_request(const char *remote_ip, time_t req_time, const char *req_line,
    int status, off_t bytes_sent)
{
	struct tm tm_buf;
	ssize_t nw;
	size_t len;
	char clean_req[1024];
	char log_buf[2048];
	char time_buf[64];
	const char *ip_str;
	char *p;

	if (g_debug_active == 0 && g_log_fd < 0) {
		return;
	}

	ip_str = (remote_ip != NULL && remote_ip[0] != '\0') ? remote_ip : "-";

	if (gmtime_r(&req_time, &tm_buf) == NULL) {
		(void)strlcpy(time_buf, "1970-01-01T00:00:00Z",
		    sizeof(time_buf));
	} else {
		(void)strftime(time_buf, sizeof(time_buf),
		    "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
	}

	if (req_line != NULL && req_line[0] != '\0') {
		(void)strlcpy(clean_req, req_line, sizeof(clean_req));
		if ((p = strchr(clean_req, '\r')) != NULL) {
			*p = '\0';
		}
		if ((p = strchr(clean_req, '\n')) != NULL) {
			*p = '\0';
		}
	} else {
		(void)strlcpy(clean_req, "-", sizeof(clean_req));
	}

	len = (size_t)snprintf(log_buf, sizeof(log_buf),
	    "%s %s \"%s\" %d %" PRIdMAX "\n",
	    ip_str, time_buf, clean_req, status, (intmax_t)bytes_sent);

	if (len >= sizeof(log_buf)) {
		len = sizeof(log_buf) - 1;
		log_buf[len] = '\0';
	}

	if (g_debug_active != 0) {
		(void)printf("%s", log_buf);
		(void)fflush(stdout);
	} else if (g_log_fd >= 0) {
		nw = write(g_log_fd, log_buf, len);
		(void)nw;
	}
}

/*
 * Close the log file descriptor if open.
 */
void
log_close(void)
{

	if (g_log_fd >= 0) {
		(void)close(g_log_fd);
		g_log_fd = -1;
	}
}
