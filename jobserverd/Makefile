#
# Copyright 2010 River Tarnell.  All rights reserved.
# Use is subject to license terms.
#
CC		= cc
LINT		= lint
CSTYLE		= /opt/onbld/bin/cstyle
HDRCHK		= /opt/onbld/bin/hdrchk -a
CPPFLAGS	= -DPREFIX=\"/opt/jobserver\"	\
		  -D_XOPEN_SOURCE=500		\
		  -D__EXTENSIONS__		\
		  -D_LARGEFILE64_SOURCE
CFLAGS		= -xO0 -g -xc99=%none
LDFLAGS		= 
LIBS		= -lnsl -lrt -lproject -lcontract -lnvpair -lcmd -lsecdb 
#LINTFLAGS	= -a -s -m -u -errchk=%all -Ncheck=%all -Nlevel=4 -errtags=yes -errsecurity=core
LINTFLAGS	= -asxmu -errchk=%all,no%longptr64 -errtags=yes -Xc99=none -errsecurity=core -erroff=E_EQUALITY_NOT_ASSIGNMENT
CSTYLEFLAGS	= -cpP
OBJS	= main.o fd.o ctl.o buffer.o state.o sched.o event.o execute.o ct.o kvdb.o jerrno.o
SRCS	= $(OBJS:.o=.c)
HDRS	= buffer.h ctl.h execute.h jobserver.h state.h ct.h event.h fd.h sched.h kvdb.h jerrno.h
PROG	= jobserverd

default: all
all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $(PROG) $(LIBS)

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

buftest: buffer.c
	$(CC) $(CPPFLAGS) -DTEST $(CFLAGS) $(LDFLAGS) buffer.c -o $@

lint:
	$(LINT) $(CPPFLAGS) $(LINTFLAGS) $(SRCS)

style:
	$(CSTYLE) $(CSTYLEFLAGS) $(SRCS) $(HDRS)
	$(HDRCHK) $(HDRS)

clean:
	rm -f $(OBJS) $(PROG)

install:
	ginstall -o root -g root -d $(DESTDIR)/opt/jobserver/lib
	ginstall -o root -g root -m 0710 jobserverd $(DESTDIR)/opt/jobserver/lib
	ginstall -d $(DESTDIR)/var/svc/manifest/system
	ginstall -o root -g root -m 0644 jobserver.xml $(DESTDIR)/var/svc/manifest/system

.KEEP_STATE:
