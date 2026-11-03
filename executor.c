/*
 * executor.c - compiles (C) and runs (C / Python) one submitted program.
 *
 * Design rules that fix the original version's bugs:
 *
 *  1. NO process-wide state. The original used a global child_pid plus
 *     alarm()/SIGALRM, which are per-PROCESS, so two concurrent requests
 *     overwrote each other's timer and PID. Here every request polls its own
 *     pipe against its own deadline, using only local variables.
 *
 *  2. NO SIGCHLD handler. A global waitpid(-1) reaper can steal the exit status
 *     of the child another thread is waiting for. Each thread now reaps exactly
 *     its own child with waitpid(pid).
 *
 *  3. The child gets its own PROCESS GROUP and the whole group is SIGKILLed.
 *     Killing only the direct child leaves grandchildren (gcc, the compiled
 *     program, anything the user's code forks) alive and holding the pipe open.
 *
 *  4. No shell. gcc / python3 are exec'd directly with an argv array, so there
 *     is no "bash -c" command string to get wrong.
 *
 *  5. Compile and run are separate stages with separate limits: gcc legitimately
 *     needs far more memory/CPU than the 128 MB / 3 s we allow the user's program.
 *
 *  6. All scratch files live in one per-job directory (mkdtemp) that is removed
 *     on EVERY exit path, including timeouts and compile errors.
 *
 *  Between fork() and exec() in a multithreaded process only async-signal-safe
 *  calls are allowed (no malloc, no stdio); everything the child needs is
 *  prepared before fork().
 */
#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SAFE_PATH "/usr/local/bin:/usr/bin:/bin"

/* ------------------------------------------------------------ configuration */
typedef struct {
    unsigned cpu_sec, wall_sec;
    rlim_t   mem, fsize, nproc, nofile;
} limits_t;

static const limits_t COMPILE_LIMITS = {
    COMPILE_CPU_SEC, COMPILE_WALL_SEC, COMPILE_MEM_BYTES, 64ULL << 20, 256, 128
};
static const limits_t RUN_LIMITS = {
    RUN_CPU_SEC, RUN_WALL_SEC, RUN_MEM_BYTES, RUN_FSIZE_BYTES, 64, 32
};

/* Adding a language = adding one row. argv[0] is looked up in SAFE_PATH, except
 * "./name" which means "a file inside the job directory". */
typedef struct {
    const char        *name;
    const char        *aliases[3];
    const char        *src_file;
    const char *const *compile_argv;     /* NULL for interpreted languages */
    const char *const *run_argv;
} lang_spec_t;

static const char *const C_COMPILE[] = { "gcc", "-O1", "-pipe", "main.c", "-o", "prog", "-lm", NULL };
static const char *const C_RUN[]     = { "./prog", NULL };
/* -u: unbuffered, so output printed before a timeout/kill is not lost.
 * -I: isolated mode (ignore PYTHON* env vars and the user's site dir).   */
static const char *const PY_RUN[]    = { "python3", "-u", "-B", "-I", "main.py", NULL };

static const lang_spec_t LANGS[] = {
    { "C",      { NULL },               "main.c",  C_COMPILE, C_RUN  },
    { "Python", { "py", "python3" },    "main.py", NULL,      PY_RUN },
};

