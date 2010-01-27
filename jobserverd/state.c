/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<sys/stat.h>
#include	<errno.h>
#include	<string.h>
#include	<assert.h>
#include	<db.h>
#include	<stdlib.h>
#include	<strings.h>
#include	<libnvpair.h>
#include	<alloca.h>
#include	<rctl.h>
#include	<project.h>
#include	<pwd.h>
#include	<ctype.h>

#include	"jobserver.h"
#include	"state.h"
#include	"sched.h"

#define DB_PATH "/var/jobserver"

static DB_ENV	*env;
static DB	*db_job;
static DB	*db_config;

static int job_update(job_t *);

int
statedb_init()
{
int		err;
struct stat	sb;
	assert(env == NULL);

	if (stat(DB_PATH, &sb) == -1) {
		if (errno != ENOENT) {
			logm(LOG_ERR, "statedb_init: cannot access database %s: %s",
					DB_PATH, strerror(errno));
			return -1;
		}

		if (mkdir(DB_PATH, 0700) == -1) {
			logm(LOG_ERR, "statedb_init: cannot create database %s: %s",
					DB_PATH, strerror(errno));
			return -1;
		}

		logm(LOG_NOTICE, "created directory %s", DB_PATH);
	}

	if (stat(DB_PATH "/db", &sb) == -1) {
		if (errno != ENOENT) {
			logm(LOG_ERR, "statedb_init: cannot access database %s: %s",
					DB_PATH "/db", strerror(errno));
			return -1;
		}

		if (mkdir(DB_PATH "/db", 0700) == -1) {
			logm(LOG_ERR, "statedb_init: cannot create database %s: %s",
					DB_PATH "/db", strerror(errno));
			return -1;
		}

		logm(LOG_NOTICE, "created directory %s", DB_PATH "/db");
	}

	if ((err = db_env_create(&env, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: cannot create environment: %s",
				db_strerror(err));
		env = NULL;
		return -1;
	}

	if ((err = env->open(env, DB_PATH "/db",
				DB_CREATE | DB_INIT_TXN | DB_INIT_LOG | DB_INIT_MPOOL, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: cannot open environment: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_create(&db_job, env, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: db_create failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->open(db_job, NULL, "job.db", NULL, DB_BTREE, 
					DB_CREATE | DB_AUTO_COMMIT, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: db open failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_create(&db_config, env, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: db_create failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->open(db_config, NULL, "config.db", NULL, DB_BTREE, 
					DB_CREATE | DB_AUTO_COMMIT, 0)) != 0) {
		logm(LOG_ERR, "statedb_init: db open failed: %s",
				db_strerror(err));
		goto err;
	}

	return 0;

err:
	if (db_job != NULL)
		(void) db_job->close(db_job, 0);
	if (db_config != NULL)
		(void) db_config->close(db_config, 0);

	db_job = db_config = NULL;
	(void) env->close(env, 0);
	env = NULL;
	return -1;
}

void
statedb_shutdown()
{
int	err;
	if (db_job) {
		if ((err = db_job->close(db_job, 0)) != 0)
			logm(LOG_ERR, "statedb_shutdown: closing job db: %s",
					db_strerror(err));
		db_job = NULL;
	}

	if (db_config) {
		if ((err = db_config->close(db_config, 0)) != 0)
			logm(LOG_ERR, "statedb_shutdown: closing config db: %s",
					db_strerror(err));
		db_config = NULL;
	}

	if (env) {
		if ((err = env->close(env, 0)) != 0)
			logm(LOG_ERR, "statedb_shutdown: closing environment: %s",
					db_strerror(err));
		env = NULL;
	}
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

DBT		 key, data;
DB_TXN		*txn;
int		 err;
job_id_t	 id;

	assert(env);
	assert(db_config);

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "next_job_id: txn start failed: %s",
				db_strerror(errno));
		return -1;
	}

	bzero(&key, sizeof key);
	bzero(&data, sizeof data);

	key.data = "next_job_id";
	key.size = strlen(key.data);

	data.data = &id;
	data.size = data.ulen = sizeof(id);
	data.flags = DB_DBT_USERMEM;

	if ((err = db_config->get(db_config, txn, &key, &data, 0)) != 0) {
		if (err != DB_NOTFOUND) {
			logm(LOG_ERR, "next_job_id: db get failed: %s",
					db_strerror(err));
			(void) txn->abort(txn);
			return -1;
		}

		/* This is the first call, so insert a new key. */
		id = 1;
		if ((err = db_config->put(db_config, txn, &key, &data, 0)) != 0) {
			logm(LOG_ERR, "next_job_id: db put failed: %s",
					db_strerror(err));
			(void) txn->abort(txn);
			return -1;
		}

		if ((err = txn->commit(txn, 0)) != 0) {
			logm(LOG_ERR, "next_job_id: commit failed: %s",
					db_strerror(err));
			return -1;
		}

		return id - 1;
	}

	/*
	 * Increment the key and put it back.
	 */
	id++;

	if ((err = db_config->put(db_config, txn, &key, &data, 0)) != 0) {
		logm(LOG_ERR, "next_job_id: db put failed: %s",
				db_strerror(err));
		(void) txn->abort(txn);
		return -1;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		logm(LOG_ERR, "next_job_id: commit failed: %s",
				db_strerror(err));
		return -1;
	}

	return id - 1;
}

int 
quota_get_jobs_per_user()
{
DBT		 key, data;
DB_TXN		*txn;
int		 err;
int		 njobs;

	assert(env);
	assert(db_config);

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "quota_get_njobs_per_user: txn start failed: %s",
				db_strerror(errno));
		return -1;
	}

	bzero(&key, sizeof key);
	bzero(&data, sizeof data);

	key.data = "quota_jobs_per_user";
	key.size = strlen(key.data);

	data.data = &njobs;
	data.size = data.ulen = sizeof(njobs);
	data.flags = DB_DBT_USERMEM;

	if ((err = db_config->get(db_config, txn, &key, &data, 0)) != 0) {
		(void) txn->abort(txn);

		if (err != DB_NOTFOUND) {
			logm(LOG_ERR, "quota_get_jobs_per_user: db get failed: %s",
					db_strerror(err));
			return -1;
		}

		return 0;
	}

	(void) txn->abort(txn);
	return njobs;
}

int 
quota_set_jobs_per_user(n)
	int	n;
{
DBT		 key, data;
DB_TXN		*txn;
int		 err;

	assert(env);
	assert(db_config);

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "quota_set_njobs_per_user: txn start failed: %s",
				db_strerror(errno));
		return -1;
	}

	bzero(&key, sizeof key);
	bzero(&data, sizeof data);

	key.data = "quota_jobs_per_user";
	key.size = strlen(key.data);

	data.data = &n;
	data.size = data.ulen = sizeof(n);
	data.flags = DB_DBT_USERMEM;

	if ((err = db_config->put(db_config, txn, &key, &data, 0)) != 0) {
		(void) txn->abort(txn);

		logm(LOG_ERR, "quota_set_jobs_per_user: db put failed: %s",
				db_strerror(err));
		return -1;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		(void) txn->abort(txn);

		logm(LOG_ERR, "quota_set_jobs_per_user: commit failed: %s",
				db_strerror(err));
		return -1;
	}

	return 0;
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

	if ((job = calloc(1, sizeof *job)) == NULL) {
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

	if (job_update(job) == -1)
		goto err;

	return job;

err:
	free(fmri);
	free_job(job);
	return NULL;
}

