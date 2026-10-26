/*
 * Portable compatibility function implementations.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"

#ifndef HAVE_GETPROGNAME
static char progname_storage[256];
static int progname_initialized = 0;

/*
 * Return program name stored during initialization.
 */
const char *
getprogname(void)
{

	if (progname_initialized == 0) {
		return "sws";
	}
	return progname_storage;
}

/*
 * Record program name, stripping directory path if present.
 */
void
setprogname(const char *name)
{
	const char *p;
	size_t len;

	if (name == NULL) {
		return;
	}

	if ((p = strrchr(name, '/')) != NULL) {
		p++;
	} else {
		p = name;
	}

	len = strlen(p);
	if (len >= sizeof(progname_storage)) {
		len = sizeof(progname_storage) - 1;
	}
	(void)memcpy(progname_storage, p, len);
	progname_storage[len] = '\0';
	progname_initialized = 1;
}
#endif /* !HAVE_GETPROGNAME */

#ifndef HAVE_STRLCPY
/*
 * Copy string src to buffer dst of size dsize. At most dsize-1 chars
 * will be copied. Always NUL-terminates if dsize > 0.
 */
size_t
strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc;
	size_t nleft;

	osrc = src;
	nleft = dsize;

	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0') {
				break;
			}
		}
	}

	if (nleft == 0) {
		if (dsize != 0) {
			*dst = '\0';
		}
		while (*src++) {
			;
		}
	}

	return (size_t)(src - osrc - 1);
}
#endif /* !HAVE_STRLCPY */

#ifndef HAVE_STRLCAT
/*
 * Appends src to string dst of size dsize (unlike strncat, dsize is the
 * full size of dst, not space left). At most dsize-1 chars will be in dst.
 */
size_t
strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst;
	const char *osrc;
	size_t dlen;
	size_t nleft;

	odst = dst;
	osrc = src;
	nleft = dsize;

	while (nleft-- != 0 && *dst != '\0') {
		dst++;
	}
	dlen = (size_t)(dst - odst);
	nleft = dsize - dlen;

	if (nleft == 0) {
		return dlen + strlen(src);
	}

	while (*src != '\0') {
		if (nleft != 1) {
			*dst++ = *src;
			nleft--;
		}
		src++;
	}
	*dst = '\0';

	return dlen + (size_t)(src - osrc);
}
#endif /* !HAVE_STRLCAT */

#if defined(__sun) && defined(__SVR4)
/*
 * Fallback timegm(3) for systems lacking native UTC mktime.
 */
time_t
timegm(struct tm *tm)
{
	char *old_tz;
	char tz_buf[256];
	time_t ret;

	old_tz = getenv("TZ");
	if (old_tz != NULL) {
		(void)strlcpy(tz_buf, old_tz, sizeof(tz_buf));
	}

	(void)setenv("TZ", "", 1);
	tzset();

	ret = mktime(tm);

	if (old_tz != NULL) {
		(void)setenv("TZ", tz_buf, 1);
	} else {
		(void)unsetenv("TZ");
	}
	tzset();

	return ret;
}
#endif
