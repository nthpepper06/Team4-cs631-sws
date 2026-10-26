/*
 * sws - a simple web server
 * CS631 Advanced Programming in the UNIX Environment
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgi.h"
#include "compat.h"
#include "file.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "sws.h"

struct config g_cfg;
static volatile sig_atomic_t g_shutdown = 0;

/*
 * Print usage summary to stderr and exit.
 */
static void
usage(int exit_code)
{

	(void)fprintf(stderr,
	    "usage: %s [-dh] [-c dir] [-i address] [-l file] [-p port] dir\n",
	    getprogname());
	exit(exit_code);
}

/*
 * Signal handler for SIGCHLD to reap terminated worker child processes.
 */
static void
sigchld_handler(int sig)
{
	int saved_errno;

	(void)sig;
	saved_errno = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) {
		;
	}
	errno = saved_errno;
}

/*
 * Signal handler for graceful termination (SIGINT, SIGTERM).
 */
static void
sigterm_handler(int sig)
{

	(void)sig;
	g_shutdown = 1;
}

/*
 * Daemonize the process: detach from controlling terminal, run in background.
 */
static void
daemonize(void)
{
	pid_t pid;
	int devnull;

	pid = fork();
	if (pid < 0) {
		err(EXIT_FAILURE, "fork failed");
	}
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	if (setsid() < 0) {
		err(EXIT_FAILURE, "setsid failed");
	}

	(void)signal(SIGHUP, SIG_IGN);

	pid = fork();
	if (pid < 0) {
		err(EXIT_FAILURE, "fork failed");
	}
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	if (chdir("/") < 0) {
		err(EXIT_FAILURE, "chdir /");
	}

	umask(0);

	devnull = open("/dev/null", O_RDWR);
	if (devnull >= 0) {
		(void)dup2(devnull, STDIN_FILENO);
		(void)dup2(devnull, STDOUT_FILENO);
		(void)dup2(devnull, STDERR_FILENO);
		if (devnull > STDERR_FILENO) {
			(void)close(devnull);
		}
	}
}

/*
 * Handle a single HTTP connection from client_fd.
 */
static void
process_client(int client_fd, const char *client_ip)
{
	struct http_request req;
	int res;

	res = http_read_request(client_fd, &req);
	if (res != 0) {
		(void)http_send_error(client_fd, &req, res, client_ip);
		return;
	}

	if (cgi_is_cgi_request(&req)) {
		(void)cgi_execute_request(client_fd, &req, client_ip);
	} else {
		(void)file_serve_request(client_fd, &req, client_ip);
	}
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct stat sb;
	char canon_docroot[PATH_MAX];
	char canon_cgi[PATH_MAX];
	char canon_log[PATH_MAX];
	char client_ip[64];
	char *ep;
	int listen_socks[MAX_LISTEN_SOCKS];
	int ch;
	int client_fd;
	int num_socks;
	long lport;

	setprogname(argv[0]);

	(void)memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.port = DEFAULT_PORT;
	g_cfg.log_fd = -1;

	while ((ch = getopt(argc, argv, "c:dhi:l:p:")) != -1) {
		switch (ch) {
		case 'c':
			g_cfg.cgi_dir = optarg;
			break;
		case 'd':
			g_cfg.debug_mode = 1;
			break;
		case 'h':
			usage(EXIT_SUCCESS);
			break;
		case 'i':
			g_cfg.bind_addr = optarg;
			break;
		case 'l':
			g_cfg.log_file = optarg;
			break;
		case 'p':
			errno = 0;
			lport = strtol(optarg, &ep, 10);
			if (optarg[0] == '\0' || *ep != '\0' || errno != 0 ||
			    lport < 1 || lport > 65535) {
				errx(EXIT_FAILURE, "Invalid port: %s", optarg);
			}
			g_cfg.port = (int)lport;
			break;
		default:
			usage(EXIT_FAILURE);
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage(EXIT_FAILURE);
	}

	g_cfg.docroot = argv[0];

	/* Verify and canonicalize document root */
	if (stat(g_cfg.docroot, &sb) < 0) {
		err(EXIT_FAILURE, "%s", g_cfg.docroot);
	}
	if (!S_ISDIR(sb.st_mode)) {
		errx(EXIT_FAILURE, "%s is not a directory", g_cfg.docroot);
	}
	if (realpath(g_cfg.docroot, canon_docroot) == NULL) {
		err(EXIT_FAILURE, "realpath: %s", g_cfg.docroot);
	}
	g_cfg.docroot = canon_docroot;

	/* Verify and canonicalize CGI directory if given */
	if (g_cfg.cgi_dir != NULL) {
		if (stat(g_cfg.cgi_dir, &sb) < 0) {
			err(EXIT_FAILURE, "%s", g_cfg.cgi_dir);
		}
		if (!S_ISDIR(sb.st_mode)) {
			errx(EXIT_FAILURE, "%s is not a directory",
			    g_cfg.cgi_dir);
		}
		if (realpath(g_cfg.cgi_dir, canon_cgi) == NULL) {
			err(EXIT_FAILURE, "realpath: %s", g_cfg.cgi_dir);
		}
		g_cfg.cgi_dir = canon_cgi;
	}

	/* Canonicalize log file path before possible daemonization */
	if (g_cfg.log_file != NULL && g_cfg.debug_mode == 0) {
		if (g_cfg.log_file[0] != '/') {
			char cwd[PATH_MAX];
			if (getcwd(cwd, sizeof(cwd)) != NULL) {
				(void)snprintf(canon_log, sizeof(canon_log),
				    "%s/%s", cwd, g_cfg.log_file);
				g_cfg.log_file = canon_log;
			}
		}
	}

	/* Signal configuration */
	(void)signal(SIGPIPE, SIG_IGN);

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL) < 0) {
		err(EXIT_FAILURE, "sigaction SIGCHLD");
	}

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);

	/* Initialize subsystems */
	if (log_init(g_cfg.log_file, g_cfg.debug_mode) < 0 &&
	    g_cfg.debug_mode == 0) {
		err(EXIT_FAILURE, "log_init failed");
	}

	if (file_init() < 0) {
		err(EXIT_FAILURE, "file_init failed");
	}

	num_socks = net_listen(g_cfg.bind_addr, g_cfg.port, listen_socks,
	    MAX_LISTEN_SOCKS);
	if (num_socks <= 0) {
		errx(EXIT_FAILURE, "Unable to bind or listen on sockets");
	}

	/* Detach into background daemon if not in debug mode */
	if (g_cfg.debug_mode == 0) {
		daemonize();
	}

	/* Main accept loop */
	while (!g_shutdown) {
		client_fd = net_accept(listen_socks, num_socks, client_ip,
		    sizeof(client_ip));
		if (client_fd < 0) {
			continue;
		}

		if (g_cfg.debug_mode != 0) {
			/* Sequential execution in debug mode */
			process_client(client_fd, client_ip);
			(void)close(client_fd);
		} else {
			/* Concurrent execution via fork in daemon mode */
			pid_t pid;

			pid = fork();
			if (pid < 0) {
				warn("fork error handling client");
				(void)close(client_fd);
				continue;
			}

			if (pid == 0) {
				net_close_sockets(listen_socks, num_socks);
				process_client(client_fd, client_ip);
				(void)close(client_fd);
				_exit(EXIT_SUCCESS);
			}

			(void)close(client_fd);
		}
	}

	net_close_sockets(listen_socks, num_socks);
	file_cleanup();
	log_close();

	return EXIT_SUCCESS;
}