int
unserialise_job(job, buf, sz)
	job_t	**job;
	size_t	  sz;
	char	 *buf;
{
nvlist_t	*nvl = NULL;
int32_t		 ct, ca1, ca2, ctid;
char		*start = NULL, *stop = NULL, *proj = NULL, *fmri = NULL, *username;
uchar_t		*rctls;
uint_t		 nrctls;

	if (nvlist_unpack(buf, sz, &nvl, 0)) {
		logm(LOG_ERR, "unserialise_job: cannot unserialise: %s",
				strerror(errno));
		goto err;
	}

	if ((*job = calloc(1, sizeof(job_t))) == NULL) {
		logm(LOG_ERR, "unserialise_job: out of memory");
		goto err;
	}

	if (	nvlist_lookup_int32(nvl, "id", &(*job)->job_id) ||
		nvlist_lookup_string(nvl, "start", &start) ||
		nvlist_lookup_string(nvl, "stop", &stop) ||
		nvlist_lookup_uint32(nvl, "flags", &(*job)->job_flags) ||
		nvlist_lookup_uint32(nvl, "exit_action", &(*job)->job_exit_action) ||
		nvlist_lookup_uint32(nvl, "crash_action", &(*job)->job_crash_action) ||
		nvlist_lookup_uint32(nvl, "fail_action", &(*job)->job_fail_action) ||
		nvlist_lookup_int32(nvl, "cron_type", &ct) ||
		nvlist_lookup_int32(nvl, "cron_arg1", &ca1) ||
		nvlist_lookup_int32(nvl, "cron_arg2", &ca2)) {

		logm(LOG_ERR, "unserialise_job: cannot unserialise: %s", strerror(errno));
		goto err;
	}

	/*
	 * Pre-FMRI jobs have a name and uid instead of an FMRI.  Check for
	 * this and convert them to new-style jobs.
	 */
	if (nvlist_lookup_string(nvl, "username", &username) == 0 &&
	    nvlist_lookup_string(nvl, "fmri", &fmri) == 0) {
		if (((*job)->job_fmri = strdup(fmri)) == NULL ||
		    ((*job)->job_username = strdup(username)) == NULL) {
			logm(LOG_ERR, "unserialise_job: out of memory");
			goto err;
		}
	} else {
	struct passwd	*pwd;
	int32_t		 uid;
	char		*name;
		if (nvlist_lookup_string(nvl, "name", &name) ||
		    nvlist_lookup_int32(nvl, "user", &uid)) {
			logm(LOG_ERR, "unserialise_job: found neither fmri nor name/uid");
			goto err;
		}

		if ((pwd = getpwuid(uid)) == NULL) {
			logm(LOG_ERR, "unserialise_job: couldn't find user %d", (int) uid);
			goto err;
		}

		if (asprintf(&(*job)->job_fmri, "job:/%s/%s", pwd->pw_name, name) == -1) {
			logm(LOG_ERR, "unserialise_job: out of memory");
			goto err;
		}

		if (((*job)->job_username = strdup(pwd->pw_name)) == NULL) {
			logm(LOG_ERR, "unserialise_job: out of memory");
			goto err;
		}
	}

	if (nvlist_lookup_int32(nvl, "ctid", &ctid) == 0)
		(*job)->job_contract = ctid;
	else
		(*job)->job_contract = -1;

	if (nvlist_lookup_byte_array(nvl, "rctls", &rctls, &nrctls) == 0) {
		/*LINTED*/
		(*job)->job_nrctls = nrctls / sizeof(job_rctl_t);
		if (((*job)->job_rctls = calloc(nrctls, sizeof(job_rctl_t))) == NULL) {
			logm(LOG_ERR, "job_update: out of memory");
			goto err;
		}

		bcopy(rctls, (*job)->job_rctls, sizeof(job_rctl_t) * nrctls);
	} else {
		(*job)->job_nrctls = 0;
	}

	if (nvlist_lookup_string(nvl, "project", &proj) == 0) {
		if (((*job)->job_project = strdup(proj)) == NULL) {
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

	(void) nvlist_free(nvl);
	return 0;

err:
	if (nvl)
		nvlist_free(nvl);

	free_job(*job);
	return -1;
}

job_t *
find_job(id)
	job_id_t	id;
{
DBT		 key, data;
DB_TXN		*txn = NULL;
job_t		*job = NULL;
int		 err;

	bzero(&key, sizeof(key));
	bzero(&data, sizeof(data));

	key.data = &id;
	key.size = sizeof(id);

	data.flags = DB_DBT_MALLOC;

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "find_job: env txn_begin failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->get(db_job, txn, &key, &data, 0)) != 0) {
		if (err != DB_NOTFOUND)
			logm(LOG_ERR, "find_job: db get failed: %s",
					db_strerror(err));
		goto err;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		logm(LOG_ERR, "find_job: txn commit failed: %s",
				db_strerror(err));
		txn = NULL;
		goto err;
	}

	txn = NULL;

	if (unserialise_job(&job, data.data, data.size) == -1)
		goto err;

	return job;

err:
	if (txn)
		(void) txn->abort(txn);
	free_job(job);
	return NULL;
}

struct find_fmri_data {
	char const	*fmri;
	int		 nfound;
	job_id_t	 id;
};

static int
find_job_fmri_callback(job, udata)
	job_t	*job;
	void	*udata;
{
struct find_fmri_data	*data = udata;
char const		*p, *q;

	/*
	 * Check for an exact match.
	 */
	if (!strcmp(data->fmri, job->job_fmri)) {
		data->id = job->job_id;
		data->nfound++;
		return 0;
	}

	/*
	 * Sanity: if the job starts with job:/ and wasn't an exact match,
	 * then it can't exist.
	 */
	if (!strncmp(data->fmri, "job:/", 5))
		return 0;

	/*
	 * If the spec is longer than the FMRI, it can't possibly match.
	 * Subtract 4 because the partial FMRI can't match the "job:".
	 */
	if (strlen(data->fmri) > strlen(job->job_fmri) - 4)
		return 0;

	/*
	 * Start at the end of the string, and match backwards.  Every time
	 * we see a / in one string, it must match a / in the other.
	 */
	p = data->fmri + strlen(data->fmri) - 1;
	q = job->job_fmri + strlen(job->job_fmri) - 1;
	for (; p >= data->fmri && q >= job->job_fmri; --p, --q) {
		if (*p != *q)
			return 0;

		if (*p == '/' && *q != '/')
			return 0;

		if (*q == '/' && *p != '/')
			return 0;
	}

	/*
	 * Ensure the last part matched is a full name, which means there
	 * must be a / before it.
	 */
	if (*q != '/')
		return 0;

	/*
	 * Match!
	 */
	data->id = job->job_id;
	data->nfound++;

	return 0;
}

job_t *
find_job_fmri(fmri)
	char const	*fmri;
{
struct find_fmri_data	data;
	bzero(&data, sizeof(data));
	data.fmri = fmri;

	if (job_enumerate(find_job_fmri_callback, &data) == -1)
		logm(LOG_WARNING, "find_job_fmri: job_enumerate failed");

	if (data.nfound != 1) {
		return NULL;
	}

	return find_job(data.id);
}

int
delete_job(job)
	job_t	*job;
{
DBT	 key;
DB_TXN	*txn = NULL;
int	 err;

	bzero(&key, sizeof(key));

	key.data = &job->job_id;
	key.size = sizeof(job->job_id);

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "delete_job: env txn_begin failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->del(db_job, txn, &key, 0)) != 0) {
		logm(LOG_ERR, "delete_job: db del failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		logm(LOG_ERR, "delete_job: txn commit failed: %s",
				db_strerror(err));
		txn = NULL;
		goto err;
	}

	sched_job_deleted(job);
	return 0;

err:
	if (txn)
		(void) txn->abort(txn);
	return -1;
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

	return ret;
}

