/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<sys/task.h>

#include	<stdio.h>
#include	<stdlib.h>
#include	<deflt.h>
#include	<project.h>
#include	<string.h>
#include	<pwd.h>
#include	<fcntl.h>
#include	<unistd.h>
#include	<errno.h>
#include	<assert.h>
#include	<alloca.h>
#include	<grp.h>
#include	<rctl.h>
#include	<strings.h>
#include	<ctype.h>

#include	"execute.h"
#include	"jobserver.h"

static int fork_logwriter(char const *, size_t, int);
static int load_environment(char ***, int, char const *);
static char *logfmt(char const *fmt, job_t *);

/*
 * Set standard environment, and load user-defined environment from
 * $HOME/.environment.  This is executed as the target user.
 */
static int
load_environment(env, i, file)
	char		***env;
	int		  i;
	char const	 *file;
{
FILE	*inf;
char	 line[4096];
char	*path;
char	**np = *env;

	if ((inf = fopen(file, "r")) == NULL)
		return (-1);

	if (defopen(DEFLT "/login") == -1 || (path = defread("PATH=")) == NULL)
		path = "/usr/bin:";

	if ((np = xrecalloc(np, i, i + 1, sizeof (char **))) == NULL)
		return (-1);

	if (asprintf(&np[i++], "PATH=%s", path) == -1)
		return (-1);

	while (fgets(line, sizeof (line), inf) != NULL) {
	char	*k = line;

		while (*k == ' ')
			k++;

		if (!*k || *k == '#')
			continue;

		if ((np = xrecalloc(np, i, i + 2, sizeof (char *))) == NULL)
			return (-1);
		np[i++] = strdup(k);
		np[i] = NULL;
		*env = np;
	}

	(void) fclose(inf);
	return (0);
}

/*
 * Set up resource controls defined in the job.
 */
static void
set_rctls(job)
	job_t	*job;
{
int		 i;
rctlblk_t	*blk = alloca(rctlblk_size()), *blk2 = alloca(rctlblk_size());

	for (i = 0; i < job->job_nrctls; ++i) {
	job_rctl_t	*r = &job->job_rctls[i];
	char		 rname[64];
	int		 i;

		/*
		 * Get the existing rctl data, then modify it.  This means
		 * we don't have to know e.g. what signal should be delivered
		 * for deny action.
		 */

		(void) snprintf(rname, sizeof (rname),
				"process.%s", r->jr_name);

		for (i = getrctl(rname, NULL, blk, RCTL_FIRST);
			i != -1;
			i = getrctl(rname, blk, blk, RCTL_NEXT))
		{
			if (rctlblk_get_privilege(blk) == RCPRIV_BASIC)
				break;
		}

		if (i == -1 && errno == ENOENT) {
			rctlblk_set_privilege(blk, RCPRIV_BASIC);
			rctlblk_set_value(blk, r->jr_value);

			if (setrctl(rname, NULL, blk, RCTL_INSERT) == -1)
				(void) printf("[ setrctl(%s, %llu) "
						"failed: %s ]\n",
						rname,
						(u_longlong_t)r->jr_name,
						strerror(errno));
			continue;
		}

		if (i == -1) {
			(void) printf("[ getrctl(%s) failed: %s ]\n",
					rname, strerror(errno));
			continue;
		}

		bcopy(blk, blk2, rctlblk_size());
		rctlblk_set_value(blk2, r->jr_value);
		if (setrctl(rname, blk, blk2, RCTL_REPLACE) == -1) {
			(void) printf("[ setrctl(%s, %llu) failed: %s ]\n",
					rname, (u_longlong_t)r->jr_name,
					strerror(errno));
			continue;
		}
	}
}

/*
 * Turn an FMRI into a name suitable for a job log.
 */
static char *
log_name(fmri)
	char const	*fmri;
{
char const	*p;
char		*n, *m;

	p = fmri;
	/* Skip the job:/ */
	p += 5;
	/* Skip the username */
	if ((p = index(p, '/')) == NULL) {
		logm(LOG_ERR, "log_name: mal-formed FMRI");
		return (NULL);
	}

	if ((n = strdup(p + 1)) == NULL) {
		logm(LOG_ERR, "log_name: out of memory");
		return (NULL);
	}

	for (m = n; *m; ++m)
		if (!isalnum(*m) && !index("-_", *m))
			*m = '_';

	return (n);
}

