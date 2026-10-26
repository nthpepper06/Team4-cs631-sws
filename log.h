/*
 * Logging subsystem header.
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <sys/types.h>
#include <time.h>

int log_init(const char *, int);
void log_request(const char *, time_t, const char *, int, off_t);
void log_close(void);

#endif /* !_LOG_H_ */