int
job_disable(job)
	job_t	*job;
{
int	 ret;

	job->job_flags &= ~JOB_ENABLED;
	ret = job_update(job);

	sched_job_disabled(job);
	return ret;
}

int
job_set_ctid(job, ctid)
	job_t	*job;
	ctid_t	 ctid;
{
int	 ret;

	job->job_contract = ctid;
	ret = job_update(job);

	return ret;
}

int
job_set_exit_action(job, flags)
	job_t	*job;
	int	flags;
{
int	 ret;

	job->job_exit_action = flags;
	ret = job_update(job);

	return ret;
}

int
job_set_crash_action(job, flags)
	job_t	*job;
	int	flags;
{
int	 ret;

	job->job_crash_action = flags;
	ret = job_update(job);

	return ret;
}

int
job_set_fail_action(job, flags)
	job_t	*job;
	int	 flags;
{
int	 ret;

	job->job_fail_action = flags;
	ret = job_update(job);

	return ret;
}

int
job_set_start_method(job, method)
	job_t		*job;
	char const	*method;
{
char	*news;
int	 ret;

	if ((news = strdup(method)) == NULL)
		return -1;

	free(job->job_start_method);
	job->job_start_method = news;

	ret = job_update(job);
	return ret;
}

int
job_set_stop_method(job, method)
	job_t		*job;
	char const	*method;
{
char	*news;
int	 ret;

	if ((news = strdup(method)) == NULL)
		return -1;

	free(job->job_stop_method);
	job->job_stop_method = news;

	ret = job_update(job);
	return ret;
}

