/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */
  
/*
 * Job client: allow users to interact with the jobserver.
 */

#pragma ident "@(#) $Id$"

#include	<sys/types.h>
#include	<sys/stropts.h>

#include	<stdio.h>
#include	<string.h>
#include	<stdlib.h>
#include	<stdarg.h>
#include	<ctype.h>
#include	<pwd.h>
#include	<unistd.h>
#include	<xti.h>
#include	<fcntl.h>
#include	<ucred.h>

typedef struct {
	int	 numeric;
	char	*text;
} reply_t;

static reply_t *read_line();
static void free_reply(reply_t *);
static int put_server(char const *, ...);
static int vput_server(char const *, va_list);
static reply_t *simple_command(char const *, ...);

#define NARG 16
static int split(char *, char **);

static FILE *server_in, *server_out;

static int	c_list(int, char **);
static int	c_enable(int, char **);
static int	c_disable(int, char **);
static int	c_delete(int, char **);
static int	c_show(int, char **);
static int	c_set(int, char **);
static int	c_add(int, char **);
static int	c_schedule(int, char **);
static int	c_unschedule(int, char **);
static int	c_clear(int, char **);
static int	c_limit(int, char **);
static int	c_unlimit(int, char **);

static struct {
	char const	*cmd;
	int		(*func) (int, char **);
} cmds[] = {
	{ "list",	c_list },
	{ "ls",		c_list },
	{ "enable",	c_enable },
	{ "en",		c_enable },
	{ "disable",	c_disable },
	{ "dis",	c_disable },
	{ "delete",	c_delete },
	{ "del",	c_delete },
	{ "show",	c_show },
	{ "set",	c_set },
	{ "add",	c_add },
	{ "schedule",	c_schedule },
	{ "sched",	c_schedule },
	{ "unschedule",	c_unschedule },
	{ "unsched",	c_unschedule },
	{ "clear",	c_clear },
	{ "limit",	c_limit },
	{ "unlimit",	c_unlimit },
};

static int debug;

char const *u_list =
"       job [-D] list|ls [-u <user>]\n"
"\n"
"         List all jobs under this account (or all accounts when run by uid 0).\n"
"\n"
"           -u <user>           Only list jobs under this account.\n";

char const *u_add =
"       job [-D] add [-e] [-o <prop>=<value>] [-S <schedule>] [-n <name>] <command>\n"
"\n"
"         Create a new job.  By default, the start command will be <command>,\n"
"         and the job name will be <command> with everything up to the first\n"
"         slash removed\n"
"\n"
"           -e                  Enable the job after adding it.\n"
"           -n <name>           Specify the name for the job.\n"
"           -o <prop>=<value>   Specify properties for the new job (see 'job set').\n"
"           -S <schedule>       Specify a schedule for the job (see 'job schedule').\n";
char const *u_endis =
"       job [-D] en[able] <id>\n"
"       job [-D] dis[able] <id>\n"
"\n"
"         Enable or disable the given job.\n";
char const *u_delete =
"       job [-D] del[ete] <id>\n"
"\n"
"         Delete the given job.  If running, the job must be stopped first.\n";
char const *u_show =
"       job [-D] show <id>\n"
"\n"
"         Show all available information about the given job.\n";
char const *u_set =
"       job [-D] set <id> property=value [property=value ...]\n"
"\n"
"         Set a property on a job.  Available properties:\n"
"\n"
"           start      The command used to start the job.\n"
"           stop       The command used to stop the job.\n"
"           name       The name of the job.\n";
char const *u_schedule =
"       job [-D] sched[ule] <id> \"<time>\"\n"
"\n"
"         Schedule a job to run at a particular time.  Any of the\n"
"         following time formats are accepted:\n"
"\n"
"           every minute\n"
"           every hour at <MM>\n"
"           every day at <HH>:<MM>\n"
"           every <DAY> at <HH>:<MM>\n"
"\n"
"         HH:MM should be specified in 24-hour format.\n";
char const *u_clear =
"       job [-D] clear <id>\n"
"\n"
"         Clear maintenance state on a job.\n";
char const *u_unschedule =
"       job [-D] unsched[ule] <id>\n"
"\n"
"         Unschedule a previously scheduled job.\n";
char const *u_limit =
"       job [-D] limit <job> <control>\n"
"       job [-D] limit <job> <control> <value>\n"
"       job [-D] unlimit <job> <control>\n"
"\n"
"         View, set or clear the resource control <control> for the specified\n"
"         <job>\n"
"\n";

