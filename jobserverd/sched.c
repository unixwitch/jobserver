/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

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
#include	<utmpx.h>

#include	"sched.h"
#include	"jobserver.h"
#include	"fd.h"
#include	"execute.h"

static sjob_t	*sjobs;
static int	 nsjobs;

static sjob_t *sjob_find(job_id_t id);
static void free_sjob(sjob_t *);

static void sched_handle_exit(sjob_t *);
static void sched_handle_fail(sjob_t *);
static void sched_handle_crash(sjob_t *);

static int do_start_job(job_t *, void *);

static int ctfd;
static int adopting = 0;

void sched_fd_callback(int, fde_evt_type_t, void *);

/*ARGSUSED*/
int
sched_init(prt)
	int prt;
{
uint_t		 inform;
struct utmpx	*ut;
time_t		 newboot, oldboot;

	if ((ctfd = open64(CTFS_ROOT "/process/template", O_RDWR)) == -1) {
		logm(LOG_ERR, "sched_init: cannot open process template: %s",
				strerror(errno));
		return (-1);
	}

	if (fd_set_cloexec(ctfd, 1) == -1) {
		logm(LOG_ERR, "sched_init: "
			"cannot set cloexec on process template fd: %s",
			strerror(errno));
		return (-1);
	}

	if (ct_tmpl_get_informative(ctfd, &inform) == -1) {
		logm(LOG_ERR, "sched_init: ct_tmpl_get_informative: %s",
				strerror(errno));
		return (-1);
	}

	inform |= (CT_PR_EV_EMPTY | CT_PR_EV_EXIT | CT_PR_EV_CORE |
			CT_PR_EV_SIGNAL | CT_PR_EV_HWERR);
	if (ct_tmpl_set_informative(ctfd, inform) == -1) {
		logm(LOG_ERR, "sched_init: ct_tmpl_set_informative: %s",
				strerror(errno));
		return (-1);
	}

#if 1	/* Remove this when contract adoption is working  */
	if (ct_pr_tmpl_set_param(ctfd, CT_PR_NOORPHAN) == -1) {
		logm(LOG_ERR, "sched_init: cannot set CT_PR_NOORPHAN: %s",
			strerror(errno));
		return (-1);
	}
#endif

	if (ct_tmpl_activate(ctfd) == -1) {
		logm(LOG_ERR, "sched_init: cannot activate template: %s",
		    strerror(errno));
		return (-1);
	}

	setutxent();
	while (ut = getutxent()) {
		if (ut->ut_type != BOOT_TIME)
			continue;
		newboot = ut->ut_xtime;
		break;
	}
	endutxent();

	if (newboot == 0)
		logm(LOG_WARNING, "boottime: "
			"cannot find boot time record in utmp?");

	oldboot = get_boottime(newboot);

	if (oldboot == newboot)
		/*
		 * The system hasn't rebooted since we last started.  Try to
		 * re-adopt any orphaned contracts.
		 */
		adopting = 1;

	/*
	 * Start all enabled jobs.
	 */
	if (job_enumerate(do_start_job, NULL) == -1)
		logm(LOG_ERR, "sched_init: job_enumerate failed");

	return (0);
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
			return (&sjobs[i]);
	}

	if (nj == NULL) {
		if ((nsj = realloc(sjobs,
				(nsjobs + 1) * sizeof (sjob_t))) == NULL) {
			logm(LOG_ERR, "sjob_find: out of memory");
			return (NULL);
		}

		sjobs = nsj;
		nj = &sjobs[nsjobs];
		nsjobs++;
	}

	bzero(nj, sizeof (*nj));
	nj->sjob_id = id;
	nj->sjob_timer = -1;
	nj->sjob_state = SJOB_STOPPED;
	return (nj);
}

int
sched_jobs_running()
{
int	 i, n = 0;
	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1)
			continue;

		if (sjobs[i].sjob_state == SJOB_RUNNING ||
		    sjobs[i].sjob_state == SJOB_STOPPING)
			n++;
	}

	return (n);
}

