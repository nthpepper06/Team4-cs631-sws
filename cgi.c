/*
 * Common Gateway Interface (CGI/1.1) execution engine (RFC 3875).
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgi.h"
#include "compat.h"
#include "http.h"
#include "log.h"
#include "sws.h"

#define CGI_TIMEOUT_SEC		30
#define MAX_ENV_VARS		64

/*
 * Check if the request is destined for CGI execution.
 */
int
cgi_is_cgi_request(const struct http_request *req)
{

	if (g_cfg.cgi_dir == NULL) {
		return 0;
	}

	if (strncmp(req->uri, "/cgi-bin", 8) == 0 &&
	    (req->uri[8] == '/' || req->uri[8] == '\0' || req->uri[8] == '?')) {
		return 1;
	}

	return 0;
}

/*
 * Build CGI/1.1 environment variables array conforming to RFC 3875.
 */
static char **
build_cgi_env(const struct http_request *req, const char *client_ip,
    const char *script_name, const char *path_info, const char *query_str)
{
	char **envp;
	char port_str[16];
	char path_trans[PATH_MAX];
	size_t count;

	if ((envp = malloc(MAX_ENV_VARS * sizeof(char *))) == NULL) {
		return NULL;
	}

	(void)snprintf(port_str, sizeof(port_str), "%d", g_cfg.port);

	count = 0;
#define ADD_ENV(fmt, ...) do {						\
		char envbuf[PATH_MAX + 1024];				\
		(void)snprintf(envbuf, sizeof(envbuf), fmt, __VA_ARGS__);\
		if (count < MAX_ENV_VARS - 1) {				\
			envp[count] = strdup(envbuf);			\
			if (envp[count] != NULL)			\
				count++;				\
		}							\
	} while (0)

	ADD_ENV("GATEWAY_INTERFACE=%s", "CGI/1.1");
	ADD_ENV("SERVER_SOFTWARE=%s", SERVER_NAME_STRING);
	ADD_ENV("SERVER_NAME=%s", (g_cfg.bind_addr != NULL) ?
	    g_cfg.bind_addr : "localhost");
	ADD_ENV("SERVER_PROTOCOL=%s", "HTTP/1.0");
	ADD_ENV("SERVER_PORT=%s", port_str);
	ADD_ENV("REQUEST_METHOD=%s", req->method);
	ADD_ENV("SCRIPT_NAME=%s", script_name);
	ADD_ENV("QUERY_STRING=%s", (query_str != NULL) ? query_str : "");
	ADD_ENV("REMOTE_ADDR=%s", client_ip);
	ADD_ENV("REMOTE_HOST=%s", client_ip);

	if (path_info != NULL && path_info[0] != '\0') {
		ADD_ENV("PATH_INFO=%s", path_info);
		(void)snprintf(path_trans, sizeof(path_trans), "%s%s",
		    g_cfg.docroot, path_info);
		ADD_ENV("PATH_TRANSLATED=%s", path_trans);
	}

	if (req->if_modified_since > 0) {
		char ims_buf[64];
		http_format_date(req->if_modified_since, ims_buf,
		    sizeof(ims_buf));
		ADD_ENV("HTTP_IF_MODIFIED_SINCE=%s", ims_buf);
	}

#undef ADD_ENV

	envp[count] = NULL;
	return envp;
}

/*
 * Free dynamically allocated environment array.
 */
static void
free_cgi_env(char **envp)
{
	size_t i;

	if (envp == NULL) {
		return;
	}

	for (i = 0; envp[i] != NULL; i++) {
		free(envp[i]);
	}
	free(envp);
}

/*
 * Execute CGI script and return formatted output to HTTP client.
 */