static void
start_job(job, cmd)
	job_t		*job;
	char const	*cmd;
{
char		 *envfile;
char		**env;
int		  i = 0;
struct passwd	 *pwd;
int		  devnullfd;
struct project	  proj;
char		  nssbuf[PROJECT_BUFSZ];
int		  logfd;
char		  tbuf[64];
struct tm	 *tm;
int		  lwfd;
char		 *logfile;
char		 *s, *p;

	if (job->job_logfmt)
		logfile = logfmt(job->job_logfmt, job);
	else
		logfile = logfmt("%h/.job/%f.log", job);

	if (logfile == NULL) {
		logm(LOG_ERR, "start_job: could not create log format");
		_exit(1);
	}

	if ((pwd = getpwnam(job->job_username)) == NULL) {
		logm(LOG_ERR, "start_job: user %s doesn't exist",
				job->job_username);
		_exit(1);
	}

	/*
	 * Ideally, all of this output would go to the logfile, but we can't
	 * open the logfile as root.
	 */
	if (getdefaultproj(pwd->pw_name, &proj,
				nssbuf, sizeof (nssbuf)) == NULL) {
		logm(LOG_ERR, "start_job: getdefaultproj: %s", strerror(errno));
		_exit(1);
	}

	if (setproject(proj.pj_name, pwd->pw_name, TASK_NORMAL) != 0) {
		logm(LOG_ERR, "start_job: setproject: %s", strerror(errno));
		_exit(1);
	}

	if (job->job_project) {
		if (inproj(pwd->pw_name, job->job_project,
				nssbuf, sizeof (nssbuf))) {
			/*
			 * Don't error out here... it might be some kind of
			 * transient issue.
			 */
			if (setproject(job->job_project,
					pwd->pw_name, TASK_NORMAL) != 0)
				logm(LOG_ERR, "start_job: setproject: %s",
						strerror(errno));
		} else {
			logm(LOG_ERR, "Warning: user \"%s\" is not "
					"a member of project \"%s\"",
					pwd->pw_name, job->job_project);
		}
	}

	if (initgroups(pwd->pw_name, pwd->pw_gid) == -1) {
		logm(LOG_ERR, "start_job: initgroups: %s", strerror(errno));
		_exit(1);
	}

	if (setgid(pwd->pw_gid) == -1) {
		logm(LOG_ERR, "start_job: setgid: %s", strerror(errno));
		_exit(1);
	}

	if (setuid(pwd->pw_uid) == -1) {
		logm(LOG_ERR, "start_job: setuid: %s", strerror(errno));
		_exit(1);
	}

	if ((devnullfd = open("/dev/null", O_RDONLY)) == -1) {
		logm(LOG_ERR, "start_job: /dev/null: %s", strerror(errno));
		_exit(1);
	}

	/*
	 * This is like mkdir -p.
	 */
	s = p = logfile + 1;
	if (*logfile == '/') {
		if (chdir("/") == -1) {
			logm(LOG_ERR, "chdir(/): %s", strerror(errno));
			_exit(1);
		}
	} else {
		if (chdir(pwd->pw_name) == -1) {
			logm(LOG_ERR, "chdir(%s): %s",
					pwd->pw_name, strerror(errno));
			_exit(1);
		}
	}

	while ((s = index(p, '/')) != NULL) {
		if (index(s + 1, '/') == NULL)
			break;

		*s = 0;
		if (mkdir(p, 0700) == -1 && errno != EEXIST) {
			logm(LOG_ERR, "mkdir(%s): %s", p, strerror(errno));
			_exit(1);
		}

		if (chdir(p) == -1) {
			logm(LOG_ERR, "chdir(%s): %s", p, strerror(errno));
			_exit(1);
		}

		*s++ = '/';
		p = s;
	}

	/*LINTED*/
	if ((logfd = open(logfile, O_RDWR | O_CREAT | O_APPEND, 0600)) == -1) {
		logm(LOG_ERR, "fork_execute: %s: %s", logfile, strerror(errno));
		_exit(1);
	}

	if (dup2(devnullfd, STDIN_FILENO) == -1 ||
	    dup2(logfd, STDOUT_FILENO) == -1 ||
	    dup2(logfd, STDERR_FILENO) == -1) {
		_exit(1);
	}

	(void) close(logfd);
	(void) close(devnullfd);

	if (chdir(pwd->pw_dir) == -1) {
		(void) printf("[ chdir(%s): %s ]\n",
				pwd->pw_dir, strerror(errno));
		(void) printf("[ Job start aborted. ]\n");
		_exit(1);
	}

	if ((env = calloc(5, sizeof (char **))) == NULL) {
		(void) printf("[ Out of memory. ]\n");
		(void) printf("[ Job start aborted. ]\n");
		_exit(1);
	}

	set_rctls(job);

	(void) asprintf(&env[i++], "HOME=%s", pwd->pw_dir);
	(void) asprintf(&env[i++], "LOGNAME=%s", pwd->pw_name);
	(void) asprintf(&env[i++], "USER=%s", pwd->pw_name);
	(void) asprintf(&env[i++], "SHELL=%s", pwd->pw_shell);

	env[i] = NULL;

	(void) asprintf(&envfile, "%s/.environment", pwd->pw_dir);
	(void) load_environment(&env, i, envfile);

	if ((lwfd = fork_logwriter(logfile, job->job_logsize, job->job_logkeep)) == -1) {
		(void) printf("[ Cannot start logwriter: %s. ]\n",
				strerror(errno));
		(void) printf("[ Job start aborted. ]\n");
		_exit(1);
	}

	if (dup2(lwfd, STDOUT_FILENO) == -1 ||
	    dup2(lwfd, STDERR_FILENO) == -1) {
		_exit(1);
	}

	(void) close(lwfd);
	tm = localtime(&current_time);
	(void) strftime(tbuf, sizeof (tbuf), "%Y-%m-%d %H:%M:%S", tm);

	(void) printf("[ %s: Executing command \"%s\" ]\n",
			tbuf, cmd);
	(void) fflush(stdout);

	(void) execle("/usr/xpg4/bin/sh", "sh", "-c", cmd, NULL, env);

	(void) printf("[ Failed: %s ]\n", strerror(errno));
	(void) fflush(stdout);

	_exit(1);
}

