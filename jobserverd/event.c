/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<stdlib.h>
#include	<time.h>
#include	<syslog.h>
#include	<errno.h>
#include	<string.h>
#include	<strings.h>

#include	"jobserver.h"
#include	"event.h"
#include	"queue.h"

static timer_t ev_timer;

typedef struct event {
	ev_id_t		 ev_id;
	int		 ev_repeat;
	time_t		 ev_abstime;
	time_t		 ev_freq;
	ev_callback	 ev_func;
	void		*ev_udata;
	LIST_ENTRY(event) ev_entries;
} event_t;
static LIST_HEAD(events_list, event) events;

static int port;
static time_t ev_next_run;
static event_t *get_event(void);
static void ev_recalc();
static ev_id_t next_ev_id;

int
ev_init(prt)
	int	prt;
{
struct sigevent	ev;
port_notify_t	nfy;

	port = prt;
	LIST_INIT(&events);

	bzero(&ev, sizeof (ev));
	bzero(&nfy, sizeof (nfy));

	ev.sigev_notify = SIGEV_PORT;
	ev.sigev_value.sival_ptr = &nfy;
	nfy.portnfy_port = port;

	if (timer_create(CLOCK_REALTIME, &ev, &ev_timer) == -1) {
		logm(LOG_ERR, "ev_init: timer_create: %s", strerror(errno));
		return (-1);
	}

	return (0);
}

static event_t *
get_event()
{
event_t	*ev;
	if ((ev = calloc(1, sizeof (*ev))) == NULL) {
		logm(LOG_ERR, "get_event: out of memory");
		return (NULL);
	}

	LIST_INSERT_HEAD(&events, ev, ev_entries);
	ev->ev_id = next_ev_id++;
	return (ev);
}

ev_id_t
ev_add(when, func, udata)
	time_t		 when;
	ev_callback	 func;
	void		*udata;
{
event_t	*ev;

	if ((ev = get_event()) == NULL)
		return (-1);

	ev->ev_func = func;
	ev->ev_udata = udata;
	ev->ev_repeat = 1;
	ev->ev_freq = when;
	ev->ev_abstime = current_time + when;

	ev_recalc();
	return (ev->ev_id);
}

ev_id_t
ev_add_once(when, func, udata)
	time_t		 when;
	ev_callback	 func;
	void		*udata;
{
event_t	*ev;

	if ((ev = get_event()) == NULL)
		return (-1);

	ev->ev_func = func;
	ev->ev_udata = udata;
	ev->ev_repeat = 0;
	ev->ev_freq = when;
	ev->ev_abstime = current_time + when;

	ev_recalc();
	return (ev->ev_id);
}

static void
ev_recalc()
{
event_t	*ev;
struct itimerspec ts;

	ev_next_run = 0;
	LIST_FOREACH(ev, &events, ev_entries) {
		if ((ev_next_run == 0 ||
		    ev_next_run > ev->ev_abstime))
			ev_next_run = ev->ev_abstime;
	}

	bzero(&ts, sizeof (ts));

	if (ev_next_run == 0) {
		if (timer_settime(ev_timer, 0, &ts, NULL) == -1)
			logm(LOG_ERR, "ev_recalc: timer_settime: %s",
			    strerror(errno));
		return;
	}

	ts.it_value.tv_sec = ev_next_run;

	if (timer_settime(ev_timer, TIMER_ABSTIME, &ts, NULL) == -1)
		logm(LOG_ERR, "ev_recalc: timer_settime: %s", strerror(errno));
}

/*ARGSUSED*/
void
ev_handle(event)
	port_event_t	*event;
{
event_t	*ev;
	LIST_FOREACH(ev, &events, ev_entries) {
		if (ev->ev_abstime <= current_time) {
			ev->ev_func(ev->ev_id, ev->ev_udata);
			if (ev->ev_repeat) {
				ev->ev_abstime = ev_next_run + ev->ev_freq;
			} else {
				LIST_REMOVE(ev, ev_entries);
			}
		}
	}

	ev_recalc();
}

int
ev_cancel(evid)
	ev_id_t	evid;
{
event_t	*ev;
	LIST_FOREACH(ev, &events, ev_entries) {
		if (ev->ev_id == evid) {
			LIST_REMOVE(ev, ev_entries);
		}
	}

	ev_recalc();
	return (0);
}
