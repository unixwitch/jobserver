/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

/*
 * The scheduler is responsible for starting and stopping jobs, and handling
 * process events.
 */

#ifndef SCHED_H
#define SCHED_H

#include	<sys/contract/process.h>

#include	"state.h"
#include	"event.h"

/*
 * If a non-scheduled job is running for less than this many seconds, we
 * consider the job failed and put it into maintenance mode.  This prevents a
 * broken job from continuously restarting.
 */
#define SCHED_MIN_RUNTIME	(60*5)

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
	ctid_t		 sjob_contract;		/* contract of the job itself */
	int		 sjob_contract_fd;
	ctid_t		 sjob_stop_contract;	/* contract of the stop method, if any */
	int		 sjob_stop_contract_fd;
	int		 sjob_eventfd;
	ev_id_t		 sjob_timer;
	pid_t		 sjob_pid;
	int		 sjob_fatal;		/* job received a fatal event */
	time_t		 sjob_nextrun;		/* next runtime for scheduled jobs */
	time_t		 sjob_start_time;	/* when the job last started */
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