int
job_set_fmri(job, fmri)
	job_t		*job;
	char const	*fmri;
{
char	*news;
int	 ret;
job_t	*ejob;

	if ((ejob = find_job_fmri(fmri)) != NULL) {
		free_job(ejob);
		return -1;
	}

	if ((news = strdup(fmri)) == NULL)
		return -1;

	free(job->job_fmri);
	job->job_fmri = news;

	ret = job_update(job);
	return ret;
}

/*
 * Update the job in the database.
 */
static int
job_update(job)
	job_t	*job;
{
size_t		 size = 0;
DBT		 key, data;
DB_TXN		*txn;
char		*xbuf = NULL;
int		 err;
nvlist_t	*nvl = NULL;

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) {
		logm(LOG_ERR, "job_update: nvlist_alloc failed: %s",
				strerror(errno));
		goto err;
	}

	if (	nvlist_add_int32(nvl, "id", job->job_id) != 0 ||
		nvlist_add_string(nvl, "username", job->job_username) != 0 ||
		nvlist_add_string(nvl, "fmri", job->job_fmri) != 0 ||
		nvlist_add_string(nvl, "start", job->job_start_method) != 0 ||
		nvlist_add_string(nvl, "stop", job->job_stop_method) != 0 ||
		nvlist_add_uint32(nvl, "flags", job->job_flags) != 0 ||
		nvlist_add_uint32(nvl, "exit_action", job->job_exit_action) != 0 ||
		nvlist_add_uint32(nvl, "crash_action", job->job_crash_action) != 0 ||
		nvlist_add_uint32(nvl, "fail_action", job->job_fail_action) != 0 ||
		nvlist_add_int32(nvl, "ctid", (int) job->job_contract) != 0 ||
		nvlist_add_int32(nvl, "cron_type", (int32_t) job->job_schedule.cron_type) != 0 ||
		nvlist_add_int32(nvl, "cron_arg1", (int32_t) job->job_schedule.cron_arg1) != 0 ||
		nvlist_add_int32(nvl, "cron_arg2", (int32_t) job->job_schedule.cron_arg2) != 0) {

		logm(LOG_ERR, "job_update: cannot serialise: %s", strerror(errno));
		goto err;
	}

	if (job->job_nrctls && nvlist_add_byte_array(nvl, "rctls", 
		(u_char *) job->job_rctls,
		sizeof(job_rctl_t) * job->job_nrctls) != 0) {

		logm(LOG_ERR, "job_update: cannot serialise: %s", strerror(errno));
		goto err;
	}

	if (job->job_project) {
		if (nvlist_add_string(nvl, "project", job->job_project) != 0) {
			logm(LOG_ERR, "job_update: cannot serialise: %s", strerror(errno));
			goto err;
		}
	}

	if (nvlist_pack(nvl, &xbuf, &size, NV_ENCODE_NATIVE, 0)) {
		logm(LOG_ERR, "job_update: cannot serialise: %s", strerror(errno));
		goto err;
	}

	bzero(&key, sizeof(key));
	bzero(&data, sizeof(data));

	key.data = &job->job_id;
	key.size = sizeof(job->job_id);

	data.data = xbuf;
	data.size = size;
	data.ulen = size;
	data.flags = DB_DBT_USERMEM;

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "job_update: env txn_begin failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->put(db_job, txn, &key, &data, 0)) != 0) {
		logm(LOG_ERR, "job_update: db put failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		logm(LOG_ERR, "job_update: txn commit failed: %s",
				db_strerror(err));
		txn = NULL;
		goto err;
	}

	nvlist_free(nvl);
	free(xbuf);

	return 0;

