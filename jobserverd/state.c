/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<sys/stat.h>
#include	<errno.h>
#include	<string.h>
#include	<assert.h>
#include	<stdlib.h>
#include	<strings.h>
#include	<libnvpair.h>
#include	<alloca.h>
#include	<rctl.h>
#include	<project.h>
#include	<pwd.h>
#include	<ctype.h>
#include	<inttypes.h>

#include	"jobserver.h"
#include	"state.h"
#include	"sched.h"
#include	"kvdb.h"

#define	DB_PATH "/var/jobserver"

static int job_update(job_t *);

static int db = -1;
static int table_jobs = -1;
static int table_config = -1;

static int unserialise_job(job_t **, nvlist_t *nvl);

static LIST_HEAD(job_list, job) jobs;

static int
load_job_callback(key, nvl, udata)
	char const	*key;
	nvlist_t	*nvl;
	void		*udata;
{
job_t	*job;
int	*nerrs = udata;
	if (unserialise_job(&job, nvl) == -1) {
		logm(LOG_ERR, "load_job_callback: unserialise failed");
		(*nerrs)++;
		return (1);
	}

	LIST_INSERT_HEAD(&jobs, job, job_entries);
	return (0);
}

int
statedb_init()
{
struct stat	sb;
int		nerrs = 0;

	if (stat(DB_PATH, &sb) == -1) {
		if (errno != ENOENT) {
			logm(LOG_ERR, "statedb_init: "
			    "cannot access database %s: %s",
			    DB_PATH, strerror(errno));
			return (-1);
		}

		if (mkdir(DB_PATH, 0700) == -1) {
			logm(LOG_ERR, "statedb_init: "
			    "cannot create database %s: %s",
			    DB_PATH, strerror(errno));
			return (-1);
		}

		logm(LOG_NOTICE, "created directory %s", DB_PATH);
	}

	if ((db = kvdb_open(DB_PATH, KVDB_CREATE)) == -1) {
		logm(LOG_ERR, "statedb_init: %s: %s",
		    DB_PATH, strerror(errno));
		goto err;
	}

	if ((table_jobs = kvtable_open(db, "jobs", KVT_CREATE)) == -1) {
		logm(LOG_ERR, "statedb_init: %s: %s",
		    "jobs", strerror(errno));
		goto err;
	}

	if ((table_config = kvtable_open(db, "config", KVT_CREATE)) == -1) {
		logm(LOG_ERR, "statedb_init: %s: %s",
		    "jobs", strerror(errno));
		goto err;
	}

	if (kvenumerate_nvlist(table_jobs, load_job_callback, &nerrs) == -1) {
		logm(LOG_ERR, "statedb_init: kvenumerate: %s",
		    strerror(errno));
		goto err;
	}

	if (nerrs)
		goto err;

	return (0);

err:
	if (table_jobs != -1)
		kvtable_close(table_jobs);
	if (table_config != -1)
		kvtable_close(table_config);
	if (db != -1)
		kvdb_close(db);

	db = table_jobs = table_config = -1;
	return (-1);
}

void
statedb_shutdown()
{
	if (table_jobs != -1)
		kvtable_close(table_jobs);
	if (table_config != -1)
		kvtable_close(table_config);
	if (db != -1)
		kvdb_close(db);

	db = table_jobs = table_config = -1;
}

static job_id_t next_job_id(void);

static job_id_t
next_job_id()
{
	/*
	 * Current job id is stored in the config db under the 'next_job_id'
	 * key.  We retrieve the current value, then increment it for the next
	 * caller.
	 */
job_id_t	*id = NULL, i;
size_t		 idsize;

	if (kvtable_get(table_config, "next_job_id",
	    (char **)&id, &idsize) == -1) {
	job_id_t	newid;
		if (errno != ENOENT) {
			logm(LOG_ERR, "next_job_id: db get failed: %s",
			    strerror(errno));
			goto err;
		}

		/* This is the first call, so insert a new key. */
		newid = 1;
		if (kvtable_insert(table_config, "next_job_id",
		    (char *)&newid, sizeof (newid)) == -1) {
			logm(LOG_ERR, "next_job_id: db put failed: %s",
			    strerror(errno));
			goto err;
		}

		return (newid - 1);
	}

	if (idsize != sizeof (id)) {
		logm(LOG_ERR, "next_job_id: wrong data size");
		goto err;
	}

	/*
	 * Increment the key and put it back.
	 */
	(*id)++;

	if (kvtable_replace(table_config, "next_job_id",
	    (char *)id, sizeof (*id)) == -1) {
		logm(LOG_ERR, "next_job_id: db put failed: %s",
		    strerror(errno));
		goto err;
	}

	i = *id;
	free(id);
	return (i - 1);

err:
	free(id);
	return (-1);
}

