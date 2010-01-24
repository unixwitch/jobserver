/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

/*
 * Handles starting the job process, including setting up the initial execution
 * environment.
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

#include	"execute.h"
#include	"jobserver.h"

static int fork_logwriter(char const *, size_t, int);
static int load_environment(char ***, int, char const *);

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
		return -1;

	if (defopen(DEFLT "/login") == -1 || (path = defread("PATH=")) == NULL)
		path = "/usr/bin:";

	if ((np = xrecalloc(np, i, i + 1, sizeof(char **))) == NULL)
		return -1;

	if (asprintf(&np[i++], "PATH=%s", path) == -1)
		return -1;

	while (fgets(line, sizeof line, inf) != NULL) {
	char	*k = line;

		while (*k == ' ')
			k++;

		if (!*k || *k == '#')
			continue;

		if ((np = xrecalloc(np, i, i + 2, sizeof(char *))) == NULL)
			return -1;
		np[i++] = strdup(k);
		np[i] = NULL;
		*env = np;
	}

	(void) fclose(inf);
	return 0;
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

		(void) snprintf(rname, sizeof(rname), "process.%s", r->jr_name);

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
				(void) printf("[ setrctl(%s, %llu) failed: %s ]\n", 
						rname, (u_longlong_t) r->jr_name,
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
					rname, (u_longlong_t) r->jr_name,
					strerror(errno));
			continue;
		}
	}
}

/*
 * Execute a program as a specific user.
 */
