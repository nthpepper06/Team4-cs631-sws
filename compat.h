/*
 * Compatibility definitions for portable UNIX development.
 * Supports NetBSD 11.0, Linux, and OmniOS.
 */

#ifndef _COMPAT_H_
#define _COMPAT_H_

#include <sys/types.h>
#include <stddef.h>
#include <time.h>

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
#define HAVE_GETPROGNAME 1
#endif

#ifndef HAVE_STRLCPY
size_t strlcpy(char *, const char *, size_t);
#endif

#ifndef HAVE_STRLCAT
size_t strlcat(char *, const char *, size_t);
#endif

#ifndef HAVE_GETPROGNAME
const char *getprogname(void);
void setprogname(const char *);
#endif

#if defined(__sun) && defined(__SVR4)
time_t timegm(struct tm *);
#endif

#endif /* !_COMPAT_H_ */
