/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	FD_H
#define	FD_H

#include	<stdarg.h>
#include	<port.h>

typedef int fde_evt_type_t;
#define	FDE_READ	0x1
#define	FDE_WRITE	0x2
#define	FDE_BOTH	(FDE_READ | FDE_WRITE)

typedef void (*fde_callback) (int fde, fde_evt_type_t, void *);

/*
 * Initialise the fd subsystem.  Should only be called once,
 * from main(0.
 */
int fd_init(int);

/*
 * Low-level i/o interface.  You can use this if you want, but
 * the managed i/o functions below provide a higher-level wrapper
 * on top of this.
 */

/*
 * Setup an fd for processing.  Must be called before using any
 * other fd functions.
 */
int fd_open(int);

/*
 * Register an fd for i/o of the given type.  FDE_READ and
 * FDE_WRITE are handled separately; register or unregister
 * of one does not affect the other.
 *
 * If using managed i/o, you *cannot* register the fd for
 * the same kind of events yourself.  For example, if you use
 * fd_readline(), don't register for FDE_READ events.
 *
 * The void* argument specifies user data that will be passed
 * to the callback.  It is shared between read and write, i.e.
 * if you change it for FDE_READ, it changes for FDE_WRITE too.
 */
int register_fd(int fd, fde_evt_type_t, fde_callback, void *);

/*
 * Unregister for events on a given fd.  Unregistering one
 * type of events does not affect the other type.
 */
int unregister_fd(int fd, fde_evt_type_t);

/*
 * Unregister the fd for all events and close it.
 */
void close_fd(int fd);

/*
 * Set or clear the O_NONBLOCK flag on the given fd.
 */
int fd_set_nonblocking(int, int);

/*
 * Set or clear the FD_CLOEXEC flag on the given fd.
 */
int fd_set_cloexec(int, int);

/*
 * Managed i/o.  This provides a higher level interface than basic read/write
 * notifications.
 */

/*
 * Read a line from an fd and call the notification function when one is ready.
 * A line is terminated by \r\n.  The callback can do whatever it wants with
 * the data, including modify it, but it will be deallocated when the callback
 * returns.
 */
typedef void (*fde_rl_callback) (int fd, char *, size_t, void *);
int fd_readline(int fd, fde_rl_callback, void *);

/*
 * Write data to the fd.
 */

/* Write with printf formatting */
int fd_vprintf(int fd, char const *fmt, va_list);
/*PRINTFLIKE2*/
int fd_printf(int fd, char const *fmt, ...);

/* Write unformatted data */
int fd_puts(int fd, char const *);

/* Write unformatted data, then write a \r\n */
int fd_putln(int fd, char const *);

/* Write unformatted data that could contain embedded nuls */
int fd_write(int fd, char const *, size_t);

/* private to main() */
void fd_handle_event(port_event_t *ev);

#endif	/* !FD_H */
