/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

#include	<sys/task.h>
#include	<sys/ctfs.h>

#include	<pwd.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<errno.h>
#include	<string.h>
#include	<project.h>
#include	<libcontract.h>
#include	<limits.h>
#include	<signal.h>
#include	<assert.h>
#include	<grp.h>
#include	<strings.h>
#include	<alloca.h>
#include	<deflt.h>

#include	"sched.h"
#include	"jobserver.h"
#include	"fd.h"
#include	"execute.h"

static sjob_t	*sjobs;
static int	 nsjobs;

static sjob_t *sjob_find(job_id_t id);
static void free_sjob(sjob_t *);

static ctid_t get_last_ctid(void);

static void sched_handle_exit(sjob_t *);
static void sched_handle_fail(sjob_t *);
static void sched_handle_crash(sjob_t *);

static int ctfd;

void sched_fd_callback(int, fde_evt_type_t, void *);

/*ARGSUSED*/
int
sched_init(prt)
	int prt;
{
uint_t	inform;

	if ((ctfd = open64(CTFS_ROOT "/process/template", O_RDWR)) == -1) {
		logm(LOG_ERR, "sched_init: cannot open process template: %s",
				strerror(errno));
		return -1;
	}

	if (fd_set_cloexec(ctfd, 1) == -1) {
		logm(LOG_ERR, "sched_init: cannot set cloexec on process template fd: %s",
				strerror(errno));
		return -1;
	}

	if (ct_pr_tmpl_set_param(ctfd, CT_PR_NOORPHAN) == -1) {
		logm(LOG_ERR, "sched_init: cannot set CT_PR_NOORPHAN: %s",
				strerror(errno));
		return -1;
	}

	if (ct_tmpl_get_informative(ctfd, &inform) == -1) {
		logm(LOG_ERR, "sched_init: ct_tmpl_get_informative: %s",
				strerror(errno));
		return -1;
	}

	inform |= (CT_PR_EV_EMPTY | CT_PR_EV_EXIT | CT_PR_EV_CORE | CT_PR_EV_SIGNAL | CT_PR_EV_HWERR);
	if (ct_tmpl_set_informative(ctfd, inform) == -1) {
		logm(LOG_ERR, "sched_init: ct_tmpl_set_informative: %s",
				strerror(errno));
		return -1;
	}

	if (ct_tmpl_activate(ctfd) == -1) {
		logm(LOG_ERR, "sched_init: cannot activate template: %s",
				strerror(errno));
		return -1;
	}

	return 0;
}

sjob_t *
sjob_find(id)
	job_id_t	id;
{
int	 i;
sjob_t	*nsj, *nj = NULL;
	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1 && nj == NULL)
			nj = &sjobs[i];

		if (sjobs[i].sjob_id == id)
			return &sjobs[i];
	}

	if (nj == NULL) {
		/*LINTED*/
		if ((nsj = realloc(sjobs, (nsjobs + 1) * sizeof(sjob_t))) == NULL) {
			logm(LOG_ERR, "sjob_find: out of memory");
			return NULL;
		}

		sjobs = nsj;
		nj = &sjobs[nsjobs];
		nsjobs++;
	}

	bzero(nj, sizeof(*nj));
	nj->sjob_id = id;
	nj->sjob_contract = -1;
	nj->sjob_contract_fd = -1;
	nj->sjob_stop_contract = -1;
	nj->sjob_stop_contract_fd = -1;
	nj->sjob_eventfd = -1;
	nj->sjob_timer = -1;
	nj->sjob_state = SJOB_STOPPED;
	return nj;
}

sjob_state_t
sched_get_state(id)
	job_id_t	id;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(id)) == NULL)
		return SJOB_UNKNOWN;

	return sjob->sjob_state;
}

/*ARGSUSED*/
void
sched_stop_timer_callback(evid, udata)
	ev_id_t	 evid;
	void	*udata;
{
sjob_t	*sjob = udata;

	/*
	 * We called the stop method of a process, but after the timeout
	 * period the process still hadn't exited.
	 */
	logm(LOG_INFO, "contract %ld: stop timeout expired, sending SIGKILL",
			(long) sjob->sjob_contract);

	if (sigsend(P_CTID, sjob->sjob_contract, SIGKILL) == -1)
		logm(LOG_ERR, "sched_stop: could not signal processes: %s",
				strerror(errno));
}

