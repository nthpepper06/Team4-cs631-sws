/*
 * CGI execution header (RFC 3875).
 */

#ifndef _CGI_H_
#define _CGI_H_

#include "http.h"

int cgi_is_cgi_request(const struct http_request *);
int cgi_execute_request(int, const struct http_request *, const char *);

#endif /* !_CGI_H_ */