void
sched_stop_all()
{
int	 i;

	for (i = 0; i < nsjobs; ++i) {
	job_t	*job;
		if (sjobs[i].sjob_id == -1)
			continue;

		if (sjobs[i].sjob_state == SJOB_RUNNING) {
#if 0	/* Enable this when contract adoption is working */
			if (ct_ctl_abandon(sjobs[i].sjob_contract->ct_ctl)
			    == -1)
				logm(LOG_WARNING, "sched_stop_all: "
				    "cannot abandon %d: %s",
				    (int)sjobs[i].sjob_contract->ct_id,
				    strerror(errno));
#else
			if ((job = find_job(sjobs[i].sjob_id)) == NULL) {
				logm(LOG_WARNING, "sched_stop_all: could not "
				    "find job for sjob id %d",
				    sjobs[i].sjob_id);
				continue;
			}

			if (sched_stop(job) == -1)
				logm(LOG_WARNING, "sched_stop_all: "
				    "sched_stop failed: %s",
				    strerror(errno));
#endif
		}
	}
}

sjob_state_t
sched_get_state(job)
	job_t	*job;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(job->job_id)) == NULL)
		return (SJOB_UNKNOWN);

	return (sjob->sjob_state);
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
			(long)sjob->sjob_contract);

	if (sigsend(P_CTID, sjob->sjob_contract->ct_id, SIGKILL) == -1)
		logm(LOG_ERR, "sched_stop: could not signal processes: %s",
				strerror(errno));
}

int
sched_stop(job)
	job_t	*job;
{
sjob_t		*sjob = NULL;

	if ((sjob = sjob_find(job->job_id)) == NULL)
		goto err;

	/*
	 * If there's no stop method defined, just send SIGTERM to the
	 * process contract.  Otherwise, execute the user's stop method.
	 */
	if (*job->job_stop_method) {
		if (fork_execute(job, job->job_stop_method) == -1) {
			logm(LOG_ERR, "sched_stop: could not start "
				"stop method: %s; sending SIGTERM",
				strerror(errno));
			if (sigsend(P_CTID, sjob->sjob_contract->ct_id,
					SIGTERM) == -1)
				logm(LOG_ERR, "sched_stop: could not "
					"signal processes: %s",
					strerror(errno));
		}

		if ((sjob->sjob_stop_contract = contract_open_latest()) == NULL)
			logm(LOG_ERR, "sched_stop: "
				"cannot open latest contract: %s",
				strerror(errno));
	} else {
		if (sigsend(P_CTID, sjob->sjob_contract->ct_id, SIGTERM) == -1)
			logm(LOG_ERR, "sched_stop: "
				"could not signal processes: %s",
				strerror(errno));
	}

	sjob->sjob_state = SJOB_STOPPING;

	/*
	 * Wait 30 seconds for the job to stop, then kill it.
	 */
	sjob->sjob_timer = ev_add_once(30, sched_stop_timer_callback, sjob);

	return (0);

err:
	return (-1);
}

int
sched_start(job)
	job_t	*job;
{
sjob_t		*sjob = NULL;

	if ((sjob = sjob_find(job->job_id)) == NULL)
		goto err;

	sjob->sjob_fatal = 0;
	sjob->sjob_start_time = current_time;

	/*
	 * Start the job.
	 */
	if ((sjob->sjob_pid = fork_execute(job, job->job_start_method)) == -1)
		goto err;

	if ((sjob->sjob_contract = contract_open_latest()) == NULL)
		goto err;

	if (fd_open(sjob->sjob_contract->ct_events) == -1)
		goto err;

	if (register_fd(sjob->sjob_contract->ct_events, FDE_READ,
			sched_fd_callback, NULL) == -1) {
		logm(LOG_ERR, "sched_start: register_fd(%d): %s",
				sjob->sjob_contract->ct_events,
				strerror(errno));
		goto err;
	}

	if (job_set_ctid(job, sjob->sjob_contract->ct_id) == -1)
		logm(LOG_WARNING, "sched_start: job_update failed");

	sjob->sjob_state = SJOB_RUNNING;
	return (0);

err:
	free_sjob(sjob);
	return (-1);
}