int
quota_get_jobs_per_user()
{
int	*njobs, nj;
size_t	 sz;

	if (kvtable_get(table_config, "quota_jobs_per_user",
	    (char **)&njobs, &sz) == -1) {
		if (errno == ENOENT)
			return (0);
		return (-1);
	}

	nj = *njobs;
	free(njobs);
	return (nj);
}

int
quota_set_jobs_per_user(n)
	int	n;
{
	if (kvtable_replace(table_config, "quota_jobs_per_user",
	    (char *)&n, sizeof (n)) == -1) {
		logm(LOG_ERR, "quota_set_jobs_per_user: db put failed: %s",
		    strerror(errno));
		return (-1);
	}

	return (0);
}

job_t *
create_job(user, name)
	char const	*user, *name;
{
job_t		*job = NULL;
char		*fmri = NULL;

	if (asprintf(&fmri, "job:/%s/%s", user, name) == -1) {
		logm(LOG_ERR, "create_job: out of memory");
		goto err;
	}

	if ((job = find_job_fmri(fmri)) != NULL) {
		goto err;
	}

	if ((job = calloc(1, sizeof (*job))) == NULL) {
		logm(LOG_ERR, "create_job: out of memory");
		goto err;
	}

	if ((job->job_id = next_job_id()) == -1) {
		logm(LOG_ERR, "create_job: cannot get job id");
		goto err;
	}

	job->job_fmri = fmri;
	fmri = NULL;

	if ((job->job_username = strdup(user)) == NULL ||
	    (job->job_start_method = strdup("")) == NULL ||
	    (job->job_stop_method = strdup("")) == NULL) {
		logm(LOG_ERR, "create_job: out of memory");
		goto err;
	}


	job->job_fail_action = ST_EXIT_DISABLE | ST_EXIT_MAIL;
	job->job_exit_action = ST_EXIT_DISABLE | ST_EXIT_MAIL;
	job->job_crash_action = ST_EXIT_DISABLE | ST_EXIT_MAIL;

	job->job_logsize = (1024 * 1024);
	job->job_logkeep = 5;

	if (job_update(job) == -1)
		goto err;

	LIST_INSERT_HEAD(&jobs, job, job_entries);
	return (job);

err:
	free(fmri);
	free_job(job);
	return (NULL);
}

static int
unserialise_job(job, nvl)
	job_t		**job;
	nvlist_t	*nvl;
{
int32_t		 ct, ca1, ca2, ctid;
char		*start = NULL, *stop = NULL, *proj = NULL,
		*fmri = NULL, *username, *logfmt;
uchar_t		*rctls;
uint_t		 nrctls;
int32_t		 i;

	if ((*job = calloc(1, sizeof (job_t))) == NULL) {
		logm(LOG_ERR, "unserialise_job: out of memory");
		goto err;
	}

	if (nvlist_lookup_int32(nvl, "id", &(*job)->job_id) ||
		nvlist_lookup_string(nvl, "start", &start) ||
		nvlist_lookup_string(nvl, "stop", &stop) ||
		nvlist_lookup_uint32(nvl, "flags", &(*job)->job_flags) ||
		nvlist_lookup_uint32(nvl, "exit_action",
			&(*job)->job_exit_action) ||
		nvlist_lookup_uint32(nvl, "crash_action",
			&(*job)->job_crash_action) ||
		nvlist_lookup_uint32(nvl, "fail_action",
			&(*job)->job_fail_action) ||
		nvlist_lookup_int32(nvl, "cron_type", &ct) ||
		nvlist_lookup_int32(nvl, "cron_arg1", &ca1) ||
		nvlist_lookup_int32(nvl, "cron_arg2", &ca2)) {

		logm(LOG_ERR, "unserialise_job: cannot unserialise: %s",
			strerror(errno));
		goto err;
	}

	if (nvlist_lookup_string(nvl, "username", &username) == 0 &&
	    nvlist_lookup_string(nvl, "fmri", &fmri) == 0) {
		if (((*job)->job_fmri = strdup(fmri)) == NULL ||
		    ((*job)->job_username = strdup(username)) == NULL) {
			logm(LOG_ERR, "unserialise_job: out of memory");
			goto err;
		}
	} else {
		logm(LOG_ERR, "unserialise_job: no FMRI");
	}

	if (nvlist_lookup_int32(nvl, "ctid", &ctid) == 0)
		(*job)->job_contract = ctid;
	else
		(*job)->job_contract = -1;

	if (nvlist_lookup_int32(nvl, "logkeep", &i) == 0)
		(*job)->job_logkeep = i;
	else
		(*job)->job_logkeep = 5;

	if (nvlist_lookup_int32(nvl, "logsize", &i) == 0)
		(*job)->job_logsize = i;
	else
		(*job)->job_logsize = 5;

	if (nvlist_lookup_byte_array(nvl, "rctls", &rctls, &nrctls) == 0) {
		/*LINTED*/
		(*job)->job_nrctls = nrctls / sizeof (job_rctl_t);
		if (((*job)->job_rctls = calloc(nrctls,
				sizeof (job_rctl_t))) == NULL) {
			logm(LOG_ERR, "job_update: out of memory");
			goto err;
		}

		bcopy(rctls, (*job)->job_rctls, sizeof (job_rctl_t) * nrctls);
	} else {
		(*job)->job_nrctls = 0;
	}

	if (nvlist_lookup_string(nvl, "project", &proj) == 0) {
		if (((*job)->job_project = strdup(proj)) == NULL) {
			logm(LOG_ERR, "job_update: out of memory");
			goto err;
		}
	}

	if (nvlist_lookup_string(nvl, "logfmt", &logfmt) == 0) {
		if (((*job)->job_logfmt = strdup(logfmt)) == NULL) {
			logm(LOG_ERR, "job_update: out of memory");
			goto err;
		}
	}

	(*job)->job_schedule.cron_type = ct;
	(*job)->job_schedule.cron_arg1 = ca1;
	(*job)->job_schedule.cron_arg2 = ca2;

	if (((*job)->job_start_method = strdup(start)) == NULL ||
	    ((*job)->job_stop_method = strdup(stop)) == NULL) {
		logm(LOG_ERR, "unserialise_job: out of memory");
		goto err;
	}

	return (0);

err:
	free_job(*job);
	return (-1);
}

