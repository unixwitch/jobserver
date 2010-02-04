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

void	c_helo(ctl_client_t *, void **);
void	c_quit(ctl_client_t *, void **);
void	c_crte(ctl_client_t *, void **);
void	c_dele(ctl_client_t *, void **);
void	c_list(ctl_client_t *, void **);
void	c_chng(ctl_client_t *, void **);
void	c_schd(ctl_client_t *, void **);
void	c_ushd(ctl_client_t *, void **);
void	c_stat(ctl_client_t *, void **);
void	c_clea(ctl_client_t *, void **);
void	c_lisr(ctl_client_t *, void **);
void	c_conf(ctl_client_t *, void **);
void	c_gcnf(ctl_client_t *, void **);
void	c_strt(ctl_client_t *, void **);
void	c_uset(ctl_client_t *, void **);
void	c_stop(ctl_client_t *, void **);

static void ctl_client_accept(int, fde_evt_type_t, void *);
static void ctl_readline(int, char *, size_t, void *);
static void ctl_close(ctl_client_t *);
/*PRINTFLIKE2*/
static int ctl_printf(ctl_client_t *, char const *, ...);

#define	ARG_T_INT	0x1
#define	ARG_T_STR	0x2
#define	ARG_T_FMRI	0x4
#define	ARG_T_REST	0x8
#define	ARG_T_MASK	0xF

#define	ARG_J_VIEW	0x1000
#define	ARG_J_STARTSTOP	0x2000
#define	ARG_J_MODIFY	0x4000
#define	ARG_J_DELETE	0x8000
#define	ARG_J_MASK	0xF000

#define	MAXARGS 16

static struct command {
	char const	*cmd;
	ctl_state_t	 state;
	int		 nargs;
	void (*handler) (ctl_client_t *, void **);
	int		 args[16];
} commands[] = {
	{ "HELO", WAIT_HELO, 1, c_helo, {
		ARG_T_INT
	} },
	{ "QUIT", ANY_STATE, 0, c_quit },
	{ "CRTE",	RUNNING,	1, c_crte, {
		ARG_T_STR
	} },
	{ "DELE", RUNNING, 1, c_dele, {
		(ARG_T_FMRI | ARG_J_DELETE)
	} },
	{ "LIST", RUNNING, 0, c_list },
	{ "CHNG", RUNNING, 2, c_chng, {
		(ARG_T_FMRI | ARG_J_MODIFY),
		ARG_T_STR
	} },
	{ "SCHD", RUNNING, 2, c_schd, {
		(ARG_T_FMRI | ARG_J_STARTSTOP),
		ARG_T_STR
	} },
	{ "USHD", RUNNING, 1, c_ushd, {
		(ARG_T_FMRI | ARG_J_STARTSTOP)
	} },
	{ "STAT", RUNNING, 1, c_stat, {
		(ARG_T_FMRI | ARG_J_VIEW)
	} },
	{ "CLEA", RUNNING, 1, c_clea, {
		(ARG_T_FMRI | ARG_J_STARTSTOP)
	} },
	{ "LISR", RUNNING, 2, c_lisr, {
		(ARG_T_FMRI | ARG_J_VIEW),
		ARG_T_STR
	} },
	{ "GCNF", RUNNING, 0, c_gcnf, {
		ARG_T_STR,
	} },
	{ "CONF", RUNNING, 1, c_conf, {
		ARG_T_STR,
		ARG_T_STR
	} },
	{ "STRT", RUNNING, 1, c_strt, {
		(ARG_T_FMRI | ARG_J_STARTSTOP)
	} },
	{ "USET", RUNNING, 2, c_uset, {
		(ARG_T_FMRI | ARG_J_MODIFY),
		ARG_T_STR
	} },
	{ "STOP", RUNNING, 1, c_stop, {
		(ARG_T_FMRI | ARG_J_STARTSTOP)
	} },
};

static ctl_client_t *find_client(int);
static ctl_client_t *new_client(int);