void
free_sjob(sjob)
	sjob_t	*sjob;
{
	if (!sjob)
		return;

	contract_close(sjob->sjob_contract);
	contract_close(sjob->sjob_stop_contract);

	sjob->sjob_contract = NULL;
	sjob->sjob_stop_contract = NULL;
	sjob->sjob_id = -1;
}

void
sched_job_enabled(job)
	job_t	*job;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(job->job_id)) == NULL)
		return;

	if (job->job_flags & JOB_SCHEDULED) {
		sched_job_scheduled(job);
	} else {
		if (sjob->sjob_state != SJOB_STOPPED)
			return;

		if (sched_start(job) == -1)
			logm(LOG_WARNING, "sched_job_enabled: "
				"sched_start failed");
	}
}

void
sched_job_disabled(job)
	job_t	*job;
{
sjob_t	*sjob;
	if ((sjob = sjob_find(job->job_id)) == NULL)
		return;

	if ((job->job_flags & JOB_ENABLED) &&
		(job->job_flags & JOB_SCHEDULED)) {
		sched_job_unscheduled(job);
	} else {
		if (sjob->sjob_state != SJOB_RUNNING)
			return;

		if (sched_stop(job) == -1)
			logm(LOG_WARNING, "sched_job_disabled: "
				"sched_stop(%ld) failed",
				(long)job->job_id);
	}
}

