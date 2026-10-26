#
# Makefile for sws - Simple Web Server
# CS631 Advanced Programming in the UNIX Environment
#

PROG =		sws
SRCS =		cgi.c compat.c file.c http.c log.c net.c sws.c
OBJS =		$(SRCS:.c=.o)

CC ?=		cc

# Base strict compiler warnings per course guidelines
CFLAGS ?=	-Wall -Werror -Wextra -Wformat=2 -Wpedantic -Wshadow
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

check:
	@echo "All source files: $(SRCS)"
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"

.PHONY: all clean check