err:
	if (txn)
		(void) txn->abort(txn);

	nvlist_free(nvl);
	free(xbuf);
	return -1;
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
	free(job);
}

int
job_enumerate(cb, udata)
	job_enumerate_callback	 cb;
	void			*udata;
{
	return job_enumerate_user(NULL, cb, udata);
}

int
job_enumerate_user(username, cb, udata)
	char const *		 username;
	job_enumerate_callback	 cb;
	void			*udata;
{
DB_TXN	*txn = NULL;
DBC	*curs = NULL;
int	 err;
DBT	 key, data;
job_t	*job = NULL;

	bzero(&key, sizeof(key));
	bzero(&data, sizeof(data));

	key.flags = DB_DBT_REALLOC;
	data.flags = DB_DBT_REALLOC;

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "job_enumerate_user: env txn_begin failed: %s",
				db_strerror(err));
		goto err;
	}

	if ((err = db_job->cursor(db_job, txn, &curs, 0)) != 0) {
		logm(LOG_ERR, "job_enumerate_user: db cursor failed: %s",
				db_strerror(err));
		goto err;
	}

	while ((err = curs->get(curs, &key, &data, DB_NEXT)) == 0) {
	job_t	*job;

		if (unserialise_job(&job, data.data, data.size) == -1)
			continue;

		if (username && strcmp(job->job_username, username)) {
			free_job(job);
			continue;
		}

		if (cb(job, udata)) {
			free_job(job);
			break;
		}

		free_job(job);
	}

	free(key.data);
	free(data.data);
	(void) curs->close(curs);
	(void) txn->commit(txn, 0);
	return 0;

