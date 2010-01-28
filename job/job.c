/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Job client: allow users to interact with the jobserver.
 */

#include	<sys/types.h>
#include	<sys/stropts.h>

#include	<strings.h>
#include	<stdio.h>
#include	<string.h>
#include	<stdlib.h>
#include	<stdarg.h>
#include	<ctype.h>
#include	<unistd.h>
#include	<xti.h>
#include	<fcntl.h>
#include	<ucred.h>
#include	<curses.h>
#include	<term.h>

typedef struct {
	int	 numeric;
	char	*text;
} reply_t;

static reply_t *read_line();
static void free_reply(reply_t *);
static int put_server(char const *, ...);
static int vput_server(char const *, va_list);
static reply_t *simple_command(char const *, ...);

#define	NARG 16
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
static int	c_quota(int, char **);
static int	c_start(int, char **);
static int	c_unset(int, char **);
static int	c_stop(int, char **);

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
	{ "quota",	c_quota },
	{ "start",	c_start },
	{ "unset",	c_unset },
	{ "stop",	c_stop },
};

static int debug;

static char const *bold = "", *red = "", *green = "", *blue = "", *reset = "";

char const *u_list =
"job [-D] list|ls [-u <user>]\n";
char const *u_add =
"       job [-D] add [-e] [-o <prop>=<value>] "
"[-S <schedule>] [-n <name>] <command>\n";
char const *u_endis =
"       job [-D] en[able] <fmri>\n"
"       job [-D] dis[able] <fmri>\n";
char const *u_delete =
"       job [-D] del[ete] <fmri>\n";
char const *u_show =
"       job [-D] show <fmri>\n";
char const *u_set =
"       job [-D] set <fmri> <property>=<value> [<property>=<value> ...]\n"
"       job [-D] unset <fmri> <property> [<property> ...]\n";
char const *u_schedule =
"       job [-D] sched[ule] <fmri> \"<time>\"\n";
char const *u_clear =
"       job [-D] clear <fmri>\n";
char const *u_unschedule =
"       job [-D] unsched[ule] [-s] <fmri>\n";
char const *u_limit =
"       job [-D] limit [-r] <fmri> <control> [value]\n"
"       job [-D] unlimit <fmri> <control>\n";
char const *u_quota =
"       job [-D] quota <quota> [value]\n";
char const *u_start =
"       job [-D] start <fmri>\n";
char const *u_stop =
"       job [-D] stop <fmri>\n";
static void
usage()
{
	(void) fprintf(stderr, "Usage:\n");
	(void) fprintf(stderr, "%s", u_list);
	(void) fprintf(stderr, "%s", u_add);
	(void) fprintf(stderr, "%s", u_endis);
	(void) fprintf(stderr, "%s", u_delete);
	(void) fprintf(stderr, "%s", u_show);
	(void) fprintf(stderr, "%s", u_set);
	(void) fprintf(stderr, "%s", u_schedule);
	(void) fprintf(stderr, "%s", u_unschedule);
	(void) fprintf(stderr, "%s", u_clear);
	(void) fprintf(stderr, "%s", u_limit);
	(void) fprintf(stderr, "%s", u_quota);
	(void) fprintf(stderr, "%s", u_start);
	(void) fprintf(stderr, "%s", u_stop);
	(void) fprintf(stderr,
	    "\nGlobal options:\n"
	    "      -D      Enable debug mode.\n");
}