static char *next_word(char **);
static job_t *next_job(char **, ctl_client_t *client, int action);

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
		return (NULL);

	ret = *str;

	if (*ret == ':') {
		*str += strlen(*str);
		return (ret + 1);
	}

	while (**str && **str != ' ')
		++(*str);

	if (**str) {
		**str = 0;
		++(*str);
	}

	return (ret);
}

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

	if (fd_printf(newfd, "200 Jobserver %s at your service.\r\n",
	    VERSION) == -1) {
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
void		*args[MAXARGS + 1];
int		 j, k = 0;
struct command	*cmds = NULL;

	client = udata;
	assert(fd == client->cc_fd);

	if (data == NULL) {
		if (size)
			logm(LOG_DEBUG, "fd=%d client read error: %s",
					/*LINTED*/
					fd, strerror((int)size));
		ctl_close(client);
		return;
	}

	if ((cmd = next_word(&data)) == NULL) {
		if (ctl_printf(client, "500 No command on line.\r\n") == -1)
			ctl_close(client);
		return;
	}

	for (i = 0; i < sizeof (commands) / sizeof (*commands); ++i) {
		if (strcmp(cmd, commands[i].cmd))
			continue;

		if (commands[i].state != ANY_STATE &&
		    (commands[i].state != client->cc_state)) {
			if (ctl_printf(client, "500 %s: Inappropriate "
			    "state for that command.\r\n", cmd) == -1)
				ctl_close(client);
			return;
		}

		cmds = &commands[i];
	}

	if (!cmds) {
		if (ctl_printf(client, "500 %s: "
		    "Unknown command.\r\n", cmd) == -1)
			ctl_close(client);
		return;
	}

	for (j = 0; j < cmds->nargs; ++j) {
	int		 fl = 0;
	char		*astr;
		switch (cmds->args[j] & ARG_T_MASK) {
		case ARG_T_FMRI:
			if (cmds->args[j] & ARG_J_STARTSTOP)
				fl |= JOB_STARTSTOP;
			if (cmds->args[j] & ARG_J_VIEW)
				fl |= JOB_VIEW;
			if (cmds->args[j] & ARG_J_MODIFY)
				fl |= JOB_MODIFY;
			if (cmds->args[j] & ARG_J_DELETE)
				fl |= JOB_DELETE;
			if ((args[k] = next_job(&data, client, fl)) == NULL)
				goto err;
			break;

		case ARG_T_STR:
			if ((args[k] = next_word(&data)) == NULL) {
				(void) ctl_printf(client,
				    "500 Not enough arguments.\r\n");
				goto err;
			}
			break;

		case ARG_T_INT:
			if ((astr = next_word(&data)) == NULL) {
				(void) ctl_printf(client,
				    "500 Not enough arguments.\r\n");
				goto err;
			}
			if ((args[k] = malloc(sizeof (long long))) == NULL) {
				(void) ctl_printf(client,
				    "500 Internal error.\r\n");
				logm(LOG_ERR, "out of memory");
				goto err;
			}
			*(long long *)args[k] = strtoll(astr, NULL, 10);
			break;

		case ARG_T_REST:
			args[k] = data;
			break;

		default:
			abort();
		}
		k++;
	}

	cmds->handler(client, args);

err:
	for (j = 0; j < cmds->nargs; ++j) {
		switch (cmds->args[j] & ARG_T_MASK) {
		case ARG_T_FMRI:
		case ARG_T_STR:
		case ARG_T_REST:
			break;

		case ARG_T_INT:
			free(args[k]);
			break;

		default:
			abort();
		}
	}
}

void
c_helo(client, args)
	ctl_client_t	*client;
	void		**args;
{
long long	version = *(long long *)args[0];

	if (version != PROTOCOL_VERSION) {
		(void) ctl_printf(client, "500- HELO: "
		    "Unsupported protocol version %lld.\r\n", version);
		(void) ctl_printf(client, "500 "
		    "I support protocol version %d.\r\n", PROTOCOL_VERSION);
		return;
	}

	if (!client->cc_user && !client->cc_admin) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		ctl_close(client);
		return;
	}

	(void) ctl_printf(client, "200 Hello (uid=%ld), "
	    "pleased to meet you.\r\n",
	    (long)client->cc_uid);
	client->cc_state = RUNNING;
}

