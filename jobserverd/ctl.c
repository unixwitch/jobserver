/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

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

#include	"fd.h"
#include	"ctl.h"
#include	"jobserver.h"
#include	"state.h"
#include	"sched.h"

#define PROTOCOL_VERSION 1
#define ADMIN_AUTH_NAME "solaris.jobs.admin"
#define USER_AUTH_NAME "solaris.jobs.user"

typedef enum {
	ANY_STATE = -1,
	WAIT_HELO = 0,
	RUNNING,
	DEAD
} ctl_state_t;

typedef struct ctl_client {
	int		cc_fd;
	uid_t		cc_uid;
	ctl_state_t	cc_state;
	int		cc_admin;
	int		cc_user;
} ctl_client_t;

void	c_helo(ctl_client_t *, char *);
void	c_quit(ctl_client_t *, char *);
void	c_crte(ctl_client_t *, char *);
void	c_dele(ctl_client_t *, char *);
void	c_list(ctl_client_t *, char *);
void	c_chng(ctl_client_t *, char *);
void	c_schd(ctl_client_t *, char *);
void	c_ushd(ctl_client_t *, char *);
void	c_stat(ctl_client_t *, char *);
void	c_clea(ctl_client_t *, char *);
void	c_setr(ctl_client_t *, char *);
void	c_getr(ctl_client_t *, char *);
void	c_lisr(ctl_client_t *, char *);
void	c_clrr(ctl_client_t *, char *);
void	c_conf(ctl_client_t *, char *);
void	c_strt(ctl_client_t *, char *);
void	c_uset(ctl_client_t *, char *);

static void ctl_client_accept(int, fde_evt_type_t, void *);
static void ctl_readline(int, char *, size_t, void *);
static void ctl_close(ctl_client_t *);
/*PRINTFLIKE2*/
static int ctl_printf(ctl_client_t *, char const *, ...);

static struct {
	char const	*cmd;
	ctl_state_t	 state;
	void (*handler) (ctl_client_t *, char *line);
} commands[] = {
	{ "HELO",	WAIT_HELO,	c_helo },
	{ "QUIT",	ANY_STATE,	c_quit },
	{ "CRTE",	RUNNING,	c_crte },	/* Create	*/
	{ "DELE",	RUNNING,	c_dele },	/* Delete	*/
	{ "LIST",	RUNNING,	c_list },
	{ "CHNG",	RUNNING,	c_chng },	/* Change	*/
	{ "SCHD",	RUNNING,	c_schd },	/* Schedule	*/
	{ "USHD",	RUNNING,	c_ushd },	/* Unschedule	*/
	{ "STAT",	RUNNING,	c_stat },
	{ "CLEA",	RUNNING,	c_clea },	/* Clear	*/
	{ "SETR",	RUNNING,	c_setr },	/* Set rctl	*/
	{ "GETR",	RUNNING,	c_getr },	/* Get rctl	*/
	{ "LISR",	RUNNING,	c_lisr },	/* List rctls	*/
	{ "CLRR",	RUNNING,	c_clrr },	/* Clear rctl	*/
	{ "CONF",	RUNNING,	c_conf },	/* Configure	*/
	{ "STRT",	RUNNING,	c_strt },	/* Start	*/
	{ "USET",	RUNNING,	c_uset },	/* Unset	*/
};

static ctl_client_t *clients;
static int nclients;

static ctl_client_t *find_client(int);
static ctl_client_t *new_client(int);

/*
 * Takes the first word from 'p', returns the old value
 * of p, and advances p to the start of the next word.
 */
static char *
next_word(str)
	char	**str;
{
char	*ret;

	while (**str && **str == ' ')
		++(*str);

	if (**str == 0)
		return NULL;

	ret = *str;

	if (*ret == ':') {
		*str += strlen(*str);
		return ret + 1;
	}

	while (**str && **str != ' ')
		++(*str);

	if (**str) {
		**str = 0;
		++(*str);
	}

	return ret;
}

static ctl_client_t *
find_client(fd)
	int	fd;
{
int	i;
	for (i = 0; i < nclients; ++i)
		if (clients[i].cc_fd == fd)
			return &clients[i];
	return NULL;
}

