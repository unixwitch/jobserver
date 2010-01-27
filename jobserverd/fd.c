/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<assert.h>
#include	<unistd.h>
#include	<poll.h>
#include	<port.h>
#include	<errno.h>
#include	<string.h>
#include	<stdlib.h>
#include	<fcntl.h>
#include	<stdio.h>
#include	<stdarg.h>
#include	<xti.h>
#include	<strings.h>

#include	"fd.h"
#include	"jobserver.h"
#include	"buffer.h"

/*
 * Create poll-style flags from fde-style flags.
 */
#define	FDE_FLAGS_TO_POLL(fl)			\
	(((fl & FDE_READ) ? POLLIN : 0)		\
	| ((fl & FDE_WRITE) ? POLLOUT : 0))

static int port = -1;

#define	FD_BUF_SIZE 16384	/* XXX Make this dynamic */

typedef struct fde {
	int		 fde_fd;
	int		 fde_flags;
	fde_callback	 fde_read_callback;
	fde_callback	 fde_write_callback;
	fde_rl_callback	 fde_rl_callback;
	buffer_t	 fde_wbuf;
	buffer_t	 fde_rbuf;
	void		*fde_udata;
} fde_t;

static fde_t *fd_table;
static int nfds;

static int fd_drain(int fd);

int
fd_init(prt)
	int	prt;
{
	if (port != -1)
		return (0);

	port = prt;

#if 0
	nfds = getdtablesize();
	if ((fd_table = calloc(sizeof (*fd_table), nfds)) == NULL)
		return (-1);
#endif

	return (0);
}

int
fd_open(fd)
	int	 fd;
{
fde_t	*nfdt, *e;
#ifndef NDEBUG
int	dts = getdtablesize();
	assert(fd >= 0 && fd < dts);
#endif	/* !NDEBUG */

	if (fd >= nfds) {
		if ((nfdt = xrecalloc(fd_table, fd, fd + 1,
		    sizeof (fde_t))) == NULL)
			return (-1);

		fd_table = nfdt;
		nfds = fd + 1;
	}

	bzero(&fd_table[fd], sizeof (fde_t));
	e = &fd_table[fd];
	e->fde_fd = fd;

	if (fd_set_cloexec(fd, 1) == -1) {
		logm(LOG_WARNING, "fd_open: "
		    "fd_set_cloexec failed: %s",
		    strerror(errno));
		close_fd(fd);
		return (-1);
	}

	return (0);
}

int
register_fd(fd, type, callback, udata)
	int		 fd;
	fde_evt_type_t	 type;
	fde_callback	 callback;
	void		*udata;
{
fde_t	*e;
int	 flags;
	assert(fd >= 0 && fd < nfds);
	assert(port != -1);
	assert((type & ~FDE_BOTH) == 0);

	/*
	 * We need to be careful here that the fd_table entry is not modified
	 * if we fail port_associate().
	 */
	e = &fd_table[fd];

	e->fde_fd = fd;
	flags = e->fde_flags | type;

	if (port_associate(port, PORT_SOURCE_FD, fd,
	    FDE_FLAGS_TO_POLL(flags), NULL) == -1) {
		logm(LOG_ERR, "cannot associate fd %d with port: %s",
				fd, strerror(errno));
		return (-1);
	}

	e->fde_flags = flags;
	e->fde_udata = udata;

	if (type & FDE_READ)
		e->fde_read_callback = callback;
	if (type & FDE_WRITE)
		e->fde_write_callback = callback;

	return (0);
}

int
unregister_fd(fd, type)
	int		fd;
	fde_evt_type_t	type;
{
int	flags = 0;
fde_t	*e;

	assert(fd < nfds && fd_table[fd].fde_fd == fd);
	assert(port != -1);
	assert((type & ~FDE_BOTH) == 0);

	e = &fd_table[fd];

	flags = FDE_FLAGS_TO_POLL(e->fde_flags);

	if (type & FDE_READ)
		flags &= ~POLLIN;
	if (type & FDE_WRITE)
		flags &= ~POLLOUT;

	if (port_associate(port, PORT_SOURCE_FD, fd, flags, NULL) == -1) {
		logm(LOG_ERR, "cannot associate fd %d with port: %s",
				fd, strerror(errno));
		return (-1);
	}

	if (type & FDE_READ) {
		e->fde_flags &= ~FDE_READ;
		e->fde_read_callback = NULL;
	}

	if (type & FDE_WRITE) {
		e->fde_flags &= ~FDE_WRITE;
		e->fde_write_callback = NULL;
	}

	return (0);
}

void
close_fd(fd)
	int	fd;
{
	assert(fd >= 0 && fd < nfds);
	assert(fd_table[fd].fde_fd == fd);
	assert(port != -1);

	(void) unregister_fd(fd, FDE_BOTH);
	(void) close(fd);

	(void) buf_clear(&fd_table[fd].fde_wbuf);
	(void) buf_clear(&fd_table[fd].fde_rbuf);
	bzero(&fd_table[fd], sizeof (fde_t));
	fd_table[fd].fde_fd = -1;
}