static int
server_connect()
{
int		 fd;
struct t_call	 tcall;
ucred_t		*ucred = NULL;

	if ((fd = t_open("/dev/ticotsord", O_RDWR, NULL)) == -1) {
		t_error("job: connecting to server");
		return (-1);
	}

	if (t_bind(fd, NULL, NULL) == -1) {
		t_error("job: connecting to server");
		return (-1);
	}

	bzero(&tcall, sizeof (tcall));
	tcall.addr.maxlen = tcall.addr.len = sizeof ("jobserver") - 1;
	tcall.addr.buf = "jobserver";

	if (t_connect(fd, &tcall, NULL) == -1) {
		if (t_errno == TLOOK) {
		int	n;
			if ((n = t_look(fd)) == T_DISCONNECT) {
			struct t_discon	discon;
				bzero(&discon, sizeof (discon));
				if (t_rcvdis(fd, &discon) == -1) {
					t_error("job: connected to server");
					return (-1);
				}
				(void) fprintf(stderr, "job: "
				    "connecting to server: %s\n",
				    strerror(discon.reason));
				return (-1);
			} else {
				(void) fprintf(stderr, "job: "
				    "connecting to server: %s\n",
				    t_strerror(n));
				return (-1);
			}
		} else {
			t_error("job: connecting to server");
			return (-1);
		}
	}

	if (ioctl(fd, I_PUSH, "tirdwr") == -1) {
		perror("job: ioctl(I_PUSH, tirdwr)");
		t_close(fd);
		return (-1);
	}

	if (getpeerucred(fd, &ucred) == -1) {
		perror("job: getpeerucred");
		return (-1);
	}

	if (ucred_geteuid(ucred) != 0) {
		(void) fprintf(stderr, "job: jobserver has incorrect uid\n");
		return (-1);
	}

	return (fd);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
int	 fd, c;
size_t	 i;
reply_t	*rep;

	if (setupterm(NULL, 1, NULL) == OK) {
	char	*s;
		if (s = tigetstr("bold"))
			bold = strdup(s);
		if (s = tigetstr("sgr0"))
			reset = strdup(s);
		if (s = tigetstr("setaf")) {
			red = strdup(tparm(s, 1, 0, 0, 0, 0, 0, 0, 0, 0));
			green = strdup(tparm(s, 2, 0, 0, 0, 0, 0, 0, 0, 0));
			blue = strdup(tparm(s, 4, 0, 0, 0, 0, 0, 0, 0, 0));
		}
	}

	while ((c = getopt(argc, argv, "D")) != -1) {
		switch (c) {
		case 'D':
			debug++;
			break;

		default:
			usage();
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return (1);
	}

	if ((fd = server_connect()) == -1)
		return (1);

	if ((server_in = fdopen(fd, "r")) == NULL) {
		perror("job: fdopen");
		return (1);
	}

	if ((server_out = fdopen(fd, "w")) == NULL) {
		perror("job: fdopen");
		return (1);
	}

	if ((rep = read_line()) == NULL)
		return (1);

	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		return (1);
	}

	(void) put_server("HELO 1");

	if ((rep = read_line()) == NULL)
		return (1);

	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		return (1);
	}

	for (i = 0; i < sizeof (cmds) / sizeof (*cmds); ++i) {
		if (strcmp(argv[0], cmds[i].cmd) == 0) {
		int	ret;
			ret = cmds[i].func(argc, argv);
			(void) simple_command("QUIT");
			return (ret);
		}
	}

	usage();
	return (1);
}