int
sched_stop(id)
	job_id_t	id;
{
sjob_t		*sjob = NULL;
job_t		*job = NULL;

	if ((sjob = sjob_find(id)) == NULL)
		goto err;

	if ((job = find_job(id)) == NULL)
		goto err;

	/*
	 * If there's no stop method defined, just send SIGTERM to the
	 * process contract.  Otherwise, execute the user's stop method.
	 */
	if (*job->job_stop_method) {
		if (fork_execute(job, job->job_stop_method) == -1) {
			logm(LOG_ERR, "sched_stop: could not start stop method: %s; sending SIGTERM",
					strerror(errno));
			if (sigsend(P_CTID, sjob->sjob_contract, SIGTERM) == -1)
				logm(LOG_ERR, "sched_stop: could not signal processes: %s",
						strerror(errno));
		}

		if ((sjob->sjob_stop_contract_fd = open64(CTFS_ROOT "/process/latest", O_RDONLY)) == -1)
			logm(LOG_ERR, "sched_stop: cannot open latest contract: %s",
					strerror(errno));

		if (fd_set_cloexec(sjob->sjob_stop_contract_fd, 1) == -1)
			logm(LOG_ERR, "sched_stop: cannot set fd_cloexec on stop contract fd: %s",
					strerror(errno));

		if ((sjob->sjob_stop_contract = get_last_ctid()) == -1)
			logm(LOG_ERR, "sched_stop: cannot get stop contract id: %s",
					strerror(errno));

	} else {
		if (sigsend(P_CTID, sjob->sjob_contract, SIGTERM) == -1)
			logm(LOG_ERR, "sched_stop: could not signal processes: %s",
					strerror(errno));
		sjob->sjob_stop_contract = -1;
		sjob->sjob_stop_contract_fd = -1;
	}
		
	sjob->sjob_state = SJOB_STOPPING;

	/*
	 * Wait 5 seconds for the job to stop, then kill it.
	 */
	sjob->sjob_timer = ev_add_once(5, sched_stop_timer_callback, sjob);

	free_job(job);
	return 0;
err:
	free_job(job);
	return -1;
}

ctid_t
get_last_ctid()
{
int		 cfd;
ctid_t		 ret;
ct_stathdl_t	 ctst;

	if ((cfd = open64(CTFS_ROOT "/process/latest", O_RDONLY)) == -1) {
		logm(LOG_ERR, "sched_start: %s/process/latest: %s",
				CTFS_ROOT, strerror(errno));
		exit(1);
	}

	if (ct_status_read(cfd, CTD_COMMON, &ctst) == -1) {
		logm(LOG_ERR, "sched_start: %s/process/latest: %s",
				CTFS_ROOT, strerror(errno));
		exit(1);
	}

	ret = ct_status_get_id(ctst);
	ct_status_free(ctst);
	(void) close(cfd);

	return ret;
}

int
sched_start(id)
	job_id_t	id;
{
sjob_t		*sjob = NULL;
job_t		*job = NULL;
char		 ctevents[PATH_MAX];

	if ((sjob = sjob_find(id)) == NULL)
		goto err;

	if ((job = find_job(id)) == NULL)
		goto err;

	sjob->sjob_start_time = time(NULL);

	/*
	 * Start the job.
	 */
	if ((sjob->sjob_pid = fork_execute(job, job->job_start_method)) == -1)
		goto err;

	/*
	 * Populate the sjob with contract information.
	 */
	if ((sjob->sjob_contract = get_last_ctid()) == -1)
		logm(LOG_ERR, "sched_start: cannot get last contract id: %s",
				strerror(errno));

	if ((sjob->sjob_contract_fd = open64(CTFS_ROOT "/process/latest", O_RDONLY)) == -1)
		logm(LOG_ERR, "sched_start: cannot open last contract: %s",
				strerror(errno));

	if (fd_set_cloexec(sjob->sjob_contract_fd, 1) == -1) {
		logm(LOG_ERR, "sched_start: cannot set cloexec on contract fd: %s",
				strerror(errno));
	}

	/*
	 * Register for events on the contract event fd.
	 */
	(void) snprintf(ctevents, sizeof ctevents, "%s/process/%lu/events",
			CTFS_ROOT, (unsigned long) sjob->sjob_contract);
	if ((sjob->sjob_eventfd = open64(ctevents, O_RDONLY)) == -1) {
		logm(LOG_ERR, "sched_start: %s: %s", ctevents, strerror(errno));
		exit(1);
	}

	if (fd_open(sjob->sjob_eventfd) == -1) {
		logm(LOG_ERR, "sched_start: fd_open failed");
		exit(1);
	}

	if (register_fd(sjob->sjob_eventfd, FDE_READ, sched_fd_callback, NULL) == -1) {
		logm(LOG_ERR, "sched_start: register_fd(%d): %s", sjob->sjob_eventfd, strerror(errno));
		exit(1);
	}

	sjob->sjob_state = SJOB_RUNNING;
	free_job(job);
	return 0;

err:
	free_job(job);
	free_sjob(sjob);
	return -1;
}

