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

.KEEP_STATE:
