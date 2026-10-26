/*
 * File serving, directory indexing, and MIME detection for sws.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <magic.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"
#include "file.h"
#include "http.h"
#include "log.h"
#include "sws.h"

static magic_t g_magic = NULL;

/*
 * Initialize libmagic for MIME type resolution.
 */
int
file_init(void)
{

	g_magic = magic_open(MAGIC_MIME_TYPE);
	if (g_magic == NULL) {
		warnx("Unable to initialize libmagic");
		return -1;
	}

	if (magic_load(g_magic, NULL) != 0) {
		warnx("Unable to load default magic database: %s",
		    magic_error(g_magic));
		magic_close(g_magic);
		g_magic = NULL;
		return -1;
	}

	return 0;
}

/*
 * Clean up libmagic resources.
 */
void
file_cleanup(void)
{

	if (g_magic != NULL) {
		magic_close(g_magic);
		g_magic = NULL;
	}
}

/*
 * Detect MIME content-type using libmagic with fallback to octet-stream.
 */
static const char *
get_mime_type(const char *path)
{
	const char *mime;

	if (g_magic != NULL) {
		mime = magic_file(g_magic, path);
		if (mime != NULL) {
			return mime;
		}
	}

	return "application/octet-stream";
}

/*
 * Compare two string pointers for qsort (alphanumeric order).
 */
static int
compare_entries(const void *a, const void *b)
{
	const char *const *sa;
	const char *const *sb;

	sa = a;
	sb = b;
	return strcmp(*sa, *sb);
}

/*
 * Sanitize and canonicalize URI path to prevent directory traversal.
 * Returns 0 on success, -1 on invalid path (e.g. escaping root).
 */
static int
sanitize_path(const char *raw_uri, char *clean_path, size_t maxlen)
{
	char seg[PATH_MAX];
	const char *src;
	char *dst;
	char *token;
	char *saveptr;
	char *stack[128];
	int depth;
	int i;

	src = raw_uri;
	while (*src == '/') {
		src++;
	}

	(void)strlcpy(seg, src, sizeof(seg));
	dst = strchr(seg, '?');
	if (dst != NULL) {
		*dst = '\0';
	}

	depth = 0;
	token = strtok_r(seg, "/", &saveptr);
	while (token != NULL) {
		if (strcmp(token, ".") == 0) {
			/* Ignore single dot */
		} else if (strcmp(token, "..") == 0) {
			if (depth > 0) {
				depth--;
			} else {
				/* Attempted to escape above root */
				return -1;
			}
		} else {
			if (depth < 127) {
				stack[depth++] = token;
			} else {
				return -1;
			}
		}
		token = strtok_r(NULL, "/", &saveptr);
	}

	clean_path[0] = '\0';
	for (i = 0; i < depth; i++) {
		if (strlcat(clean_path, "/", maxlen) >= maxlen ||
		    strlcat(clean_path, stack[i], maxlen) >= maxlen) {
			return -1;
		}
	}

	if (clean_path[0] == '\0') {
		(void)strlcpy(clean_path, "/", maxlen);
	}

	return 0;
}

/*
 * Generate and serve an HTML directory index in alphanumeric order.
 * Files starting with '.' are ignored.
 */