void
free_sjob(sjob)
	sjob_t	*sjob;
{
	if (sjob->sjob_contract_fd != -1)
		(void) close(sjob->sjob_contract_fd);
	if (sjob->sjob_stop_contract_fd != -1)
		(void) close(sjob->sjob_stop_contract_fd);
	if (sjob->sjob_eventfd != -1)
		(void) close_fd(sjob->sjob_eventfd);

	sjob->sjob_id = -1;
}

void
sched_job_enabled(id)
	job_id_t	id;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(id)) == NULL)
		return;

	if (sjob->sjob_state != SJOB_STOPPED)
		return;

	if (sched_start(id) == -1)
		logm(LOG_WARNING, "sched_job_enabled: sched_start failed");
}

void
sched_job_disabled(id)
	job_id_t	id;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(id)) == NULL)
		return;

	if (sjob->sjob_state != SJOB_RUNNING)
		return;

	if (sched_stop(id) == -1)
		logm(LOG_WARNING, "sched_job_disabled: sched_stop(%ld) failed",
				(long) id);
}

void
sched_job_deleted(id)
	job_id_t	id;
{
sjob_t	*sjob;
int	 i;
	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1)
			continue;

		if (sjobs[i].sjob_id == id) {
			sjob = &sjobs[i];
			break;
		}
	}

	free_sjob(sjob);
}