static void
usage()
{
	(void) fprintf(stderr, "Usage:\n");
	(void) fprintf(stderr, "%s\n", u_list);
	(void) fprintf(stderr, "%s\n", u_add);
	(void) fprintf(stderr, "%s\n", u_endis);
	(void) fprintf(stderr, "%s\n", u_delete);
	(void) fprintf(stderr, "%s\n", u_show);
	(void) fprintf(stderr, "%s\n", u_set);
	(void) fprintf(stderr, "%s\n", u_schedule);
	(void) fprintf(stderr, "%s\n", u_unschedule);
	(void) fprintf(stderr, "%s\n", u_clear);
	(void) fprintf(stderr, "%s\n", u_limit);
	(void) fprintf(stderr, 
"Global options:\n"
"      -D      Enable debug mode.\n"
);
}

static int
server_connect()
{
int		 fd;
struct t_call	 tcall;
ucred_t		*ucred = NULL;

	if ((fd = t_open("/dev/ticotsord", O_RDWR, NULL)) == -1) {
		t_error("job: connecting to server");
		return -1;
	}

	if (t_bind(fd, NULL, NULL) == -1) {
		t_error("job: connecting to server");
		return -1;
	}

	bzero(&tcall, sizeof(tcall));
	tcall.addr.maxlen = tcall.addr.len = sizeof("jobserver") - 1;
	tcall.addr.buf = "jobserver";

	if (t_connect(fd, &tcall, NULL) == -1) {
		if (t_errno == TLOOK) {
		int	n;
			if ((n = t_look(fd)) == T_DISCONNECT) {
			struct t_discon	discon;
				bzero(&discon, sizeof(discon));
				if (t_rcvdis(fd, &discon) == -1) {
					t_error("job: connected to server");
					return -1;
				}
				(void) fprintf(stderr, "job: connecting to server: %s\n",
						strerror(discon.reason));
				return -1;
			} else {
				(void) fprintf(stderr, "job: connecting to server: %s\n",
						t_strerror(n));
				return -1;
			}
		} else {
			t_error("job: connecting to server");
			return -1;
		}
	}

	if (ioctl(fd, I_PUSH, "tirdwr") == -1) {
		perror("job: ioctl(I_PUSH, tirdwr)");
		t_close(fd);
		return -1;
	}

	if (getpeerucred(fd, &ucred) == -1) {
		perror("job: getpeerucred");
		return -1;
	}

	if (ucred_geteuid(ucred) != 0) {
		(void) fprintf(stderr, "job: jobserver has incorrect uid\n");
		return -1;
	}

	return fd;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
int	 fd, c;
size_t	 i;
reply_t	*rep;

	while ((c = getopt(argc, argv, "D")) != -1) {
		switch (c) {
		case 'D':
			debug++;
			break;

		default:
			usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return 1;
	}

	if ((fd = server_connect()) == -1)
		return 1;

	if ((server_in = fdopen(fd, "r")) == NULL) {
		perror("job: fdopen");
		return 1;
	}

	if ((server_out = fdopen(fd, "w")) == NULL) {
		perror("job: fdopen");
		return 1;
	}

	if ((rep = read_line()) == NULL)
		return 1;

	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		return 1;
	}

	(void) put_server("HELO 1");

	if ((rep = read_line()) == NULL)
		return 1;

	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		return 1;
	}

	for (i = 0; i < sizeof cmds / sizeof *cmds; ++i) {
		if (!strcmp(argv[0], cmds[i].cmd)) {
		int	ret;
			ret = cmds[i].func(argc, argv);
			simple_command("QUIT");
			return ret;
		}
	}

	usage();
	return 1;
}