void
fd_handle_event(ev)
	port_event_t	*ev;
{
fde_t	*e;
	assert(ev);
	assert(ev->portev_object < nfds);

	e = &fd_table[ev->portev_object];

	if (ev->portev_events & POLLIN) {
		assert((e->fde_flags & FDE_READ) && e->fde_read_callback);
		e->fde_read_callback(e->fde_fd, FDE_READ, e->fde_udata);
		e = &fd_table[ev->portev_object];
	}

	if (ev->portev_events & POLLOUT) {
		assert((e->fde_flags & FDE_WRITE) && e->fde_write_callback);
		e->fde_write_callback(e->fde_fd, FDE_WRITE, e->fde_udata);
		e = &fd_table[ev->portev_object];
	}

	/*
	 * If the callback didn't explicitly unregister the fd, we need
	 * to re-associate it, since port events are one-shot.
	 */
	if (e->fde_flags) {
		if (port_associate(port, PORT_SOURCE_FD, e->fde_fd,
		    FDE_FLAGS_TO_POLL(e->fde_flags), NULL) == -1) {
			logm(LOG_ERR, "cannot associate fd %d with port: %s",
					e->fde_fd, strerror(errno));
			/* signal error? */
			return;
		}
	}
}

/*
 * Callback for fd_readline.  Reads pending data, and checks
 * if an entire line has been read yet.  If so, calls the
 * readline callback function for each line.
 */
/*ARGSUSED*/
static void
fd_readline_callback(fd, type, udata)
	int		 fd;
	fde_evt_type_t	 type;
	void		*udata;
{
fde_t	*e;
char	*p, *q;
size_t	 bytesleft; /* space left in fde_rbuf */
ssize_t	 i;
int	 save_errno, save_terrno;
char	 rbuf[1024];
int	 flags = 0;

	assert(fd < nfds);
	assert(type == FDE_READ || type == FDE_WRITE);

	e = &fd_table[fd];
	bytesleft = min(FD_BUF_SIZE - e->fde_rbuf.b_size, 1024);

	/*
	 * Fill the buffer with data.  We only read up to FD_BUF_SIZE; if more
	 * data is still available, it'll be caught the next time round.  This
	 * gives other fds a chance to be processed even if one fd is sending
	 * an excessive amount of data.
	 */
	while ((i = t_rcv(fd, rbuf, bytesleft, &flags)) > 0) {
		if (buf_append(&e->fde_rbuf, rbuf, i) == -1) {
			logm(LOG_ERR, "fd=%d "
			    "fd_readline_callback: buf_append failed",
			    e->fde_fd);
			return;
		}

		if (e->fde_rbuf.b_size >= FD_BUF_SIZE)
			break;

		bytesleft -= i;
	}

	save_terrno = t_errno;
	save_errno = errno;

	if (e->fde_rbuf.b_size) {
	size_t	nbytes = 0;
		/*
		 * Handle any pending lines before we handle the error from
		 * t_rcv().  This means that if the fd is closed for some
		 * reason, we still handle pending data sent before the close.
		 */
		/* Insert a nul byte */
		if (buf_append(&e->fde_rbuf, "", 1) == -1) {
			logm(LOG_ERR, "fd=%d fd_readline_callback: "
			    "buf_append failed: %s",
			    e->fde_fd, strerror(errno));
			return;
		}

		for (p = e->fde_rbuf.b_data,
		    q = strstr(e->fde_rbuf.b_data, "\r\n");
		    q != NULL;
		    p = q + 2, q = strstr(p, "\r\n"))
		{
			*q = 0;
			/*LINTED*/
			nbytes += (q - p) + 2;

			e->fde_rl_callback(e->fde_fd, p,
			    strlen(p), e->fde_udata);

			/*
			 * The address of 'e' can change after the callback if
			 * something causes fd_table to be reallocated.
			 */
			e = &fd_table[fd];

			/* Check if the user unregistered the callback */
			if (e->fde_rl_callback == NULL)
				break;
		}

		/* Remove the nul byte we added */
		if (e->fde_rbuf.b_size) {
			if (buf_resize(&e->fde_rbuf,
			    e->fde_rbuf.b_size - 1) == -1) {
				logm(LOG_ERR, "fd_readline_callback:"
				    "buf_resize failed");
				if (e->fde_rl_callback)
					e->fde_rl_callback(e->fde_fd,
					    NULL, errno, e->fde_udata);
				return;
			}

			/*
			 * Move the remaining data back to the start of the
			 * buffer.
			 */
			if (buf_erase(&e->fde_rbuf, 0, nbytes) == -1) {
				logm(LOG_ERR, "fd_readline_callback:"
				    "buf_erase failed");
				if (e->fde_rl_callback)
					e->fde_rl_callback(e->fde_fd, NULL,
					    errno, e->fde_udata);
				return;
			}
		}
	}

	if (i == -1) {
		if (save_terrno == TNODATA ||
		    (save_terrno == TSYSERR && save_errno == EINTR))
			return;
		if (e->fde_rl_callback)
			e->fde_rl_callback(e->fde_fd, NULL,
			    errno, e->fde_udata);
	} else if (i == 0) {
		/* EOF */
		if (unregister_fd(fd, FDE_READ) == -1)
			logm(LOG_WARNING, "fd_readline_callback: "
			    "unregister_fd failed: %s",
			    strerror(errno));

		e->fde_rl_callback(fd, NULL, 0, e->fde_udata);
	}
}