void
sched_job_deleted(job)
	job_t	*job;
{
sjob_t	*sjob;
int	 i;
	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1)
			continue;

		if (sjobs[i].sjob_id == job->job_id) {
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
pid_t		 pid;
int		 sig, status = 0;
job_t		*job = NULL;
int		 i;

	for (i = 0; i < nsjobs; ++i) {
		if (sjobs[i].sjob_id == -1)
			continue;

		if (sjobs[i].sjob_contract &&
		    sjobs[i].sjob_contract->ct_events == fd) {
			sjob = &sjobs[i];
			break;
		}
	}

	assert(type == FDE_READ);
	assert(sjob);
	assert(fd == sjob->sjob_contract->ct_events);

	bzero(&ev, sizeof (ev));

	if (ct_event_read(sjob->sjob_contract->ct_events, &ev) != 0) {
		logm(LOG_ERR, "sched_fd_callback: ct_event_read failed: %s",
				strerror(errno));
		return;
	}

	switch (ct_event_get_type(ev)) {
	case CT_PR_EV_EMPTY:
		/* A process contract is now empty. */
		/*
		 * If the sjob has a stop contract running, kill it.  There's
		 * no need to be delicate here as the stop method is transient
		 * anyway.
		 */
		if (sjob->sjob_stop_contract) {
			(void) sigsend(P_CTID,
				sjob->sjob_stop_contract->ct_id, SIGKILL);
			if (ct_ctl_abandon(sjob->sjob_stop_contract->ct_ctl)
			    == -1)
				logm(LOG_WARNING, "ct_ctl_abandon of stop "
					"method contract failed: %s",
					strerror(errno));
			contract_close(sjob->sjob_stop_contract);
			sjob->sjob_stop_contract = NULL;
		}

		if (ct_ctl_abandon(sjob->sjob_contract->ct_ctl) == -1)
			logm(LOG_WARNING, "job %ld: ct_ctl_abandon failed: %s",
					(long)sjob->sjob_id, strerror(errno));
		contract_close(sjob->sjob_contract);

		if ((job = find_job(sjob->sjob_id)) == NULL)
			abort();

		if (job_set_ctid(job, -1) == -1)
			logm(LOG_WARNING, "job %ld: "
				"cannot set ctid",
				(long)sjob->sjob_id);

		sjob->sjob_contract = NULL;

		if (sjob->sjob_timer != -1 && ev_cancel(sjob->sjob_timer) == -1)
			logm(LOG_WARNING, "job %ld: cannot cancel "
				"stop timeout: %s",
				(long)sjob->sjob_id, strerror(errno));

		sjob->sjob_state = SJOB_STOPPED;

		if (shutting_down)
			break;

		/*
		 * See if we should restart the job.
		 */
		if (!(job->job_flags & JOB_MAINTENANCE)) {
			if (job->job_flags & JOB_SCHEDULED) {
				if (job->job_schedule.cron_type
				    != CRON_ABSOLUTE)
					sched_job_scheduled(job);
				else {
					if (job_unschedule(job) == -1)
						logm(LOG_ERR, "job %ld: cannot "
							"unschedule: %s",
							(long)job->job_id,
							strerror(errno));
				}
			} else if (job->job_flags & JOB_ENABLED) {
				if (sjob->sjob_start_time + SCHED_MIN_RUNTIME
				    > current_time) {
					if (job_set_maintenance(job,
						"Restarting too quickly") == -1)
						logm(LOG_WARNING, "job %ld: "
							"could not set "
							"maintenance mode",
							(long)job->job_id);
				} else {
					if (sched_start(job) == -1)
						logm(LOG_WARNING, "job %ld: "
							"restart failed",
							(long)job->job_id);
				}
			}
		}

		free_job(job);
		break;

	case CT_PR_EV_CORE:
	case CT_PR_EV_HWERR:
		break;

	case CT_PR_EV_EXIT:
		/* A process in the contract exited. */

		if (shutting_down)
			break;

		if (ct_pr_event_get_pid(ev, &pid) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_pid "
				"failed: %s",
				(long)sjob->sjob_id, strerror(errno));
			pid = 0;
		}

		if (ct_pr_event_get_exitstatus(ev, &status) == -1) {
			logm(LOG_WARNING, "job %ld: ct_pr_event_get_exitstatus "
				"failed: %s (assuming 0)",
				(long)sjob->sjob_id, strerror(errno));
			status = 0;
		}

		if (WIFEXITED(status) && pid == sjob->sjob_pid) {
			/*
			 * Normal exit - status 0 means 'exit', anything else
			 * means 'fail'.
			 */
			if (WEXITSTATUS(status) == 0) {
				logm(LOG_INFO, "job %ld: pid=%ld normal "
					"exit status 0",
					(long)sjob->sjob_id, (long)pid);
				sched_handle_exit(sjob);
			} else {
				logm(LOG_INFO, "job %ld: pid=%ld abnormal "
					"exit status %d",
					(long)sjob->sjob_id, (long)pid,
					WEXITSTATUS(status));
				sched_handle_fail(sjob);
			}
		} else if (WIFSIGNALED(status)) {
			/*
			 * A process in the contract exited from a signal.
			 * Certain signals are considered crash events, others
			 * are simple exits.
			 */
			sig = WTERMSIG(status);

			switch (sig) {
			case SIGILL:
			case SIGTRAP:
			case SIGABRT:
			case SIGEMT:
			case SIGFPE:
			case SIGBUS:
			case SIGSEGV:
			case SIGSYS:
				logm(LOG_INFO, "job %ld: pid=%ld crash on "
					"fatal signal %d",
					(long)sjob->sjob_id,
					(long)pid, sig);
				sched_handle_crash(sjob);
				break;

			default:
				if (pid == sjob->sjob_pid) {
					logm(LOG_INFO, "job %ld: pid=%ld fail "
						"on fatal signal %d",
						(long)sjob->sjob_id,
						(long)pid, sig);
					sched_handle_fail(sjob);
				}
				break;
			}
			break;
		}
		break;

	case CT_PR_EV_SIGNAL:

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
job_t		*job;
char		 msg[4096];
char		 hostname[64];
char		 timestr[128];

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if (sjob->sjob_state == SJOB_STOPPING)
		return;

	if (gethostname(hostname, sizeof (hostname)) == -1)
		(void) strlcpy(hostname, "unknown", sizeof (hostname));
	else
		hostname[sizeof (hostname) - 1] = 0;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_exit_action & ST_EXIT_MAIL) {
		(void) strftime(timestr, sizeof (timestr),
			"%a, %d %b %Y %H:%M:%S %z", gmtime(&current_time));
		(void) snprintf(msg, sizeof (msg),
			"To: %s\n"
			"Subject: \"%s\" exited\n"
			"Date: %s"
			"\n"
			"Your job \"%s\" on the host \"%s\" has "
			"exited successfully.\n",
			job->job_username,
			job->job_fmri,
			timestr,
			job->job_fmri,
			hostname);
	}

	if ((job->job_flags & JOB_ENABLED) &&
	    !(job->job_flags & JOB_SCHEDULED) &&
	    sjob->sjob_start_time + SCHED_MIN_RUNTIME > current_time) {
		if (job_set_maintenance(job, "Restarting too quickly") == -1)
			logm(LOG_WARNING, "job %ld: could not set maintenance",
					(long)job->job_id);
		if (job->job_exit_action & ST_EXIT_MAIL)
			(void) strlcat(msg, "\nThe job did not run for long "
				"enough, and has been placed in the "
				"maintenance state.\n", sizeof (msg));
	}

	if (job->job_exit_action & ST_EXIT_DISABLE) {
		if (job_disable(job) == -1)
			logm(LOG_WARNING, "sched_handle_exit: "
				"job_disable failed");
		else if (job->job_exit_action & ST_EXIT_MAIL)
			(void) strlcat(msg, "\nThe exit action for this job "
				"is 'disable', so the job has been disabled.\n",
				sizeof (msg));
	}

	if (job->job_exit_action & ST_EXIT_MAIL) {
		(void) strlcat(msg, "\nRegards,\n"
			"\tThe job server.\n", sizeof (msg));
		if (send_mail(job->job_username, msg) == -1)
			logm(LOG_ERR, "sched_handle_fail: "
				"cannot send mail: %s", strerror(errno));
	}
	free_job(job);
}

