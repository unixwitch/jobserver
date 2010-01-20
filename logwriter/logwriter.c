/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */
  
/*
 * logwriter: used to write the job log in $HOME/.job, and rotate it when
 * necessary.
 */

#pragma ident "@(#) $Id$"

#include	<sys/types.h>
#include	<sys/stat.h>

#include	<stdio.h>
#include	<stdlib.h>
#include	<fcntl.h>
#include	<alloca.h>
#include	<unistd.h>
#include	<string.h>

static int		outfile = -1;

/*
 * Reopen the log file, or fail silently if we can't.
 */
void
reopen(file)
	char const	*file;
{
	if (outfile != -1) {
		(void) close(outfile);
		outfile = -1;
	}

	/*LINTED*/
	if ((outfile = open(file, O_WRONLY | O_APPEND | O_CREAT, 0600)) == -1)
		return;
}

/*
 * Rotate the logfile and delete any extraneous versions.
 */
void
rotate(file, keep)
	char const	*file;
	int		 keep;
{
size_t	fs = strlen(file) + 32;
char	*fname = alloca(fs), *ofname = alloca(fs);

	(void) close(outfile);
	outfile = -1;

	/*
	 * Delete the last file.
	 */
	(void) snprintf(fname, fs, "%s.%d", file, keep);
	(void) unlink(fname);

	/*
	 * Rename the rest.
	 */
	while (keep--) {
		(void) snprintf(fname, fs, "%s.%d", file, keep);
		(void) snprintf(ofname, fs, "%s.%d", file, keep - 1);
		(void) rename(ofname, fname);
	}
	
	/*
	 * Rename the current file to .0
	 */
	(void) snprintf(fname, fs, "%s.0", file);
	(void) rename(file, fname);

	/*
	 * And finally, reopen the log.
	 */
	reopen(file);
}

int
main(argc, argv)
	int	  argc;
	char	**argv;
{
off_t		maxsize;
int		keep;
static char	buf[1024];
ssize_t		nbytes, nwrt;
time_t		lastcheck = 0;
static const int check_every = 30;

	if (argc != 4) {
		(void) fprintf(stderr, "usage: %s <file> <maxsize> <keep>\n", argv[0]);
		return 1;
	}

	maxsize = atoi(argv[2]);
	keep = atoi(argv[3]);

	reopen(argv[1]);

	for (;;) {
		/*
		 * Exit on input failure.
		 */
		if ((nbytes = read(STDIN_FILENO, buf, sizeof buf)) < 1)
			return 0;

		if (outfile == -1)
			reopen(argv[1]);

		/*
		 * Discard output on write failure and try again.
		 */
		while (nbytes) {
			if ((nwrt = write(outfile, buf, nbytes)) == -1)
				break;
			nbytes -= nwrt;
		}

		if (lastcheck + check_every < time(NULL)) {
		struct stat	sb;

			lastcheck = time(NULL);

			if (fstat(outfile, &sb) == -1)
				continue;

			if (sb.st_size >= maxsize)
				rotate(argv[1], keep);
		}
	}
}