static int
serve_directory_index(int client_fd, const struct http_request *req,
    const char *dir_path, const char *display_uri,
    const struct stat *dir_sb, const char *client_ip)
{
	DIR *dirp;
	struct dirent *dp;
	char **entries;
	char entry_path[PATH_MAX];
	struct stat ent_sb;
	char *html_buf;
	size_t buf_cap;
	size_t buf_len;
	size_t count;
	size_t max_entries;
	size_t i;
	ssize_t nw;
	int is_dir;

	if ((dirp = opendir(dir_path)) == NULL) {
		if (errno == EACCES) {
			return http_send_error(client_fd, req, HTTP_FORBIDDEN,
			    client_ip);
		}
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	count = 0;
	max_entries = 64;
	if ((entries = malloc(max_entries * sizeof(char *))) == NULL) {
		(void)closedir(dirp);
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.') {
			continue;
		}

		if (count >= max_entries) {
			char **new_entries;
			max_entries *= 2;
			new_entries = realloc(entries,
			    max_entries * sizeof(char *));
			if (new_entries == NULL) {
				for (i = 0; i < count; i++) {
					free(entries[i]);
				}
				free(entries);
				(void)closedir(dirp);
				return http_send_error(client_fd, req,
				    HTTP_INTERNAL_ERROR, client_ip);
			}
			entries = new_entries;
		}

		entries[count] = strdup(dp->d_name);
		if (entries[count] != NULL) {
			count++;
		}
	}
	(void)closedir(dirp);

	qsort(entries, count, sizeof(char *), compare_entries);

	buf_cap = 4096;
	if ((html_buf = malloc(buf_cap)) == NULL) {
		for (i = 0; i < count; i++) {
			free(entries[i]);
		}
		free(entries);
		return http_send_error(client_fd, req, HTTP_INTERNAL_ERROR,
		    client_ip);
	}

	buf_len = (size_t)snprintf(html_buf, buf_cap,
	    "<!DOCTYPE html>\n"
	    "<html>\n"
	    "<head>\n"
	    "<meta charset=\"utf-8\">\n"
	    "<title>Index of %s</title>\n"
	    "</head>\n"
	    "<body>\n"
	    "<h1>Index of %s</h1>\n"
	    "<hr>\n"
	    "<ul>\n",
	    display_uri, display_uri);

	for (i = 0; i < count; i++) {
		char line[1024];
		size_t line_len;

		(void)snprintf(entry_path, sizeof(entry_path), "%s/%s",
		    dir_path, entries[i]);
		is_dir = 0;
		if (stat(entry_path, &ent_sb) == 0 &&
		    S_ISDIR(ent_sb.st_mode)) {
			is_dir = 1;
		}

		line_len = (size_t)snprintf(line, sizeof(line),
		    "<li><a href=\"%s%s\">%s%s</a></li>\n",
		    entries[i], is_dir ? "/" : "",
		    entries[i], is_dir ? "/" : "");

		while (buf_len + line_len + 256 >= buf_cap) {
			char *new_buf;
			buf_cap *= 2;
			new_buf = realloc(html_buf, buf_cap);
			if (new_buf == NULL) {
				for (i = 0; i < count; i++) {
					free(entries[i]);
				}
				free(entries);
				free(html_buf);
				return http_send_error(client_fd, req,
				    HTTP_INTERNAL_ERROR, client_ip);
			}
			html_buf = new_buf;
		}

		(void)memcpy(html_buf + buf_len, line, line_len);
		buf_len += line_len;
		html_buf[buf_len] = '\0';
	}

	for (i = 0; i < count; i++) {
		free(entries[i]);
	}
	free(entries);

	buf_len += (size_t)snprintf(html_buf + buf_len, buf_cap - buf_len,
	    "</ul>\n"
	    "<hr>\n"
	    "<address>%s</address>\n"
	    "</body>\n"
	    "</html>\n",
	    SERVER_NAME_STRING);

	(void)http_send_headers(client_fd, req, HTTP_OK, "text/html",
	    (off_t)buf_len, dir_sb->st_mtime);

	if (req->is_head == 0) {
		nw = write(client_fd, html_buf, buf_len);
		(void)nw;
	}

	log_request(client_ip, req->request_time, req->request_line,
	    HTTP_OK, (req->is_head != 0) ? 0 : (off_t)buf_len);

	free(html_buf);
	return 0;
}

/*
 * Serve a regular file over HTTP.
 */