/*ARGSUSED*/
void
sched_fd_callback(fd, type, udata)
	int		 fd;
	fde_evt_type_t	 type;
	void		*udata;
{
ct_evthdl_t	 ev;
sjob_t		*sjob = NULL;
ctid_t		 ctid;
pid_t		 pid;
int		 sig, status = 0;
job_t		*job;
int		 i;

	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1)
			continue;
		if (sjobs[i].sjob_eventfd == fd) {
			sjob = &sjobs[i];
			break;
		}
	}

	assert(type == FDE_READ);
	assert(sjob);
	assert(fd == sjob->sjob_eventfd);

	bzero(&ev, sizeof(ev));

	if (ct_event_read(sjob->sjob_eventfd, &ev) != 0) {
		logm(LOG_ERR, "sched_fd_callback: ct_event_read failed: %s",
				strerror(errno));
		return;
	}

	ctid = ct_event_get_ctid(ev);

	/*
	 * We don't care about events from a stop method contract.
	 */
	if (ctid == sjob->sjob_stop_contract) {
		ct_event_free(ev);
		return;
	}

	switch (ct_event_get_type(ev)) {
	case CT_PR_EV_EMPTY:
		/* A process contract is now empty. */
		/*
		 * If the sjob has a stop contract running, kill it.  There's no need
		 * to be delicate here as the stop method is transient anyway.
		 */
		if (sjob->sjob_stop_contract != -1) {
			(void) sigsend(P_CTID, sjob->sjob_stop_contract, SIGKILL);
			if (ct_ctl_abandon(sjob->sjob_stop_contract_fd) == -1)
				logm(LOG_WARNING, "ct_ctl_abandon of stop method contract failed: %s", 
						strerror(errno));
			(void) close(sjob->sjob_stop_contract_fd);
			sjob->sjob_stop_contract = -1;
		}

		if (ct_ctl_abandon(sjob->sjob_contract_fd) == -1)
			logm(LOG_WARNING, "job %ld: ct_ctl_abandon failed: %s",
					(long) sjob->sjob_id, strerror(errno));
		(void) close(sjob->sjob_contract_fd);
		(void) close_fd(sjob->sjob_eventfd);

		sjob->sjob_eventfd = -1;
		sjob->sjob_contract_fd = -1;
		sjob->sjob_contract = -1;

		if (sjob->sjob_timer != -1 && ev_cancel(sjob->sjob_timer) == -1)
			logm(LOG_WARNING, "job %ld: cannot cancel stop timeout: %s",
					(long) sjob->sjob_id, strerror(errno));

		sjob->sjob_state = SJOB_STOPPED;

		/*
		 * See if we should restart the job.
		 */
		if ((job = find_job(sjob->sjob_id)) == NULL)
			abort();

		if (!(job->job_flags & JOB_MAINTENANCE)) {
			if (job->job_flags & JOB_ENABLED) {
				if (sjob->sjob_start_time + SCHED_MIN_RUNTIME > time(NULL)) {
					job_set_maintenance(job->job_id, "Restarting too quickly");
				} else {
					if (sched_start(job->job_id) == -1)
						logm(LOG_WARNING, "job %ld: restart failed",
								(long) job->job_id);
				}
			} else if (job->job_flags & JOB_SCHEDULED) {
				sched_job_scheduled(job->job_id);
			}
		}

		free_job(job);
		break;

	case CT_PR_EV_CORE:
	case CT_PR_EV_HWERR:
		break;

	case CT_PR_EV_EXIT:
		/* A process in the contract exited. */
		if (ct_pr_event_get_pid(ev, &pid) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_pid failed: %s",
					(long) sjob->sjob_id, strerror(errno));
			pid = 0;
		}

		if (ct_pr_event_get_exitstatus(ev, &status) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_exitstatus failed: %s (assuming 0)",
					(long) sjob->sjob_id, strerror(errno));
			status = 0;
		}

		/*
		 * If this was a signal exit, we already handled it.
		 */
		if (WIFEXITED(status) && pid == sjob->sjob_pid) {
			if (WEXITSTATUS(status) == 0) {
				logm(LOG_DEBUG, "job %ld: pid=%ld normal exit status 0",
						(long) sjob->sjob_id, (long) pid);
				sched_handle_exit(sjob);
			} else {
				logm(LOG_DEBUG, "job %ld: pid=%ld abnormal exit status %d",
						(long) sjob->sjob_id, (long) pid, WEXITSTATUS(status));
				sched_handle_exit(sjob);
			}
		}
		break;

	case CT_PR_EV_SIGNAL:
		/* 
		 * A process in the contract exited from a signal.  Certain
		 * signals are considered crash events, others are simple
		 * exits.
		 */
		if (ct_pr_event_get_pid(ev, &pid) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_pid failed: %s",
					(long) sjob->sjob_id, strerror(errno));
			pid = 0;
		}

		if (ct_pr_event_get_signal(ev, &sig) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_signal failed: %s (assuming SIGTERM)",
					(long) sjob->sjob_id, strerror(errno));
			sig = SIGTERM;
		}
		
		switch (sig) {
		case SIGILL:
		case SIGTRAP:
		case SIGABRT:
		case SIGEMT:
		case SIGFPE:
		case SIGBUS:
		case SIGSEGV:
		case SIGSYS:
			logm(LOG_INFO, "job %ld: pid=%ld crash on fatal signal %d",
					(long) sjob->sjob_id, (long) pid, sig);
			sched_handle_crash(sjob);
			break;

		default:
			if (pid == sjob->sjob_pid) {
				logm(LOG_INFO, "job %ld: pid=%ld fail on fatal signal %d",
						(long) sjob->sjob_id, (long) pid, sig);
				sched_handle_fail(sjob);
			}
			break;
		}
		break;

	case CT_PR_EV_FORK:
		/* We don't care about these events. */
		break;
	}

	ct_event_free(ev);
}