static void
sched_handle_fail(sjob)
	sjob_t	*sjob;
{
job_t		*job;
char		 msg[4096];
char		 hostname[64];
char		 timestr[128];

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if (sjob->sjob_state == SJOB_STOPPING)
		return;

	if (gethostname(hostname, sizeof (hostname)) == -1)
		(void) strlcpy(hostname, "unknown", sizeof (hostname));
	else
		hostname[sizeof (hostname) - 1] = 0;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_fail_action & ST_EXIT_MAIL) {
		(void) strftime(timestr, sizeof (timestr),
			"%a, %d %b %Y %H:%M:%S %z", gmtime(&current_time));
		(void) snprintf(msg, sizeof (msg),
			"To: %s\n"
			"Subject: \"%s\" failed\n"
			"Date: %s"
			"\n"
			"Your job \"%s\" on the host \"%s\" has failed.\n",
			job->job_username,
			job->job_fmri,
			timestr,
			job->job_fmri,
			hostname);
	}

	if (job->job_fail_action & ST_EXIT_DISABLE) {
		if (job_disable(job) == -1)
			logm(LOG_WARNING, "sched_handle_fail: "
				"job_disabled failed");
		else if (job->job_fail_action & ST_EXIT_MAIL)
			(void) strlcat(msg, "\nThe fail action for this job "
				"is 'disable', so the job has been disabled.\n",
				sizeof (msg));
	}

	if (job->job_fail_action & ST_EXIT_MAIL) {
		(void) strlcat(msg, "\nRegards,\n\tThe job server.\n",
			sizeof (msg));
		if (send_mail(job->job_username, msg) == -1)
			logm(LOG_ERR, "sched_handle_fail: "
				"cannot send mail: %s", strerror(errno));
	}

	free_job(job);
}