job_t *
find_job(id)
	job_id_t	id;
{
job_t	*job = NULL;

	LIST_FOREACH(job, &jobs, job_entries) {
		if (job->job_id == id)
			return (job);
	}

	return (NULL);

}

job_t *
find_job_fmri(fmri)
	char const	*fmri;
{
job_t		*job;
char const	*p, *q;
	LIST_FOREACH(job, &jobs, job_entries) {
		/*
		 * Check for an exact match.
		 */
		if (strcmp(fmri, job->job_fmri) == 0)
			return (job);

		/*
		 * Sanity: if the job starts with job:/ and wasn't an exact
		 * match, then it can't exist.
		 */
		if (strncmp(fmri, "job:/", 5) == 0)
			continue;

		/*
		 * If the spec is longer than the FMRI, it can't possibly match.
		 * Subtract 4 because the partial FMRI can't match the "job:".
		 */
		if (strlen(fmri) > strlen(job->job_fmri) - 4)
			continue;

		/*
		 * Start at the end of the string, and match backwards.  Every
		 * time we see a / in one string, it must match a / in the
		 * other.
		 */
		p = fmri + strlen(fmri) - 1;
		q = job->job_fmri + strlen(job->job_fmri) - 1;
		for (; p >= fmri && q >= job->job_fmri; --p, --q) {
			if (*p != *q)
				goto next;

			if (*p == '/' && *q != '/')
				goto next;

			if (*q == '/' && *p != '/')
				goto next;
		}

		/*
		 * Ensure the last part matched is a full name, which means
		 * there must be a / before it.
		 */
		if (*q != '/')
			continue;

		/* Match! */
		return (job);

next:		;
	}

	return (NULL);
}

int
delete_job(job)
	job_t	*job;
{
char	id[64];
	(void) snprintf(id, sizeof (id), "%ld", (long)job->job_id);

	if (kvtable_delete(table_jobs, id) == -1) {
		logm(LOG_ERR, "delete_job: db del failed: %s",
		    strerror(errno));
		goto err;
	}

	sched_job_deleted(job);
	LIST_REMOVE(job, job_entries);
	return (0);

err:
	return (-1);
}

int
job_enable(job)
	job_t	*job;
{
int	 ret;

	job->job_flags |= JOB_ENABLED;
	ret = job_update(job);

	if (!(job->job_flags & JOB_MAINTENANCE))
		sched_job_enabled(job);

	return (ret);
}

int
job_disable(job)
	job_t	*job;
{
int	 ret;

	job->job_flags &= ~JOB_ENABLED;
	ret = job_update(job);

	sched_job_disabled(job);
	return (ret);
}

int
job_set_ctid(job, ctid)
	job_t	*job;
	ctid_t	 ctid;
{
int	 ret;

	job->job_contract = ctid;
	ret = job_update(job);

	return (ret);
}

int
job_set_exit_action(job, flags)
	job_t	*job;
	int	flags;
{
int	 ret;

	job->job_exit_action = flags;
	ret = job_update(job);

	return (ret);
}

int
job_set_crash_action(job, flags)
	job_t	*job;
	int	flags;
{
int	 ret;

	job->job_crash_action = flags;
	ret = job_update(job);

	return (ret);
}

int
job_set_fail_action(job, flags)
	job_t	*job;
	int	 flags;
{
int	 ret;

	job->job_fail_action = flags;
	ret = job_update(job);

	return (ret);
}

