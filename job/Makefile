CC		= cc
LINT		= lint
CPPFLAGS	= -D_XOPEN_SOURCE=500 -D__EXTENSIONS__ 
CFLAGS		= -xO0 -g -xc99=%none
LDFLAGS		= 
LINTFLAGS	= -asm -u -errhdr -errchk -Ncheck -Nlevel -errtags=yes -Xc99=%none -errsecurity=core
LIBS		= -lnsl -lcurses

OBJS	= job.o
SRCS	= $(OBJS:.o=.c)
PROG	= job

MANPAGES	= 	\
	job.1		\
	job_enable.1	\
	job_limit.1	\
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

.KEEP_STATE:
