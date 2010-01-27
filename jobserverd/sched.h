/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * The scheduler is responsible for starting and stopping jobs, and handling
 * process events.
 */

#ifndef SCHED_H
#define	SCHED_H

#include	<sys/contract/process.h>

#include	"state.h"
#include	"event.h"
#include	"ct.h"

/*
 * If a non-scheduled job is running for less than this many seconds, we
 * consider the job failed and put it into maintenance mode.  This prevents a
 * broken job from continuously restarting.
 */
#define	SCHED_MIN_RUNTIME	(60*5)

int sched_init(int port);

typedef enum {
	SJOB_UNKNOWN = 0,
	SJOB_RUNNING,
	SJOB_STOPPING,
	SJOB_STOPPED
} sjob_state_t;

typedef struct {
	job_id_t	 sjob_id;		/* job this sjob represents */
	sjob_state_t	 sjob_state;
	contract_t	*sjob_contract;
	contract_t	*sjob_stop_contract;
	ev_id_t		 sjob_timer;
	pid_t		 sjob_pid;
	int		 sjob_fatal;		/* job received a fatal event */
	time_t		 sjob_nextrun;
	time_t		 sjob_start_time;
	char const	*sjob_lasterr;
} sjob_t;

int sched_start(job_t *);
int sched_stop(job_t *);
sjob_state_t sched_get_state(job_t *);
void sched_stop_all(void);

/*
 * Call one of these functions when a job is enabled or disabled
 * to have the scheduler take the appropriate action.
 */
void sched_job_enabled(job_t *);
void sched_job_disabled(job_t *);
void sched_job_deleted(job_t *);
void sched_job_scheduled(job_t *);
void sched_job_unscheduled(job_t *);

/*
 * Get the next runtime of a scheduled job.
 */
time_t sched_nextrun(cron_t *);

/*
 * Return the number of running jobs.
 */
int sched_jobs_running(void);

#endif	/* !SCHED_H */