int
job_set_start_method(job, method)
	job_t		*job;
	char const	*method;
{
char	*news;
int	 ret;

	if ((news = strdup(method)) == NULL)
		return (-1);

	free(job->job_start_method);
	job->job_start_method = news;

	ret = job_update(job);
	return (ret);
}

int
job_set_stop_method(job, method)
	job_t		*job;
	char const	*method;
{
char	*news;
int	 ret;

	if ((news = strdup(method)) == NULL)
		return (-1);

	free(job->job_stop_method);
	job->job_stop_method = news;

	ret = job_update(job);
	return (ret);
}

int
job_set_fmri(job, fmri)
	job_t		*job;
	char const	*fmri;
{
char	*news;
int	 ret;

	if (find_job_fmri(fmri) != NULL)
		return (-1);

	if ((news = strdup(fmri)) == NULL)
		return (-1);

	free(job->job_fmri);
	job->job_fmri = news;

	ret = job_update(job);
	return (ret);
}

/*
 * Update the job in the database.
 */
static int
job_update(job)
	job_t	*job;
{
nvlist_t	*nvl = NULL;
char		 id[64];

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) {
		logm(LOG_ERR, "job_update: nvlist_alloc failed: %s",
				strerror(errno));
		goto err;
	}

	if (nvlist_add_int32(nvl, "id", job->job_id) != 0 ||
		nvlist_add_string(nvl, "username",
			job->job_username) != 0 ||
		nvlist_add_string(nvl, "fmri",
			job->job_fmri) != 0 ||
		nvlist_add_string(nvl, "start",
			job->job_start_method) != 0 ||
		nvlist_add_string(nvl, "stop",
			job->job_stop_method) != 0 ||
		nvlist_add_uint32(nvl, "flags",
			job->job_flags) != 0 ||
		nvlist_add_uint32(nvl, "exit_action",
			job->job_exit_action) != 0 ||
		nvlist_add_uint32(nvl, "crash_action",
			job->job_crash_action) != 0 ||
		nvlist_add_uint32(nvl, "fail_action",
			job->job_fail_action) != 0 ||
		nvlist_add_int32(nvl, "ctid",
			(int32_t)job->job_contract) != 0 ||
		nvlist_add_int32(nvl, "logkeep",
			(int32_t)job->job_logkeep) != 0 ||
		nvlist_add_int32(nvl, "logsize",
			(int32_t)job->job_logsize) != 0 ||
		nvlist_add_int32(nvl, "cron_type",
			(int32_t)job->job_schedule.cron_type) != 0 ||
		nvlist_add_int32(nvl, "cron_arg1",
			(int32_t)job->job_schedule.cron_arg1) != 0 ||
		nvlist_add_int32(nvl, "cron_arg2",
			(int32_t)job->job_schedule.cron_arg2) != 0) {

		logm(LOG_ERR, "job_update: cannot serialise: %s",
			strerror(errno));
		goto err;
	}

	if (job->job_nrctls && nvlist_add_byte_array(nvl, "rctls",
		(uchar_t *)job->job_rctls,
		sizeof (job_rctl_t) * job->job_nrctls) != 0) {

		logm(LOG_ERR, "job_update: cannot serialise: %s",
			strerror(errno));
		goto err;
	}

	if (job->job_project) {
		if (nvlist_add_string(nvl, "project", job->job_project) != 0) {
			logm(LOG_ERR, "job_update: " "cannot serialise: %s",
				strerror(errno));
			goto err;
		}
	}

	if (job->job_logfmt) {
		if (nvlist_add_string(nvl, "logfmt", job->job_logfmt) != 0) {
			logm(LOG_ERR, "job_update: " "cannot serialise: %s",
				strerror(errno));
			goto err;
		}
	}

	(void) snprintf(id, sizeof (id), "%ld", (long)job->job_id);
	if (kvtable_replace_nvlist(table_jobs, id, nvl) == -1) {
		logm(LOG_ERR, "job_update: db put failed: %s",
		    strerror(errno));
		goto err;
	}

	nvlist_free(nvl);

	return (0);

err:
	nvlist_free(nvl);
	return (-1);
}

void
free_job(job)
	job_t	*job;
{
	if (!job)
		return;

	free(job->job_rctls);
	free(job->job_start_method);
	free(job->job_stop_method);
	free(job->job_project);
	free(job->job_logfmt);
	free(job->job_fmri);
	free(job->job_username);
	free(job);
}

int
job_enumerate(cb, udata)
	job_enumerate_callback	 cb;
	void			*udata;
{
	return (job_enumerate_user(NULL, cb, udata));
}

int
job_enumerate_user(username, cb, udata)
	char const *		 username;
	job_enumerate_callback	 cb;
	void			*udata;
{
job_t	*job;

	LIST_FOREACH(job, &jobs, job_entries) {
		if (username && strcmp(job->job_username, username))
			continue;

		if (cb(job, udata))
			break;
	}
	return (0);
}

