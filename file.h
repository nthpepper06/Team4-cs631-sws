/*
 * File resolution, directory indexing, and MIME detection header.
 */

#ifndef _FILE_H_
#define _FILE_H_

#include "http.h"

int file_init(void);
void file_cleanup(void);
int file_serve_request(int, const struct http_request *, const char *);

#endif /* !_FILE_H_ */
