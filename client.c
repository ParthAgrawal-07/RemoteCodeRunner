/*
 * client.c - command-line client for the Remote Code Runner.
 *
 *   ./client [server_ip] [-p port] [-l C|Python] [-f source_file] [-t timeout_sec]
 *
 *   interactive:  ./client 192.168.1.10
 *   from a file:  ./client 192.168.1.10 -l Python -f hello.py
 *
 * Exit status: 0 program exited 0 | 1 program failed / rejected | 2 connection or protocol error
 */
#define _GNU_SOURCE
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_MAX_OUTPUT (1024 * 1024)       /* never trust a length field from the network */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [server_ip] [-p port] [-l C|Python] [-f source_file] [-t timeout_sec]\n"
            "  server_ip   default 127.0.0.1\n"
            "  -p port     default %d\n"
            "  -l lang     language (prompted if omitted)\n"
            "  -f file     read source code from a file (otherwise typed/piped on stdin)\n"
            "  -t seconds  how long to wait for the result, default 60\n",
            prog, DEFAULT_PORT);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    char *buf = malloc(MAX_CODE_BYTES + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, MAX_CODE_BYTES + 1, f);
    fclose(f);
    if (*len > MAX_CODE_BYTES) { fprintf(stderr, "%s: larger than %d bytes\n", path, MAX_CODE_BYTES); free(buf); return NULL; }
    return buf;
}

/* Interactive input: a line containing only "." (or Ctrl-D) ends the code.
 * Blank lines are now allowed inside the program. */
static char *read_stdin_code(size_t *len) {
    char *buf = malloc(MAX_CODE_BYTES + 1);
    if (!buf) return NULL;
    size_t used = 0;
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        if (strcmp(line, ".\n") == 0 || strcmp(line, ".") == 0) break;
        size_t n = strlen(line);
        if (used + n > MAX_CODE_BYTES) { fprintf(stderr, "code larger than %d bytes\n", MAX_CODE_BYTES); free(buf); return NULL; }
        memcpy(buf + used, line, n);
        used += n;
    }
    *len = used;
    return buf;
}

static const char *describe(run_state_t st, int code, char *tmp, size_t cap) {
    switch (st) {
    case RS_EXITED:        snprintf(tmp, cap, "exit code %d", code); break;
    case RS_SIGNALED:      snprintf(tmp, cap, "killed by signal %d (%s)", code, strsignal(code)); break;
    case RS_TIMEOUT:       snprintf(tmp, cap, "time limit exceeded"); break;
    case RS_OUTPUT_LIMIT:  snprintf(tmp, cap, "output limit exceeded"); break;
    case RS_COMPILE_ERROR: snprintf(tmp, cap, "compilation failed"); break;
    case RS_UNSUPPORTED:   snprintf(tmp, cap, "unsupported language"); break;
    default:               snprintf(tmp, cap, "server-side error"); break;
    }
    return tmp;
}

int main(int argc, char **argv) {
    const char *port_s = NULL, *lang_arg = NULL, *file = NULL;
    int timeout_s = 60;
    int opt;
    while ((opt = getopt(argc, argv, "p:l:f:t:h")) != -1) {
        switch (opt) {
        case 'p': port_s = optarg; break;
        case 'l': lang_arg = optarg; break;
        case 'f': file = optarg; break;
        case 't': timeout_s = atoi(optarg); if (timeout_s <= 0) timeout_s = 60; break;
        default:  usage(argv[0]); return 2;
        }
    }
    const char *ip = optind < argc ? argv[optind] : "127.0.0.1";
    int port = port_s ? atoi(port_s) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }

    signal(SIGPIPE, SIG_IGN);

    /* ---- gather language + code ---- */
    char lang[32];
    if (lang_arg) {
        snprintf(lang, sizeof lang, "%s", lang_arg);
    } else {
        printf("Enter Language (C/Python): "); fflush(stdout);
        if (!fgets(lang, sizeof lang, stdin)) return 2;
        lang[strcspn(lang, "\r\n")] = '\0';
    }
    for (char *p = lang; *p; p++) if (*p == ' ') { fprintf(stderr, "language must be a single word\n"); return 2; }
    if (lang[0] == '\0') { fprintf(stderr, "no language given\n"); return 2; }

    size_t code_len = 0;
    char *code = NULL, *out = NULL;
    int fd = -1;
    int rc = 2;                                   /* default: connection / protocol error */

    if (file) {
        code = read_file(file, &code_len);
    } else {
        if (isatty(STDIN_FILENO)) printf("Enter code. Finish with a line containing only '.' (or Ctrl-D):\n");
        code = read_stdin_code(&code_len);
    }
    if (!code) return 2;
    if (code_len == 0) { fprintf(stderr, "no code given\n"); goto done; }

    /* ---- connect ---- */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); goto done; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { fprintf(stderr, "invalid IPv4 address: %s\n", ip); goto done; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); goto done; }

    /* ---- send: one header line, then exactly code_len bytes ---- */
    char hdr[MAX_LINE];
    int hl = snprintf(hdr, sizeof hdr, "RUN %s %zu\n", lang, code_len);
    if (send_all(fd, hdr, (size_t)hl) < 0 || send_all(fd, code, code_len) < 0) {
        fprintf(stderr, "failed to send request\n");
        goto done;
    }

    /* ---- receive: one header line, then exactly <nbytes> bytes ---- */
    char line[MAX_LINE];
    if (recv_line(fd, line, sizeof line, timeout_s * 1000) < 0) {
        fprintf(stderr, "no response from server (timeout or connection closed)\n");
        goto done;
    }

    char word[32], state_s[32];
    int rcode = 0;
    unsigned long long nbytes = 0;
    if (sscanf(line, "ERR %31s", word) == 1) {
        fprintf(stderr, "server rejected the request: %s\n", word);
        rc = 1;
        goto done;
    }
    run_state_t state;
    if (sscanf(line, "RESULT %31s %d %llu", state_s, &rcode, &nbytes) != 3 || run_state_parse(state_s, &state) != 0) {
        fprintf(stderr, "protocol error: unexpected reply '%.60s'\n", line);
        goto done;
    }
    if (nbytes > CLIENT_MAX_OUTPUT) { fprintf(stderr, "protocol error: absurd output length\n"); goto done; }

    out = malloc((size_t)nbytes + 1);
    if (!out || recv_all(fd, out, (size_t)nbytes, timeout_s * 1000) < 0) {
        fprintf(stderr, "connection lost while receiving output\n");
        goto done;
    }
    out[nbytes] = '\0';

    char desc[96];
    printf("\n--- Execution Output ---\n");
    fwrite(out, 1, (size_t)nbytes, stdout);
    if (nbytes > 0 && out[nbytes - 1] != '\n') putchar('\n');
    printf("--- End: %s ---\n", describe(state, rcode, desc, sizeof desc));

    rc = (state == RS_EXITED && rcode == 0) ? 0 : 1;

done:                                             /* single cleanup path: no leaks on any exit */
    free(code);
    free(out);
    if (fd >= 0) close(fd);
    return rc;
}