int
job_unschedule(job)
	job_t	*job;
{
	job->job_flags &= ~(JOB_SCHEDULED | JOB_ENABLED);

	/*
	 * When unschedling a shceduled job with exit=restart,
	 * change it back to 'disable'.
	 */
	if ((job->job_exit_action & ST_EXIT_RESTART))
		job->job_exit_action = (job->job_exit_action
		    & ~ST_EXIT_RESTART) | ST_EXIT_DISABLE;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_schedule: warning: job_update failed");

	sched_job_unscheduled(job);

	return (0);
}

int
job_set_maintenance(job, reason)
	job_t		*job;
	char const	*reason;
{
	job->job_flags |= JOB_MAINTENANCE;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_set_maintenance: "
			"warning: job_update failed");

	return (0);
}

int
job_clear_maintenance(job)
	job_t	*job;
{
	job->job_flags &= ~JOB_MAINTENANCE;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_clear_maintenance: "
			"warning: job_update failed");

	if (job->job_flags & JOB_ENABLED)
		sched_job_enabled(job);

	return (0);
}

int
job_set_schedule(job, sched)
	job_t		*job;
	char const	*sched;
{
cron_t	 cron;
int	 i, j, y, mo, d, h, mi;
char	 s[64];

	if (job->job_flags & JOB_ENABLED) {
		errno = EINVAL;
		goto err;
	}

	/* Try to parse the sched into a cron_t. */

	bzero(&cron, sizeof (cron));
	if (strcmp(sched, "every minute") == 0) {
		cron.cron_type = CRON_EVERY_MINUTE;
	} else if (sscanf(sched, "every hour at %d", &i) == 1) {
		if (i < 0 || i > 59) {
			errno = EINVAL;
			goto err;
		}

		cron.cron_type = CRON_EVERY_HOUR;
		cron.cron_arg1 = i;
	} else if (sscanf(sched, "every day at %d:%d", &i, &j) == 2) {
		if (i < 0 || i > 24) {
			errno = EINVAL;
			goto err;
		}

		if (j < 0 || j > 59) {
			errno = EINVAL;
			goto err;
		}

		cron.cron_type = CRON_EVERY_DAY;
		cron.cron_arg1 = (i * 60) + j;
	} else if (sscanf(sched, "every %15s at %d:%d", s, &i, &j) == 3) {
		if (i < 0 || i > 24) {
			errno = EINVAL;
			goto err;
		}

		if (j < 0 || j > 59) {
			errno = EINVAL;
			goto err;
		}

		cron.cron_type = CRON_EVERY_WEEK;
		cron.cron_arg2 = (i * 60) + j;

		if (strcasecmp(s, "sunday") == 0)
			cron.cron_arg1 = 0;
		else if (strcasecmp(s, "monday") == 0)
			cron.cron_arg1 = 1;
		else if (strcasecmp(s, "tuesday") == 0)
			cron.cron_arg1 = 2;
		else if (strcasecmp(s, "wednesday") == 0)
			cron.cron_arg1 = 3;
		else if (strcasecmp(s, "thursday") == 0)
			cron.cron_arg1 = 4;
		else if (strcasecmp(s, "friday") == 0)
			cron.cron_arg1 = 5;
		else if (strcasecmp(s, "saturday") == 0)
			cron.cron_arg1 = 6;
		else {
			errno = EINVAL;
			goto err;
		}
	} else if (sscanf(sched, "in %d %15s", &i, s) == 2) {
		if (strcmp(s, "minutes") == 0 || strcmp(s, "minute") == 0) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60);
		} else if (strcmp(s, "hours") == 0 || strcmp(s, "hour") == 0) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60);
		} else if (strcmp(s, "days") == 0 || strcmp(s, "day") == 0) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60 * 24);
		} else if (strcmp(s, "weeks") == 0 || strcmp(s, "week") == 0) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60 * 24 * 7);
		} else {
			errno = EINVAL;
			goto err;
		}
	} else if (sscanf(sched, "at %d-%d-%d %d:%d",
			&y, &mo, &d, &h, &mi) == 5) {
	struct tm	tm;
		bzero(&tm, sizeof (tm));
		tm.tm_year = y - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = d;
		tm.tm_hour = h;
		tm.tm_min = mi;
		cron.cron_type = CRON_ABSOLUTE;
		cron.cron_arg1 = mktime(&tm);

		if (cron.cron_arg1 < current_time) {
			errno = EINVAL;
			goto err;
		}
	} else if (sscanf(sched, "at %d:%d", &h, &mi) == 2) {
	struct tm	*tm = gmtime(&current_time);
		tm->tm_hour = h;
		tm->tm_min = mi;
		cron.cron_type = CRON_ABSOLUTE;
		cron.cron_arg1 = mktime(tm);

		if (cron.cron_arg1 < current_time) {
			tm->tm_mday++;
			cron.cron_arg1 = mktime(tm);
		}

		if (cron.cron_arg1 < current_time) {
			errno = EINVAL;
			goto err;
		}
	} else {
		errno = EINVAL;
		goto err;
	}

	job->job_schedule = cron;
	job->job_flags |= (JOB_SCHEDULED | JOB_ENABLED);

	/*
	 * When scheduling a job with exit=disable, automatically change it to
	 * exit=restart, since this is invariably the desired behaviour.
	 */
	if ((job->job_exit_action & ST_EXIT_DISABLE) &&
	    cron.cron_type != CRON_ABSOLUTE)
		job->job_exit_action = (job->job_exit_action
		    & ~ST_EXIT_DISABLE) | ST_EXIT_RESTART;

	/*
	 * Remove 'mail' from the exit action.  A scheduled job exiting is
	 * not normally an interesting event.
	 */
	job->job_exit_action &= ~ST_EXIT_MAIL;

	if (job_update(job) == -1)
		logm(LOG_ERR, "job_schedule: warning: job_update failed");

	sched_job_scheduled(job);
	sched_job_enabled(job);

	return (0);

