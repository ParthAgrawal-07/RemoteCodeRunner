#define _GNU_SOURCE
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *const STATE_NAMES[RS_STATE_COUNT] = {
    "EXITED", "SIGNALED", "TIMEOUT", "OUTPUT_LIMIT",
    "COMPILE_ERROR", "UNSUPPORTED", "INTERNAL"
};

const char *run_state_name(run_state_t s) {
    return (s >= 0 && s < RS_STATE_COUNT) ? STATE_NAMES[s] : "INTERNAL";
}

int run_state_parse(const char *name, run_state_t *out) {
    for (int i = 0; i < RS_STATE_COUNT; i++)
        if (strcmp(name, STATE_NAMES[i]) == 0) { *out = (run_state_t)i; return 0; }
    return -1;
}

long long monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Wait until fd is readable or the absolute deadline passes (deadline < 0: never). */
static int wait_readable(int fd, long long deadline_ms) {
    for (;;) {
        int to = -1;
        if (deadline_ms >= 0) {
            long long left = deadline_ms - monotonic_ms();
            if (left <= 0) return -1;
            to = left > 1000000 ? 1000000 : (int)left;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, to);
        if (r > 0) return 0;                       /* readable, or HUP/ERR: recv() will tell */
        if (r < 0 && errno != EINTR) return -1;
    }
}

int send_all(int fd, const void *buf_, size_t n) {
    const char *buf = buf_;
    size_t sent = 0;
    while (sent < n) {
        /* MSG_NOSIGNAL: a vanished peer yields EPIPE, never a process-killing SIGPIPE */
        ssize_t w = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;                             /* includes SO_SNDTIMEO expiry (EAGAIN) */
        }
        sent += (size_t)w;
    }
    return 0;
}

int recv_all(int fd, void *buf_, size_t n, int timeout_ms) {
    char *buf = buf_;
    long long deadline = timeout_ms > 0 ? monotonic_ms() + timeout_ms : -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, MSG_DONTWAIT);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;                     /* peer closed before sending everything */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_readable(fd, deadline) < 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* Reads one '\n'-terminated line WITHOUT consuming anything after it, so the
 * binary payload that follows stays untouched in the socket. A line longer than
 * the buffer is a protocol violation (-1), never silently truncated. */
int recv_line(int fd, char *out, size_t cap, int timeout_ms) {
    long long deadline = timeout_ms > 0 ? monotonic_ms() + timeout_ms : -1;
    size_t len = 0;
    for (;;) {
        char c;
        ssize_t r = recv(fd, &c, 1, MSG_DONTWAIT);
        if (r == 1) {
            if (c == '\n') {
                if (len > 0 && out[len - 1] == '\r') len--;
                out[len] = '\0';
                return (int)len;
            }
            if (len + 1 >= cap) return -1;
            out[len++] = c;
            continue;
        }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_readable(fd, deadline) < 0) return -1;
            continue;
        }
        return -1;
    }
}