static void
sched_handle_crash(sjob)
	sjob_t	*sjob;
{
job_t		*job;
char		 msg[4096];
char		 hostname[64];
char		 timestr[128];

	if (sjob->sjob_fatal)
		return;
	sjob->sjob_fatal = 1;

	if (sjob->sjob_state == SJOB_STOPPING)
		return;

	if (gethostname(hostname, sizeof (hostname)) == -1)
		(void) strlcpy(hostname, "unknown", sizeof (hostname));
	else
		hostname[sizeof (hostname) - 1] = 0;

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_crash_action & ST_EXIT_MAIL) {
		(void) strftime(timestr, sizeof (timestr),
			"%a, %d %b %Y %H:%M:%S %z", gmtime(&current_time));
		(void) snprintf(msg, sizeof (msg),
			"To: %s\n"
			"Subject: \"%s\" crashed\n"
			"Date: %s"
			"\n"
			"Your job \"%s\" on the host \"%s\" has crashed.\n",
			job->job_username,
			job->job_fmri,
			timestr,
			job->job_fmri,
			hostname);
	}

	if ((job = find_job(sjob->sjob_id)) == NULL)
		abort();

	if (job->job_crash_action & ST_EXIT_DISABLE) {
		if (job_disable(job) == -1)
			logm(LOG_WARNING, "sched_handle_crash: "
				"job_disabled failed");
		else if (job->job_fail_action & ST_EXIT_MAIL)
			(void) strlcat(msg, "\nThe crash action for this job "
				"is 'disable', so the job has been disabled.\n",
				sizeof (msg));
	}

	if (job->job_crash_action & ST_EXIT_MAIL) {
		(void) strlcat(msg, "\nRegards,\n"
			"\tThe job server.\n", sizeof (msg));
		if (send_mail(job->job_username, msg) == -1)
			logm(LOG_ERR, "sched_handle_crash: "
				"cannot send mail: %s", strerror(errno));
	}

	free_job(job);
}