pid_t
fork_execute(job, cmd)
	job_t		*job;
	char const	*cmd;
{
struct passwd	*pwd;
char		*logdir = NULL, *logfile = NULL;
pid_t		 pid;
int		 devnullfd;
struct project	 proj;
char		 nssbuf[PROJECT_BUFSZ];
int		 logfd;
char		 tbuf[64];
time_t		 now;
struct tm	*tm;
int		 lwfd;

	assert(cmd);

	if ((pwd = getpwuid(job->job_user)) == NULL) {
		logm(LOG_ERR, "fork_execute: (uid=%ld) doesn't exist", (long) job->job_user);
		goto err;
	}

	if (asprintf(&logdir, "%s/.job", pwd->pw_dir) == -1) {
		logm(LOG_ERR, "fork_execute: out of memory");
		goto err;
	}

	if (asprintf(&logfile, "%s/job_%ld.log", logdir, (long) job->job_id) == -1) {
		logm(LOG_ERR, "fork_execute: out of memory");
		goto err;
	}

	(void) fflush(stdout);

	switch (pid = fork()) {
	case -1:
		logm(LOG_ERR, "fork failed: %s", strerror(errno));
		goto err;

	case 0: {
	char	 *envfile;
	char	**env;
	int	  i = 0;

		/*
		 * Ideally, all of this output would go to the logfile, but we can't
		 * open the logfile as root.
		 */
		if (getdefaultproj(pwd->pw_name, &proj, nssbuf, sizeof(nssbuf)) == NULL) {
			logm(LOG_ERR, "fork_execute: getdefaultproj: %s", strerror(errno));
			exit(1);
		}

		if (setproject(proj.pj_name, pwd->pw_name, TASK_NORMAL) != 0) {
			logm(LOG_ERR, "fork_execute: setproject: %s", strerror(errno));
			exit(1);
		}

		if (job->job_project) {
			if (inproj(pwd->pw_name, job->job_project, nssbuf, sizeof(nssbuf))) {
				/*
				 * Don't error out here... it might be some kind of transient
				 * issue.
				 */
				if (setproject(job->job_project, pwd->pw_name, TASK_NORMAL) != 0)
					logm(LOG_ERR, "fork_execute: setproject: %s", strerror(errno));
			} else {
				logm(LOG_ERR, "Warning: user \"%s\" is not a member of project \"%s\"",
						pwd->pw_name, job->job_project);
			}
		}
					
		if (initgroups(pwd->pw_name, pwd->pw_gid) == -1) {
			logm(LOG_ERR, "fork_execute: initgroups: %s", strerror(errno));
			exit(1);
		}

		if (setgid(pwd->pw_gid) == -1) {
			logm(LOG_ERR, "fork_execute: setgid: %s", strerror(errno));
			exit(1);
		}

		if (setuid(pwd->pw_uid) == -1) {
			logm(LOG_ERR, "fork_execute: setuid: %s", strerror(errno));
			exit(1);
		}

		if ((devnullfd = open("/dev/null", O_RDONLY)) == -1) {
			logm(LOG_ERR, "fork_execute: /dev/null: %s", strerror(errno));
			exit(1);
		}

		if (mkdir(logdir, 0700) == -1 && errno != EEXIST) {
			logm(LOG_ERR, "fork_execute: mkdir(%s): %s", logdir, strerror(errno));
			exit(1);
		}

		/*LINTED*/
		if ((logfd = open(logfile, O_RDWR | O_CREAT | O_APPEND, 0600)) == -1) {
			logm(LOG_ERR, "fork_execute: %s: %s", logdir, strerror(errno));
			exit(1);
			goto err;
		}

		if (dup2(devnullfd, STDIN_FILENO) == -1 ||
		    dup2(logfd, STDOUT_FILENO) == -1 ||
		    dup2(logfd, STDERR_FILENO) == -1) {
			exit(1);
		}

		(void) close(logfd);
		(void) close(devnullfd);	

		if (chdir(pwd->pw_dir) == -1) {
			(void) printf("[ chdir(%s): %s ]\n", pwd->pw_dir, strerror(errno));
			(void) printf("[ Job start aborted. ]\n");
			exit(1);
		}

		if ((env = calloc(5, sizeof(char **))) == NULL) {
			(void) printf("[ Out of memory. ]\n");
			(void) printf("[ Job start aborted. ]\n");
			exit(1);
		}

		set_rctls(job);

		(void) asprintf(&env[i++], "HOME=%s", pwd->pw_dir);
		(void) asprintf(&env[i++], "LOGNAME=%s", pwd->pw_name);
		(void) asprintf(&env[i++], "USER=%s", pwd->pw_name);
		(void) asprintf(&env[i++], "SHELL=%s", pwd->pw_shell);

		env[i] = NULL;

		(void) asprintf(&envfile, "%s/.environment", pwd->pw_dir);
		(void) load_environment(&env, i, envfile);

		if ((lwfd = fork_logwriter(logfile, 1024 * 1024, 5)) == -1) {
			(void) printf("[ Cannot start logwriter: %s. ]\n", strerror(errno));
			(void) printf("[ Job start aborted. ]\n");
			exit(1);
		}

		if (dup2(lwfd, STDOUT_FILENO) == -1 ||
		    dup2(lwfd, STDERR_FILENO) == -1) {
			exit(1);
		}

		(void) close(lwfd);
		(void) time(&now);
		tm = localtime(&now);
		(void) strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", tm);

		(void) printf("[ %s: Executing command \"%s\" ]\n",
				tbuf, cmd);
		(void) fflush(stdout);

		(void) execle("/usr/xpg4/bin/sh", "sh", "-c", cmd, NULL, env);

		(void) time(&now);
		tm = localtime(&now);
		(void) strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", tm);

		(void) printf("[ %s: Failed: %s ]\n", tbuf, strerror(errno));
		exit(1);
	}

	default:
		break;
	}

	free(logfile);
	free(logdir);

	return pid;

err:
	free(logfile);
	free(logdir);
	return -1;
}

static int
fork_logwriter(file, maxsize, keep)
	char const	*file;
	size_t		 maxsize;
	int		 keep;
{
char	maxs[32], keeps[16];
int	fds[2];

	(void) snprintf(maxs, sizeof maxs, "%lu", (unsigned long) maxsize);
	(void) snprintf(keeps, sizeof keeps, "%d", keep);

	if (pipe(fds) == -1)
		return -1;

	switch (fork()) {
	case -1:
		(void) close(fds[1]);
		return -1;

	case 0:
		(void) close(fds[1]);
		(void) dup2(fds[0], STDIN_FILENO);
		(void) close(fds[0]);

		(void) execl(LOGWRITER, "logwriter", file, maxs, keeps, NULL);
		_exit(1);

	default:
		(void) close(fds[0]);
		return fds[1];
	}
}