static const lang_spec_t *find_lang(const char *lang) {
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) {
        if (strcasecmp(lang, LANGS[i].name) == 0) return &LANGS[i];
        for (int a = 0; a < 3 && LANGS[i].aliases[a]; a++)
            if (strcasecmp(lang, LANGS[i].aliases[a]) == 0) return &LANGS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ helpers */
static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int resolve_exe(const char *dir, const char *name, char *out, size_t cap) {
    if (name[0] == '.' && name[1] == '/') {
        snprintf(out, cap, "%s/%s", dir, name + 2);
        return access(out, X_OK) == 0 ? 0 : -1;
    }
    char path[] = SAFE_PATH;
    char *save = NULL;
    for (char *d = strtok_r(path, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(out, cap, "%s/%s", d, name);
        if (access(out, X_OK) == 0) return 0;
    }
    return -1;
}

static int rm_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    return remove(path);
}
static void remove_tree(const char *dir) {
    nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static void set_text(run_result_t *r, run_state_t st, int code, const char *text) {
    free(r->output);
    r->state = st;
    r->code = code;
    r->output = strdup(text);
    r->out_len = r->output ? strlen(r->output) : 0;
    if (!r->output) { r->output = calloc(1, 1); r->out_len = 0; }
}

void run_result_free(run_result_t *r) {
    free(r->output);
    r->output = NULL;
    r->out_len = 0;
}

/* ------------------------------------------------------------ one stage run */
typedef struct {
    char  *buf;                 /* captured stdout+stderr, NUL-terminated */
    size_t len;
    int    timed_out, truncated;
    int    exited, exit_code;
    int    signaled, term_sig;
} stage_t;

static void child_die(const char *msg) {            /* async-signal-safe */
    ssize_t r = write(2, msg, strlen(msg));
    (void)r;
    _exit(127);
}

static void set_rlim(int res, rlim_t v) {
    struct rlimit r = { v, v };
    if (setrlimit(res, &r) != 0) child_die("rcr: setrlimit failed\n");
}

/* Runs in the forked child. Only async-signal-safe calls. Never returns. */
static void child_exec(const char *dir, const char *exe, char *const argv[], char *const envp[],
                       const limits_t *lim, int out_fd) {
    setpgid(0, 0);                                   /* own process group -> group kill works */

    int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nul >= 0 && nul != 0) dup2(nul, 0);
    else if (nul == 0) fcntl(0, F_SETFD, 0);
    if (dup2(out_fd, 1) < 0 || dup2(out_fd, 2) < 0) child_die("rcr: dup2 failed\n");

    if (chdir(dir) != 0) child_die("rcr: chdir failed\n");
    umask(022);

    int drop = (geteuid() == 0);
    set_rlim(RLIMIT_CORE, 0);
    {   struct rlimit r = { lim->cpu_sec, lim->cpu_sec + 1 };     /* SIGXCPU, then SIGKILL */
        if (setrlimit(RLIMIT_CPU, &r) != 0) child_die("rcr: setrlimit cpu failed\n"); }
    set_rlim(RLIMIT_AS, lim->mem);
    set_rlim(RLIMIT_FSIZE, lim->fsize);
    set_rlim(RLIMIT_NOFILE, lim->nofile);
    if (drop) set_rlim(RLIMIT_NPROC, lim->nproc);   /* only meaningful for a dedicated uid */

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    if (drop) {
        unshare(CLONE_NEWNET);                       /* best effort: no network for the job */
        if (setgroups(0, NULL) != 0 || setgid(SANDBOX_GID) != 0 || setuid(SANDBOX_UID) != 0)
            child_die("rcr: cannot drop privileges\n");
        if (setuid(0) == 0) child_die("rcr: privileges could be regained\n");
    }

    execve(exe, argv, envp);
    child_die("rcr: exec failed\n");
}

static int drain_pipe(int fd, stage_t *st) {
    /* 0 = no more data right now, 1 = EOF, 2 = output cap exceeded */
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            size_t room = MAX_OUTPUT_BYTES - st->len;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(st->buf + st->len, tmp, take);
            st->len += take;
            if ((size_t)n > take) { st->truncated = 1; return 2; }
            continue;
        }
        if (n == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return 1;
    }
}