void
c_quit(client, args)
	ctl_client_t	*client;
	void		**args;
{
	(void) ctl_printf(client, "200 Call again soon.\r\n");
	ctl_close(client);
}

/*
 * Create a new job and return its id.
 */
void
c_crte(client, args)
	ctl_client_t	*client;
	void		**args;
{
char const	*name = args[0];
job_t		*job;
char const	*p;
int		 quota;

	/*
	 * Ensure the job name is valid.  / is allowed, except not at the start
	 * or end; double // is not allowed; any characters other than
	 * [a-zA-Z0-9_-] is not allowed.
	 */

	for (p = name; *p; ++p) {
		if (*p == '/') {
			if (p == name) {
				(void) ctl_printf(client, "500 "
				    "Job name cannot start with a /.\r\n");
				return;
			}

			if (*(p + 1) == '/') {
				(void) ctl_printf(client, "500 "
				    "Job name cannot contain //.\r\n");
				return;
			}
		}

		if (!isalnum(*p) && strchr("-_/", *p) == NULL) {
			(void) ctl_printf(client, "500 "
			    "Job name contains invalid characters.\r\n");
			return;
		}
	}

	if (*(p - 1) == '/') {
		(void) ctl_printf(client, "500 "
		    "Job name cannot end with a /.\r\n");
		return;
	}

	if (quota = quota_get_jobs_per_user()) {
		if (njobs_for_user(client->cc_name) >= quota) {
			(void) ctl_printf(client, "500 "
			    "Job quota exceeded.\r\n");
			return;
		}
	}

	if ((job = create_job(client->cc_name, name)) == NULL) {
		(void) ctl_printf(client, "500 "
		    "Failed to allocate new job.\r\n");
		return;
	}

	(void) ctl_printf(client, "200 %s\r\n", job->job_fmri);
}

void
c_dele(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];

	if (sched_get_state(job) != SJOB_STOPPED) {
		(void) ctl_printf(client, "505 "
		    "Cannot delete a running job.\r\n");
		return;
	}

	if (delete_job(job) != 0)
		(void) ctl_printf(client, "502 "
		    "Could not delete job.\r\n");
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

static int
ctl_printf(ctl_client_t *client, char const *fmt, ...)
{
va_list	ap;
int	ret = 0;

	if (client->cc_state == DEAD)
		return (-1);

	va_start(ap, fmt);
	if ((fd_vprintf(client->cc_fd, fmt, ap)) == -1)
		ret = -1, client->cc_state = DEAD;
	va_end(ap);

	return (ret);
}

int
list_callback(job, udata)
	job_t	*job;
	void	*udata;
{
ctl_client_t	*client = udata;
char const	*state, *rstate;

	if (!client->cc_admin && !job_access(job, client->cc_name, JOB_VIEW))
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

	(void) ctl_printf(client, "201 %s %s %s\r\n",
			job->job_fmri, state, rstate);
	return (0);
}

void
c_list(client, args)
	ctl_client_t	*client;
	void		**args;
{
	(void) ctl_printf(client, "200 Job list follows.\r\n");

	if (job_enumerate(list_callback, client) == -1) {
		logm(LOG_WARNING, "c_list: job_enumerate failed");
		(void) ctl_printf(client, "500 Internal server error.\r\n");
		return;
	}

	(void) ctl_printf(client, "202 End of job list.\r\n");
}

