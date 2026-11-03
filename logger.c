#define _GNU_SOURCE
#include "server.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log = NULL;

/* Opened once (not per call) and with O_CLOEXEC ("e"), so the untrusted
 * programs we fork never inherit the log file descriptor. */
int log_init(const char *path) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    pthread_mutex_lock(&g_log_mutex);
    g_log = fopen(path, "ae");
    pthread_mutex_unlock(&g_log_mutex);
    return g_log ? 0 : -1;
}

void log_event(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* Client-controlled text (e.g. the language name) ends up in messages. Neutralise
     * control characters so a client cannot forge extra log lines with "\n". */
    for (char *p = msg; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = '?';

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);                       /* thread-safe, unlike localtime() */
    char ts[32];
    strftime(ts, sizeof ts, "%F %T", &tmv);

    pthread_mutex_lock(&g_log_mutex);
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "[%s] %s\n", ts, msg);
    fflush(out);
    pthread_mutex_unlock(&g_log_mutex);
}

void log_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log) { fclose(g_log); g_log = NULL; }
    pthread_mutex_unlock(&g_log_mutex);
}
