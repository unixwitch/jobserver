#
# Copyright 2010 River Tarnell.  All rights reserved.
# Use is subject to license terms.
#
CC		= cc
LINT		= lint
CSTYLE          = /opt/onbld/bin/cstyle
CPPFLAGS	= -D_XOPEN_SOURCE=500 -D__EXTENSIONS__ 
CFLAGS		= -xO0 -g -xc99=%none
LDFLAGS		= 
LIBS		= -lnsl -lcurses
LINTFLAGS	= -axsmu -errchk=%all,no%longptr64 -errhdr=%user -errtags=yes -Ncheck -Nlevel -errtags=yes -Xc99=%none -errsecurity=core
#LINTFLAGS	= -asxmu -erroff=E_EQUALITY_NOT_ASSIGNMENT
CSTYLEFLAGS	= -cpP

OBJS	= job.o
SRCS	= $(OBJS:.o=.c)
PROG	= job

MANPAGES	= 	\
	job.1		\
	job_enable.1	\
	job_schedule.1	\
	job_show.1	\
	job_add.1	\
	job_intro.1	\
	job_quota.1	\
	job_set.1

default: all
all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $(PROG) $(LIBS)

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

lint:
	$(LINT) $(CPPFLAGS) $(LINTFLAGS) $(SRCS)

clean:
	rm -f $(OBJS) $(PROG)

install:
	ginstall -o root -g root -d $(DESTDIR)/opt/jobserver/bin
	ginstall -o root -g root -m 0711 job $(DESTDIR)/opt/jobserver/bin
	ginstall -o root -g root -d $(DESTDIR)/opt/jobserver/man/man1
	for m in $(MANPAGES); do \
		ginstall -o root -g root -m 0644 $$m $(DESTDIR)/opt/jobserver/man/man1; \
	done

style:
	$(CSTYLE) $(CSTYLEFLAGS) $(SRCS) $(HDRS)

.KEEP_STATE:
