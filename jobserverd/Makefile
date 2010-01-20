CC		= cc
LINT		= lint
CPPFLAGS	= -DPREFIX=\"/opt/jobserver\"	\
		  -D_XOPEN_SOURCE=500		\
		  -D__EXTENSIONS__		\
		  -D_LARGEFILE64_SOURCE		\
		  -I/opt/ts/include/db-4.7
CFLAGS		= -xO0 -g -xc99=%none
LDFLAGS		= -L/opt/ts/lib -R/opt/ts/lib
#LINTFLAGS	= -a -s -m -u -errchk=%all -Ncheck=%all -Nlevel=4 -errtags=yes -errsecurity=core
LINTFLAGS	= -asxmu -errchk -errtags=yes -Xc99=none -errsecurity=core
LIBS		= -lnsl -lrt -lproject -lcontract -lnvpair -lcmd -ldb-4.7

OBJS	= main.o fd.o ctl.o buffer.o state.o sched.o event.o execute.o
SRCS	= $(OBJS:.o=.c)
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

clean:
	rm -f $(OBJS) $(PROG)

install:
	ginstall -o root -g root -d $(DESTDIR)/opt/jobserver/lib
	ginstall -o root -g root -m 0710 jobserverd $(DESTDIR)/opt/jobserver/lib
	ginstall -d $(DESTDIR)/var/svc/manifest/system
	ginstall -o root -g root -m 0644 jobserver.xml $(DESTDIR)/var/svc/manifest/system

.KEEP_STATE:
