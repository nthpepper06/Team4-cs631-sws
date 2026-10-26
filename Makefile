#
# Makefile for sws - Simple Web Server
# CS631 Advanced Programming in the UNIX Environment
#

PROG =		sws
SRCS =		cgi.c compat.c file.c http.c log.c net.c sws.c
HDRS =		cgi.h compat.h file.h http.h log.h net.h sws.h
OBJS =		$(SRCS:.c=.o)
DOCS =		Makefile README checklist sws.1 test.sh
DISTFILES =	$(SRCS) $(HDRS) $(DOCS)

CC ?=		cc
USERS ?=	user1-user2-user3

# Base strict compiler warnings per course guidelines
CFLAGS ?=	-Wall -Werror -Wextra -Wformat=2 -Wno-format-truncation -Wpedantic -Wshadow
CPPFLAGS +=	-D_GNU_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700

# OS-specific library handling (OmniOS / SunOS requires -lsocket -lnsl)
SYS !=		uname -s 2>/dev/null || echo Unknown
LDLIBS_SunOS =	-lsocket -lnsl
LDLIBS +=	-lmagic $(LDLIBS_$(SYS)) $(NETLIBS)

.SUFFIXES: .c .o

all: $(PROG)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

clean:
	rm -f $(PROG) $(OBJS) *.core core *~
	rm -rf $(USERS) $(USERS).tar

check:
	@echo "All source files: $(SRCS)"
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"

dist: clean
	mkdir -p $(USERS)
	cp -p $(DISTFILES) $(USERS)/
	git log > $(USERS)/git-log.txt 2>/dev/null || true
	tar -cf $(USERS).tar $(USERS)
	rm -rf $(USERS)
	@echo "Created $(USERS).tar extracting into $(USERS)/"

tar: dist

.PHONY: all clean check dist tar