int
cgi_execute_request(int client_fd, const struct http_request *req,
    const char *client_ip)
{
	struct stat sb;
	pid_t pid;
	int pipefd[2];
	int status;
	int cgi_status;
	char uri_copy[2048];
	char script_path[PATH_MAX];
	char script_name[PATH_MAX];
	char resolved_cgi[PATH_MAX];
	char resolved_script[PATH_MAX];
	char date_str[64];
	char *path_info;
	char *query_str;
	char *p;
	char *slash;
	char **envp;
	char *output;
	size_t out_cap;
	size_t out_len;
	ssize_t nr, nw;
	struct pollfd pfd;
	char *body;
	size_t hdr_len;
	size_t body_len;
	size_t clen;

	(void)strlcpy(uri_copy, req->uri, sizeof(uri_copy));

	query_str = strchr(uri_copy, '?');
	if (query_str != NULL) {
		*query_str = '\0';
		query_str++;
	}

	/* Skip /cgi-bin */
	p = uri_copy + 8;
	if (*p == '/') {
		p++;
	}

	if (*p == '\0') {
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	/*
	 * Find the script file and path info components.
	 */
	path_info = NULL;
	slash = strchr(p, '/');
	if (slash != NULL) {
		*slash = '\0';
		path_info = slash + 1;
	}

	(void)snprintf(script_name, sizeof(script_name), "/cgi-bin/%s", p);
	(void)snprintf(script_path, sizeof(script_path), "%s/%s",
	    g_cfg.cgi_dir, p);

	if (stat(script_path, &sb) < 0) {
		if (errno == EACCES) {
			return http_send_error(client_fd, req, HTTP_FORBIDDEN,
			    client_ip);
		}
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	if (!S_ISREG(sb.st_mode) || access(script_path, X_OK) != 0) {
		return http_send_error(client_fd, req, HTTP_FORBIDDEN,
		    client_ip);
	}

	if (realpath(g_cfg.cgi_dir, resolved_cgi) == NULL ||
	    realpath(script_path, resolved_script) == NULL) {
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	clen = strlen(resolved_cgi);
	if (strncmp(resolved_script, resolved_cgi, clen) != 0 ||
	    (resolved_script[clen] != '\0' && resolved_script[clen] != '/')) {
		return http_send_error(client_fd, req, HTTP_FORBIDDEN,
		    client_ip);
	}

	envp = build_cgi_env(req, client_ip, script_name, path_info,
	    query_str);
	if (envp == NULL) {
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	if (pipe(pipefd) < 0) {
		free_cgi_env(envp);
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	pid = fork();
	if (pid < 0) {
		(void)close(pipefd[0]);
		(void)close(pipefd[1]);
		free_cgi_env(envp);
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	if (pid == 0) {
		/* Child Process */
		int devnull;

		(void)close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
			_exit(127);
		}
		(void)close(pipefd[1]);

		devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			(void)dup2(devnull, STDIN_FILENO);
			(void)close(devnull);
		}

		(void)close(client_fd);
		execle(resolved_script, resolved_script, (char *)NULL, envp);
		_exit(127);
	}

	/* Parent Process */
	(void)close(pipefd[1]);
	free_cgi_env(envp);

	out_cap = 8192;
	out_len = 0;
	if ((output = malloc(out_cap)) == NULL) {
		(void)close(pipefd[0]);
		(void)waitpid(pid, &status, 0);
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	pfd.fd = pipefd[0];
	pfd.events = POLLIN;

	for (;;) {
		int ready;

		ready = poll(&pfd, 1, CGI_TIMEOUT_SEC * 1000);
		if (ready <= 0) {
			(void)kill(pid, SIGKILL);
			(void)close(pipefd[0]);
			(void)waitpid(pid, &status, 0);
			free(output);
			return http_send_error(client_fd, req,
			    HTTP_INTERNAL_ERROR, client_ip);
		}

		if (out_len + 2048 >= out_cap) {
			char *new_out;
			out_cap *= 2;
			new_out = realloc(output, out_cap);
			if (new_out == NULL) {
				(void)kill(pid, SIGKILL);
				(void)close(pipefd[0]);
				(void)waitpid(pid, &status, 0);
				free(output);
				return http_send_error(client_fd, req,
				    HTTP_INTERNAL_ERROR, client_ip);
			}
			output = new_out;
		}

		nr = read(pipefd[0], output + out_len, out_cap - out_len - 1);
		if (nr <= 0) {
			break;
		}
		out_len += (size_t)nr;
	}
	output[out_len] = '\0';
	(void)close(pipefd[0]);
	(void)waitpid(pid, &status, 0);

	/*
	 * Parse CGI Response (RFC 3875 Section 6).
	 */
	body = strstr(output, "\r\n\r\n");
	if (body != NULL) {
		hdr_len = (size_t)(body - output);
		body += 4;
	} else {
		body = strstr(output, "\n\n");
		if (body != NULL) {
			hdr_len = (size_t)(body - output);
			body += 2;
		} else {
			free(output);
			return http_send_error(client_fd, req,
			    HTTP_INTERNAL_ERROR, client_ip);
		}
	}

	body_len = out_len - (size_t)(body - output);

	/* Check for Status: header */
	cgi_status = HTTP_OK;
	p = strstr(output, "Status:");
	if (p != NULL && (size_t)(p - output) < hdr_len) {
		p += 7;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		cgi_status = atoi(p);
		if (cgi_status < 100 || cgi_status > 599) {
			cgi_status = HTTP_OK;
		}
	}

	http_format_date(time(NULL), date_str, sizeof(date_str));

	/* Send HTTP status and standard headers */
	{
		char resp_hdr[4096];
		size_t rlen;
		char *line;
		char *saveptr2;
		char hdr_copy[2048];
		int has_content_length;

		has_content_length = 0;
		rlen = (size_t)snprintf(resp_hdr, sizeof(resp_hdr),
		    "HTTP/1.0 %d %s\r\n"
		    "Date: %s\r\n"
		    "Server: %s\r\n",
		    cgi_status, http_reason_phrase(cgi_status),
		    date_str, SERVER_NAME_STRING);

		if (hdr_len >= sizeof(hdr_copy)) {
			hdr_len = sizeof(hdr_copy) - 1;
		}
		(void)memcpy(hdr_copy, output, hdr_len);
		hdr_copy[hdr_len] = '\0';

		line = strtok_r(hdr_copy, "\r\n", &saveptr2);
		while (line != NULL) {
			if (strncasecmp(line, "Status:", 7) != 0) {
				if (strncasecmp(line, "Content-Length:", 15) == 0) {
					has_content_length = 1;
				}
				rlen += (size_t)snprintf(resp_hdr + rlen,
				    sizeof(resp_hdr) - rlen, "%s\r\n", line);
			}
			line = strtok_r(NULL, "\r\n", &saveptr2);
		}

		if (has_content_length == 0) {
			rlen += (size_t)snprintf(resp_hdr + rlen,
			    sizeof(resp_hdr) - rlen,
			    "Content-Length: %" PRIdMAX "\r\n",
			    (intmax_t)body_len);
		}

		rlen += (size_t)snprintf(resp_hdr + rlen,
		    sizeof(resp_hdr) - rlen, "\r\n");

		nw = write(client_fd, resp_hdr, rlen);
		(void)nw;
	}

	if (req->is_head == 0) {
		nw = write(client_fd, body, body_len);
		(void)nw;
	}

	log_request(client_ip, req->request_time, req->request_line,
	    cgi_status, (req->is_head != 0) ? 0 : (off_t)body_len);

	free(output);
	return 0;
}