/* Returns 0 on success (inspect *st), -1 if the stage could not even be started. */
static int run_stage(const char *dir, const char *const *argv_src, const limits_t *lim, stage_t *st) {
    memset(st, 0, sizeof *st);
    st->buf = malloc(MAX_OUTPUT_BYTES + 1);
    if (!st->buf) return -1;

    /* ---- everything the child needs is built BEFORE fork (no malloc after) ---- */
    char exe[PATH_MAX];
    if (resolve_exe(dir, argv_src[0], exe, sizeof exe) != 0) {
        snprintf(st->buf, MAX_OUTPUT_BYTES, "rcr: '%s' not found on this server\n", argv_src[0]);
        st->len = strlen(st->buf);
        st->exited = 1; st->exit_code = 127;
        return 0;
    }
    char *argv[16];
    int ac = 0;
    for (; argv_src[ac] && ac < 15; ac++) argv[ac] = (char *)argv_src[ac];
    argv[ac] = NULL;

    static char e_path[] = "PATH=" SAFE_PATH;
    static char e_lang[] = "LANG=C.UTF-8";
    static char e_pyc[]  = "PYTHONDONTWRITEBYTECODE=1";
    char e_home[PATH_MAX + 8], e_tmp[PATH_MAX + 8];
    snprintf(e_home, sizeof e_home, "HOME=%s", dir);
    snprintf(e_tmp,  sizeof e_tmp,  "TMPDIR=%s", dir);
    char *envp[] = { e_path, e_lang, e_pyc, e_home, e_tmp, NULL };   /* deliberately minimal */

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) child_exec(dir, exe, argv, envp, lim, pfd[1]);

    /* ------------------------------------------------------------- parent */
    setpgid(pid, pid);                               /* also done by the child; avoids a race */
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    const long long deadline = monotonic_ms() + (long long)lim->wall_sec * 1000;
    int eof = 0;
    siginfo_t info;

    for (;;) {
        long long left = deadline - monotonic_ms();
        if (left <= 0) { st->timed_out = 1; break; }

        if (!eof) {
            struct pollfd p = { .fd = pfd[0], .events = POLLIN, .revents = 0 };
            int to = left < 50 ? (int)left + 1 : 50;
            int pr = poll(&p, 1, to);
            if (pr > 0) {
                int d = drain_pipe(pfd[0], st);
                if (d == 2) break;                   /* output flood: stop it */
                if (d == 1) eof = 1;
            }
        } else {
            usleep(5000);                            /* stdout closed but process may still run */
        }

        /* Has the child exited? WNOWAIT keeps the zombie, so its PID (= our
         * process-group id) cannot be recycled before we kill the group. */
        info.si_pid = 0;
        if (waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid) break;
    }

    /* Whatever the reason we left the loop, make sure nothing from this job survives:
     * stragglers (e.g. a background process) would keep the pipe open and keep running. */
    kill(-pid, SIGKILL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }

    /* All writers are dead now: collect what is left in the pipe. */
    if (!st->truncated) {
        for (int i = 0; i < 20; i++) {
            int d = drain_pipe(pfd[0], st);
            if (d != 0) break;
            usleep(2000);
        }
    }
    close(pfd[0]);

    st->buf[st->len] = '\0';
    if (!st->timed_out && !st->truncated) {
        if (WIFEXITED(status))        { st->exited = 1;   st->exit_code = WEXITSTATUS(status); }
        else if (WIFSIGNALED(status)) { st->signaled = 1; st->term_sig  = WTERMSIG(status); }
    }
    return 0;
}

static void stage_free(stage_t *st) { free(st->buf); st->buf = NULL; }

static void stage_to_result(run_result_t *r, stage_t *st, run_state_t state, int code, const char *note) {
    free(r->output);
    size_t nlen = note ? strlen(note) : 0;
    char *out = realloc(st->buf, st->len + nlen + 1);
    if (!out) { out = st->buf; nlen = 0; }          /* realloc failed: keep the output, drop the note */
    if (nlen) memcpy(out + st->len, note, nlen);
    out[st->len + nlen] = '\0';
    r->output = out;
    r->out_len = st->len + nlen;                    /* explicit length: output may contain NUL bytes */
    r->state = state;
    r->code = code;
    st->buf = NULL;                                 /* ownership moved to the result */
}