time_t
sched_nextrun(sched)
	cron_t	*sched;
{
struct tm	*tm = gmtime(&current_time);
int		 hr, min, wday;
int		 a1, a2;

	a1 = sched->cron_arg1;
	a2 = sched->cron_arg2;

	switch (sched->cron_type) {
	case CRON_ABSOLUTE:
		return (a1);

	case CRON_EVERY_MINUTE:
		tm->tm_sec = 0;
		tm->tm_min++;
		return (mktime(tm));

	case CRON_EVERY_HOUR:
		if (a1 <= tm->tm_min)
			tm->tm_hour++;
		tm->tm_min = a1;
		return (mktime(tm));

	case CRON_EVERY_DAY:
		min = a1 % 60;
		hr = a1 / 60;

		if ((hr < tm->tm_hour) ||
			(hr == tm->tm_hour && min <= tm->tm_min))
			tm->tm_mday++;
		tm->tm_hour = hr;
		tm->tm_min = min;
		return (mktime(tm));

	case CRON_EVERY_WEEK:
		wday = a1;
		min = a2 % 60;
		hr = a2 / 60;

		if (wday < tm->tm_wday ||
		    (wday == tm->tm_wday && hr < tm->tm_hour) ||
		    (wday == tm->tm_wday && hr == tm->tm_hour &&
			min <= tm->tm_min)) {
			tm->tm_mday += ((wday + 7) - tm->tm_wday);
		} else {
			tm->tm_mday += (wday - tm->tm_wday);
		}

		tm->tm_hour = hr;
		tm->tm_min = min;
		return (mktime(tm));

	default:
		return (-1);
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

	if (sched_start(job) == -1)
		logm(LOG_WARNING, "sched_run_scheduled: sched_start failed");

	free_job(job);
}

void
sched_job_scheduled(job)
	job_t	*job;
{
sjob_t	*sjob;

	if (!(job->job_flags & JOB_ENABLED))
		return;

	if ((sjob = sjob_find(job->job_id)) == NULL)
		return;

	sjob->sjob_nextrun = sched_nextrun(&job->job_schedule);

	if ((sjob->sjob_timer = ev_add_once(sjob->sjob_nextrun - current_time,
					sjob_run_scheduled, sjob)) == -1) {
		logm(LOG_ERR, "sched_job_schedule: ev_add_once failed: %s",
				strerror(errno));
	}
}

void
sched_job_unscheduled(job)
	job_t	*job;
{
sjob_t	*sjob;

	if ((sjob = sjob_find(job->job_id)) == NULL)
		return;

	sjob->sjob_nextrun = 0;
	if (ev_cancel(sjob->sjob_timer) == -1)
		logm(LOG_WARNING, "sched_job_unscheduled: "
			"warning: ev_cancel failed");
}

/*ARGSUSED*/
static int
do_start_job(job, udata)
	job_t	*job;
	void	*udata;
{
#if 0	/* Not working yet */
sjob_t	*sjob = NULL;
	/*
	 * If we're adopting, and a contract still exists, but the job is
	 * either disabled or maintenance is set, it's not clear what we should
	 * do.  The least worst action seems to be overriding our state,
	 * clearing maintenance and setting enabled.  This at least means we
	 * will agree with the system's actual state.
	 */
	if (adopting && job->job_contract != -1) {
	int	 ctfd;
	char	 statfile[128], ctevents[128], ctlfile[128];

		if ((sjob = sjob_find(job->job_id)) == NULL)
			goto err;

		sjob->sjob_fatal = 0;
		sjob->sjob_start_time = current_time;
		sjob->sjob_contract = job->job_contract;

		/*
		 * Populate the sjob with contract information.
		 */
		snprintf(statfile, sizeof (statfile),
			CTFS_ROOT "/process/%d/status", job->job_contract);
		if ((sjob->sjob_contract_fd =
				open64(statfile, O_RDONLY)) == -1) {
			logm(LOG_ERR, "do_start_job: %s: %s",
				statfile, strerror(errno));
			goto err;
		}

		if (fd_set_cloexec(sjob->sjob_contract_fd, 1) == -1) {
			logm(LOG_ERR, "do_start_job: cannot set "
				"cloexec on contract fd: %s",
				strerror(errno));
			goto err;
		}

		snprintf(ctlfile, sizeof (ctlfile),
			CTFS_ROOT "/process/%d/ctl", job->job_contract);
		if ((ctfd = open64(ctlfile, O_WRONLY)) == -1) {
			logm(LOG_ERR, "do_start_job: %s: %s",
				ctlfile, strerror(errno));
			goto err;
		}
		if (ct_ctl_adopt(ctfd) != 0) {
			logm(LOG_ERR, "do_start_job: cannot adopt %d: %s",
					job->job_contract, strerror(errno));
			close(ctfd);
			goto err;
		}
		close(ctfd);

		/*
		 * Register for events on the contract event fd.
		 */
		(void) snprintf(ctevents, sizeof (ctevents),
				"%s/process/%d/events",
				CTFS_ROOT, sjob->sjob_contract);
		if ((sjob->sjob_eventfd = open64(ctevents, O_RDONLY)) == -1) {
			logm(LOG_ERR, "do_start_job: %s: %s",
				ctevents, strerror(errno));
			goto err;
		}

		if (fd_open(sjob->sjob_eventfd) == -1) {
			logm(LOG_ERR, "do_start_job: fd_open failed");
			goto err;
		}

		if (register_fd(sjob->sjob_eventfd, FDE_READ,
				sched_fd_callback, NULL) == -1) {
			logm(LOG_ERR, "do_start_job: register_fd(%d): %s",
				sjob->sjob_eventfd, strerror(errno));
			goto err;
		}

		sjob->sjob_state = SJOB_RUNNING;
		return (0);
	}

err:
	free_sjob(sjob);
#endif

	if (job->job_flags & JOB_MAINTENANCE)
		return (0);

	if (job->job_flags & JOB_ENABLED) {
		if (job->job_flags & JOB_SCHEDULED) {
			sched_job_scheduled(job);
		} else {
			if (sched_start(job) == -1)
				logm(LOG_ERR, "do_start_job: job %ld: "
						"sched_start failed",
						(long)job->job_id);
		}
		return (0);
	}

	return (0);
}