static void
sched_handle_exit(sjob)
	sjob_t	*sjob;
{
job_t	*job;

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if ((job->job_flags & JOB_ENABLED) &&
	    sjob->sjob_start_time + SCHED_MIN_RUNTIME > time(NULL)) {
		job_set_maintenance(job->job_id, "Restarting too quickly");
		free_job(job);
		return;
	}

	if (job->job_exit_action & ST_EXIT_DISABLE)
		if (job_disable(job->job_id) == -1)
			logm(LOG_WARNING, "sched_handle_exit: job_disabled failed");

	free_job(job);
}

static void
sched_handle_fail(sjob)
	sjob_t	*sjob;
{
job_t	*job;

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_fail_action & ST_EXIT_DISABLE)
		if (job_disable(job->job_id) == -1)
			logm(LOG_WARNING, "sched_handle_fail: job_disabled failed");
	
	free_job(job);
}

static void
sched_handle_crash(sjob)
	sjob_t	*sjob;
{
job_t	*job;

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_crash_action & ST_EXIT_DISABLE)
		if (job_disable(job->job_id) == -1)
			logm(LOG_WARNING, "sched_handle_crash: job_disabled failed");

	free_job(job);
}

static time_t
sjob_nextrun(sched)
	cron_t	sched;
{
time_t		 now = time(NULL);
struct tm	*tm = gmtime(&now);
int		 hr, min, wday;
int		 a1, a2;

	a1 = sched.cron_arg1;
	a2 = sched.cron_arg2;

	switch (sched.cron_type) {
	case CRON_ABSOLUTE:
		return a1;

	case CRON_EVERY_MINUTE:
		return now + 60;

	case CRON_EVERY_HOUR:
		if (a1 <= tm->tm_min)
			tm->tm_hour++;
		tm->tm_min = a1;
		return mktime(tm);

	case CRON_EVERY_DAY:
		min = a1 % 60;
		hr = a1 / 60;

		if ((hr < tm->tm_hour) || (hr == tm->tm_hour && min <= tm->tm_min))
			tm->tm_mday++;
		tm->tm_hour = hr;
		tm->tm_min = min;
		return mktime(tm);

	case CRON_EVERY_WEEK:
		wday = a1;
		min = a2 % 60;
		hr = a2 / 60;

		if (wday < tm->tm_wday || 
		    (wday == tm->tm_wday && hr < tm->tm_hour) ||
		    (wday == tm->tm_wday && hr == tm->tm_hour && min <= tm->tm_min)) {
			tm->tm_mday += ((wday + 7) - tm->tm_wday);
		} else {
			tm->tm_mday += (wday - tm->tm_wday);
		}

		tm->tm_hour = hr;
		tm->tm_min = min;
		return mktime(tm);

	default:
		return -1;
	}
}

/*ARGSUSED*/
static void
sjob_run_scheduled(evid, udata)
	ev_id_t	 evid;
	void	*udata;
{
sjob_t	*sjob = udata;
job_t	*job;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		return;

	sjob->sjob_nextrun = 0;

	if (sched_start(sjob->sjob_id) == -1)
		logm(LOG_WARNING, "sched_run_scheduled: sched_start failed");

	free_job(job);
}

void
sched_job_scheduled(id)
	job_id_t	id;
{
job_t	*job;
sjob_t	*sjob;

	if ((job = find_job(id)) == NULL)
		return;

	if ((sjob = sjob_find(id)) == NULL) {
		free_job(job);
		return;
	}

	sjob->sjob_nextrun = sjob_nextrun(job->job_schedule);

	if ((sjob->sjob_timer = ev_add_once(sjob->sjob_nextrun - time(NULL),
					sjob_run_scheduled, sjob)) == -1) {
		logm(LOG_ERR, "sched_job_schedule: ev_add_once failed: %s",
				strerror(errno));
	}

	free_job(job);
}

void
sched_job_unscheduled(id)
	job_id_t	id;
{
sjob_t	*sjob;

	if ((sjob = sjob_find(id)) == NULL)
		return;

	sjob->sjob_nextrun = 0;
	if (ev_cancel(sjob->sjob_timer) == -1)
		logm(LOG_WARNING, "sched_job_unscheduled: warning: ev_cancel failed");
}
