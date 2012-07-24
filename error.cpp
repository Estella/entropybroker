#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <syslog.h>

void error_exit(const char *format, ...)
{
	char buffer[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	fprintf(stderr, "FATAL|%s: errno=%d (if applicable) -> %s\n", buffer, errno, strerror(errno));
	syslog(LOG_ERR, "FATAL|%s: %m", buffer);

	exit(EXIT_FAILURE);
}