int
fd_readline(fd, callback, udata)
	int		 fd;
	fde_rl_callback	 callback;
	void		*udata;
{
	assert(fd > 0);
	assert(callback);

	fd_table[fd].fde_rl_callback = callback;
	return (register_fd(fd, FDE_READ, fd_readline_callback, udata));
}

int
fd_set_nonblocking(fd, nb)
	int	fd, nb;
{
int	flags;

	assert(fd >= 0);
	assert(nb == 1 || nb == 0);

	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		return (-1);

	if (nb)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	return (fcntl(fd, F_SETFL, flags));
}

int
fd_set_cloexec(fd, ce)
	int	fd, ce;
{
int	flags;

	assert(fd >= 0);
	assert(ce == 1 || ce == 0);

	if ((flags = fcntl(fd, F_GETFD, 0)) == -1)
		return (-1);

	if (ce)
		flags |= FD_CLOEXEC;
	else
		flags &= ~FD_CLOEXEC;

	return (fcntl(fd, F_SETFD, flags));
}

/*ARGSUSED*/
void
fd_write_callback(fd, type, udata)
	int		 fd;
	fde_evt_type_t	 type;
	void		*udata;
{
fde_t	*e;

	assert(fd >= 0 && fd < nfds);

	e = &fd_table[fd];
	if (fd_drain(fd) == -1)
		logm(LOG_WARNING, "fd_write_callback: fd_drain failed");

	if (!e->fde_wbuf.b_size)
		if (unregister_fd(fd, FDE_WRITE) == -1)
			logm(LOG_WARNING, "fd_write_callback: "
			    "unregister_fd failed");
}

int
fd_write(fd, buf, sz)
	int		 fd;
	char const	*buf;
	size_t		 sz;
{
fde_t	*e;

	assert(fd >= 0 && fd < nfds);

	e = &fd_table[fd];
	if (buf_append(&e->fde_wbuf, buf, sz) == -1)
		return (-1);

	if (fd_drain(fd) == -1 && errno != EAGAIN)
		logm(LOG_WARNING, "fd_write: fd_drain failed: %s",
				strerror(errno));

	if (e->fde_wbuf.b_size)
		if (register_fd(fd, FDE_WRITE, fd_write_callback, NULL) == -1)
			return (-1);
	return (0);
}

/*
 * Write out all pending data from fd's buffer, if possible.
 */
static int
fd_drain(fd)
	int	 fd;
{
fde_t	*e;

	assert(fd >= 0 && fd < nfds);

	e = &fd_table[fd];
	for (;;) {
	ssize_t	n;
		if ((n = t_snd(e->fde_fd, e->fde_wbuf.b_data,
		    e->fde_wbuf.b_size, 0)) == -1) {
			if (t_errno == TSYSERR && errno == EINTR)
				continue;
			return (-1);
		}

		(void) buf_erase(&e->fde_wbuf, 0, n);

		if (e->fde_wbuf.b_size == 0)
			return (0);
	}
}

int
fd_puts(fd, str)
	int		 fd;
	char const	*str;
{
	return (fd_write(fd, str, strlen(str)));
}

int
fd_putln(fd, str)
	int		 fd;
	char const	*str;
{
	if (fd_puts(fd, str) == -1)
		return (-1);
	return (fd_puts(fd, "\r\n"));
}

#if 0
int
fd_vprintf(fd, fmt, ap)
	int		 fd;
	char const	*fmt;
	va_list		 ap;
#endif
int
fd_vprintf(int fd, char const *fmt, va_list ap)
{
int	 len;
char	*buf;
	if ((len = vsnprintf(NULL, 0, fmt, ap)) == -1)
		return (-1);

	if ((buf = malloc(len + 1)) == NULL)
		return (-1);

	len = vsnprintf(buf, len + 1, fmt, ap);

	if (fd_write(fd, buf, len) == -1) {
		free(buf);
		return (-1);
	}

	free(buf);
	return (0);
}

int
fd_printf(int fd, char const *fmt, ...)
{
int	i;
va_list	ap;
	va_start(ap, fmt);
	i = fd_vprintf(fd, fmt, ap);
	va_end(ap);
	return (i);
}
