/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Holds the database of configured jobs.
 */

#ifndef	STATE_H
#define	STATE_H

#include	<sys/types.h>
#include	<rctl.h>

#include	"queue.h"

typedef int32_t job_id_t;

#define	ST_EXIT_DISABLE		0x1	/* Disable on exit */
#define	ST_EXIT_RESTART		0x2	/* Restart on exit */
#define	ST_EXIT_MAIL		0x4	/* Mail user on exit */

#define	JOB_MAINTENANCE		0x1
#define	JOB_ENABLED		0x2
#define	JOB_SCHEDULED		0x4

#define DEFAULT_LOGFMT		"%h/.job/%f.log"

/*
 * Schedule information for scheduled jobs.
 */
typedef enum {
	CRON_ABSOLUTE = 0,
	CRON_EVERY_MINUTE,
	CRON_EVERY_HOUR,
	CRON_EVERY_DAY,
	CRON_EVERY_WEEK
} cron_type_t;

typedef struct {
	cron_type_t	cron_type;
	/*
	 * For ABSOLUTE, we store the time_t of the start time.
	 * For EVERY_MINUTE, we store nothing (the job will run at a pseudo-
	 * random time approximately one minute from its last run).
	 * For EVERY_HOUR, we store the minute of the hour it should run at.
	 * For EVERY_DAY, we store the hour and minute it should run at, as
	 * minutes since midnight.
	 * For EVERY_WEEK, we store the day it should run at (0=Sunday), and
	 * the hour+minute in the same way as EVERY_DAY.
	 */
	int32_t	cron_arg1;
	int32_t	cron_arg2;
} cron_t;

char *cron_to_string(cron_t *);
char *cron_to_string_interval(cron_t *);

typedef struct {
	char		jr_name[32];
	rctl_qty_t	jr_value;
} job_rctl_t;

/*
 * Do not modify the contents of this struct; use the functions below.
 */
typedef struct job {
	/*
	 * General information about a job.
	 */
	job_id_t	 job_id;
	char		*job_username;
	char		*job_fmri;
	char		*job_start_method;
	char		*job_stop_method;
	uint32_t	 job_flags;
	uint32_t	 job_exit_action;	/* action on successful exit */
	uint32_t	 job_crash_action;	/* action on crash */
	uint32_t	 job_fail_action;	/* action on non-0 exit */
	cron_t		 job_schedule;
	job_rctl_t	*job_rctls;
	int		 job_nrctls;
	char		*job_project;
	ctid_t		 job_contract;
	char		*job_logfmt;
	int		 job_logsize;
	int		 job_logkeep;
	LIST_ENTRY(job)	 job_entries;
} job_t;

int	 statedb_init(void);
void	 statedb_shutdown(void);

/* Create a new job, insert it into the database, and return it. */
job_t	*create_job(char const *user, char const *name);

/* Find an existing job in the database. */
job_t	*find_job(job_id_t);

/* Delete a job from the database. */
int	 delete_job(job_t *);

/* Deallocate a job_t.  Does not remove the job from the database. */
void	 free_job(job_t *);

/* Rename a job in the database. */
int	 job_set_fmri(job_t *, char const *);

/*
 * Find a job by partial or full FMRI.  For the job name:
 *
 *   job:/jsmith/foo/myjob
 *
 * The following specifications will be accepted:
 *
 *   job:/jsmith/foo/myjob
 *   jsmith/foo/myjob
 *   foo/myjob
 *   myjob
 *
 * If the specification matches more than one FMRI, an error is returned.
 */
job_t	*find_job_fmri(char const *);

/*
 * Enumerate all jobs, or all jobs owned by a particular user.  For each job
 * found, the callback function is called.  If the callback function returns a
 * value other than 0, enumeration is canceled.
 */
typedef int (*job_enumerate_callback) (job_t *, void *);
int	 job_enumerate(job_enumerate_callback, void *);
int	 job_enumerate_user(char const *, job_enumerate_callback, void *);

/* Change the start/stop methods for the job in the database. */
int	 job_set_start_method(job_t *, char const *);
int	 job_set_stop_method(job_t *, char const *);

/* Change the various exit actions. */
int	job_set_exit_action(job_t *, int);
int	job_set_fail_action(job_t *, int);
int	job_set_crash_action(job_t *, int);

/*
 * Set or replace the schedule for a job.  This also reschedules
 * the job.
 */
int	job_set_schedule(job_t *, char const *);
int	job_unschedule(job_t *);

/*
 * Enable or disable a job.  It is not an error to enable or disable a job more
 * than once.  A disabled job will never be started.
 */
int	 job_enable(job_t *);
int	 job_disable(job_t *);

/* Set the last seen error message for a job. */
int	job_set_lasterr(job_t *, char const *);

/*
 * Set or clear maintenance flag on a job.
 */
int	job_set_maintenance(job_t *, char const *reason);
int	job_clear_maintenance(job_t *);

/*
 * Set (replace), remove or fetch an rctl value for a job.
 */
int		job_set_rctl(job_t *, char const *name, rctl_qty_t value);
rctl_qty_t	job_get_rctl(job_t *, char const *name);
int		job_clear_rctl(job_t *, char const *name);

int		 is_valid_rctl(char const *name);
int		 get_rctl_type(char const *name);
char const	*format_rctl(rctl_qty_t value, int type);

/* Set a job's contract, for restart recovery. */
int	 job_set_ctid(job_t *, ctid_t);

/* Set the project for a job. */
int	job_set_project(job_t *, char const *);

/* Set the log format */
int	job_set_logfmt(job_t *, char const *);
int	job_set_logsize(job_t *, size_t);
int	job_set_logkeep(job_t *, int);

/* Count the number of jobs created by a given user. */
int	njobs_for_user(char const *);

/* Fetch/change quotas. */
int	quota_get_jobs_per_user(void);
int	quota_set_jobs_per_user(int);

/* Check if a particular user has access to a job. */
#define	JOB_VIEW	0x1	/* View information about a job */
#define	JOB_MODIFY	0x2	/* Change a job's definition */
#define	JOB_DELETE	0x4	/* Delete a job */
#define	JOB_STARTSTOP	0x8	/* Enable or disable a job */

int	job_access(job_t *, char const *, int);

/* Check if an FMRI is syntactically valid. */
int	valid_fmri(char const *);

/*
 * Track the system boot time at each startup.  If the system hasn't
 * rebooted since the last boot, we try to reassociate orphaned
 * contracts.
 */
time_t	get_boottime(time_t);

#endif	/* !STATE_H */