err:
	return (-1);
}

char *
cron_to_string(cron)
	cron_t	*cron;
{
static char	 buf[128];
struct tm	*tm;
time_t		 t;
int		 hr, min;
int		 a1, a2;

	a1 = cron->cron_arg1;
	a2 = cron->cron_arg2;

	switch (cron->cron_type) {
	case CRON_ABSOLUTE:
		t = a1;
		tm = gmtime(&t);
		(void) strcpy(buf, "at ");
		(void) strftime(buf + 3, sizeof (buf) - 3,
			"%Y-%m-%d %H:%M", tm);
		return (buf);

	case CRON_EVERY_MINUTE:
		(void) strcpy(buf, "every minute");
		return (buf);

	case CRON_EVERY_HOUR:
		(void) snprintf(buf, sizeof (buf), "every hour at %02d", a1);
		return (buf);

	case CRON_EVERY_DAY:
		min = a1 % 60;
		hr = a1 / 60;
		(void) snprintf(buf, sizeof (buf),
			"every day at %02d:%02d", hr, min);
		return (buf);

	case CRON_EVERY_WEEK:
		(void) strcpy(buf, "every ");
		switch (a1) {
		case 0: (void) strlcat(buf, "Sunday", sizeof (buf)); break;
		case 1: (void) strlcat(buf, "Monday", sizeof (buf)); break;
		case 2: (void) strlcat(buf, "Tuesday", sizeof (buf)); break;
		case 3: (void) strlcat(buf, "Wednesday", sizeof (buf)); break;
		case 4: (void) strlcat(buf, "Thursday", sizeof (buf)); break;
		case 5: (void) strlcat(buf, "Friday", sizeof (buf)); break;
		case 6: (void) strlcat(buf, "Saturday", sizeof (buf)); break;
		}


		min = a2 % 60;
		hr = a2 / 60;
		(void) snprintf(buf + strlen(buf), sizeof (buf) - strlen(buf),
				" at %02d:%02d", hr, min);
		return (buf);

	default:
		return ("unknown");
	}
}

char *
cron_to_string_interval(cron)
	cron_t	*cron;
{
time_t		when = sched_nextrun(cron) - current_time;
static char	buf[128];
size_t		i = 0;

	if (when <= 0)
		return ("a very short time");

	if (when > (60*60*24*7)) {
		(void) snprintf(buf + i, sizeof (buf) - i,
			"%dw", (when / (60 * 60 * 24 * 7)));
		i += strlen(buf + i);
		when %= (60 * 60 * 24 * 7);
	}

	if (when > (60*60*24)) {
		(void) snprintf(buf + i, sizeof (buf) - i,
			"%dd", (when / (60 * 60 * 24)));
		i += strlen(buf + i);
		when %= (60 * 60 * 24);
	}

	if (when > (60*60)) {
		(void) snprintf(buf + i, sizeof (buf) - i,
			"%dh", (when / (60 * 60)));
		i += strlen(buf + i);
		when %= (60 * 60);
	}

	if (when > 60) {
		(void) snprintf(buf + i, sizeof (buf) - i,
			"%dm", (when / 60));
		i += strlen(buf + i);
		when %= 60;
	}

	if (when)
		(void) snprintf(buf + i, sizeof (buf) - i,
			"%ds", when);

	return (buf);
}

int
job_set_lasterr(job, err)
	job_t		*job;
	char const	*err;
{
	return (0);
}

rctl_qty_t
job_get_rctl(job, name)
	job_t		*job;
	char const	*name;
{
int	 i;

	for (i = 0; i < job->job_nrctls; ++i) {
		if (strcmp(job->job_rctls[i].jr_name, name))
			continue;

		return (job->job_rctls[i].jr_value);
	}

	return (-1);
}