err:
	free(key.data);
	free(data.data);

	if (curs)
		(void) curs->close(curs);

	if (txn)
		(void) txn->abort(txn);

	free_job(job);

	return -1;
}

int
job_unschedule(job)
	job_t	*job;
{
	job->job_flags &= ~JOB_SCHEDULED;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_schedule: warning: job_update failed");

	sched_job_unscheduled(job);

	return 0;
}

int
job_set_maintenance(job, reason)
	job_t		*job;
	char const	*reason;
{
	job->job_flags |= JOB_MAINTENANCE;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_set_maintenance: warning: job_update failed");

	return 0;
}

int
job_clear_maintenance(job)
	job_t	*job;
{
	job->job_flags &= ~JOB_MAINTENANCE;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_clear_maintenance: warning: job_update failed");

	if (job->job_flags & JOB_ENABLED)
		sched_job_enabled(job);

	return 0;
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

	bzero(&cron, sizeof(cron));
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
	} else if (sscanf(sched, "in %d %15s", &i, s) == 1 || 
	           sscanf(sched, "in %d minute", &i) == 1) {
		if (!strcmp(s, "minutes") || !strcmp(s, "minute")) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60);
		} else if (!strcmp(s, "hours") || !strcmp(s, "hour")) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60);
		} else if (!strcmp(s, "days") || !strcmp(s, "day")) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60 * 24);
		} else if (!strcmp(s, "weeks") || !strcmp(s, "week")) {
			cron.cron_type = CRON_ABSOLUTE;
			cron.cron_arg1 = current_time + (i * 60 * 60 * 24 * 7);
		} else {
			errno = EINVAL;
			goto err;
		}
	} else if (sscanf(sched, "at %d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
	struct tm	tm;
		bzero(&tm, sizeof(tm));
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
	job->job_flags |= JOB_SCHEDULED;
	if (job_update(job) == -1)
		logm(LOG_ERR, "job_schedule: warning: job_update failed");

	sched_job_scheduled(job);

	return 0;

err:
	return -1;
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
		(void) strftime(buf + 3, sizeof(buf) - 3, "%Y-%m-%d %H:%M", tm);
		return buf;

	case CRON_EVERY_MINUTE:
		(void) strcpy(buf, "every minute");
		return buf;

	case CRON_EVERY_HOUR:
		(void) snprintf(buf, sizeof buf, "every hour at %02d", a1);
		return buf;

	case CRON_EVERY_DAY:
		min = a1 % 60;
		hr = a1 / 60;
		(void) snprintf(buf, sizeof buf, "every day at %02d:%02d", hr, min);
		return buf;

	case CRON_EVERY_WEEK:
		(void) strcpy(buf, "every ");
		switch (a1) {
		case 0: (void) strlcat(buf, "Sunday", sizeof buf); break;
		case 1: (void) strlcat(buf, "Monday", sizeof buf); break;
		case 2: (void) strlcat(buf, "Tuesday", sizeof buf); break;
		case 3: (void) strlcat(buf, "Wednesday", sizeof buf); break;
		case 4: (void) strlcat(buf, "Thursday", sizeof buf); break;
		case 5: (void) strlcat(buf, "Friday", sizeof buf); break;
		case 6: (void) strlcat(buf, "Saturday", sizeof buf); break;
		}


		min = a2 % 60;
		hr = a2 / 60;
		(void) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
				" at %02d:%02d", hr, min);
		return buf;

	default:
		return "unknown";
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
		return "a very short time";

	if (when > (60*60*24*7)) {
		(void) snprintf(buf + i, sizeof(buf) - i, "%dw", (when / (60 * 60 * 24 * 7)));
		i += strlen(buf + i);
		when %= (60 * 60 * 24 * 7);
	}

	if (when > (60*60*24)) {
		(void) snprintf(buf + i, sizeof(buf) - i, "%dd", (when / (60 * 60 * 24)));
		i += strlen(buf + i);
		when %= (60 * 60 * 24);
	}

	if (when > (60*60)) {
		(void) snprintf(buf + i, sizeof(buf) - i, "%dh", (when / (60 * 60)));
		i += strlen(buf + i);
		when %= (60 * 60);
	}

	if (when > 60) {
		(void) snprintf(buf + i, sizeof(buf) - i, "%dm", (when / 60));
		i += strlen(buf + i);
		when %= 60;
	}

	if (when)
		(void) snprintf(buf + i, sizeof(buf) - i, "%ds", when);

	return buf;
}