int
c_schedule(argc, argv)
	int	  argc;
	char	**argv;
{
	if (argc != 3) {
		(void) fprintf(stderr, "schedule: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_schedule);
		return (1);
	}

	(void) simple_command("SCHD %s :%s", argv[1], argv[2]);
	return 0;
}

int
c_unschedule(argc, argv)
	int	  argc;
	char	**argv;
{
int	c, stop = 0;
	while ((c = getopt(argc, argv, "s")) != -1) {
		switch (c) {
		case 's':
			stop++;
			break;

		default:
			(void) fprintf(stderr, "%s", u_unschedule);
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1) {
		(void) fprintf(stderr, "unschedule: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_unschedule);
		return (1);
	}

	if (stop)
		(void) simple_command("STOP %s", argv[0]);
	(void) simple_command("USHD %s", argv[0]);
	return 0;
}

/*ARGSUSED*/
int
c_list(argc, argv)
	int	  argc;
	char	**argv;
{
reply_t	*rep;
char	*vec[NARG];
int	 narg;
int	 fmri_w = 0, state_w = 0, rstate_w = 0, i;
struct {
	char	*fmri, *state, *rstate, *name;
} *ents = NULL;
int nents = 0;

	if (argc != 1) {
		(void) fprintf(stderr, "list: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_list);
		return (1);
	}

	(void) put_server("LIST");

	while ((rep = read_line()) != NULL) {
		switch (rep->numeric) {
		case 200:
			break;

		case 201:
			narg = split(rep->text, vec);
			if (narg < 3) {
				(void) fprintf(stderr, "job: "
				    "malformed line from server\n");
				return (1);
			}

			if ((ents = realloc(ents, sizeof (*ents) *
			    (nents + 1))) == NULL) {
				(void) fprintf(stderr, "out of memory\n");
				return (1);
			}

			ents[nents].fmri = vec[0];
			ents[nents].state = vec[1];
			ents[nents].rstate = vec[2];

			if ((i = strlen(vec[0])) > fmri_w) fmri_w = i + 2;
			if ((i = strlen(vec[1])) > state_w) state_w = i + 2;
			if ((i = strlen(vec[2])) > rstate_w) rstate_w = i + 2;

			nents++;
			break;

		case 202:
			if (nents == 0) {
				(void) printf("No jobs found.\n");
				return (0);
			}

			(void) printf("%s%-*s %-*s FMRI%s\n",
				bold,
				state_w, "STATE",
				rstate_w, "RSTATE",
				reset);

			for (i = 0; i < nents; ++i) {
			char const	*scol, *rcol;

				if (strcmp(ents[i].state, "enabled") == 0)
					scol = green;
				else if (strcmp(ents[i].state,
				    "scheduled/enabled") == 0)
					scol = blue;
				else
					scol = "";

				if (strcmp(ents[i].rstate, "maintenance") == 0)
					rcol = red;
				else if (strcmp(ents[i].rstate, "running") == 0)
					rcol = green;
				else
					rcol = "";

				(void) printf("%s%-*s%s %s%-*s%s %s\n",
					scol, state_w, ents[i].state, reset,
					rcol, rstate_w, ents[i].rstate, reset,
					ents[i].fmri);
			}
			return (0);

		default:
			(void) fprintf(stderr, "%s\n", rep->text);
			return (1);
		}
	}

	(void) fprintf(stderr, "job: unexpected EOF\n");
	return (1);
}

int
c_enable(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "enable: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_endis);
		return (1);
	}

	(void) simple_command("CHNG %s ENABLED=1", argv[1]);
	return (0);
}

int
c_stop(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "stop: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_stop);
		return (1);
	}

	(void) simple_command("STOP %s", argv[1]);
	return (0);
}

int
c_start(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "start: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_start);
		return (1);
	}

	(void) simple_command("STRT %s", argv[1]);
	return (0);
}

int
c_limit(argc, argv)
	int argc;
	char **argv;
{
char	*vec[NARG];
int	 c, raw = 0;
	optind = 1;

	while ((c = getopt(argc, argv, "r")) != -1) {
		switch (c) {
		case 'r':
			raw++;
			break;

		default:
			(void) fprintf(stderr, "%s", u_limit);
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1 || argc > 3) {
		(void) fprintf(stderr, "limit: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_limit);
		return (1);
	}

	if (argc == 1) {
	reply_t	*rep;
		(void) put_server("LISR %s%s", argv[0], raw ? " RAW" : "");
		while (rep = read_line()) {
			switch (rep->numeric) {
			case 200:
				(void) split(rep->text, vec);
				(void) printf("%s = %s\n", vec[0], vec[1]);
				break;

			case 201:
				return (0);

			default:
				(void) printf("%s\n", rep->text);
				return (1);
			}
		}

		(void) fprintf(stderr, "job: unexpected EOF\n");
		return (1);
	} else if (argc == 2) {
	reply_t	*rep;
		rep = simple_command("GETR %s %s%s",
		    argv[0], argv[1], raw ? " RAW" : "");
		(void) printf("%s = %s\n", argv[1], rep->text);
	} else
		(void) simple_command("SETR %s %s %s", argv[0], argv[1], argv[2]);

	return (0);
}

int
c_unlimit(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 3) {
		(void) fprintf(stderr, "unlimit: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_limit);
		return (1);
	}

	(void) simple_command("CLRR %s %s", argv[1], argv[2]);

	return (0);
}

int
c_quota(argc, argv)
	int argc;
	char **argv;
{
	if (argc < 2 || argc > 3) {
		(void) fprintf(stderr, "quota: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_quota);
		return (1);
	}

	if (argc == 2) {
	reply_t	*rep;
		rep = simple_command("CONF %s", argv[1]);
		(void) printf("%s = %s\n", argv[1], rep->text);
	} else
		(void) simple_command("CONF %s %s", argv[1], argv[2]);

	return (0);
}

int
c_disable(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "enable: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_endis);
		return (1);
	}

	(void) simple_command("CHNG %s ENABLED=0", argv[1]);
	return (0);
}

int
c_delete(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "delete: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_delete);
		return (1);
	}

	(void) simple_command("DELE %s", argv[1]);
	return (0);
}

int
c_clear(argc, argv)
	int argc;
	char **argv;
{
	if (argc != 2) {
		(void) fprintf(stderr, "clear: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_clear);
		return (1);
	}

	(void) simple_command("CLEA %s", argv[1]);
	return (0);
}

int
c_show(argc, argv)
	int argc;
	char **argv;
{
reply_t	*rep;
int	 first = 1;
char	*vec[NARG];

	if (argc != 2) {
		(void) fprintf(stderr, "show: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_show);
		return (1);
	}

	(void) put_server("STAT %s", argv[1]);
	while ((rep = read_line()) != NULL) {
		if (*rep->text == ':')
			rep->text++;

		if (rep->numeric >= 500) {
			(void) fprintf(stderr, "%s\n", rep->text);
			return (1);
		}

		switch (rep->numeric) {
		case 201: (void) printf("%s%s%s:\n", bold, rep->text, reset);
			break;
		case 204: (void) printf("       state: %s\n", rep->text); break;
		case 205: (void) printf("      rstate: %s\n", rep->text); break;
		case 206: (void) printf("start method: %s\n", rep->text); break;
		case 207: (void) printf(" stop method: %s\n", rep->text); break;
		case 208: (void) printf("    schedule: %s\n", rep->text); break;
		case 213: (void) printf("              (in %s)\n", rep->text);
			break;
		case 209: (void) printf("     project: %s\n", rep->text); break;
		case 214: (void) printf("  log format: %s\n", rep->text); break;
		case 210: (void) printf("     on exit: %s\n", rep->text); break;
		case 211: (void) printf("     on fail: %s\n", rep->text); break;
		case 212: (void) printf("    on crash: %s\n", rep->text); break;
		}

		if (rep->numeric == 299) {
			free_reply(rep);
			break;
		}

		free_reply(rep);

	}

	(void) printf("      limits:");
	(void) put_server("LISR %s", argv[1]);
	while (rep = read_line()) {
		switch (rep->numeric) {
		case 200:
			(void) split(rep->text, vec);
			if (first)
				(void) printf(" %s = %s\n", vec[0], vec[1]);
			else
				(void) printf("              "
				    "%s = %s\n", vec[0], vec[1]);
			first = 0;
			break;

		case 201:
			if (first)
				(void) printf(" -\n");
			return (0);

		default:
			(void) printf("%s\n", rep->text);
			return (1);
		}
	}

	(void) fprintf(stderr, "job: unexpected EOF\n");
	return (1);
}

static int
do_unset_property(fmri, prop)
	char const	*fmri, *prop;
{
char	*key;
reply_t	*rep;

	if (strcmp(prop, "start") == 0)
		key = "START";
	else if (strcmp(prop, "stop") == 0)
		key = "STOP";
	else if (strcmp(prop, "name") == 0)
		key = "NAME";
	else if (strcmp(prop, "project") == 0)
		key = "PROJECT";
	else if (strcmp(prop, "logfmt") == 0)
		key = "LOGFMT";
	else if (strcmp(prop, "exit") == 0)
		key = "EXIT";
	else if (strcmp(prop, "fail") == 0)
		key = "FAIL";
	else if (strcmp(prop, "crash") == 0)
		key = "CRASH";
	else
		return (-1);

	rep = simple_command("USET %s %s", fmri, key);
	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		exit(1);
	}

	free_reply(rep);
	return (0);
}

static int
do_set_property(fmri, prop, value)
	char const	*fmri;
	char const	*prop;
	char const	*value;
{
char	*key;
reply_t	*rep;

	if (strcmp(prop, "start") == 0)
		key = "START";
	else if (strcmp(prop, "stop") == 0)
		key = "STOP";
	else if (strcmp(prop, "fmri") == 0)
		key = "FMRI";
	else if (strcmp(prop, "project") == 0)
		key = "PROJECT";
	else if (strcmp(prop, "logfmt") == 0)
		key = "LOGFMT";
	else if (strcmp(prop, "exit") == 0)
		key = "EXIT";
	else if (strcmp(prop, "fail") == 0)
		key = "FAIL";
	else if (strcmp(prop, "crash") == 0)
		key = "CRASH";
	else
		return (-1);

	rep = simple_command("CHNG %s :%s=%s", fmri, key, value);
	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		exit(1);
	}

	free_reply(rep);
	return (0);
}

int
c_unset(argc, argv)
	int argc;
	char **argv;
{
char	*fmri;

	if (argc < 2) {
		(void) fprintf(stderr, "unset: not enough arguments\n");
		(void) fprintf(stderr, "%s", u_set);
		return (1);
	}

	fmri = argv[1];
	argv += 2;
	argc -= 2;

	while (argc) {
		if (do_unset_property(fmri, argv[0]) == -1) {
			(void) fprintf(stderr, "set: "
			    "unknown property \"%s\"\n", argv[0]);
			(void) fprintf(stderr, "%s", u_set);
			return (1);
		}

		argc--;
		argv++;
	}

	return (0);
}

int
c_set(argc, argv)
	int argc;
	char **argv;
{
char	*fmri;

	if (argc < 3) {
		(void) fprintf(stderr, "set: not enough arguments\n");
		(void) fprintf(stderr, "%s", u_set);
		return (1);
	}

	fmri = argv[1];
	argv += 2;
	argc -= 2;

	while (argc) {
	char	*k = argv[0], *v;
		if ((v = strchr(k, '=')) == NULL) {
			(void) fprintf(stderr, "set: "
			    "invalid prop=value pair \"%s\"\n", k);
			(void) fprintf(stderr, "%s", u_set);
			return (1);
		}

		*v++ = 0;

		if (do_set_property(fmri, k, v) == -1) {
			(void) fprintf(stderr, "set: "
			    "unknown property \"%s\"\n", k);
			(void) fprintf(stderr, "%s", u_set);
			return (1);
		}

		argc--;
		argv++;
	}

	return (0);
}

int
c_add(argc, argv)
	int	  argc;
	char	**argv;
{
reply_t	*rep;
int	 c, do_enable = 0;
char	*schedule = NULL;
char	*name = NULL;
char	*id;
size_t	 i;
struct {
	char *prop;
	char *value;
} *opts = NULL;
int nopts = 0;

	optind = 1;

	while ((c = getopt(argc, argv, "eo:S:")) != -1) {
		switch (c) {
		case 'e':
			do_enable = 1;
			break;

		case 'o':
			if ((opts = realloc(opts,
			    sizeof (*opts) * (nopts + 1))) == NULL) {
				(void) fprintf(stderr, "out of memory");
				return (1);
			}

			opts[nopts].prop = optarg;
			if ((opts[nopts].value = strchr(optarg, '=')) == NULL) {
				usage();
				return (1);
			}
			*opts[nopts].value++ = 0;
			nopts++;
			break;

		case 'S':
			schedule = optarg;
			break;

		default:
			(void) fprintf(stderr, "%s", u_add);
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (schedule && do_enable) {
		(void) fprintf(stderr, "add: "
		    "only one of -S and -e may be specified\n");
		(void) fprintf(stderr, "%s", u_add);
		return (1);
	}

	if (argc > 2) {
		(void) fprintf(stderr, "add: wrong number of arguments\n");
		(void) fprintf(stderr, "%s", u_add);
		return (1);
	}

	if (argc > 1) {
		name = argv[0];
		argc--;
		argv++;
	}

	if (name == NULL) {
	char	*p;
		if ((name = strdup(argv[0])) == NULL) {
			(void) fprintf(stderr, "out of memory\n");
			return (1);
		}

		/*
		 * Turn the command into a valid FMRI.
		 */
		if (p = rindex(name, '/'))
			name = p + 1;
		for (p = name; *p; ++p)
			if (!isalnum(*p) && !strchr("_-", *p))
				*p = '_';
	}

	rep = simple_command("CRTE :%s", name);
	(void) printf("New job FMRI is %s.\n", rep->text);
	if ((id = strdup(rep->text)) == NULL) {
		(void) fprintf(stderr, "out of memory\n");
		return (1);
	}

	(void) simple_command("CHNG %s :START=%s", id, argv[0]);

	for (i = 0; i < nopts; ++i) {
		if (do_set_property(id, opts[i].prop, opts[i].value) == -1) {
			(void) fprintf(stderr, "unrecognised property \"%s\"\n",
					opts[i].prop);
			(void) fprintf(stderr, "%s", u_add);
			return (1);
		}
	}

	if (do_enable)
		(void) simple_command("CHNG %s ENABLED=1", id);
	else if (schedule)
		(void) simple_command("SCHD %s :%s", id, schedule);

	return (0);
}

static reply_t *
read_line()
{
int	 cont = 0, prev = 0;
reply_t	*rep;
char	 line[1024];

	if ((rep = calloc(1, sizeof (reply_t))) == NULL) {
		(void) fprintf(stderr, "job: out of memory");
		exit(1);
	}

	while (fgets(line, sizeof (line), server_in) != NULL) {
	char	*p;
	int	 ocont = cont;
		cont = 0;

		line[strlen(line) - 2] = 0;

		if (debug)
			(void) fprintf(stderr, "<<  %s\n", line);


		if (strlen(line) < 5) {
			(void) fprintf(stderr, "job: "
			    "malformed line from server\n");
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
			(void) fprintf(stderr, "job: "
			    "malformed line from server\n");
			exit(1);
		}

		if (ocont) {
			if (rep->numeric != prev) {
				(void) fprintf(stderr, "job: "
				    "malformed line from server\n");
				exit(1);
			}

			if ((rep->text = realloc(rep->text,
			    strlen(rep->text) + 1 + strlen(p) + 1)) == NULL) {
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
			return (rep);
	}

	perror("job: network read");
	exit(1);
	/*NOTREACHED*/
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
	return (i);
}

static int
put_server(char const *fmt, ...)
{
va_list	ap;
int	i;
	va_start(ap, fmt);
	i = vput_server(fmt, ap);
	va_end(ap);
	return (i);
}

static int
split(line, vec)
	char	*line;
	char	**vec;
{
char	*p;
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
			return (i);

		if (*line == ':')
			break;
	}

	if (*line == ':')
		line++;

	vec[i++] = line;
	return (i);
}

static reply_t *
simple_command(char const *fmt, ...)
{
va_list	 ap;
reply_t	*rep;

	va_start(ap, fmt);
	(void) vput_server(fmt, ap);
	va_end(ap);

	rep = read_line();
	if (rep->numeric != 200) {
		(void) fprintf(stderr, "%s\n", rep->text);
		exit(1);
	}

	return (rep);
}

static void
free_reply(rep)
	reply_t	*rep;
{
	free(rep->text);
	free(rep);
}