int
job_clear_rctl(job, name)
	job_t		*job;
	char const	*name;
{
int		 i, j;
job_rctl_t	*nr;
int		 newn;

	if (job->job_rctls == NULL) {
		return (0);
	}

	/*
	 * We could allocate nrctls-1 here, but we don't actually know if we'll
	 * be removing an rctl.
	 */
	if ((nr = calloc(sizeof (*nr), job->job_nrctls)) == NULL) {
		logm(LOG_ERR, "job_clear_rctl: out of memory");
		goto err;
	}

	for (i = 0, j = 0, newn = job->job_nrctls; i < job->job_nrctls; ++i) {
		if (strcmp(name, job->job_rctls[i].jr_name) == 0) {
			newn--;
			continue;
		}
		bcopy(&job->job_rctls[i], &nr[j], sizeof (job_rctl_t));
		j++;
	}

	free(job->job_rctls);
	job->job_rctls = nr;
	job->job_nrctls = newn;

	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_clear_rctl: job_update failed");
		goto err;
	}

	return (0);

err:
	free(nr);
	return (-1);
}

int
job_set_rctl(job, name, value)
	job_t		*job;
	char const	*name;
	rctl_qty_t	 value;
{
int		 i;
job_rctl_t	*nr = NULL;

	for (i = 0; i < job->job_nrctls; ++i) {
		if (strcmp(job->job_rctls[i].jr_name, name))
			continue;

		job->job_rctls[i].jr_value = value;

		if (job_update(job) == -1) {
			logm(LOG_ERR, "job_clear_rctl: job_update failed");
			goto err;
		}

		return (0);
	}

	if ((nr = xrecalloc(job->job_rctls,
			job->job_nrctls, job->job_nrctls + 1,
			sizeof (job_rctl_t))) == NULL) {
		logm(LOG_ERR, "job_set_rctl: out of memory");
		goto err;
	}

	(void) strlcpy(nr[job->job_nrctls].jr_name, name,
			sizeof (nr[job->job_nrctls].jr_name));
	nr[job->job_nrctls].jr_value = value;

	job->job_rctls = nr;
	job->job_nrctls++;
	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_clear_rctl: job_update failed");
		goto err;
	}

	return (0);

err:
	free(nr);
	return (-1);
}

int
is_valid_rctl(name)
	char const	*name;
{
rctlblk_t	*blk = alloca(rctlblk_size());
char		 rname[64];
	(void) snprintf(rname, sizeof (rname), "process.%s", name);
	if (getrctl(rname, NULL, blk, RCTL_FIRST) == -1)
		return (0);
	return (1);
}

int
get_rctl_type(name)
	char const	*name;
{
rctlblk_t	*blk = alloca(rctlblk_size());
uint_t		 fl;
char		 rname[64];
	(void) snprintf(rname, sizeof (rname), "process.%s", name);
	if (getrctl(rname, NULL, blk, RCTL_FIRST) == -1)
		return (RCTL_GLOBAL_COUNT);

	fl = rctlblk_get_global_flags(blk);
	if (fl & RCTL_GLOBAL_BYTES)
		return (RCTL_GLOBAL_BYTES);
	if (fl & RCTL_GLOBAL_SECONDS)
		return (RCTL_GLOBAL_SECONDS);
	return (RCTL_GLOBAL_COUNT);
}

static char const *
_format_bytes(n)
	rctl_qty_t	n;
{
double		d = n;
static char	res[32];
int		i = 0;
char const *const names[] = {
	"KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"
};
	while (d > 1024) {
		d /= 1024;
		i++;
	}

	(void) snprintf(res, sizeof (res), "%.2lf%s", d, names[i]);
	return (res);
}

static char const *
_format_seconds(n)
	rctl_qty_t	n;
{
char		t[16];
static char	res[64];

	bzero(res, sizeof (res));
	if (n >= (60 * 60 * 24 * 7)) {
		(void) snprintf(t, sizeof (t), "%" PRIu64 "w",
			(uint64_t)(n / (60 * 60 * 24 * 7)));
		(void) strlcat(res, t, sizeof (res));
		n %= (60 * 60 * 24 * 7);
	}

	if (n >= (60 * 60 * 24)) {
		(void) snprintf(t, sizeof (t), "%" PRIu64 "d",
			(uint64_t)(n / (60 * 60 * 24)));
		(void) strlcat(res, t, sizeof (res));
		n %= (60 * 60 * 24);
	}

	if (n >= (60 * 60)) {
		(void) snprintf(t, sizeof (t), "%" PRIu64 "h",
			(uint64_t)(n / (60 * 60)));
		(void) strlcat(res, t, sizeof (res));
		n %= (60 * 60);
	}

	if (n >= 60) {
		(void) snprintf(t, sizeof (t), "%" PRIu64 "m",
		    (uint64_t)(n / 60));
		(void) strlcat(res, t, sizeof (res));
		n %= 60;
	}

	if (n > 0 || strlen(res) == 0) {
		(void) snprintf(t, sizeof (t), "%" PRIu64 "s",
		    (uint64_t)n);
		(void) strlcat(res, t, sizeof (res));
	}
	return (res);
}