static ctl_client_t *
new_client(fd)
	int	fd;
{
int		 i;
ctl_client_t	*c = NULL;

	assert(fd >= 0);
	assert(find_client(fd) == NULL);

	for (i = 0; i < nclients; ++i) {
		if (clients[i].cc_fd == -1) {
			c = &clients[i];
			break;
		}
	}

	if (c == NULL) {
	ctl_client_t	*ncs;
		/*LINTED*/
		if ((ncs = realloc(clients, sizeof(*clients) * (nclients + 1))) == NULL) {
			logm(LOG_ERR, "new_client: out of memory");
			return NULL;
		}

		clients = ncs;
		c = &clients[nclients];
		nclients++;
	}

	bzero(c, sizeof(*c));

	c->cc_fd = fd;
	c->cc_uid = (uid_t) -1;
	c->cc_state = WAIT_HELO;
	return c;
}

int
ctl_init()
{
int		fd;
struct t_bind	tbind;
struct t_info	tinfo;

	if ((fd = t_open("/dev/ticotsord", O_RDWR, &tinfo)) == -1)
		return -1;

	bzero(&tbind, sizeof(tbind));
	tbind.addr.maxlen = tbind.addr.len = sizeof("jobserver") - 1;
	tbind.addr.buf = "jobserver";
	/*
	 * Set qlen to 1 to avoid dealing with XTI TLOOK errors.  In theory
	 * this hurts performance, but it's unlikely that the jobserver will be
	 * handling a high client load.
	 */
	tbind.qlen = 1;

	if (t_bind(fd, &tbind, NULL) < 0)
		return -1;

	if (fd_open(fd) == -1)
		goto err;

	if (fd_set_nonblocking(fd, 1) == -1)
		goto err;

	if (fd_set_cloexec(fd, 1) == -1)
		goto err;

	if (register_fd(fd, FDE_READ, ctl_client_accept, NULL) == -1)
		goto err;

	return 0;

err:
	(void) close(fd);
	return -1;
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
		logm(LOG_ERR, "ctl_client_accept: fd_set_nonblocking(%d, %d): %s",
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
				(long) uid);
		goto err;
	}

	if (chkauthattr(ADMIN_AUTH_NAME, pwd->pw_name))
		c->cc_admin = 1;
	else if (chkauthattr(USER_AUTH_NAME, pwd->pw_name))
		c->cc_user = 1;

	if (fd_printf(newfd, "200 Jobserver %s at your service.\r\n", VERSION) == -1) {
		logm(LOG_ERR, "ctl_client_accept: fd_printf(%d): %s",
				newfd, strerror(errno));
		goto err;
	}

	if (fd_readline(newfd, ctl_readline, c) == -1) {
		logm(LOG_ERR, "ctl_client_accept: fd_readline(%d): %s",
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
	return;
}

static void
ctl_readline(fd, data, size, udata)
	int	 fd;
	char	*data;
	size_t	 size;
	void	*udata;
{
char		*cmd;
size_t		 i;
ctl_client_t	*client;

	client = udata;
	assert(fd == client->cc_fd);

	if (data == NULL) {
		if (size)
			logm(LOG_DEBUG, "fd=%d client read error: %s",
					/*LINTED*/
					fd, strerror((int) size));
		ctl_close(client);
		return;
	}

	if ((cmd = next_word(&data)) == NULL) {
		if (ctl_printf(client, "500 No command on line.\r\n") == -1)
			ctl_close(client);
		return;
	}

	for (i = 0; i < sizeof commands / sizeof *commands; ++i) {
		if (strcmp(cmd, commands[i].cmd))
			continue;

		if (commands[i].state != ANY_STATE &&
		    (commands[i].state != client->cc_state)) {
			if (ctl_printf(client, "500 %s: Inappropriate state for that command.\r\n", cmd) == -1)
				ctl_close(client);
			return;
		}

		commands[i].handler(client, data);
		return;
	}

	if (ctl_printf(client, "500 %s: Unknown command.\r\n", cmd) == -1)
		ctl_close(client);
}

void
c_helo(client, line)
	ctl_client_t	*client;
	char		*line;
{
char	*version;
	if ((version = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 HELO: Argument expected.\r\n");
		return;
	}

	if (strcmp(version, "1")) {
		(void) ctl_printf(client, "500- HELO: Unsupported protocol version %s.\r\n", version);
		(void) ctl_printf(client, "500 I support protocol version %d.\r\n", PROTOCOL_VERSION);
		return;
	}

	if (!client->cc_user && !client->cc_admin) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		ctl_close(client);
		return;
	}

	(void) ctl_printf(client, "200 Hello (uid=%ld), pleased to meet you.\r\n",
			(long) client->cc_uid);
	client->cc_state = RUNNING;
}

void
c_quit(client, line)
	ctl_client_t	*client;
	char		*line;
{
	if (*line) {
		(void) ctl_printf(client, "500 Too many arguments.\r\n");
		return;
	}

	(void) ctl_printf(client, "200 Call again soon.\r\n");
	ctl_close(client);
}

/*
 * Create a new job and return its id.
 */
void
c_crte(client, line)
	ctl_client_t	*client;
	char		*line;
{
job_t	*job;
char	*name;
char	*p;
int	 quota;

	if ((name = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		return;
	}

	if (*line) {
		(void) ctl_printf(client, "500 Too many arguments.\r\n");
		return;
	}

	if (*name == ':') {
		(void) ctl_printf(client, "500 Job name cannot start with a colon.\r\n");
		return;
	}
	
	for (p = name; *p; ++p) {
		if (*p <= 0x20 || *p == 0x7f) {
			(void) ctl_printf(client, "500 Job name contains illegal characters.\r\n");
			return;
		}
	}

	if (quota = quota_get_jobs_per_user()) {
		if (njobs_for_user(client->cc_uid) >= quota) {
			(void) ctl_printf(client, "500 Job quota exceeded.\r\n");
			return;
		}
	}

	if ((job = create_job(client->cc_uid, name)) == NULL) {
		(void) ctl_printf(client, "500 Failed to allocate new job.\r\n");
		return;
	}

	(void) ctl_printf(client, "200 %ld\r\n", (long) job->job_id);
	free_job(job);
}

void
c_dele(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*ids;
job_id_t	 id;
char		*endp;
job_t		*job = NULL;

	if ((ids = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	if (*line) {
		(void) ctl_printf(client, "500 Too many arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(ids, &endp, 10);
	if (endp != (ids + strlen(ids))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "501 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_DELETE)) {
		(void) ctl_printf(client, "503 Permission denied.\r\n");
		goto err;
	}

	if (sched_get_state(job->job_id) != SJOB_STOPPED) {
		(void) ctl_printf(client, "505 Cannot delete a running job.\r\n");
		goto err;
	}

	if (delete_job(id) != 0)
		(void) ctl_printf(client, "502 Could not delete job.\r\n");
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

static int
ctl_printf(ctl_client_t *client, char const *fmt, ...)
{
va_list	ap;
int	ret = 0;

	if (client->cc_state == DEAD)
		return -1;

	va_start(ap, fmt);
	if ((fd_vprintf(client->cc_fd, fmt, ap)) == -1)
		ret = -1, client->cc_state = DEAD;
	va_end(ap);

	return ret;
}

int
list_callback(job, udata)
	job_t	*job;
	void	*udata;
{
ctl_client_t	*client = udata;
char const	*state, *rstate;

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_VIEW))
		return 0;

	if (job->job_flags & JOB_SCHEDULED)
		state = "scheduled";
	else if (job->job_flags & JOB_MAINTENANCE)
		state = "maintenance";
	else if (job->job_flags & JOB_ENABLED)
		state = "enabled";
	else
		state = "disabled";

	switch (sched_get_state(job->job_id)) {
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

	(void) ctl_printf(client, "201 %ld %ld %s %s %s :%s\r\n",
			(long) job->job_id,
			(long) job->job_user,
			job->job_name ? job->job_name : "<unnamed>",
			state,
			rstate,
			job->job_start_method);
	return 0;
}

void
c_list(client, line)
	ctl_client_t	*client;
	char		*line;
{
	if (*line) {
		(void) ctl_printf(client, "500 Too many arguments.\r\n");
		return;
	}

	(void) ctl_printf(client, "200 Job list follows.\r\n");

	if (job_enumerate(list_callback, client) == -1) {
		logm(LOG_WARNING, "c_list: job_enumerate failed");
		(void) ctl_printf(client, "500 Internal server error.\r\n");
		return;
	}

	(void) ctl_printf(client, "202 End of job list.\r\n");
}

void
c_clea(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*ids, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
	if ((ids = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtoll(ids, &endp, 10);
	if (endp != (ids + strlen(ids))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_STARTSTOP)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (!(job->job_flags & JOB_MAINTENANCE)) {
		(void) ctl_printf(client, "500 Job is not in a maintenance state.\r\n");
		goto err;
	}

	if (job_clear_maintenance(job) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_ushd(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*ids, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
	if ((ids = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(ids, &endp, 10);
	if (endp != (ids + strlen(ids))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_STARTSTOP)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_printf(client, "500 Job is not scheduled.\r\n");
		goto err;
	}

	if (job_unschedule(job) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_schd(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*ids, *time, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
	if ((ids = next_word(&line)) == NULL ||
	    (time = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtoll(ids, &endp, 10);
	if (endp != (ids + strlen(ids))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_STARTSTOP)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (job->job_flags & JOB_ENABLED) {
		(void) ctl_printf(client, "500 Cannot schedule an enabled job.\r\n");
		goto err;
	}

	if (job->job_flags & JOB_MAINTENANCE) {
		(void) ctl_printf(client, "500 Cannot schedule a job in maintenance state.\r\n");
		goto err;
	}

	if (job_set_schedule(job, time) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_stat(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
char const	*state, *rstate;

	if ((arg = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_VIEW)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	(void) ctl_printf(client, "200 Job status follows.\r\n");

	if (job->job_flags & JOB_SCHEDULED)
		state = "scheduled";
	else if (job->job_flags & JOB_MAINTENANCE)
		state = "maintenance";
	else if (job->job_flags & JOB_ENABLED)
		state = "enabled";
	else
		state = "disabled";

	switch (sched_get_state(job->job_id)) {
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

	(void) ctl_printf(client, "201 :%ld\r\n", (long) job->job_id);
	(void) ctl_printf(client, "202 :%ld\r\n", (long) job->job_user);
	(void) ctl_printf(client, "203 :%s\r\n", job->job_name);
	(void) ctl_printf(client, "204 :%s\r\n", state);
	(void) ctl_printf(client, "205 :%s\r\n", rstate);
	(void) ctl_printf(client, "206 :%s\r\n", job->job_start_method);
	(void) ctl_printf(client, "207 :%s\r\n", job->job_stop_method);
	if (job->job_project)
		(void) ctl_printf(client, "209 :%s\r\n", job->job_project);
	else
		(void) ctl_printf(client, "209 :default\r\n");
	if (job->job_flags & JOB_SCHEDULED)
		(void) ctl_printf(client, "208 :%s\r\n", cron_to_string(&job->job_schedule));
	else
		(void) ctl_printf(client, "208 :-\r\n");
	(void) ctl_printf(client, "299 End of dump.\r\n");

err:
	free_job(job);
}

void
c_chng(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;

	if ((arg = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_MODIFY)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	while (arg = next_word(&line)) {
	char	*key = arg, *value;
		if ((value = index(arg, '=')) == NULL) {
			(void) ctl_printf(client, "500 Invalid format.\r\n");
			goto err;
		}

		*value++ = 0;

		if (!strcmp(key, "START")) {
			if (job_set_start_method(job, value) == -1) {
				(void) ctl_printf(client, "500 Could not change start method.\r\n");
				goto err;
			}
		} else if (!strcmp(key, "STOP")) {
			if (job_set_stop_method(job, value) == -1) {
				(void) ctl_printf(client, "500 Could not change stop method.\r\n");
				goto err;
			}
		} else if (!strcmp(key, "NAME")) {
			if (job_set_name(job, value) == -1) {
				(void) ctl_printf(client, "500 Could not change job name.\r\n");
				goto err;
			}
		} else if (!strcmp(key, "PROJECT")) {
			if (job_set_project(job, value) == -1) {
				(void) ctl_printf(client, "500 Could not change project.\r\n");
				goto err;
			}
		} else if (!strcmp(key, "ENABLED")) {
			if (job->job_flags & JOB_SCHEDULED) {
				(void) ctl_printf(client, "500 Cannot %s a scheduled job.\r\n",
						(strcmp(value, "1") ? "disable" : "enable"));
				goto err;
			}

			if (!strcmp(value, "1")) {
				if (job_enable(job) == -1) {
					(void) ctl_printf(client, "500 Could not enable job.\r\n");
					goto err;
				}
			} else if (!strcmp(value, "0")) {
				if (job_disable(job) == -1) {
					(void) ctl_printf(client, "500 Could not disable job.\r\n");
					goto err;
				}
			} else {
				(void) ctl_printf(client, "500 Invalid syntax.\r\n");
				goto err;
			}
		} else {
			(void) ctl_printf(client, "500 Invalid syntax.\r\n");
			goto err;
		}
	}

	(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

static void
ctl_close(client)
	ctl_client_t	*client;
{
	if (unregister_fd(client->cc_fd, FDE_BOTH) == -1)
		logm(LOG_WARNING, "ctl_close: unregister_fd failed");

	close_fd(client->cc_fd);
	client->cc_fd = -1;
}

void
c_getr(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
char		*ctl;
rctl_qty_t	 value;

	if ((arg = next_word(&line)) == NULL ||
	    (ctl = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_MODIFY)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	/*LINTED sign extension*/
	if ((value = job_get_rctl(job, ctl)) == (rctl_qty_t) -1)
		(void) ctl_printf(client, "500 Resource control \"%s\" not set.\r\n", ctl);
	else
		(void) ctl_printf(client, "200 %s\r\n",
			format_rctl(value, get_rctl_type(ctl)));

err:
	free_job(job);
}

void
c_setr(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
char		*ctl, *vs;
u_longlong_t	 value = 0;

	if ((arg = next_word(&line)) == NULL ||
	    (ctl = next_word(&line)) == NULL ||
	    (vs = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	value = strtoull(vs, &endp, 10);
	if (endp != (vs + strlen(vs))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}
	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_MODIFY)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (!is_valid_rctl(ctl)) {
		(void) ctl_printf(client, "500 Invalid resource control.\r\n");
		goto err;
	}

	/*LINTED sign extension*/
	if ((value = job_set_rctl(job, ctl, value)) == -1)
		(void) ctl_printf(client, "500 Resource control \"%s\" not set.\r\n", ctl);
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_lisr(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
int		 i;

	if ((arg = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_VIEW)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	for (i = 0; i < job->job_nrctls; ++i) {
		(void) ctl_printf(client, "200 %s %s\r\n",
				job->job_rctls[i].jr_name,
				format_rctl(job->job_rctls[i].jr_value,
					get_rctl_type(job->job_rctls[i].jr_name)));
	}

	(void) ctl_printf(client, "201 End of resource control list.\r\n");

err:
	free_job(job);
}

void
c_conf(client, line)
	ctl_client_t	*client;
	char		*line;
{
char	*opt, *value;

	if (!client->cc_admin) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		return;
	}

	if ((opt = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		return;
	}

	value = next_word(&line);

	if (!strcmp(opt, "jobs-per-user")) {

		if (value) {
		int	 njobs;
		char	*endp;

			/*LINTED*/
			njobs = strtol(value, &endp, 10);
			if (endp != (value + strlen(value)) || njobs < 0) {
				(void) ctl_printf(client, "500 Invalid format.\r\n");
				return;
			}

			if (quota_set_jobs_per_user(njobs) == -1)
				(void) ctl_printf(client, "500 Cannot set quota.\r\n");
			else
				(void) ctl_printf(client, "200 OK.\r\n");
		} else {
		int	njobs;
			if ((njobs = quota_get_jobs_per_user()) == -1)
				(void) ctl_printf(client, "500 Cannot get quota.\r\n");
			else
				(void) ctl_printf(client, "200 %d\r\n", njobs);
		}

		return;
	} else {
		(void) ctl_printf(client, "500 Invalid parameter.\r\n");
	}
}

void
c_clrr(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;
char		*ctl;

	if ((arg = next_word(&line)) == NULL ||
	    (ctl = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_MODIFY)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (job_clear_rctl(job, ctl) == -1)
		(void) ctl_printf(client, "500 Could not clear rctl.\r\n");
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_strt(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;

	if ((arg = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_STARTSTOP)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_printf(client, "500 Only scheduled jobs can be manually started.\r\n");
		goto err;
	}

	if (sched_start(job->job_id) == -1)
		(void) ctl_printf(client, "500 Could not start job.\r\n");
	else
		(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}

void
c_uset(client, line)
	ctl_client_t	*client;
	char		*line;
{
char		*arg, *endp = NULL;
job_id_t	 id;
job_t		*job = NULL;

	if ((arg = next_word(&line)) == NULL) {
		(void) ctl_printf(client, "500 Not enough arguments.\r\n");
		goto err;
	}

	/*LINTED*/
	id = strtol(arg, &endp, 10);
	if (endp != (arg + strlen(arg))) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		goto err;
	}

	if ((job = find_job(id)) == NULL) {
		(void) ctl_printf(client, "500 No such job.\r\n");
		goto err;
	}

	if (!client->cc_admin && !job_access(job, client->cc_uid, JOB_MODIFY)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		goto err;
	}

	while (arg = next_word(&line)) {
		if (!strcmp(arg, "START") ||
		    !strcmp(arg, "STOP") ||
		    !strcmp(arg, "NAME") ||
		    !strcmp(arg, "ENABLED")) {
			(void) ctl_printf(client, "500 This option cannot be unset.\r\n");
			goto err;
		} else if (!strcmp(arg, "PROJECT")) {
			if (job_set_project(job, NULL) == -1) {
				(void) ctl_printf(client, "500 Could not change project.\r\n");
				goto err;
			}
		} else {
			(void) ctl_printf(client, "500 Invalid syntax.\r\n");
			goto err;
		}
	}

	(void) ctl_printf(client, "200 OK.\r\n");

err:
	free_job(job);
}