void
c_clea(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];

	if (!(job->job_flags & JOB_MAINTENANCE)) {
		(void) ctl_printf(client, "500 Job is "
		    "not in a maintenance state.\r\n");
		return;
	}

	if (job_clear_maintenance(job) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

void
c_ushd(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];

	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_printf(client, "500 Job is not scheduled.\r\n");
		return;
	}

	if (job_unschedule(job) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

void
c_stop(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];

	if (sched_get_state(job) != SJOB_RUNNING) {
		(void) ctl_printf(client, "500 Job is not running.\r\n");
		return;
	}

	if (sched_stop(job) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

void
c_schd(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];
char	*time = args[1];

	if (job->job_flags & JOB_ENABLED) {
		(void) ctl_printf(client, "500 Cannot "
		    "schedule an enabled job.\r\n");
		return;
	}

	if (job_set_schedule(job, time) == -1)
		(void) ctl_printf(client, "500 %s\r\n", strerror(errno));
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

void
c_stat(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t		*job = args[0];
char const	*state, *rstate;
char		 buf[64];

	(void) ctl_printf(client, "200 Job status follows.\r\n");

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

	(void) ctl_printf(client, "201 :%s\r\n", job->job_fmri);

	(void) ctl_printf(client, "204 :%s\r\n", state);
	(void) ctl_printf(client, "205 :%s\r\n", rstate);
	(void) ctl_printf(client, "206 :%s\r\n", job->job_start_method);
	(void) ctl_printf(client, "207 :%s\r\n", job->job_stop_method);
	if (job->job_logfmt)
		(void) ctl_printf(client, "214 :%s\r\n", job->job_logfmt);
	(void) ctl_printf(client, "215 :%d %d\r\n",
	    job->job_logsize, job->job_logkeep);

	buf[0] = 0;
	if (job->job_exit_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_exit_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_exit_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	(void) ctl_printf(client, "210 :%s\r\n", buf);

	buf[0] = 0;
	if (job->job_fail_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_fail_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_fail_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	(void) ctl_printf(client, "211 :%s\r\n", buf);

	buf[0] = 0;
	if (job->job_crash_action & ST_EXIT_RESTART)
		(void) strlcat(buf, "restart", sizeof (buf));
	else if (job->job_crash_action & ST_EXIT_DISABLE)
		(void) strlcat(buf, "disable", sizeof (buf));
	if (job->job_crash_action & ST_EXIT_MAIL)
		(void) strlcat(buf, ",mail", sizeof (buf));
	(void) ctl_printf(client, "212 :%s\r\n", buf);

	if (job->job_project)
		(void) ctl_printf(client, "209 :%s\r\n", job->job_project);
	else
		(void) ctl_printf(client, "209 :default\r\n");
	if (job->job_flags & JOB_SCHEDULED) {
		(void) ctl_printf(client, "208 :%s\r\n",
		    cron_to_string(&job->job_schedule));

		if (job->job_flags & JOB_ENABLED)
			(void) ctl_printf(client, "213 :%s\r\n",
			    cron_to_string_interval(&job->job_schedule));
	} else
		(void) ctl_printf(client, "208 :-\r\n");
	(void) ctl_printf(client, "299 End of dump.\r\n");
}

void
c_chng(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t	*job = args[0];
char	*arg = args[1];
char	*key = arg, *value;

	if ((value = index(arg, '=')) == NULL) {
		(void) ctl_printf(client, "500 Invalid format.\r\n");
		return;
	}

	*value++ = 0;

	if (strcmp(key, "start") == 0) {
		if (job_set_start_method(job, value) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change start method.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "stop") == 0) {
		if (job_set_stop_method(job, value) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change stop method.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "fmri") == 0) {
	char	*pfx;

		if (!valid_fmri(value)) {
			(void) ctl_printf(client,
			    "500 Invalid FMRI.\r\n");
			return;
		}

		if (asprintf(&pfx, "job:/%s/", client->cc_name) == -1) {
			(void) ctl_printf(client,
			    "500 Internal error.\r\n");
			logm(LOG_ERR, "out of memory");
			return;
		}

		if (strncmp(pfx, value, strlen(pfx))) {
			(void) ctl_printf(client, "500 New FMRI "
			    "is outside your namespace (%s).\r\n", pfx);
			free(pfx);
			return;
		}

		free(pfx);
		if (job_set_fmri(job, value) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change job FMRI.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "name") == 0) {
	char	*fmri;
		if (asprintf(&fmri, "job:/%s/%s",
		    client->cc_name, value) == -1) {
			(void) ctl_printf(client,
			    "500 Internal error.\r\n");
			logm(LOG_ERR, "out of memory");
			return;
		}

		if (!valid_fmri(fmri)) {
			(void) ctl_printf(client,
			    "500 Invalid name.\r\n");
			free(fmri);
			return;
		}

		if (job_set_fmri(job, fmri) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change job FMRI.\r\n");
		} else {
			(void) ctl_printf(client, "200 OK.\r\n");
		}

		free(fmri);
		return;
	} else if (strcmp(key, "project") == 0) {
		if (job_set_project(job, value) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change project.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "logfmt") == 0) {
		if (job_set_logfmt(job, value) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change log format.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "logkeep") == 0) {
	int	logkeep;
	char	*endp;
		errno = 0;
		logkeep = strtol(value, &endp, 10);
		if ((logkeep == 0 && errno != 0) ||
		    endp != (value + strlen(value))) {
			(void) ctl_printf(client, "500 "
			    "Invalid number.\r\n");
			return;
		}
		if (job_set_logkeep(job, logkeep) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change value.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "logsize") == 0) {
	int	logsize;
	char	*endp;
		errno = 0;
		logsize = strtol(value, &endp, 10);
		if ((logsize == 0 && errno != 0) ||
		    endp != (value + strlen(value))) {
			(void) ctl_printf(client, "500 "
			    "Invalid number.\r\n");
			return;
		}
		if (job_set_logsize(job, logsize) == -1) {
			(void) ctl_printf(client, "500 "
			    "Could not change value.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "enabled") == 0) {
		if (strcmp(value, "1") == 0) {
			if (job_enable(job) == -1) {
				(void) ctl_printf(client, "500 "
				    "Could not enable job.\r\n");
				return;
			}
		} else if (strcmp(value, "0") == 0) {
			if (job_disable(job) == -1) {
				(void) ctl_printf(client, "500 "
				    "Could not disable job.\r\n");
				return;
			}
		} else {
			(void) ctl_printf(client,
			    "500 Invalid syntax.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (strcmp(key, "exit") == 0 ||
		strcmp(key, "crash") == 0 ||
		strcmp(key, "fail") == 0) {
	char	*v;
	int	 action = 0;
	int (*func)(job_t *, int);

		for (v = strtok(value, ", "); v;
		    v = strtok(NULL, ", ")) {
			if (strcmp(v, "mail") == 0)
				action |= ST_EXIT_MAIL;
			else if (strcmp(v, "disable") == 0)
				action |= ST_EXIT_DISABLE;
			else if (strcmp(v, "restart") == 0)
				action |= ST_EXIT_RESTART;
			else {
				(void) ctl_printf(client, "500 "
				    "Invalid action \"%s\".\r\n", v);
				return;
			}
		}

		if ((action & (ST_EXIT_DISABLE | ST_EXIT_RESTART))
		    == 0) {
			(void) ctl_printf(client, "500 Either restart "
			    "or disable must be specified.\r\n");
			return;
		}

		if (strcmp(key, "exit") == 0)
			func = job_set_exit_action;
		else if (strcmp(key, "crash") == 0)
			func = job_set_crash_action;
		else if (strcmp(key, "fail") == 0)
			func = job_set_fail_action;
		else
			abort();

		if (func(job, action) == -1) {
			(void) ctl_printf(client, "500 "
			    "Cannot set action.\r\n");
			return;
		}
		(void) ctl_printf(client, "200 OK.\r\n");
	} else if (is_valid_rctl(key)) {
	u_longlong_t	 qty;
	char		*endp;
		errno = 0;
		qty = strtoull(value, &endp, 10);
		if ((qty == 0 && errno != 0) ||
		    endp != (value + strlen(value))) {
			(void) ctl_printf(client, "500 "
			    "Invalid number.\r\n");
			return;
		}

		if (job_set_rctl(job, key, qty) == -1)
			(void) ctl_printf(client, "500 "
			    "Resource control \"%s\" not set.\r\n", key);
		else
			(void) ctl_printf(client, "200 OK.\r\n");
	} else {
		(void) ctl_printf(client, "500 Invalid property.\r\n");
		return;
	}
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
c_lisr(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t		*job = args[0];
char		*raws = args[1];
int		 i;

	for (i = 0; i < job->job_nrctls; ++i) {
		if (strcmp(raws, "RAWS") == 0)
			(void) ctl_printf(client, "200 %s %llu\r\n",
			    job->job_rctls[i].jr_name,
			    (u_longlong_t)job->job_rctls[i].jr_value);
		else
			(void) ctl_printf(client, "200 %s %s\r\n",
			    job->job_rctls[i].jr_name,
			    format_rctl(job->job_rctls[i].jr_value,
			    get_rctl_type(job->job_rctls[i].jr_name)));
	}

	(void) ctl_printf(client, "201 End of resource control list.\r\n");
}

void
c_gcnf(client, args)
	ctl_client_t	*client;
	void		**args;
{
char	*opt = args[0];

	if (!client->cc_admin) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		return;
	}

	if (strcmp(opt, "jobs-per-user") == 0) {
	int	njobs;
		if ((njobs = quota_get_jobs_per_user()) == -1)
			(void) ctl_printf(client,
			    "500 Cannot get quota.\r\n");
		else
			(void) ctl_printf(client,
			    "200 %d\r\n", njobs);
	} else {
		(void) ctl_printf(client, "500 Invalid parameter.\r\n");
	}
}

void
c_conf(client, args)
	ctl_client_t	*client;
	void		**args;
{
char	*opt = args[0];
char	*value = args[1];

	if (!client->cc_admin) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		return;
	}

	if (strcmp(opt, "jobs-per-user") == 0) {
	int	 njobs;
	char	*endp;

		njobs = strtol(value, &endp, 10);
		if (endp != (value + strlen(value)) || njobs < 0) {
			(void) ctl_printf(client,
			    "500 Invalid format.\r\n");
			return;
		}

		if (quota_set_jobs_per_user(njobs) == -1)
			(void) ctl_printf(client,
			    "500 Cannot set quota.\r\n");
		else
			(void) ctl_printf(client,
			    "200 OK.\r\n");
	} else {
		(void) ctl_printf(client, "500 Invalid parameter.\r\n");
	}
}

void
c_strt(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t		*job = args[0];

	if (!(job->job_flags & JOB_SCHEDULED)) {
		(void) ctl_printf(client, "500 Only scheduled jobs "
		    "can be manually started.\r\n");
		return;
	}

	if (sched_start(job) == -1)
		(void) ctl_printf(client, "500 Could not start job.\r\n");
	else
		(void) ctl_printf(client, "200 OK.\r\n");
}

void
c_uset(client, args)
	ctl_client_t	*client;
	void		**args;
{
job_t		*job = args[0];
char		*arg = args[1];

	if (strcmp(arg, "start") == 0 ||
	    strcmp(arg, "stop") == 0 ||
	    strcmp(arg, "name") == 0 ||
	    strcmp(arg, "enabled") == 0) {
		(void) ctl_printf(client, "500 This option "
		    "cannot be unset.\r\n");
		return;
	} else if (strcmp(arg, "project") == 0) {
		if (job_set_project(job, NULL) == -1) {
			(void) ctl_printf(client,
			    "500 Could not change project.\r\n");
			return;
		}
	} else if (is_valid_rctl(arg)) {
		if (job_clear_rctl(job, arg) == -1) {
			(void) ctl_printf(client,
			    "500 Could not clear rctl.\r\n");
			return;
		}
	} else {
		(void) ctl_printf(client, "500 Invalid syntax.\r\n");
		return;
	}

	(void) ctl_printf(client, "200 OK.\r\n");
}

static job_t *
next_job(str, client, action)
	char		**str;
	ctl_client_t	 *client;
	int		  action;
{
job_t	*job = NULL;
char	*fmri;

	if ((fmri = next_word(str)) == NULL) {
		(void) ctl_printf(client, "500 Missing FMRI.\r\n");
		return (NULL);
	}

	if ((job = find_job_fmri(fmri)) == NULL) {
		(void) ctl_printf(client, "500 Job not found.\r\n");
		return (NULL);
	}

	if (!client->cc_admin && !job_access(job, client->cc_name, action)) {
		(void) ctl_printf(client, "500 Permission denied.\r\n");
		return (NULL);
	}

	return (job);
}
