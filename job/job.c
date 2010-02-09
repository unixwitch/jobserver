/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Job client: allow users to interact with the jobserver.
 */

#include	<sys/types.h>
#include	<sys/stropts.h>
#include	<netinet/in.h>

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
#include	<libnvpair.h>
#include	<inttypes.h>
#include	<errno.h>

#define	DATA_TYPE_NVINLINE -1

static int put_server(nvlist_t *);
static nvlist_t *read_nvlist();
static nvlist_t *simple_command(char const *, ...);
static void print_nvlist(FILE *, nvlist_t *);

int server_fd;

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
int	 c;
size_t	 i;
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

	if ((server_fd = server_connect()) == -1)
		return (1);

	(void) simple_command("helo",
		"version", DATA_TYPE_INT16, 1,
		NULL);

	for (i = 0; i < sizeof (cmds) / sizeof (*cmds); ++i) {
		if (strcmp(argv[0], cmds[i].cmd) == 0) {
		int	ret;
			ret = cmds[i].func(argc, argv);
			(void) simple_command("quit", NULL);
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
		(void) fprintf(stderr, "schedule: "
		    "wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_schedule);
		return (1);
	}

	(void) simple_command("schedule",
		"fmri", DATA_TYPE_STRING, argv[1],
		"schedule", DATA_TYPE_STRING, argv[2],
		NULL);
	return (0);
}

int
c_unschedule(argc, argv)
	int	  argc;
	char	**argv;
{
int	c, stop = 0;
	optind = 1;
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
		(void) fprintf(stderr, "unschedule: "
		    "wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_unschedule);
		return (1);
	}

	if (stop)
		(void) simple_command("stop",
			"fmri", DATA_TYPE_STRING, argv[0],
			NULL);
	(void) simple_command("unschedule",
	    "fmri", DATA_TYPE_STRING, argv[0],
	    NULL);
	return (0);
}

/*ARGSUSED*/
int
c_list(argc, argv)
	int	  argc;
	char	**argv;
{
nvlist_t *reply;
nvlist_t **jobs;
size_t i, njobs;
int	 fmri_w = 0, state_w = 0, rstate_w = 0, w;
struct {
	char	*fmri, *state, *rstate, *name;
} *ents = NULL;

	if (argc != 1) {
		(void) fprintf(stderr, "list: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_list);
		return (1);
	}

	reply = simple_command("list", NULL);
	if (nvlist_lookup_nvlist_array(reply, "jobs", &jobs, &njobs)) {
		(void) fprintf(stderr, "list: unexpected reply from server\n");
		return (1);
	}

	if (njobs == 0) {
		(void) printf("No jobs found.\n");
		return (0);
	}

	if ((ents = malloc(sizeof(*ents) * njobs)) == NULL) {
		(void) fprintf(stderr, "out of memory");
		return (1);
	}

	for (i = 0; i < njobs; ++i) {
		nvlist_lookup_string(jobs[i], "fmri", &ents[i].fmri);
		nvlist_lookup_string(jobs[i], "state", &ents[i].state);
		nvlist_lookup_string(jobs[i], "rstate", &ents[i].rstate);

		if ((w = strlen(ents[i].fmri)) > fmri_w) fmri_w = w + 2;
		if ((w = strlen(ents[i].state)) > state_w) state_w = w + 2;
		if ((w = strlen(ents[i].rstate)) > rstate_w) rstate_w = w + 2;
	}

	(void) printf("%s%-*s %-*s FMRI%s\n",
		bold,
		state_w, "STATE",
		rstate_w, "RSTATE",
		reset);

	for (i = 0; i < njobs; ++i) {
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

	(void) simple_command("enable",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
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

	(void) simple_command("stop",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
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

	(void) simple_command("start",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
	return (0);
}

int
c_quota(argc, argv)
	int argc;
	char **argv;
{
nvlist_t *cmd;
nvlist_t *reply;
	if (argc < 2 || argc > 3) {
		(void) fprintf(stderr, "quota: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_quota);
		return (1);
	}

	if (argc == 2) {
	uint32_t	value;
		nvlist_alloc(&cmd, NV_UNIQUE_NAME, 0);
		nvlist_add_boolean(cmd, argv[1]);
		reply = simple_command("get_config",
		    "opts", DATA_TYPE_NVLIST, cmd,
		    NULL);
		if (nvlist_lookup_uint32(reply, argv[1], &value)) {
			(void) fprintf(stderr, "unexpected reply from server\n");
			return (1);
		}
		(void) printf("%s = %"PRIu32"\n", argv[1], value);
	} else {
		nvlist_alloc(&cmd, NV_UNIQUE_NAME, 0);
		if (strcmp(argv[1], "jobs-per-user") == 0)
			nvlist_add_int32(cmd, "jobs_per_user", atoi(argv[2]));
		else {
			(void) fprintf(stderr, "Invalid option.\n");
			return (1);
		}

		simple_command("set_config",
		    "opts", DATA_TYPE_NVLIST, cmd,
		    NULL);
	}

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

	(void) simple_command("disable",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
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

	(void) simple_command("delete",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
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

	(void) simple_command("clear",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);
	return (0);
}

int
c_show(argc, argv)
	int argc;
	char **argv;
{
nvlist_t	*reply, *job, *rctls;
char		*fmri, *state, *rstate, *start, *stop,
		*schedule = NULL, *nextrun = NULL, *project, *logfmt,
		*exit, *fail, *crash;
nvpair_t	*pair = NULL;
uint32_t	 logkeep;
uint64_t	 logsize;
int		 first = 1;

	if (argc != 2) {
		(void) fprintf(stderr, "show: wrong number of arguments\n\n");
		(void) fprintf(stderr, "%s", u_show);
		return (1);
	}

	reply = simple_command("stat",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);

	if (nvlist_lookup_nvlist(reply, "job", &job)) {
		(void) fprintf(stderr, "show: invalid reply from server\n");
		return (1);
	}

	if (nvlist_lookup_pairs(job, 0,
	    "fmri", DATA_TYPE_STRING, &fmri,
	    "state", DATA_TYPE_STRING, &state,
	    "rstate", DATA_TYPE_STRING, &rstate,
	    "start", DATA_TYPE_STRING, &start,
	    "stop", DATA_TYPE_STRING, &stop,
	    "project", DATA_TYPE_STRING, &project,
	    "logfmt", DATA_TYPE_STRING, &logfmt,
	    "exit", DATA_TYPE_STRING, &exit,
	    "fail", DATA_TYPE_STRING, &fail,
	    "crash", DATA_TYPE_STRING, &crash,
	    "logsize", DATA_TYPE_UINT64, &logsize,
	    "logkeep", DATA_TYPE_UINT32, &logkeep,
	    NULL)) {
		(void) fprintf(stderr, "show: invalid reply from server\n");
		return (1);
	}

	(void) printf("%s%s%s:\n", bold, fmri, reset);
	(void) printf("       state: %s\n", state);
	(void) printf("      rstate: %s\n", rstate);
	(void) printf("start method: %s\n", start);
	(void) printf(" stop method: %s\n", stop);
	if (nvlist_lookup_string(job, "schedule", &schedule) == 0)
		(void) printf("    schedule: %s\n", schedule);
	if (nvlist_lookup_string(job, "nextrun", &nextrun) == 0)
		(void) printf("              (in %s)\n", nextrun);
	(void) printf("     project: %s\n", project);
	(void) printf("  log format: %s\n", logfmt);
	(void) printf("log rotation: size %"PRIu64", keep %"PRIu32"\n",
	    logsize, logkeep);
	(void) printf("     on exit: %s\n", exit);
	(void) printf("     on fail: %s\n", fail);
	(void) printf("    on crash: %s\n", crash);

	(void) printf("      limits: ");
	reply = simple_command("list_rctls",
	    "fmri", DATA_TYPE_STRING, argv[1],
	    NULL);

	if (nvlist_lookup_nvlist(reply, "rctls", &rctls)) {
		(void) fprintf(stderr, "show: invalid reply from server\n");
		return (1);
	}

	while ((pair = nvlist_next_nvpair(rctls, pair)) != NULL) {
	char	*value;
		nvpair_value_string(pair, &value);
		if (first)
			(void) printf("%s = %s\n",
			  nvpair_name(pair),
			  value);
		else
			(void) printf("              "
			    "%s = %s\n",
			    nvpair_name(pair),
			    value);
		first = 0;
	}

	if (first)
		printf("-\n");

	return (0);
}

int
c_unset(argc, argv)
	int argc;
	char **argv;
{
char		*fmri;
nvlist_t	*request, *reply;
nvpair_t	*pair = NULL;
	if (argc < 2) {
		(void) fprintf(stderr, "unset: not enough arguments\n");
		(void) fprintf(stderr, "%s", u_set);
		return (1);
	}

	fmri = argv[1];
	argv += 2;
	argc -= 2;

	nvlist_alloc(&request, NV_UNIQUE_NAME, 0);
	while (argc)
		nvlist_add_boolean_value(request, argv[argc-- - 1], 0);

	reply = simple_command("set_property",
	    "fmri", DATA_TYPE_STRING, fmri,
	    "opts", DATA_TYPE_NVLIST, request,
	    NULL);
	nvlist_free(request);

	while ((pair = nvlist_next_nvpair(reply, pair)) != NULL) {
	char	*err;
		nvpair_value_string(pair, &err);
		(void) fprintf(stderr, "%s: %s\n",
		     nvpair_name(pair), err);
	}

	return (0);
}

static int
do_set_property(fmri, prop, val)
	char const	*fmri, *prop, *val;
{
	/*
	 * The jobserver requireds that properties be set with the correct
	 * type.  We list non-string properties here, and assume anything
	 * not listed is a string.  Lists must be in alphabetical order.
	 */
static char const ui64[][20] = {
	"logsize",
	"max-address-space",
	"max-core-size",
	"max-cpu-time",
	"max-data-size",
	"max-file-descriptor",
	"max-file-size",
	"max-lwps",
	"max-msg-messages",
	"max-msg-qbytes",
	"max-port-events",
	"max-sem-ops",
	"max-sem-nsems",
	"max-stack-size",
};
static char const *const ui16[][20] = {
	"logkeep",
};
nvlist_t	*reply;
nvpair_t	*pair = NULL;
int		 ret = 0;
	if (bsearch(prop, ui64, sizeof(ui64) / sizeof(*ui64), 20, (int (*)(void const *, void const *)) strcmp)) {
		reply = simple_command("set_property",
		    "fmri", DATA_TYPE_STRING, fmri,
		    "opts", DATA_TYPE_NVINLINE,
		    	prop, DATA_TYPE_UINT64, strtoll(val, NULL, 0),
			NULL,
		    NULL);
	} else if (bsearch(prop, ui16, sizeof(ui16) / sizeof(*ui16), 20, (int (*)(void const *, void const *)) strcmp)) {
		reply = simple_command("set_property",
		    "fmri", DATA_TYPE_STRING, fmri,
		    "opts", DATA_TYPE_NVINLINE,
		    	prop, DATA_TYPE_UINT16, atoi(val),
			NULL,
		    NULL);
	} else {
		reply = simple_command("set_property",
		    "fmri", DATA_TYPE_STRING, fmri,
		    "opts", DATA_TYPE_NVINLINE,
			prop, DATA_TYPE_STRING, val,
			NULL,
		    NULL);
	}

	while ((pair = nvlist_next_nvpair(reply, pair)) != NULL) {
	char	*err;
		nvpair_value_string(pair, &err);
		(void) fprintf(stderr, "%s: %s\n",
		     nvpair_name(pair), err);
		ret = -1;
	}

	return (ret);
}

int
c_set(argc, argv)
	int argc;
	char **argv;
{
char		*fmri;
nvlist_t	*request, *reply;
nvpair_t	*pair = NULL;
int		 ret = 0;

	if (argc < 3) {
		(void) fprintf(stderr, "set: not enough arguments\n");
		(void) fprintf(stderr, "%s", u_set);
		return (1);
	}

	fmri = argv[1];
	argv += 2;
	argc -= 2;

	nvlist_alloc(&request, NV_UNIQUE_NAME, 0);
	while (argc) {
	char	*k = argv[0], *v;
		if ((v = strchr(k, '=')) == NULL) {
			(void) fprintf(stderr, "set: "
			    "invalid prop=value pair \"%s\"\n", k);
			(void) fprintf(stderr, "%s", u_set);
			return (1);
		}

		*v++ = 0;

		if (do_set_property(fmri, k, v) == -1)
			ret = 1;
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
nvlist_t *reply, *props;
char	*id;
int	 c, do_enable = 0;
char	*schedule = NULL;
char	*name = NULL;
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

	reply = simple_command("create",
	    "name", DATA_TYPE_STRING, name,
	    NULL);
	if (nvlist_lookup_string(reply, "fmri", &id)) {
		(void) fprintf(stderr, "invalid reply from server\n");
		return (1);
	}

	(void) printf("New job FMRI is %s.\n", id);

	do_set_property(id, "start", argv[0]);

	nvlist_alloc(&props, NV_UNIQUE_NAME, 0);
	for (i = 0; i < nopts; ++i) {
		do_set_property(id, opts[i].prop, opts[i].value);
		if (strcmp(opts[i].prop, "logkeep") == 0)
			nvlist_add_uint32(props, opts[i].prop, atoi(opts[i].value));
		else if (strcmp(opts[i].prop, "logsize") == 0)
			nvlist_add_uint64(props, opts[i].prop, strtoull(opts[i].value, NULL, 0));
		else
			nvlist_add_string(props, opts[i].prop, opts[i].value);
	}

	if (do_enable)
		(void) simple_command("enable",
		    "fmri", DATA_TYPE_STRING, id,
		    NULL);
	else if (schedule)
		(void) simple_command("schedule",
		    "fmri", DATA_TYPE_STRING, id,
		    "schedule", DATA_TYPE_STRING, schedule,
		    NULL);

	return (0);
}

static nvlist_t *
read_nvlist()
{
nvlist_t	*nvl;
uint32_t	 sz = 0;
char		*buf;
int		 flags = 0;
int		 i;
	if ((i = t_rcv(server_fd, &sz, sizeof(sz), &flags)) < sizeof(sz)) {
		if (i == 0)
			(void) fprintf(stderr, "unexpected EOF from server\n");
		else if (i == -1)
			(void) fprintf(stderr, "server read error: %s\n", strerror(errno));
		else
			(void) fprintf(stderr, "short read from server\n");
		exit(1);
	}
	sz = ntohl(sz);
	if ((buf = malloc(sz)) == NULL) {
		(void) fprintf(stderr, "out of memory\n");
		exit(1);
	}
	if ((i = t_rcv(server_fd, buf, sz, &flags)) < sz) {
		if (i == 0)
			(void) fprintf(stderr, "unexpected EOF from server\n");
		else if (i == -1)
			(void) fprintf(stderr, "server read error: %s\n", strerror(errno));
		else
			(void) fprintf(stderr, "short read from server (%d < %d)\n",
					(int) i, (int) sz);
		exit(1);
	}

	if (nvlist_unpack(buf, sz, &nvl, 0)) {
		(void) fprintf(stderr, "cannot unpack server data: %s\n",
				strerror(errno));
		exit(1);
	}

	if (debug) {
		(void) fprintf(stderr, "<< ");
		print_nvlist(stderr, nvl);
		(void) fprintf(stderr, "\n");
	}

	return nvl;
}

static void
print_nvlist(fl, nvl)
	FILE		*fl;
	nvlist_t	*nvl;
{
nvpair_t	*pair = NULL;
char		*strval;
uint16_t	 ui16val;
int16_t		 i16val;
uint32_t	 ui32val;
uint64_t	 ui64val;
nvlist_t	*nvarg;
nvlist_t	**nvlarg;
uint_t		 num, i;
boolean_t	 boolval;
	(void) fprintf(fl, "{ ");
	while ((pair = nvlist_next_nvpair(nvl, pair)) != NULL) {
		(void) fprintf(fl, "%s=", nvpair_name(pair));
		switch (nvpair_type(pair)) {
		case DATA_TYPE_STRING:
			nvpair_value_string(pair, &strval);
			(void) fprintf(fl, "\"%s\" ", strval);
			break;
		case DATA_TYPE_INT16:
			nvpair_value_int16(pair, &i16val);
			(void) fprintf(fl, "%"PRIi16" ", i16val);
			break;
		case DATA_TYPE_UINT16:
			nvpair_value_uint16(pair, &ui16val);
			(void) fprintf(fl, "%"PRIu16" ", ui16val);
			break;
		case DATA_TYPE_UINT32:
			nvpair_value_uint32(pair, &ui32val);
			(void) fprintf(fl, "%"PRIu32" ", ui32val);
			break;
		case DATA_TYPE_UINT64:
			nvpair_value_uint64(pair, &ui64val);
			(void) fprintf(fl, "%"PRIu64" ", ui64val);
			break;
		case DATA_TYPE_NVLIST:
			nvpair_value_nvlist(pair, &nvarg);
			print_nvlist(fl, nvarg);
			break;
		case DATA_TYPE_BOOLEAN_VALUE:
			nvpair_value_boolean_value(pair, &boolval);
			(void) fprintf(fl, "%s ",
			    boolval ? "true" : "false");
			break;
		case DATA_TYPE_NVLIST_ARRAY: {
			nvpair_value_nvlist_array(pair, &nvlarg, &num);
			(void) fprintf(fl, "[ ");
			for (i = 0; i < num; ++i) {
				print_nvlist(fl, nvlarg[i]);
				if ((i + 1) != num)
					(void) fprintf(fl, ",");
				(void) fprintf(fl, " ");
			}
			(void) fprintf(fl, "] ");
			break;
		}

		default:
			(void) fprintf(fl, "<unknown type> ");
			break;
		}
	}
	(void) fprintf(fl, "}");
}

static int
put_server(nvl)
	nvlist_t	*nvl;
{
size_t		len;
char		*buf = NULL;
uint32_t	sz;
int		ret;
	if (debug) {
		(void) fprintf(stderr, " >> ");
		print_nvlist(stderr, nvl);
		(void) fprintf(stderr, "\n");
	};

	if (nvlist_pack(nvl, &buf, &len, NV_ENCODE_XDR, 0))
		return -1;
	sz = htonl(len);
	ret = t_snd(server_fd, &sz, sizeof(sz), 0);
	if (ret != -1) 
		ret = t_snd(server_fd, buf, len, 0);
	free(buf);
	return (ret);
}

static void
vadd_nvargs(nvlist_t *nvl, va_list *ap)
{
char const	*name;
	while ((name = va_arg(*ap, char const *)) != NULL) {
		switch (va_arg(*ap, data_type_t)) {
		case DATA_TYPE_STRING:
			nvlist_add_string(nvl, name, va_arg(*ap, char const *));
			break;
		case DATA_TYPE_INT16:
			nvlist_add_int16(nvl, name, va_arg(*ap, int16_t));
			break;
		case DATA_TYPE_UINT16:
			nvlist_add_uint16(nvl, name, va_arg(*ap, uint16_t));
			break;
		case DATA_TYPE_UINT32:
			nvlist_add_uint32(nvl, name, va_arg(*ap, uint32_t));
			break;
		case DATA_TYPE_UINT64:
			nvlist_add_uint64(nvl, name, va_arg(*ap, uint64_t));
			break;
		case DATA_TYPE_NVLIST:
			nvlist_add_nvlist(nvl, name, va_arg(*ap, nvlist_t *));
			break;
		case DATA_TYPE_NVINLINE: {
		nvlist_t	*nv;
			nvlist_alloc(&nv, NV_UNIQUE_NAME, 0);
			vadd_nvargs(nv, ap);
			nvlist_add_nvlist(nvl, name, nv);
			break;
		}
		default:
			(void) fprintf(stderr, "simple_command: unknown data type\n");
			abort();
		}
	}
}

static nvlist_t *
simple_command(char const *cmd, ...)
{
nvlist_t	*nvl;
va_list		 ap;
char		*err;
	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) {
		(void) fprintf(stderr, "nvlist_alloc: %s\n",
				strerror(errno));
		exit(1);
	}

	nvlist_add_string(nvl, "command", cmd);
	va_start(ap, cmd);
	vadd_nvargs(nvl, &ap);
	va_end(ap);

	if (put_server(nvl) < 0) {
		(void) fprintf(stderr, "write to server: %s",
				strerror(errno));
		exit(1);
	}

	nvlist_free(nvl);
	nvl = read_nvlist();
	if (nvlist_lookup_string(nvl, "error", &err) == 0) {
		(void) fprintf(stderr, "%s\n", err);
		exit(1);
	}

	return nvl;
}