int
job_set_lasterr(job, err)
	job_t		*job;
	char const	*err;
{
	return 0;
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

		return job->job_rctls[i].jr_value;
	}

	return -1;
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
		return 0;
	}

	/*
	 * We could allocate nrctls-1 here, but we don't actually know if we'll
	 * be removing an rctl.
	 */
	if ((nr = calloc(sizeof(*nr), job->job_nrctls)) == NULL) {
		logm(LOG_ERR, "job_clear_rctl: out of memory");
		goto err;
	}

	for (i = 0, j = 0, newn = job->job_nrctls; i < job->job_nrctls; ++i) {
		if (strcmp(name, job->job_rctls[i].jr_name) == 0) {
			newn--;
			continue;
		}
		bcopy(&job->job_rctls[i], &nr[j], sizeof(job_rctl_t));
		j++;
	}

	free(job->job_rctls);
	job->job_rctls = nr;
	job->job_nrctls = newn;

	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_clear_rctl: job_update failed");
		goto err;
	}

	return 0;

err:
	free(nr);
	return -1;
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

		return 0;
	}

	if ((nr = xrecalloc(job->job_rctls, job->job_nrctls, job->job_nrctls + 1,
					sizeof(job_rctl_t))) == NULL) {
		logm(LOG_ERR, "job_set_rctl: out of memory");
		goto err;
	}

	(void) strlcpy(nr[job->job_nrctls].jr_name, name, 
			sizeof(nr[job->job_nrctls].jr_name));
	nr[job->job_nrctls].jr_value = value;

	job->job_rctls = nr;
	job->job_nrctls++;
	if (job_update(job) == -1) {
		logm(LOG_ERR, "job_clear_rctl: job_update failed");
		goto err;
	}

	return 0;

err:
	free(nr);
	return -1;
}

int
is_valid_rctl(name)
	char const	*name;
{
rctlblk_t	*blk = alloca(rctlblk_size());
char		 rname[64];
	(void) snprintf(rname, sizeof rname, "process.%s", name);
	if (getrctl(rname, NULL, blk, RCTL_FIRST) == -1)
		return 0;
	return 1;
}

int
get_rctl_type(name)
	char const	*name;
{
rctlblk_t	*blk = alloca(rctlblk_size());
u_int		 fl;
char		 rname[64];
	(void) snprintf(rname, sizeof rname, "process.%s", name);
	if (getrctl(rname, NULL, blk, RCTL_FIRST) == -1)
		return RCTL_GLOBAL_COUNT;
	
	fl = rctlblk_get_global_flags(blk);
	if (fl & RCTL_GLOBAL_BYTES)
		return RCTL_GLOBAL_BYTES;
	if (fl & RCTL_GLOBAL_SECONDS)
		return RCTL_GLOBAL_SECONDS;
	return RCTL_GLOBAL_COUNT;
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

	(void) snprintf(res, sizeof res, "%.2lf%s", d, names[i]);
	return res;
}

