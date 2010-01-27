/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<stdlib.h>
#include	<time.h>
#include	<syslog.h>
#include	<errno.h>
#include	<string.h>
#include	<strings.h>

#include	"jobserver.h"
#include	"event.h"

static timer_t ev_timer;

typedef struct {
	int 		 ev_repeat;
	time_t		 ev_abstime;
	time_t		 ev_freq;
	ev_callback	 ev_func;
	void		*ev_udata;
} event_t;

static int port;
static int nevs;
static time_t ev_next_run;
static event_t *events;
static event_t *get_event(void);
static void ev_recalc();

int
ev_init(prt, nev)
	int	prt, nev;
{
struct sigevent	ev;
port_notify_t	nfy;

	port = prt;
	nevs = nev;
	if ((events = calloc(sizeof(*events), nevs)) == NULL) {
		logm(LOG_ERR, "ev_init: out of memory");
		return -1;
	}

	bzero(&ev, sizeof(ev));
	bzero(&nfy, sizeof(nfy));

	ev.sigev_notify = SIGEV_PORT;
	ev.sigev_value.sival_ptr = &nfy;
	nfy.portnfy_port = port;

	if (timer_create(CLOCK_REALTIME, &ev, &ev_timer) == -1) {
		logm(LOG_ERR, "ev_init: timer_create: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static event_t *
get_event()
{
int	i;
	for (i = 0; i < nevs; ++i)
		if (events[i].ev_func == NULL)
			return &events[i];
	logm(LOG_ERR, "get_event: out of event slots!");
	return NULL;
}

ev_id_t
ev_add(when, func, udata)
	time_t		 when;
	ev_callback	 func;
	void		*udata;
{
event_t	*ev;

	if ((ev = get_event()) == NULL)
		return -1;

	ev->ev_func = func;
	ev->ev_udata = udata;
	ev->ev_repeat = 1;
	ev->ev_freq = when;
	ev->ev_abstime = current_time + when;

	ev_recalc();
	return (ev_id_t) (ev - events);
}

ev_id_t
ev_add_once(when, func, udata)
	time_t		 when;
	ev_callback	 func;
	void		*udata;
{
event_t	*ev;

	if ((ev = get_event()) == NULL)
		return -1;

	ev->ev_func = func;
	ev->ev_udata = udata;
	ev->ev_repeat = 0;
	ev->ev_freq = when;
	ev->ev_abstime = current_time + when;

	ev_recalc();
	return (ev_id_t) (ev - events);
}

static void
ev_recalc()
{
int			i;
struct itimerspec	ts;

	ev_next_run = 0;
	for (i = 0; i < nevs; ++i) {
		if (events[i].ev_func && (ev_next_run == 0 || ev_next_run > events[i].ev_abstime))
			ev_next_run = events[i].ev_abstime;
	}

	bzero(&ts, sizeof(ts));

	if (ev_next_run == 0) {
		if (timer_settime(ev_timer, 0, &ts, NULL) == -1)
			logm(LOG_ERR, "ev_recalc: timer_settime: %s", strerror(errno));
		return;
	}

	ts.it_value.tv_sec = ev_next_run;

	if (timer_settime(ev_timer, TIMER_ABSTIME, &ts, NULL) == -1)
		logm(LOG_ERR, "ev_recalc: timer_settime: %s", strerror(errno));
}

/*ARGSUSED*/
void
ev_handle(ev)
	port_event_t	*ev;
{
int	i;
	for (i = 0; i < nevs; ++i) {
		if (events[i].ev_abstime <= ev_next_run && events[i].ev_func) {
			events[i].ev_func(i, events[i].ev_udata);
			if (events[i].ev_func && events[i].ev_repeat) {
				events[i].ev_abstime = ev_next_run + events[i].ev_freq;
			} else {
				events[i].ev_func = NULL;
			}
		}
	}

	ev_recalc();
}

int
ev_cancel(ev)
	ev_id_t	ev;
{
	events[ev].ev_func = NULL;
	ev_recalc();
	return 0;
}
