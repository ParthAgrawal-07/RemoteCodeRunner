/*
 * server.h - declarations and tunables shared by the server-side modules.
 */
#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <sys/types.h>
#include "protocol.h"

/* ---------------------------------------------------------------- tunables */
#define DEFAULT_MAX_CLIENTS       8          /* concurrently served clients        */
#define DEFAULT_READ_TIMEOUT_SEC  10         /* total time a client has to send a request */
#define SEND_TIMEOUT_SEC          10
#define LISTEN_BACKLOG            128

#define COMPILE_CPU_SEC           10         /* compiler limits (gcc needs room)   */
#define COMPILE_WALL_SEC          15
#define COMPILE_MEM_BYTES         (1024ULL << 20)

#define RUN_CPU_SEC               3          /* limits for the user's program      */
#define RUN_WALL_SEC              5
#define RUN_MEM_BYTES             (128ULL << 20)
#define RUN_FSIZE_BYTES           (8ULL << 20)

#define SANDBOX_UID               65534      /* "nobody": used when server runs as root */
#define SANDBOX_GID               65534
#define JOB_ROOT                  "/tmp"     /* per-job scratch directories live here */
#define LOG_PATH                  "logs/run.log"

/* ------------------------------------------------------------------ executor */
typedef struct {
    run_state_t state;
    int         code;          /* exit status / signal number, see protocol.h */
    char       *output;        /* malloc'd, always NUL-terminated             */
    size_t      out_len;
} run_result_t;

void executor_init(void);      /* logs which isolation features are available */
void execute_code(const char *lang, const char *code, size_t code_len, run_result_t *res);
void run_result_free(run_result_t *res);

/* -------------------------------------------------------------------- logger */
int  log_init(const char *path);
void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_close(void);

#endif