static char const *
_format_seconds(n)
	rctl_qty_t	n;
{
char		t[16];
static char	res[64];
	
	bzero(res, sizeof(res));
	if (n >= (60 * 60 * 24 * 7)) {
		/*LINTED*/
		(void) snprintf(t, sizeof t, "%dw", (int) (n / (60 * 60 * 24 * 7)));
		(void) strlcat(res, t, sizeof(res));
		n %= (60 * 60 * 24 * 7);
	}

	if (n >= (60 * 60 * 24)) {
		/*LINTED*/
		(void) snprintf(t, sizeof t, "%dd", (int) (n / (60 * 60 * 24)));
		(void) strlcat(res, t, sizeof(res));
		n %= (60 * 60 * 24);
	}

	if (n >= (60 * 60)) {
		/*LINTED*/
		(void) snprintf(t, sizeof t, "%dh", (int) (n / (60 * 60)));
		(void) strlcat(res, t, sizeof(res));
		n %= (60 * 60);
	}

	if (n >= 60) {
		/*LINTED*/
		(void) snprintf(t, sizeof t, "%dm", (int) (n / 60));
		(void) strlcat(res, t, sizeof(res));
		n %= 60;
	}

	if (n > 0 || !strlen(res)) {
		/*LINTED*/
		(void) snprintf(t, sizeof t, "%ds", (int) n);
		(void) strlcat(res, t, sizeof(res));
	}
	return res;
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

	(void) snprintf(res, sizeof res, "%.2lf%c", d, names[i]);
	return res;
}

char const *
format_rctl(qty, type)
	rctl_qty_t	qty;
	int		type;
{
	switch (type) {
	case RCTL_GLOBAL_BYTES:
		return _format_bytes(qty);
	case RCTL_GLOBAL_SECONDS:
		return _format_seconds(qty);
	case RCTL_GLOBAL_COUNT:
	default:
		return _format_count(qty);
	}
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
		if (!inproj(job->job_username, proj, nssbuf, sizeof(nssbuf)))
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
		logm(LOG_ERR, "job_set_project: job_updated failed");
		goto err;
	}

	return 0;

err:
	free(np);
	return -1;
}

/*ARGSUSED*/
static int
_njobs_for_user_callback(job, udata)
	job_t	*job;
	void	*udata;
{
	(*(int *)udata)++;
	return 0;
}

int
njobs_for_user(username)
	char const	*username;
{
int	n = 0;
	if (job_enumerate_user(username, _njobs_for_user_callback, &n) == -1)
		return -1;
	return n;
}

int
valid_fmri(fmri)
	char const	*fmri;
{
char const	*p;

	if (strncmp(fmri, "job:/", 5))
		return 0;

	fmri += 5;
	for (p = fmri; *p; ++p) {
		if (*p == '/') {
			if (p == fmri)
				return 0;

			if (*(p + 1) == '/')
				return 0;
		}

		if (!isalnum(*p) && !index("-_/", *p))
			return 0;
	}

	if (*(p - 1) == '/')
		return 0;

	return 1;
}

time_t
get_boottime(new)
	time_t	new;
{
time_t		 old = 0;
DBT		 key, data;
DB_TXN		*txn;
int		 err;

	assert(env);
	assert(db_config);

	if ((err = env->txn_begin(env, NULL, &txn, 0)) != 0) {
		logm(LOG_ERR, "get_boottime: txn start failed: %s",
				db_strerror(errno));
		return -1;
	}

	bzero(&key, sizeof key);
	bzero(&data, sizeof data);

	key.data = "last_boottime";
	key.size = strlen(key.data);

	data.data = &old;
	data.size = data.ulen = sizeof(old);
	data.flags = DB_DBT_USERMEM;

	if ((err = db_config->get(db_config, txn, &key, &data, 0)) != 0) {
		if (err != DB_NOTFOUND) {
			logm(LOG_ERR, "get_boottime: db get failed: %s",
					db_strerror(err));
			(void) txn->abort(txn);
			return -1;
		}
	}

	data.data = &new;

	if ((err = db_config->put(db_config, txn, &key, &data, 0)) != 0) {
		logm(LOG_ERR, "get_boottime: db put failed: %s",
				db_strerror(err));
		(void) txn->abort(txn);
		return -1;
	}

	if ((err = txn->commit(txn, 0)) != 0) {
		logm(LOG_ERR, "get_boottime: commit failed: %s",
				db_strerror(err));
		return -1;
	}

	return old;
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
	if (!strcmp(job->job_username, username))
		return 1;

	/* Should support ACLs here... */
	return 0;
}

