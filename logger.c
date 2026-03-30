#include "server.h"
#include <stdarg.h>


pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;


void log_event(const char *fmt, ...) {
pthread_mutex_lock(&log_mutex);
FILE *f = fopen("logs/run.log", "a");
if (!f) {
perror("fopen logs/run.log");
pthread_mutex_unlock(&log_mutex);
return;
}
time_t t = time(NULL);
char timestr[64];
strftime(timestr, sizeof(timestr), "%F %T", localtime(&t));
fprintf(f, "[%s] ", timestr);
va_list ap;
va_start(ap, fmt);
vfprintf(f, fmt, ap);
va_end(ap);
fprintf(f, "\n");
fclose(f);
pthread_mutex_unlock(&log_mutex);
}