static int
serve_regular_file(int client_fd, const struct http_request *req,
    const char *file_path, const struct stat *sb, const char *client_ip)
{
	char io_buf[8192];
	const char *mime;
	ssize_t nr, nw;
	int fd;

	if (access(file_path, R_OK) != 0) {
		return http_send_error(client_fd, req, HTTP_FORBIDDEN,
		    client_ip);
	}

	if (req->if_modified_since > 0 &&
	    sb->st_mtime <= req->if_modified_since) {
		(void)http_send_headers(client_fd, req, HTTP_NOT_MODIFIED,
		    NULL, 0, sb->st_mtime);
		log_request(client_ip, req->request_time, req->request_line,
		    HTTP_NOT_MODIFIED, 0);
		return 0;
	}

	if ((fd = open(file_path, O_RDONLY)) < 0) {
		if (errno == EACCES) {
			return http_send_error(client_fd, req, HTTP_FORBIDDEN,
			    client_ip);
		}
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	mime = get_mime_type(file_path);

	(void)http_send_headers(client_fd, req, HTTP_OK, mime,
	    sb->st_size, sb->st_mtime);

	if (req->is_head == 0) {
		while ((nr = read(fd, io_buf, sizeof(io_buf))) > 0) {
			nw = write(client_fd, io_buf, (size_t)nr);
			if (nw < 0) {
				break;
			}
		}
	}
	(void)close(fd);

	log_request(client_ip, req->request_time, req->request_line,
	    HTTP_OK, (req->is_head != 0) ? 0 : sb->st_size);

	return 0;
}

/*
 * Resolve URI to local filesystem path and serve content.
 * Handles document root, user directories (/~user/sws), and index.html.
 */
int
file_serve_request(int client_fd, const struct http_request *req,
    const char *client_ip)
{
	struct passwd *pw;
	struct stat sb, index_sb;
	char base_dir[PATH_MAX];
	char sanitized[PATH_MAX];
	char full_path[PATH_MAX];
	char index_path[PATH_MAX];
	char resolved_base[PATH_MAX];
	char resolved_full[PATH_MAX];
	const char *uri;
	size_t blen;

	uri = req->uri;

	/*
	 * User Directory Resolution (/~username/...)
	 */
	if (strncmp(uri, "/~", 2) == 0) {
		char username[64];
		const char *u_end;
		size_t ulen;

		u_end = strchr(uri + 2, '/');
		if (u_end != NULL) {
			ulen = (size_t)(u_end - (uri + 2));
		} else {
			ulen = strlen(uri + 2);
		}

		if (ulen == 0 || ulen >= sizeof(username)) {
			return http_send_error(client_fd, req, HTTP_NOT_FOUND,
			    client_ip);
		}
		(void)memcpy(username, uri + 2, ulen);
		username[ulen] = '\0';

		pw = getpwnam(username);
		if (pw == NULL) {
			return http_send_error(client_fd, req, HTTP_NOT_FOUND,
			    client_ip);
		}

		(void)snprintf(base_dir, sizeof(base_dir), "%s/sws",
		    pw->pw_dir);
		if (u_end != NULL) {
			if (sanitize_path(u_end, sanitized, sizeof(sanitized))
			    < 0) {
				return http_send_error(client_fd, req,
				    HTTP_FORBIDDEN, client_ip);
			}
		} else {
			(void)strlcpy(sanitized, "/", sizeof(sanitized));
		}
	} else {
		/* Regular document root */
		(void)strlcpy(base_dir, g_cfg.docroot, sizeof(base_dir));
		if (sanitize_path(uri, sanitized, sizeof(sanitized)) < 0) {
			return http_send_error(client_fd, req, HTTP_FORBIDDEN,
			    client_ip);
		}
	}

	/* Verify base directory exists */
	if (realpath(base_dir, resolved_base) == NULL) {
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	if (strcmp(sanitized, "/") == 0) {
		(void)strlcpy(full_path, resolved_base, sizeof(full_path));
	} else {
		(void)snprintf(full_path, sizeof(full_path), "%s%s",
		    resolved_base, sanitized);
	}

	if (stat(full_path, &sb) < 0) {
		if (errno == EACCES) {
			return http_send_error(client_fd, req, HTTP_FORBIDDEN,
			    client_ip);
		}
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	/* Prevent symlink sandbox escapes */
	if (realpath(full_path, resolved_full) == NULL) {
		return http_send_error(client_fd, req, HTTP_NOT_FOUND,
		    client_ip);
	}

	blen = strlen(resolved_base);
	if (strncmp(resolved_full, resolved_base, blen) != 0 ||
	    (resolved_full[blen] != '\0' && resolved_full[blen] != '/')) {
		return http_send_error(client_fd, req, HTTP_FORBIDDEN,
		    client_ip);
	}

	/* Directory Handling */
	if (S_ISDIR(sb.st_mode)) {
		(void)snprintf(index_path, sizeof(index_path), "%s/index.html",
		    full_path);
		if (stat(index_path, &index_sb) == 0 &&
		    S_ISREG(index_sb.st_mode)) {
			return serve_regular_file(client_fd, req, index_path,
			    &index_sb, client_ip);
		}
		return serve_directory_index(client_fd, req, full_path,
		    req->uri, &sb, client_ip);
	}

	/* Regular File Handling */
	if (S_ISREG(sb.st_mode)) {
		return serve_regular_file(client_fd, req, full_path, &sb,
		    client_ip);
	}

	return http_send_error(client_fd, req, HTTP_FORBIDDEN, client_ip);
}
