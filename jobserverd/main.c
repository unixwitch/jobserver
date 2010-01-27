/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */
  
/*
 * jobserver: start processes and keep them running.
 */

/*
 * This is the job server, which stores configuration for jobs and is in charge
 * of (re)starting jobs at the appropriate time.
 */

#include	<sys/types.h>

#include	<stdio.h>
#include	<errno.h>
#include	<string.h>
#include	<syslog.h>
#include	<stdarg.h>
#include	<assert.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<poll.h>
#include	<port.h>
#include	<signal.h>
#include	<strings.h>

#include	"jobserver.h"
#include	"fd.h"
#include	"ctl.h"
#include	"event.h"
#include	"state.h"
#include	"sched.h"

time_t current_time;
int shutting_down;

/*
 * Log some message.
 */
void
logm(int sev, char const *msg, ...)
{
va_list	ap;

	assert(msg);

	va_start(ap, msg);
#if 1
	vsyslog(sev, msg, ap);
#else
	(void) vfprintf(stderr, msg, ap);
	(void) fputs("\n", stderr);
#endif
	va_end(ap);
}

static int port;

void
sighandle(int sig)
{
	/*
	 * Could fail, e.g. if there are too many events queued, but
	 * it's safe to ignore.  logm() cannot be called here.
	 */
	(void) port_send(port, sig, NULL);
}

int
main(argc, argv)
	char **argv;
{
	/*
	 * We need quite a few fds.
	 */
struct rlimit	nofile;
	if (getrlimit(RLIMIT_NOFILE, &nofile) == -1) {
		logm(LOG_ERR, "getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
		return 1;
	}
	nofile.rlim_max = nofile.rlim_cur = 65535;
	if (setrlimit(RLIMIT_NOFILE, &nofile) == -1) {
		logm(LOG_ERR, "setrlimit(RLIMIT_NOFILE): %s", strerror(errno));
		return 1;
	}

	if (argc != 1) {
		(void) fprintf(stderr, "%s: usage: %s",
				argv[0], argv[0]);
		return 1;
	}

	if ((port = port_create()) == -1) {
		logm(LOG_ERR, "cannot create event port: %s", strerror(errno));
		return 1;
	}

	if (fd_set_cloexec(port, 1) == -1) {
		logm(LOG_ERR, "cannot set FD_CLOEXEC on port: %s", strerror(errno));
		return 1;
	}

	if (fd_init(port) == -1) {
		logm(LOG_ERR, "cannot initialise i/o system: %s", strerror(errno));
		return 1;
	}

	if (ev_init(port, 512) == -1) {
		logm(LOG_ERR, "cannot initialise event system");
		return 1;
	}

	if (statedb_init() == -1) {
		logm(LOG_ERR, "cannot initialise database");
		return 1;
	}

	if (sched_init(port) == -1) {
		logm(LOG_ERR, "cannot initialise scheduler");
		return 1;
	}

	if (ctl_init() == -1) {
		logm(LOG_ERR, "cannot create control server: %s", strerror(errno));
		return 1;
	}

	(void) signal(SIGPIPE, SIG_IGN);
	(void) signal(SIGCHLD, SIG_IGN);
	(void) signal(SIGINT, sighandle);
	(void) signal(SIGTERM, sighandle);

	for (;;) {
	port_event_t	ev;
		/*
		 * If we're shutting down, see if everything has exited yet.
		 */
		if (shutting_down && sched_jobs_running() == 0) {
			statedb_shutdown();
			return 0;
		}

		if (port_get(port, &ev, NULL) == -1) {
			if (errno != EINTR) {
				logm(LOG_ERR, "port_get: %s", strerror(errno));
				return 1;
			}

			continue;
		}

		current_time = time(NULL);

		switch (ev.portev_source) {
		case PORT_SOURCE_FD:
			fd_handle_event(&ev);
			break;

		case PORT_SOURCE_USER:	/* signal */
			switch (ev.portev_events) {
			case SIGINT:
			case SIGTERM:
				logm(LOG_NOTICE, "shutting down (signal)");
				sched_stop_all();
				shutting_down = 1;
				exit(0);
				break;

			default:
				abort();
			}
			break;

		case PORT_SOURCE_TIMER:
			ev_handle(&ev);
			break;

		default:
			logm(LOG_ERR, "main: unexpected event type");
			abort();
		}
	}
}

void *
xrecalloc(ptr, o, n, size)
	void	*ptr;
	size_t	 o, n;
	size_t	 size;
{
	if ((ptr = realloc(ptr, n * size)) == NULL) {
		logm(LOG_ERR, "xrecalloc: out of memory");
		return NULL;
	}

	bzero(((char *) ptr) + (o * size), (n - o) * size);
	return ptr;
}

int
vasprintf(buf, fmt, ap)
	char		**buf;
	char const	 *fmt;
	va_list		  ap;
{
int	sz, ret;
va_list	ap2;
	va_copy(ap2, ap);
	if ((sz = vsnprintf(NULL, 0, fmt, ap2)) == -1)
		return -1;

	if ((*buf = malloc(sz + 1)) == NULL)
		return -1;

	ret = vsnprintf(*buf, sz + 1, fmt, ap);
	if (ret == -1) {
		free(*buf);
		*buf = NULL;
		return -1;
	}

	return ret;
}

int
asprintf(char **buf, char const *fmt, ...)
{
va_list	ap;
int	ret;
	va_start(ap, fmt);
	ret = vasprintf(buf, fmt, ap);
	va_end(ap);
	return ret;
}