static char const *
_format_count(n)
	rctl_qty_t	n;
{
double		d = n;
static char	res[32];
int		i = 0;
char const *names = "KMGTPEZB";

	while (d > 1024) {
		d /= 1024;
		i++;
	}

	(void) snprintf(res, sizeof (res), "%.2lf%c", d, names[i]);
	return (res);
}

char const *
format_rctl(qty, type)
	rctl_qty_t	qty;
	int		type;
{
	switch (type) {
	case RCTL_GLOBAL_BYTES:
		return (_format_bytes(qty));
	case RCTL_GLOBAL_SECONDS:
		return (_format_seconds(qty));
	case RCTL_GLOBAL_COUNT:
	default:
		return (_format_count(qty));
	}
}

int
job_set_logfmt(job, logfmt)
	job_t		*job;
	char const	*logfmt;
{
char	*np = NULL;

	assert(job);

	if (logfmt && *logfmt) {
		if ((np = strdup(logfmt)) == NULL) {
			logm(LOG_ERR, "job_set_logfmt: out of memory");
			goto err;
		}

		free(job->job_logfmt);
		job->job_logfmt = np;
	} else {
		free(job->job_logfmt);
		job->job_logfmt = NULL;
	}

	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_set_logfmt: job_updated failed");
		goto err;
	}

	return (0);

err:
	free(np);
	return (-1);
}

int
job_set_logkeep(job, logkeep)
	job_t	*job;
	int	logkeep;
{
	assert(job);

	job->job_logkeep = logkeep;
	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_set_logkeep: job_update failed");
		return (-1);
	}

	return (0);
}

int
job_set_logsize(job, logsize)
	job_t	*job;
	size_t	logsize;
{
	assert(job);

	job->job_logsize = logsize;
	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_set_logsize: job_update failed");
		return (-1);
	}

	return (0);
}

int
job_set_project(job, proj)
	job_t		*job;
	char const	*proj;
{
char	*np = NULL;

	assert(job);

	if (proj && *proj && strcmp(proj, "default")) {
	char		 nssbuf[PROJECT_BUFSZ];

		/*
		 * Make sure the user is actually in the project.
		 */
		if (!inproj(job->job_username, proj, nssbuf, sizeof (nssbuf)))
			goto err;

		if ((np = strdup(proj)) == NULL) {
			logm(LOG_ERR, "job_set_project: out of memory");
			goto err;
		}

		free(job->job_project);
		job->job_project = np;
	} else {
		free(job->job_project);
		job->job_project = NULL;
	}

	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_set_project: job_update failed");
		goto err;
	}

	return (0);

err:
	free(np);
	return (-1);
}

/*ARGSUSED*/
static int
_njobs_for_user_callback(job, udata)
	job_t	*job;
	void	*udata;
{
	(*(int *)udata)++;
	return (0);
}

int
njobs_for_user(username)
	char const	*username;
{
int	n = 0;
	if (job_enumerate_user(username, _njobs_for_user_callback, &n) == -1)
		return (-1);
	return (n);
}

int
valid_fmri(fmri)
	char const	*fmri;
{
char const	*p;

	if (strncmp(fmri, "job:/", 5))
		return (0);

	fmri += 5;
	for (p = fmri; *p; ++p) {
		if (*p == '/') {
			if (p == fmri)
				return (0);

			if (*(p + 1) == '/')
				return (0);
		}

		if (!isalnum(*p) && !index("-_/", *p))
			return (0);
	}

	if (*(p - 1) == '/')
		return (0);

	return (1);
}

time_t
get_boottime(new)
	time_t	new;
{
time_t		*old;
size_t		 sz;
time_t		 t;

	if (kvtable_get(table_config, "last_boottime",
	    (char **)&old, &sz) == -1) {
		if (errno != ENOENT) {
			logm(LOG_ERR, "get_boottime: db get failed: %s",
			    strerror(errno));
			free(old);
			return (-1);
		}

		t = 0;
	} else {
		if (sz != sizeof (*old)) {
			logm(LOG_ERR, "get_boottime: wrong size %d expected %d",
			    sz, sizeof (*old));
			free(old);
			return (-1);
		}

		t = *old;
		free(old);
	}

	if (kvtable_replace(table_config, "last_boottime",
	    (char *)&new, sizeof (new)) == -1) {
		logm(LOG_ERR, "get_boottime: db put failed: %s",
		    strerror(errno));
		return (-1);
	}

	return (t);
}

int
job_access(job, username, access)
	job_t		*job;
	char const	*username;
	int		 access;
{
	/*
	 * Currently, we just allow users access to their own jobs and disallow
	 * access to all other users.
	 */
	if (strcmp(job->job_username, username) == 0)
		return (1);

	/* Should support ACLs here... */
	return (0);
}
