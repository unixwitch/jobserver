/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<sys/types.h>

#include	<fcntl.h>
#include	<assert.h>
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>
#include	<errno.h>
#include	<ucred.h>
#include	<xti.h>
#include	<strings.h>
#include	<pwd.h>
#include	<auth_attr.h>
#include	<secdb.h>
#include	<ctype.h>

#include	"fd.h"
#include	"ctl.h"
#include	"jobserver.h"
#include	"state.h"
#include	"sched.h"
#include	"queue.h"

#define	PROTOCOL_VERSION 1
#define	ADMIN_AUTH_NAME "solaris.jobs.admin"
#define	USER_AUTH_NAME "solaris.jobs.user"

typedef enum {
	ANY_STATE = -1,
	WAIT_HELO = 0,
	RUNNING,
	DEAD
} ctl_state_t;

typedef struct ctl_client {
	int		 cc_fd;
	uid_t		 cc_uid;
	char		*cc_name;
	ctl_state_t	 cc_state;
	int		 cc_admin;
	int		 cc_user;
	LIST_ENTRY(ctl_client) cc_entries;
} ctl_client_t;
static LIST_HEAD(client_list, ctl_client) clients;

static void	c_helo(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_quit(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_create(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_delete(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_list(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_set_property(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_schedule(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_unschedule(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_stat(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_clear(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_list_rctls(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_set_config(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_get_config(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_start(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_stop(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_enable(ctl_client_t *, job_t *job, nvlist_t *);
static void	c_disable(ctl_client_t *, job_t *job, nvlist_t *);

static void ctl_client_accept(int, fde_evt_type_t, void *);
static void ctl_close(ctl_client_t *);
static int ctl_send(ctl_client_t *, ...);
static int ctl_send_nvlist(ctl_client_t *, nvlist_t *);
static void ctl_readnv(int, nvlist_t *, void *);
#define ctl_error(c, e) ctl_send((c), "error", DATA_TYPE_STRING, e, NULL)
#define ctl_message(c, m) ctl_send((c), "message", DATA_TYPE_STRING, m, NULL)

#define CMD_F_FMRI	0x001
#define CMD_F_MASK	0x0FF
#define CMD_J_VIEW	0x100
#define CMD_J_DELETE	0x200
#define CMD_J_STARTSTOP	0x400
#define CMD_J_MODIFY	0x800
#define CMD_J_MASK	0xF00

static struct command {
	char const	*cmd;
	ctl_state_t	 state;
	void (*handler) (ctl_client_t *, job_t *, nvlist_t *);
	int		 flags;
} commands[] = {
	{ "helo",	WAIT_HELO, c_helo, 0 },
	{ "quit",	ANY_STATE, c_quit, 0 },
	{ "create",	RUNNING, c_create, 0 },
	{ "delete",	RUNNING, c_delete, CMD_F_FMRI | CMD_J_DELETE },
	{ "list",	RUNNING, c_list, 0 },
	{ "set_property", RUNNING, c_set_property, CMD_F_FMRI | CMD_J_MODIFY },
	{ "schedule",	RUNNING, c_schedule, CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "unschedule",	RUNNING, c_unschedule, CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "stat",	RUNNING, c_stat, CMD_F_FMRI | CMD_J_VIEW },
	{ "clear", 	RUNNING, c_clear, CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "list_rctls",	RUNNING, c_list_rctls, CMD_F_FMRI | CMD_J_VIEW },
	{ "get_config",	RUNNING, c_get_config, 0 },
	{ "set_config",	RUNNING, c_set_config, 0 },
	{ "start",	RUNNING, c_start, CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "stop",	RUNNING, c_stop, CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "enable",	RUNNING, c_enable,	CMD_F_FMRI | CMD_J_STARTSTOP },
	{ "disable",	RUNNING, c_disable,	CMD_F_FMRI | CMD_J_STARTSTOP },
};

static ctl_client_t *find_client(int);
static ctl_client_t *new_client(int);

static ctl_client_t *
find_client(fd)
	int	fd;
{
ctl_client_t	*cc;
	LIST_FOREACH(cc, &clients, cc_entries) {
		if (cc->cc_fd == fd)
			return (cc);
	}

	return (NULL);
}

static ctl_client_t *
new_client(fd)
	int	fd;
{
ctl_client_t	*c = NULL;

	assert(fd >= 0);
	assert(find_client(fd) == NULL);

	if ((c = calloc(1, sizeof (*c))) == NULL) {
		logm(LOG_ERR, "new_client: out of memory");
		return (NULL);
	}

	c->cc_fd = fd;
	c->cc_uid = (uid_t)-1;
	c->cc_state = WAIT_HELO;
	LIST_INSERT_HEAD(&clients, c, cc_entries);
	return (c);
}

int
ctl_init()
{
int		fd;
struct t_bind	tbind;
struct t_info	tinfo;

	LIST_INIT(&clients);

	if ((fd = t_open("/dev/ticotsord", O_RDWR, &tinfo)) == -1)
		return (-1);

	bzero(&tbind, sizeof (tbind));
	tbind.addr.maxlen = tbind.addr.len = sizeof ("jobserver") - 1;
	tbind.addr.buf = "jobserver";
	/*
	 * Set qlen to 1 to avoid dealing with XTI TLOOK errors.  In theory
	 * this hurts performance, but it's unlikely that the jobserver will be
	 * handling a high client load.
	 */
	tbind.qlen = 1;

	if (t_bind(fd, &tbind, NULL) < 0)
		return (-1);

	if (fd_open(fd) == -1)
		goto err;

	if (fd_set_nonblocking(fd, 1) == -1)
		goto err;

	if (fd_set_cloexec(fd, 1) == -1)
		goto err;

	if (register_fd(fd, FDE_READ, ctl_client_accept, NULL) == -1)
		goto err;

	return (0);

err:
	(void) close(fd);
	return (-1);
}

/*ARGSUSED*/
static void
ctl_client_accept(fd, type, udata)
	int		 fd;
	fde_evt_type_t	 type;
	void		*udata;
{
int		 newfd = -1;
ctl_client_t	*c;
ucred_t		*ucred = NULL;
uid_t		 uid;
struct t_call	*callp;
struct passwd	*pwd;

	assert(type == FDE_READ);

	if ((callp = t_alloc(fd, T_CALL, T_ALL)) == NULL) {
		logm(LOG_ERR, "ctl_client_accept: out of memory");
		goto err;
	}

	if (t_listen(fd, callp) == -1) {
		logm(LOG_ERR, "ctl_client_accept: t_listen: %s",
				strerror(errno));
		goto err;
	}

	if ((newfd = t_open("/dev/ticotsord", O_RDWR, NULL)) == NULL) {
		logm(LOG_ERR, "ctl_client_accept: t_open: %s",
				strerror(errno));
		goto err;
	}

	if (t_bind(newfd, NULL, NULL) == -1) {
		logm(LOG_ERR, "ctl_client_accept: t_bind: %s",
				strerror(errno));
		goto err;
	}

	if (t_accept(fd, newfd, callp) == -1) {
		logm(LOG_ERR, "ctl_client_accept: t_accept: %s",
				strerror(errno));
		goto err;
	}

	t_free(callp, T_CALL);
	callp = NULL;

	if (fd_open(newfd) == -1) {
		logm(LOG_ERR, "ctl_client_accept: fd_open(%d): %s",
				fd, strerror(errno));
		goto err;
	}

	if (fd_set_nonblocking(newfd, 1) == -1) {
		logm(LOG_ERR, "ctl_client_accept: "
		    "fd_set_nonblocking(%d, %d): %s",
		    newfd, 1, strerror(errno));
		goto err;
	}

	if (fd_set_cloexec(newfd, 1) == -1) {
		logm(LOG_ERR, "ctl_client_accept: fd_set_cloexec(%d, %d): %s",
		    newfd, 1, strerror(errno));
		goto err;
	}

	if (getpeerucred(newfd, &ucred) == -1) {
		logm(LOG_ERR, "ctl_client_accept: getpeerucred: %s",
		    strerror(errno));
		goto err;
	}

	if ((uid = ucred_geteuid(ucred)) == -1) {
		logm(LOG_ERR, "ctl_client_accept: ucred_getuid failed: %s",
		    strerror(errno));
		goto err;
	}

	ucred_free(ucred);
	ucred = NULL;

	if ((c = new_client(newfd)) == NULL) {
		logm(LOG_ERR, "ctl_client_accept: new_client: %s",
		    strerror(errno));
		goto err;
	}

	c->cc_uid = uid;

	if ((pwd = getpwuid(uid)) == NULL) {
		logm(LOG_ERR, "ctl_client_accept: uid %ld not found?",
		    (long)uid);
		goto err;
	}

	if (chkauthattr(ADMIN_AUTH_NAME, pwd->pw_name))
		c->cc_admin = 1;
	else if (chkauthattr(USER_AUTH_NAME, pwd->pw_name))
		c->cc_user = 1;

	if ((c->cc_name = strdup(pwd->pw_name)) == NULL) {
		logm(LOG_ERR, "ctl_client_accept: out of memory");
		goto err;
	}

	if (fd_readnvlist(newfd, ctl_readnv, c) == -1) {
		logm(LOG_ERR, "ctl_client_accept: fd_readnvlist(%d): %s",
		    newfd, strerror(errno));
		goto err;
	}

	return;

err:
	if (newfd != -1)
		t_close(newfd);
	if (callp)
		t_free(callp, T_CALL);
	if (ucred)
		ucred_free(ucred);
}

static void
ctl_readnv(fd, nvl, udata)
	int	 fd;
	nvlist_t *nvl;
	void	*udata;
{
char		*cmd;
size_t		 i;
ctl_client_t	*client;
struct command	*cmds = NULL;
job_t		*job = NULL;

	client = udata;
	assert(fd == client->cc_fd);

	if (nvl == NULL) {
		if (errno)
			logm(LOG_DEBUG, "fd=%d client read error: %s",
					fd, strerror(errno));
		ctl_close(client);
		return;
	}

	if (nvlist_lookup_string(nvl, "command", &cmd)) {
		if (ctl_error(client, "No command given") == -1)
			ctl_close(client);
		return;
	}

	for (i = 0; i < sizeof (commands) / sizeof (*commands); ++i) {
		if (strcmp(cmd, commands[i].cmd))
			continue;

		if (commands[i].state != ANY_STATE &&
		    (commands[i].state != client->cc_state)) {
			if (ctl_error(client,
			    "Inappropriate state for that command") == -1)
				ctl_close(client);
			return;
		}

		cmds = &commands[i];
	}

	if (!cmds) {
		if (ctl_error(client, "Unknown command") == -1)
			ctl_close(client);
		return;
	}

	if (cmds->flags & CMD_F_FMRI) {
	char	*fmri;
	int	 action = 0;
		if (nvlist_lookup_string(nvl, "fmri", &fmri)) {
			if (ctl_error(client, "FMRI not specified") == -1)
				ctl_close(client);
			return;
		}

		if ((job = find_job_fmri(fmri)) == NULL) {
			if (ctl_error(client, "Job not found") == -1)
				ctl_close(client);
			return;
		}

		if (!client->cc_admin) {
			if ((cmds->flags & CMD_J_MASK) & CMD_J_VIEW)
				action |= JOB_VIEW;
			if ((cmds->flags & CMD_J_MASK) & CMD_J_MODIFY)
				action |= JOB_MODIFY;
			if ((cmds->flags & CMD_J_MASK) & CMD_J_STARTSTOP)
				action |= JOB_STARTSTOP;
			if ((cmds->flags & CMD_J_MASK) & CMD_J_DELETE)
				action |= JOB_DELETE;
			if (!job_access(job, client->cc_name, action)) {
				if (ctl_error(client, "Permission denied") == -1)
					ctl_close(client);
				return;
			}
		}
	}

	cmds->handler(client, job, nvl);
	if (client->cc_state == DEAD)
		ctl_close(client);
}

void
c_helo(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
int16_t	version;
	(void) job;
	if (nvlist_lookup_int16(args, "version", &version)) {
		(void) ctl_error(client, "No version specified");
		return;
	}

	if (version != PROTOCOL_VERSION) {
		(void) ctl_error(client, "Unsupported protocol version");
		return;
	}

	if (!client->cc_user && !client->cc_admin) {
		(void) ctl_error(client, "Permission denied");
		client->cc_state = DEAD;
		return;
	}

	(void) ctl_message(client, "OK");
	client->cc_state = RUNNING;
}

void
c_quit(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) job;
	(void) args;
	(void) ctl_message(client, "Bye");
	client->cc_state = DEAD;
}

/*
 * Create a new job and return its id.
 */
void
c_create(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
char 	*name;
char 	*p;
int	 quota;

	if (nvlist_lookup_string(args, "name", &name)) {
		(void) ctl_error(client, "No name specified");
		return;
	}

	/*
	 * Ensure the job name is valid.  / is allowed, except not at the start
	 * or end; double // is not allowed; any characters other than
	 * [a-zA-Z0-9_-] is not allowed.
	 */

	for (p = name; *p; ++p) {
		if (*p == '/') {
			if (p == name) {
				(void) ctl_error(client,
				    "Job name cannot start with a /.");
				return;
			}

			if (*(p + 1) == '/') {
				(void) ctl_error(client,
				    "Job name cannot contain //.\r\n");
				return;
			}
		}

		if (!isalnum(*p) && strchr("-_/", *p) == NULL) {
			(void) ctl_error(client,
			    "Job name contains invalid characters.");
			return;
		}
	}

	if (*(p - 1) == '/') {
		(void) ctl_error(client,
		    "Job name cannot end with a /.");
		return;
	}

	if (quota = quota_get_jobs_per_user()) {
		if (njobs_for_user(client->cc_name) >= quota) {
			(void) ctl_error(client,
			    "Job quota exceeded.");
			return;
		}
	}

	if ((job = create_job(client->cc_name, name)) == NULL) {
		(void) ctl_error(client,
		    "Failed to allocate new job.");
		return;
	}

	(void) ctl_send(client,
	    "fmri", DATA_TYPE_STRING, job->job_fmri,
	    NULL);
}

void
c_delete(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (sched_get_state(job) != SJOB_STOPPED) {
		(void) ctl_error(client,
		    "Cannot delete a running job.");
		return;
	}

	if (delete_job(job) != 0)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

static int
ctl_send_nvlist(client, nvl)
	ctl_client_t	*client;
	nvlist_t	*nvl;
{
int	ret = 0;
	if ((ret = fd_write_nvlist(client->cc_fd, nvl, NV_ENCODE_XDR)) == -1)
		client->cc_state = DEAD;
	return ret;
}

static int
ctl_send(ctl_client_t *client, ...)
{
va_list		 ap;
int		 ret = 0;
nvlist_t	*nvl;

	if (client->cc_state == DEAD)
		return (-1);

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0))
		return -1;

	va_start(ap, client);
	for (;;) {
	char const	*name;

		if ((name = va_arg(ap, char const *)) == NULL)
			break;

		switch (va_arg(ap, data_type_t)) {
		case DATA_TYPE_STRING:
			nvlist_add_string(nvl, name, va_arg(ap, char const *));
			break;
		default:
			abort();
		}
	}
	va_end(ap);

	if ((fd_write_nvlist(client->cc_fd, nvl, NV_ENCODE_XDR)) == -1)
		ret = -1, client->cc_state = DEAD;
	
	nvlist_free(nvl);
	return (ret);

err:
	nvlist_free(nvl);
	return -1;
}

struct list_callback {
	ctl_client_t *client;
	nvlist_t **jobs;
	size_t njobs;
};

int
list_callback(job, udata)
	job_t	*job;
	void	*udata;
{
struct list_callback *data = udata;
char const	*state, *rstate;
nvlist_t	**nj;
nvlist_t	*nvl;
	if (!data->client->cc_admin && !job_access(job, data->client->cc_name, JOB_VIEW))
		return (0);

	if (job->job_flags & JOB_SCHEDULED) {
		if (job->job_flags & JOB_ENABLED)
			state = "scheduled/enabled";
		else
			state = "scheduled/disabled";
	} else if (job->job_flags & JOB_ENABLED)
		state = "enabled";
	else
		state = "disabled";

	if (job->job_flags & JOB_MAINTENANCE)
		rstate = "maintenance";
	else {
		switch (sched_get_state(job)) {
		case SJOB_RUNNING:
			rstate = "running";
			break;
		case SJOB_STOPPING:
			rstate = "stopping";
			break;
		case SJOB_STOPPED:
			rstate = "stopped";
			break;
		default:
		case SJOB_UNKNOWN:
			rstate = "unknown";
			break;
		}
	}

	if ((nj = realloc(data->jobs, sizeof(nvlist_t *) * (data->njobs + 1))) == NULL)
		return 1;
	data->jobs = nj;

	nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0);
	nvlist_add_string(nvl, "fmri", job->job_fmri);
	nvlist_add_string(nvl, "state", state);
	nvlist_add_string(nvl, "rstate", rstate);
	data->jobs[data->njobs] = nvl;
	data->njobs++;
	return (0);
}

void
c_list(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
nvlist_t *resp;
struct list_callback data;

	(void) args;

	bzero(&data, sizeof(data));
	data.client = client;
	if (job_enumerate(list_callback, &data) == -1) {
		logm(LOG_WARNING, "c_list: job_enumerate failed");
		(void) ctl_error(client, "Internal error");
		return;
	}

	nvlist_alloc(&resp, NV_UNIQUE_NAME, 0);
	nvlist_add_nvlist_array(resp, "jobs", data.jobs, data.njobs);
	(void) ctl_send_nvlist(client, resp);
	nvlist_free(resp);
}

void
c_clear(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (!(job->job_flags & JOB_MAINTENANCE)) {
		(void) ctl_error(client,
		    "Job is not in a maintenance state.");
		return;
	}

	if (job_clear_maintenance(job) == -1)
		ctl_error(client, strerror(errno));
	else
		ctl_message(client, "OK");
}

void
c_unschedule(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_error(client, "Job is not scheduled.");
		return;
	}

	if (job_unschedule(job) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

void
c_stop(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (sched_get_state(job) != SJOB_RUNNING) {
		(void) ctl_error(client, "Job is not running");
		return;
	}

	if (sched_stop(job) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

void
c_schedule(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
char	*time;

	if (nvlist_lookup_string(args, "schedule", &time)) {
		(void) ctl_error(client, "No schedule specified");
		return;
	}

	if (job->job_flags & JOB_ENABLED) {
		(void) ctl_error(client, "Cannot schedule an enabled job");
		return;
	}

	if (job_set_schedule(job, time) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

void
c_stat(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
nvlist_t	*njob, *resp;
char		 buf[64];
	(void) args;

	if (nvlist_alloc(&njob, NV_UNIQUE_NAME, 0)) {
		(void) ctl_error(client, "Internal error");
		logm(LOG_ERR, "out of memory");
		return;
	}

	if (job->job_flags & JOB_SCHEDULED) {
		if (job->job_flags & JOB_ENABLED)
			nvlist_add_string(njob, "state", "scheduled/enabled");
		else
			nvlist_add_string(njob, "state", "scheduled/disabled");
	} else if (job->job_flags & JOB_ENABLED)
		nvlist_add_string(njob, "state", "enabled");
	else
		nvlist_add_string(njob, "state", "disabled");

	if (job->job_flags & JOB_MAINTENANCE)
		nvlist_add_string(njob, "rstate", "maintenance");
	else {
		switch (sched_get_state(job)) {
		case SJOB_RUNNING:
			nvlist_add_string(njob, "rstate", "running");
			break;
		case SJOB_STOPPING:
			nvlist_add_string(njob, "rstate", "stopping");
			break;
		case SJOB_STOPPED:
			nvlist_add_string(njob, "rstate", "stopped");
			break;
		default:
		case SJOB_UNKNOWN:
			nvlist_add_string(njob, "rstate", "unknown");
			break;
		}
	}

	nvlist_add_string(njob, "fmri", job->job_fmri);
	nvlist_add_string(njob, "start", job->job_start_method);
	if (job->job_stop_method)
		nvlist_add_string(njob, "stop", job->job_stop_method);
	if (job->job_logfmt)
		nvlist_add_string(njob, "logfmt", job->job_logfmt);
	else
		nvlist_add_string(njob, "logfmt", DEFAULT_LOGFMT);
	nvlist_add_uint64(njob, "logsize", job->job_logsize);
	nvlist_add_uint32(njob, "logkeep", job->job_logkeep);

	buf[0] = 0;
	if (job->job_exit_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_exit_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_exit_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	nvlist_add_string(njob, "exit", buf);

	buf[0] = 0;
	if (job->job_fail_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_fail_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_fail_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	nvlist_add_string(njob, "fail", buf);

	buf[0] = 0;
	if (job->job_crash_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_crash_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_crash_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	nvlist_add_string(njob, "crash", buf);

	if (job->job_project)
		nvlist_add_string(njob, "project", job->job_project);
	else
		nvlist_add_string(njob, "project", "default");

	if (job->job_flags & JOB_SCHEDULED) {
		nvlist_add_string(njob, "schedule",
		    cron_to_string(&job->job_schedule));

		if (job->job_flags & JOB_ENABLED)
			nvlist_add_string(njob, "nextrun",
			    cron_to_string_interval(&job->job_schedule));
	}

	nvlist_alloc(&resp, NV_UNIQUE_NAME, 0);
	nvlist_add_nvlist(resp, "job", njob);
	ctl_send_nvlist(client, resp);
	nvlist_free(resp);
	nvlist_free(njob);
}

/*
 * set_property:
 * Handle setting or unsetting properties on a job.  Each property that can be
 * set has its own function.  c_set_property handles dispatch.
 */
static char const *
set_start_method(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char	*method;
	nvpair_value_string(value, &method);
	if (job_set_start_method(job, method) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_stop_method(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char	*method = NULL;
	if (nvpair_type(value) == DATA_TYPE_BOOLEAN)
		nvpair_value_string(value, &method);
	if (job_set_start_method(job, method) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_fmri(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char		*pfx;
char		*fmri;
char const	*err = NULL;

	nvpair_value_string(value, &fmri);

	if (!valid_fmri(fmri))
		return "Invalid FMRI";

	if (asprintf(&pfx, "job:/%s/", client->cc_name) == -1) {
		logm(LOG_ERR, "out of memory");
		return "Internal error";
	}

	if (strncmp(pfx, fmri, strlen(pfx))) {
		free(pfx);
		return "New FMRI is outside your namespace";
	}

	free(pfx);
	if (job_set_fmri(job, fmri) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_name(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char		*fmri;
char		*name;
char const	*err = NULL;

	nvpair_value_string(value, &name);

	if (asprintf(&fmri, "job:/%s/%s",
	    client->cc_name, name) == -1) {
		logm(LOG_ERR, "out of memory");
		return "Internal error";
	}

	if (!valid_fmri(fmri)) {
		free(fmri);
		return "Invalid name";
	}

	if (job_set_fmri(job, fmri) == -1)
		err = strerror(errno);
	free(fmri);
	return err;
}

static char const *
set_project(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char	*project;
	if (nvpair_type(value) == DATA_TYPE_BOOLEAN)
		project = NULL;
	else
		nvpair_value_string(value, &project);

	if (job_set_project(job, project) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_logfmt(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
char	*fmt;
	if (nvpair_type(value) == DATA_TYPE_BOOLEAN) {
		if (job_set_logfmt(job, NULL) == -1)
			return strerror(errno);
		return NULL;
	}

	nvpair_value_string(value, &fmt);
	if (job_set_logfmt(job, fmt) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_logkeep(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
uint16_t	logkeep;
	nvpair_value_uint16(value, &logkeep);
	if (job_set_logkeep(job, logkeep) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_logsize(client, job, value)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*value;
{
uint64_t	logsize;

	nvpair_value_uint64(value, &logsize);
	if (job_set_logsize(job, logsize) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_exit(client, job, pair)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*pair;
{
char	*v;
int	 action = 0;
char	*value;

	nvpair_value_string(pair, &value);

	for (v = strtok(value, ", "); v;
	    v = strtok(NULL, ", ")) {
		if (strcmp(v, "mail") == 0)
			action |= ST_EXIT_MAIL;
		else if (strcmp(v, "disable") == 0)
			action |= ST_EXIT_DISABLE;
		else if (strcmp(v, "restart") == 0)
			action |= ST_EXIT_RESTART;
		else
			return "Invalid action";
	}

	if ((action & (ST_EXIT_DISABLE | ST_EXIT_RESTART)) == 0)
		return "Either restart or disable must be specified";

	if (job_set_exit_action(job, action) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_fail(client, job, pair)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*pair;
{
char	*v;
int	 action = 0;
char	*value;

	nvpair_value_string(pair, &value);

	for (v = strtok(value, ", "); v;
	    v = strtok(NULL, ", ")) {
		if (strcmp(v, "mail") == 0)
			action |= ST_EXIT_MAIL;
		else if (strcmp(v, "disable") == 0)
			action |= ST_EXIT_DISABLE;
		else if (strcmp(v, "restart") == 0)
			action |= ST_EXIT_RESTART;
		else
			return "Invalid action";
	}

	if ((action & (ST_EXIT_DISABLE | ST_EXIT_RESTART)) == 0)
		return "Either restart or disable must be specified";

	if (job_set_exit_action(job, action) == -1)
		return strerror(errno);
	return NULL;
}

static char const *
set_crash(client, job, pair)
	ctl_client_t	*client;
	job_t		*job;
	nvpair_t	*pair;
{
char	*value;
char	*v;
int	 action = 0;

	nvpair_value_string(pair, &value);

	for (v = strtok(value, ", "); v;
	    v = strtok(NULL, ", ")) {
		if (strcmp(v, "mail") == 0)
			action |= ST_EXIT_MAIL;
		else if (strcmp(v, "disable") == 0)
			action |= ST_EXIT_DISABLE;
		else if (strcmp(v, "restart") == 0)
			action |= ST_EXIT_RESTART;
		else
			return "Invalid action";
	}

	if ((action & (ST_EXIT_DISABLE | ST_EXIT_RESTART)) == 0)
		return "Either restart or disable must be specified";

	if (job_set_exit_action(job, action) == -1)
		return strerror(errno);
	return NULL;
}

struct props {
	char const	*propname;
	data_type_t	 type;
	char const * (*handler) (ctl_client_t *client, job_t *job, nvpair_t *pair);
	int can_unset;
} set_properties[] = {
	{ "start",	DATA_TYPE_STRING,	set_start_method,	0 },
	{ "stop",	DATA_TYPE_STRING,	set_stop_method,	1 },
	{ "fmri",	DATA_TYPE_STRING,	set_fmri,		0 },
	{ "name",	DATA_TYPE_STRING,	set_name,		0 },
	{ "project",	DATA_TYPE_STRING,	set_project,		1 },
	{ "logfmt",	DATA_TYPE_STRING,	set_logfmt,		1 },
	{ "logkeep",	DATA_TYPE_UINT16,	set_logkeep,		0 },
	{ "logsize",	DATA_TYPE_UINT64,	set_logsize,		0 },
	{ "exit",	DATA_TYPE_STRING,	set_exit,		0 },
	{ "fail",	DATA_TYPE_STRING,	set_fail,		0 },
	{ "crash",	DATA_TYPE_STRING,	set_crash,		0 },
};

void
c_set_property(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
nvpair_t	*pair = NULL;
nvlist_t	*opts;
nvlist_t	*resp;
size_t		 i;
char const	*err;

	if (nvlist_lookup_nvlist(args, "opts", &opts)) {
		(void) ctl_error(client, "Invalid request");
		return;
	}

	nvlist_alloc(&resp, NV_UNIQUE_NAME, 0);
	while ((pair = nvlist_next_nvpair(opts, pair)) != NULL) {
		for (i = 0; i < sizeof(set_properties) / sizeof(*set_properties); ++i) {
			if (strcmp(nvpair_name(pair), set_properties[i].propname))
				continue;

			if (nvpair_type(pair) == DATA_TYPE_BOOLEAN_VALUE &&
			    !set_properties[i].can_unset) {
				nvlist_add_string(resp, nvpair_name(pair), "This property cannot be unset");
				goto done;
			}

			if (nvpair_type(pair) != set_properties[i].type &&
			    nvpair_type(pair) != DATA_TYPE_BOOLEAN_VALUE) {
				nvlist_add_string(resp, nvpair_name(pair), "Invalid type");
				goto done;
			}

			if ((err = set_properties[i].handler(client, job, pair)) != NULL)
				nvlist_add_string(resp, nvpair_name(pair), err);
			goto done;
		}

		/* Property not found - see if it's an rctl */
		if (is_valid_rctl(nvpair_name(pair))) {
		uint64_t	value;
			if (nvpair_type(pair) == DATA_TYPE_BOOLEAN_VALUE) {
				if (job_clear_rctl(job, nvpair_name(pair)) == -1)
					nvlist_add_string(resp, nvpair_name(pair), strerror(errno));
			} else {
				if (nvpair_type(pair) != DATA_TYPE_UINT64) {
					nvlist_add_string(resp, nvpair_name(pair), "Invalid type for rctl");
					continue;
				}

				if (nvpair_value_uint64(pair, &value)) {
					nvlist_add_string(resp, nvpair_name(pair), "Internal error");
					continue;
				}

				if (job_set_rctl(job, nvpair_name(pair), value) == -1)
					nvlist_add_string(resp, nvpair_name(pair), strerror(errno));
			}
			continue;
		}

		nvlist_add_string(resp, nvpair_name(pair), "Invalid property");
done:		;
	}

	(void) ctl_send_nvlist(client, resp);
	nvlist_free(resp);
}

static void
ctl_close(client)
	ctl_client_t	*client;
{
	if (unregister_fd(client->cc_fd, FDE_BOTH) == -1)
		logm(LOG_WARNING, "ctl_close: unregister_fd failed");

	free(client->cc_name);
	close_fd(client->cc_fd);

	LIST_REMOVE(client, cc_entries);
	free(client);
}

void
c_list_rctls(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
int		i;
boolean_t	raw;
nvlist_t	*rctls, *resp;

	if (nvlist_lookup_boolean_value(args, "raw", &raw))
		raw = 0;

	nvlist_alloc(&rctls, NV_UNIQUE_NAME, 0);
	nvlist_alloc(&resp, NV_UNIQUE_NAME, 0);
	for (i = 0; i < job->job_nrctls; ++i) {
		if (raw)
			nvlist_add_uint64(rctls,
			    job->job_rctls[i].jr_name,
			    job->job_rctls[i].jr_value);
		else
			nvlist_add_string(rctls,
			    job->job_rctls[i].jr_name,
			    format_rctl(job->job_rctls[i].jr_value,
				get_rctl_type(job->job_rctls[i].jr_name)));
	}

	nvlist_add_nvlist(resp, "rctls", rctls);
	(void) ctl_send_nvlist(client, resp);
	nvlist_free(resp);
	nvlist_free(rctls);
}

static void
c_get_config(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
nvlist_t *opts;
nvpair_t *pair = NULL;
nvlist_t *resp;
	(void) job;

	if (!client->cc_admin) {
		(void) ctl_error(client, "Permission denied");
		return;
	}

	if (nvlist_lookup_nvlist(args, "opts", &opts)) {
		(void) ctl_error(client, "Option not specified");
		return;
	}

	nvlist_alloc(&resp, NV_UNIQUE_NAME, 0);
	while ((pair = nvlist_next_nvpair(opts, pair)) != NULL) {
		if (strcmp(nvpair_name(pair), "jobs-per-user") == 0) {
		int	njobs;
			if ((njobs = quota_get_jobs_per_user()) == -1) {
				(void) ctl_error(client, strerror(errno));
				return;
			} else {
				nvlist_add_uint32(resp, "jobs-per-user", njobs);
			}
		} else {
			(void) ctl_error(client, "Invalid parameter");
		}
	}
	(void) ctl_send_nvlist(client, resp);
	nvlist_free(resp);
}

static void
c_set_config(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
nvpair_t	*pair = NULL;
nvlist_t	*resp;
nvlist_t	*opts;

	(void) job;
	if (!client->cc_admin) {
		(void) ctl_error(client, "Permission denied");
		return;
	}

	if (nvlist_alloc(&resp, NV_UNIQUE_NAME, 0)) {
		(void) ctl_error(client, "Internal error");
		return;
	}

	if (nvlist_lookup_nvlist(args, "opts", &opts)) {
		(void) ctl_error(client, "Invalid request");
		return;
	}

	while ((pair = nvlist_next_nvpair(opts, pair)) != NULL) {
		if (strcmp(nvpair_name(pair), "jobs-per-user") == 0) {
		uint32_t	njobs;
			if (nvlist_lookup_uint32(args, "jobs-per-user", &njobs)) {
				nvlist_add_string(resp, "jobs-per-user", "Invalid type");
			if (quota_set_jobs_per_user((int) njobs) == -1)
				nvlist_add_string(resp, "jobs-per-user", strerror(errno));
			}
		}
	} 

	(void) ctl_send_nvlist(client, resp);
	nvlist_free(resp);
}

void
c_start(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_error(client, "Only scheduled jobs can be "
		    "manually started");
		return;
	}

	if (sched_start(job) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

static void
c_enable(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (job_enable(job) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}

static void
c_disable(client, job, args)
	ctl_client_t	*client;
	job_t		*job;
	nvlist_t	*args;
{
	(void) args;
	if (job_disable(job) == -1)
		(void) ctl_error(client, strerror(errno));
	else
		(void) ctl_message(client, "OK");
}
