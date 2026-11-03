/*
 * server.c - multi-threaded TCP front end of the Remote Code Runner.
 *
 *   ./server [port] [max_clients] [read_timeout_sec]
 *
 * Concurrency model: one detached thread per connection, bounded by a counting
 * semaphore that is taken BEFORE accept(). When all slots are busy the server
 * simply stops accepting; new clients wait in the kernel's listen backlog, so
 * we get a request queue and back-pressure without any queue code.
 *
 * Because bounded slots make "connect and stay silent" an easy denial of
 * service, every request must arrive within a TOTAL read deadline.
 */
#define _GNU_SOURCE
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static sem_t g_slots;
static int g_read_timeout_ms = DEFAULT_READ_TIMEOUT_SEC * 1000;
static atomic_ulong g_next_req = 1;

typedef struct {
    int           fd;
    char          ip[INET_ADDRSTRLEN];
    int           port;
    unsigned long id;
} conn_t;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int send_err(int fd, const char *reason) {
    char b[96];
    int n = snprintf(b, sizeof b, "ERR %s\n", reason);
    return send_all(fd, b, (size_t)n);
}

static void handle_client(conn_t *c) {
    const int fd = c->fd;

    struct timeval snd = { SEND_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* ---- 1. header line: "RUN <lang> <nbytes>" ---- */
    char line[MAX_LINE];
    if (recv_line(fd, line, sizeof line, g_read_timeout_ms) < 0) {
        log_event("req=%lu %s:%d no valid request line (timeout, disconnect or overlong line)",
                  c->id, c->ip, c->port);
        send_err(fd, "BAD_REQUEST");
        return;
    }

    char cmd[8], lang[32], extra[2];
    unsigned long long nbytes = 0;
    int fields = sscanf(line, "%7s %31s %llu %1s", cmd, lang, &nbytes, extra);
    if (fields != 3 || strcmp(cmd, "RUN") != 0 || nbytes == 0) {
        log_event("req=%lu %s:%d malformed request: %.60s", c->id, c->ip, c->port, line);
        send_err(fd, "BAD_REQUEST");
        return;
    }
    if (nbytes > MAX_CODE_BYTES) {
        log_event("req=%lu %s:%d rejected: %llu bytes exceeds limit %d",
                  c->id, c->ip, c->port, nbytes, MAX_CODE_BYTES);
        send_err(fd, "TOO_LARGE");
        return;
    }

    /* ---- 2. exactly nbytes of source code ---- */
    char *code = malloc((size_t)nbytes + 1);
    if (!code) { send_err(fd, "INTERNAL"); return; }
    if (recv_all(fd, code, (size_t)nbytes, g_read_timeout_ms) < 0) {
        log_event("req=%lu %s:%d incomplete code (wanted %llu bytes)", c->id, c->ip, c->port, nbytes);
        send_err(fd, "INCOMPLETE");
        free(code);
        return;
    }
    code[nbytes] = '\0';
    log_event("req=%lu %s:%d lang=%s bytes=%llu", c->id, c->ip, c->port, lang, nbytes);

    /* ---- 3. run it ---- */
    long long t0 = monotonic_ms();
    run_result_t res;
    execute_code(lang, code, (size_t)nbytes, &res);
    long long ms = monotonic_ms() - t0;
    free(code);

    /* ---- 4. one header + exact-length body ---- */
    char hdr[96];
    int hl = snprintf(hdr, sizeof hdr, "RESULT %s %d %zu\n", run_state_name(res.state), res.code, res.out_len);
    char *msg = malloc((size_t)hl + res.out_len);
    if (msg) {
        memcpy(msg, hdr, (size_t)hl);
        memcpy(msg + hl, res.output, res.out_len);
        if (send_all(fd, msg, (size_t)hl + res.out_len) < 0)
            log_event("req=%lu client went away before the result was delivered", c->id);
        free(msg);
    }
    log_event("req=%lu result=%s code=%d output=%zuB time=%lldms",
              c->id, run_state_name(res.state), res.code, res.out_len, ms);
    run_result_free(&res);
}

static void *thread_main(void *arg) {
    conn_t *c = arg;
    handle_client(c);
    close(c->fd);
    log_event("req=%lu connection closed", c->id);
    free(c);
    sem_post(&g_slots);                              /* release the slot on every path */
    return NULL;
}

static int parse_int(const char *s, int lo, int hi, const char *what) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        fprintf(stderr, "invalid %s '%s' (expected %d..%d)\n", what, s, lo, hi);
        exit(2);
    }
    return (int)v;
}

int main(int argc, char **argv) {
    int port        = argc > 1 ? parse_int(argv[1], 1, 65535, "port") : DEFAULT_PORT;
    int max_clients = argc > 2 ? parse_int(argv[2], 1, 1024, "max_clients") : DEFAULT_MAX_CLIENTS;
    if (argc > 3) g_read_timeout_ms = parse_int(argv[3], 1, 3600, "read_timeout_sec") * 1000;

    /* A client that disconnects mid-write must produce EPIPE, not kill the server.
     * (send_all also uses MSG_NOSIGNAL; this is the second line of defence.)
     * SIGCHLD is deliberately left at its default: the executor reaps its own children. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                       /* no SA_RESTART: accept()/sem_wait() return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (log_init(LOG_PATH) != 0) fprintf(stderr, "warning: cannot open %s, logging to stderr\n", LOG_PATH);
    executor_init();

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);   /* CLOEXEC: jobs never inherit it */
    if (lfd < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, LISTEN_BACKLOG) < 0) { perror("listen"); return 1; }

    sem_init(&g_slots, 0, (unsigned)max_clients);
    printf("[Server] listening on port %d (max %d concurrent clients, %ds request deadline)\n",
           port, max_clients, g_read_timeout_ms / 1000);
    fflush(stdout);
    log_event("server started: port=%d max_clients=%d read_timeout=%dms", port, max_clients, g_read_timeout_ms);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (!g_stop) {
        if (sem_wait(&g_slots) != 0) continue;       /* EINTR: loop re-checks g_stop */
        if (g_stop) { sem_post(&g_slots); break; }

        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cfd = accept4(lfd, (struct sockaddr *)&cli, &clen, SOCK_CLOEXEC);
        if (cfd < 0) {
            sem_post(&g_slots);
            if (errno == EMFILE || errno == ENFILE) usleep(100000);   /* out of fds: avoid a hot loop */
            continue;
        }

        conn_t *c = malloc(sizeof *c);               /* heap, so the thread owns its argument */
        if (!c) { close(cfd); sem_post(&g_slots); continue; }
        c->fd = cfd;
        c->id = atomic_fetch_add(&g_next_req, 1);
        c->port = ntohs(cli.sin_port);
        inet_ntop(AF_INET, &cli.sin_addr, c->ip, sizeof c->ip);

        pthread_t tid;
        if (pthread_create(&tid, &attr, thread_main, c) != 0) {
            log_event("pthread_create failed");
            close(cfd); free(c); sem_post(&g_slots);
        }
    }

    /* graceful shutdown: stop accepting, let in-flight jobs finish */
    log_event("shutdown requested, waiting for in-flight jobs");
    close(lfd);
    for (int i = 0; i < max_clients; i++)
        while (sem_wait(&g_slots) != 0 && errno == EINTR) { }
    log_event("server stopped");
    log_close();
    printf("[Server] stopped\n");
    return 0;
}