/*
 * Execute a program as a specific user.
 */
pid_t
fork_execute(job, cmd)
	job_t		*job;
	char const	*cmd;
{
pid_t	pid;
	assert(cmd);

	switch (pid = fork()) {
	case -1:
		logm(LOG_ERR, "fork failed: %s", strerror(errno));
		goto err;

	case 0:
		/* Doesn't return */
		start_job(job, cmd);

	default:
		break;
	}

	return (pid);

err:
	return (-1);
}

static int
fork_logwriter(file, maxsize, keep)
	char const	*file;
	size_t		 maxsize;
	int		 keep;
{
char	maxs[32], keeps[16];
int	fds[2];

	(void) snprintf(maxs, sizeof (maxs), "%lu", (unsigned long) maxsize);
	(void) snprintf(keeps, sizeof (keeps), "%d", keep);

	if (pipe(fds) == -1)
		return (-1);

	switch (fork()) {
	case -1:
		(void) close(fds[1]);
		return (-1);

	case 0:
		(void) close(fds[1]);
		(void) dup2(fds[0], STDIN_FILENO);
		(void) close(fds[0]);

		(void) execl(LOGWRITER, "logwriter", file, maxs, keeps, NULL);
		_exit(1);

	default:
		(void) close(fds[0]);
		return (fds[1]);
	}
}

int
send_mail(recip, msg)
	char const *recip, *msg;
{
char const *args[] = {
	"/usr/lib/sendmail",
	"-oi",
	"-bm",
	"--",
	NULL,
	NULL
};
int	fds[2];

	args[4] = recip;

	if (pipe(fds) == -1) {
		logm(LOG_ERR, "send_mail: pipe: %s", strerror(errno));
		return (-1);
	}

	switch (fork()) {
	case 0:
		if (dup2(fds[0], 0) == -1) {
			logm(LOG_ERR, "send_mail: dup2: %s", strerror(errno));
			_exit(1);
		}

		(void) close(fds[0]);
		(void) close(fds[1]);

		/*
		 * This does not break const correctness, since execv() is only
		 * missing the appropriate 'const' qualifier for historical
		 * reasons.
		 */
		(void) execv("/usr/lib/sendmail", (char **)args);
		logm(LOG_ERR, "send_mail: execv: %s", strerror(errno));
		_exit(1);
		/*FALLTHROUGH*/

	case -1:
		logm(LOG_ERR, "send_mail: fork: %s", strerror(errno));
		return (-1);

	default:
		(void) close(fds[0]);
		(void) write(fds[1], msg, strlen(msg));
		(void) close(fds[1]);
	}

	/* We're ignoring SIGCHLD, so no need to wait */
	return (0);
}

static char *
logfmt(fmt, job)
	char const	*fmt;
	job_t		*job;
{
static char	 fl[1024];
char const	*p = fmt;
char		*f = fl;
size_t		 nleft = sizeof (fl);
struct passwd	*pwd;
char		 dbuf[128], tbuf[128];
struct tm	*tm;
char		*fmri = log_name(job->job_fmri);

	tm = gmtime(&current_time);
	(void) strftime(tbuf, sizeof (tbuf), "%Y-%m-%d_%H:%M:%S", tm);
	(void) strftime(dbuf, sizeof (dbuf), "%Y-%m-%d", tm);
	if ((pwd = getpwnam(job->job_username)) == NULL)
		return (NULL);

	bzero(fl, sizeof (fl));

	while (*p) {
		if (*p != '%') {
			if (nleft)
				*f++ = *p;
			nleft--;
			p++;
			continue;
		}

		p++;
		switch (*p) {
		case '%':
			if (nleft)
				*f++ = '%';
			nleft--;
			break;

		case 'h':
			(void) strlcat(fl, pwd->pw_dir, sizeof (fl));
			f = fl + strlen(fl);
			nleft = sizeof (fl) - strlen(fl);
			break;

		case 'f':
			(void) strlcat(fl, fmri, sizeof (fl));
			f = fl + strlen(fl);
			nleft = sizeof (fl) - strlen(fl);
			break;

		case 't':
			(void) strlcat(fl, tbuf, sizeof (fl));
			f = fl + strlen(fl);
			nleft = sizeof (fl) - strlen(fl);
			break;

		case 'd':
			(void) strlcat(fl, dbuf, sizeof (fl));
			f = fl + strlen(fl);
			nleft = sizeof (fl) - strlen(fl);
			break;
		}

		p++;
	}

	return (fl);
}