/* ------------------------------------------------------------- public API */
void executor_init(void) {
    char tmp[PATH_MAX];
    if (geteuid() == 0)
        log_event("sandbox: running as root -> jobs execute as uid %d with RLIMIT_NPROC", SANDBOX_UID);
    else
        log_event("sandbox: NOT root -> jobs run as the server's own user (dev mode). "
                  "Run as root or inside a container for uid separation");

    if (geteuid() == 0) {
        pid_t p = fork();
        if (p == 0) _exit(unshare(CLONE_NEWNET) == 0 ? 0 : 1);
        int s = 0;
        if (p > 0 && waitpid(p, &s, 0) == p && WIFEXITED(s) && WEXITSTATUS(s) == 0)
            log_event("sandbox: network namespace isolation AVAILABLE");
        else
            log_event("sandbox: network namespace isolation UNAVAILABLE (jobs can reach the network)");
    }
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) {
        const char *prog = LANGS[i].compile_argv ? LANGS[i].compile_argv[0] : LANGS[i].run_argv[0];
        if (resolve_exe("/", prog, tmp, sizeof tmp) != 0)
            log_event("WARNING: '%s' not found in %s - %s requests will fail", prog, SAFE_PATH, LANGS[i].name);
    }
}

void execute_code(const char *lang, const char *code, size_t code_len, run_result_t *res) {
    memset(res, 0, sizeof *res);

    const lang_spec_t *ls = find_lang(lang);
    if (!ls) { set_text(res, RS_UNSUPPORTED, 0, "Unsupported language. Supported: C, Python\n"); return; }

    stage_t cs = {0}, rs = {0};
    char dir[64];
    snprintf(dir, sizeof dir, "%s/rcr-XXXXXX", JOB_ROOT);
    if (!mkdtemp(dir)) { set_text(res, RS_INTERNAL, 0, "internal error: cannot create job directory\n"); return; }

    if (geteuid() == 0 && chown(dir, SANDBOX_UID, SANDBOX_GID) != 0) {
        set_text(res, RS_INTERNAL, 0, "internal error: chown failed\n");
        goto cleanup;
    }

    char src[PATH_MAX];
    snprintf(src, sizeof src, "%s/%s", dir, ls->src_file);
    int fd = open(src, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 || write_all(fd, code, code_len) != 0) {
        if (fd >= 0) close(fd);
        set_text(res, RS_INTERNAL, 0, "internal error: cannot write source file\n");
        goto cleanup;
    }
    if (geteuid() == 0 && fchown(fd, SANDBOX_UID, SANDBOX_GID) != 0) {
        close(fd);
        set_text(res, RS_INTERNAL, 0, "internal error: chown of source file failed\n");
        goto cleanup;
    }
    close(fd);

    /* -------- stage 1: compile (compiled languages only) -------- */
    if (ls->compile_argv) {
        if (run_stage(dir, ls->compile_argv, &COMPILE_LIMITS, &cs) != 0) {
            set_text(res, RS_INTERNAL, 0, "internal error: could not start the compiler\n");
            goto cleanup;
        }
        if (cs.timed_out) {
            stage_to_result(res, &cs, RS_COMPILE_ERROR, 124, "\n[compilation timed out]\n");
            goto cleanup;
        }
        if (cs.truncated) {
            stage_to_result(res, &cs, RS_COMPILE_ERROR, 1, "\n[compiler output truncated]\n");
            goto cleanup;
        }
        if (cs.signaled || cs.exit_code != 0) {
            stage_to_result(res, &cs, RS_COMPILE_ERROR, cs.signaled ? 128 + cs.term_sig : cs.exit_code, NULL);
            goto cleanup;
        }
        /* compiler warnings (exit 0) are intentionally dropped; only errors are reported */
    }

    /* -------- stage 2: run -------- */
    if (run_stage(dir, ls->run_argv, &RUN_LIMITS, &rs) != 0) {
        set_text(res, RS_INTERNAL, 0, "internal error: could not start the program\n");
        goto cleanup;
    }
    if (rs.timed_out)       stage_to_result(res, &rs, RS_TIMEOUT, 0, NULL);
    else if (rs.truncated)  stage_to_result(res, &rs, RS_OUTPUT_LIMIT, 0, NULL);
    else if (rs.signaled)   stage_to_result(res, &rs, RS_SIGNALED, rs.term_sig, NULL);
    else                    stage_to_result(res, &rs, RS_EXITED, rs.exit_code, NULL);

cleanup:
    stage_free(&cs);
    stage_free(&rs);
    remove_tree(dir);                                /* runs on EVERY path - no temp-file leaks */
}