int
c_schedule(argc, argv)
	int	  argc;
	char	**argv;
{
	if (argc != 3) {
		fprintf(stderr, "schedule: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_schedule);
		return 1;
	}

	simple_command("SCHD %s :%s", argv[1], argv[2]);
}

int
c_unschedule(argc, argv)
	int	  argc;
	char	**argv;
{
	if (argc != 2) {
		fprintf(stderr, "unschedule: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_unschedule);
		return 1;
	}

	simple_command("USHD %s", argv[1]);
}

/*ARGSUSED*/
int
c_list (argc, argv)
	int	  argc;
	char	**argv;
{
reply_t	*rep;
char	*vec[NARG];
int	 narg;
int	 id_w = 0, user_w = 0, state_w = 0, rstate_w = 0, name_w = 0, i;
struct	 passwd *pwd = NULL;
uid_t	 uid;
struct {
	char	*id, *user, *state, *rstate, *name, *cmd;
} *ents = NULL;
int nents = 0;

	if (argc != 1) {
		fprintf(stderr, "list: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_list);
		return 1;
	}

	put_server("LIST");

	while ((rep = read_line()) != NULL) {
		switch (rep->numeric) {
		case 200:
			break;

		case 201:
			narg = split(rep->text, vec);
			if (narg < 6) {
				(void) fprintf(stderr, "job: malformed line from server\n");
				return 1;
			}

			if ((ents = realloc(ents, sizeof(*ents) * (nents + 1))) == NULL) {
				(void) fprintf(stderr, "out of memory\n");
				return 1;
			}

			ents[nents].id = vec[0];
			ents[nents].user = vec[1];
			ents[nents].name = vec[2];
			ents[nents].state = vec[3];
			ents[nents].rstate = vec[4];
			ents[nents].cmd = vec[5];

			uid = atoi(ents[nents].user);
			if ((pwd = getpwuid(uid)) != NULL)
				ents[nents].user = pwd->pw_name;

			if ((i = strlen(vec[0])) > id_w) id_w = i + 2;
			if ((i = strlen(ents[nents].user)) > user_w) user_w = i + 2;
			if ((i = strlen(vec[2])) > name_w) name_w = i + 2;
			if ((i = strlen(vec[3])) > state_w) state_w = i + 2;
			if ((i = strlen(vec[4])) > rstate_w) rstate_w = i + 2;

			nents++;
			break;

		case 202:
			if (nents == 0) {
				(void) printf("No jobs found.\n");
				return 0;
			}

			printf("%*s %-*s %-*s %-*s %-*s CMD\n",
				id_w, "ID", 
				name_w, "NAME",
				user_w, "USER",
				state_w, "STATE",
				rstate_w, "RSTATE");

			for (i = 0; i < nents; ++i)
				printf("%*s %-*s %-*s %-*s %-*s %s\n",
					id_w, ents[i].id,
					name_w, ents[i].name,
					user_w, ents[i].user,
					state_w, ents[i].state,
					rstate_w, ents[i].rstate,
					ents[i].cmd);
			return 0;

		default:
			(void) fprintf(stderr, "%s\n", rep->text);
			return 1;
		}
	}

	(void) fprintf(stderr, "job: unexpected EOF\n");
	return 1;
}

int
c_enable (argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		fprintf(stderr, "enable: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_endis);
		return 1;
	}

	simple_command("CHNG %s ENABLED=1", argv[1]);
	return 0;
}

int
c_limit(argc, argv)
	int argc;
	char **argv;
{
char	*vec[NARG];
	if (argc < 2 || argc > 4) {
		fprintf(stderr, "limit: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_limit);
		return 1;
	}

	if (argc == 2) {
	reply_t	*rep;
		put_server("LISR %s", argv[1]);
		while (rep = read_line()) {
			switch (rep->numeric) {
			case 200:
				split(rep->text, vec);
				(void) printf("%s = %s\n", vec[0], vec[1]);
				break;

			case 201:
				return 0;

			default:
				(void) printf("%s\n", rep->text);
				return 1;
			}
		}

		(void) fprintf(stderr, "job: unexpected EOF\n");
		return 1;
	} else if (argc == 3) {
	reply_t	*rep;
		rep = simple_command("GETR %s %s", argv[1], argv[2]);
		(void) printf("%s = %s\n", argv[2], rep->text);
	} else
		simple_command("SETR %s %s %s", argv[1], argv[2], argv[3]);
	
	return 0;
}

int
c_unlimit(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 3) {
		fprintf(stderr, "unlimit: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_limit);
		return 1;
	}

	simple_command("CLRR %s %s", argv[1], argv[2]);
	
	return 0;
}

int
c_disable (argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		fprintf(stderr, "enable: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_endis);
		return 1;
	}

	simple_command("CHNG %s ENABLED=0", argv[1]);
	return 0;
}

int
c_delete (argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		fprintf(stderr, "delete: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_delete);
		return 1;
	}

	simple_command("DELE %s", argv[1]);
	return 0;
}

int
c_clear(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		fprintf(stderr, "clear: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_clear);
		return 1;
	}

	simple_command("CLEA %s", argv[1]);
	return 0;
}

int
c_show (argc, argv)
	int argc;
	char **argv;
{
reply_t	*rep;

	if (argc != 2) {
		fprintf(stderr, "show: wrong number of arguments\n\n");
		fprintf(stderr, "%s", u_show);
		return 1;
	}

	put_server("STAT %s", argv[1]);
	while ((rep = read_line()) != NULL) {
		if (*rep->text == ':')
			rep->text++;

		if (rep->numeric >= 500) {
			(void) fprintf(stderr, "%s\n", rep->text);
			return 1;
		}

		switch (rep->numeric) {
		case 201: (void) printf("          id: %s\n", rep->text); break;
		case 202: (void) printf("        user: %s\n", rep->text); break;
		case 203: (void) printf("        name: %s\n", rep->text); break;
		case 204: (void) printf("       state: %s\n", rep->text); break;
		case 205: (void) printf("      rstate: %s\n", rep->text); break;
		case 206: (void) printf("start method: %s\n", rep->text); break;
		case 207: (void) printf(" stop method: %s\n", rep->text); break;
		case 208: (void) printf("    schedule: %s\n", rep->text); break;
		}

		free_reply(rep);

		if (rep->numeric == 299)
			break;
	}

	return 0;
}

static int
do_set_property(id, prop, value)
	int		 id;
	char const	*prop;
	char const	*value;
{
char	*key;
reply_t	*rep;

	if (!strcmp(prop, "start"))
		key = "START";
	else if (!strcmp(prop, "stop"))
		key = "STOP";
	else if (!strcmp(prop, "name"))
		key = "NAME";
	else
		return -1;

	rep = simple_command("CHNG %d :%s=%s", id, key, value);
	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		exit(1);
	}

	free_reply(rep);
	return 0;
}

int
c_set (argc, argv)
	int argc;
	char **argv;
{
int	id;

	if (argc < 3) {
		(void) fprintf(stderr, "set: not enough arguments\n");
		(void) fprintf(stderr, "%s", u_set);
		return 1;
	}

	id = atoi(argv[1]);
	argv += 2;
	argc -= 2;

	while (argc) {
	char	*k = argv[0], *v;
		if ((v = strchr(k, '=')) == NULL) {
			(void) fprintf(stderr, "set: invalid prop=value pair \"%s\"\n", k);
			(void) fprintf(stderr, "%s", u_set);
			return 1;
		}

		*v++ = 0;

		if (do_set_property(id, k, v) == -1) {
			(void) fprintf(stderr, "set: unknown property \"%s\"\n", k);
			(void) fprintf(stderr, "%s", u_set);
			return 1;
		}

		argc--;
		argv++;
	}

	return 0;
}

int
c_add (argc, argv)
	int	  argc;
	char	**argv;
{
reply_t	*rep;
int	 c, do_enable = 0;
char	*schedule = NULL;
char	*name = NULL, *p;
char	*id;
size_t	 i;
struct {
	char *prop;
	char *value;
} *opts = NULL;
int nopts = 0;

	optind = 1;

	while ((c = getopt(argc, argv, "n:eo:S:")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;

		case 'e':
			do_enable = 1;
			break;

		case 'o':
			if ((opts = realloc(opts, sizeof(*opts) * (nopts + 1))) == NULL) {
				(void) fprintf(stderr, "out of memory");
				return 1;
			}

			opts[nopts].prop = optarg;
			if ((opts[nopts].value = strchr(optarg, '=')) == NULL) {
				usage();
				return 1;
			}
			*opts[nopts].value++ = 0;
			nopts++;
			break;

		case 'S':
			schedule = optarg;
			break;

		default:
			(void) fprintf(stderr, "%s", u_add);
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (schedule && do_enable) {
		(void) fprintf(stderr, "add: only one of -S and -e may be specified\n");
		(void) fprintf(stderr, "%s", u_add);
		return 1;
	}

	if (argc != 1) {
		(void) fprintf(stderr, "add: wrong number of arguments\n");
		(void) fprintf(stderr, "%s", u_add);
		return 1;
	}

	if (name == NULL)
		name = argv[0];

	rep = simple_command("CRTE :%s", name);
	(void) printf("New job ID is %s.\n", rep->text);
	if ((id = strdup(rep->text)) == NULL) {
		(void) fprintf(stderr, "out of memory");
		return 1;
	}

	if (name)
		simple_command("CHNG %s :START=%s", id, argv[0]);

	for (i = 0; i < nopts; ++i) {
		if (do_set_property(opts[i].prop, opts[i].value) == -1) {
			(void) fprintf(stderr, "unrecognised property \"%s\"\n",
					opts[i].prop);
			(void) fprintf(stderr, "%s", u_add);
			return 1;
		}
	}

	if (do_enable)
		simple_command("CHNG %s ENABLED=1", id);
	else if (schedule)
		simple_command("SCHD %s :%s", id, schedule);

	return 0;
}

static reply_t *
read_line()
{
int	 cont = 0, prev = 0;
reply_t	*rep;
char	 line[1024];

	if ((rep = calloc(1, sizeof(*rep))) == NULL) {
		(void) fprintf(stderr, "job: out of memory");
		exit(1);
	}

	while (fgets(line, sizeof line, server_in) != NULL) {
	char	*p = line;
	int	 ocont = cont;
		cont = 0;

		line[strlen(line) - 2] = 0;

		if (debug)
			(void) fprintf(stderr, "<<  %s\n", line);


		if (strlen(line) < 5) {
			(void) fprintf(stderr, "job: malformed line from server\n");
			exit(1);
		}

		if (isdigit(line[0]) && isdigit(line[1]) && isdigit(line[2])) {
			rep->numeric = atoi(line);
			p = &line[3];

			if (*p == '-')
				cont = 1, p++;

			while (*p == ' ')
				p++;
		} else {
			(void) fprintf(stderr, "job: malformed line from server\n");
			exit(1);
		}

		if (ocont) {
			if (rep->numeric != prev) {
				(void) fprintf(stderr, "job: malformed line from server\n");
				exit(1);
			}

			if ((rep->text = realloc(rep->text, strlen(rep->text) + 1 + strlen(p) + 1)) == NULL) {
				(void) fprintf(stderr, "job: out of memory\n");
				exit(1);
			}

			(void) strcat(rep->text, p);
		} else {
			if ((rep->text = strdup(p)) == NULL) {
				(void) fprintf(stderr, "out of memory\n");
				exit(1);
			}
		}

		prev = rep->numeric;
		if (!cont)
			return rep;
	}

	perror("job: network read");
	exit(1);
}

static int
vput_server(fmt, ap)
	char const	*fmt;
	va_list		 ap;
{
int	i;
	if (debug) {
	va_list	ap2;
		va_copy(ap2, ap);
		(void) fputs(" >> ", stderr);
		(void) vfprintf(stderr, fmt, ap2);
		(void) fputs("\n", stderr);
	}

	i = vfprintf(server_out, fmt, ap);
	if (i != -1)
		i = fputs("\r\n", server_out);
	if (i != -1)
		i = fflush(server_out);
	return i;
}

static int
put_server(char const *fmt, ...)
{
va_list	ap;
int	i;
	va_start(ap, fmt);
	i = vput_server(fmt, ap);
	va_end(ap);
	return i;
}

static int
split(line, vec)
	char	*line;
	char	**vec;
{
char	*p = line;
int	 i = 0;
	while ((p = strchr(line, ' ')) != NULL) {
		while (*line == ' ')
			++line;

		vec[i++] = line;
		*p++ = 0;
		line = p;

		while (*line == ' ')
			++line;

		if (i+2 == NARG)
			return i;

		if (*line == ':')
			break;
	}

	if (*line == ':')
		line++;

	vec[i++] = line;
	return i;
}

static reply_t *
simple_command(char const *fmt, ...)
{
va_list	 ap;
reply_t	*rep;

	va_start(ap, fmt);
	vput_server(fmt, ap);
	va_end(ap);

	rep = read_line();
	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		exit(1);
	}

	return rep;
}

static void
free_reply(rep)
	reply_t	*rep;
{
	free(rep->text);
	free(rep);
}
