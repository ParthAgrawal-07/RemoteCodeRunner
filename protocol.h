/*
 * protocol.h - wire protocol and I/O helpers shared by client and server.
 *
 * TCP is a byte stream: it keeps order but NOT message boundaries. Every
 * message therefore carries its own length.
 *
 *   Request   RUN <lang> <nbytes>\n<nbytes of source code>
 *   Response  RESULT <STATE> <code> <nbytes>\n<nbytes of program output>
 *        or   ERR <REASON>\n                       (request rejected)
 *
 *   STATE      meaning                         <code>
 *   EXITED     program ran to completion       exit status
 *   SIGNALED   program killed by a signal      signal number
 *   TIMEOUT    wall-clock limit exceeded       0
 *   OUTPUT_LIMIT  produced too much output     0
 *   COMPILE_ERROR compiler failed              compiler exit status
 *   UNSUPPORTED   language not supported       0
 *   INTERNAL   server-side failure             0
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_PORT      9000
#define MAX_LINE          256               /* longest header line               */
#define MAX_CODE_BYTES    (64 * 1024)       /* largest accepted source file      */
#define MAX_OUTPUT_BYTES  (64 * 1024)       /* largest captured program output   */

typedef enum {
    RS_EXITED = 0,
    RS_SIGNALED,
    RS_TIMEOUT,
    RS_OUTPUT_LIMIT,
    RS_COMPILE_ERROR,
    RS_UNSUPPORTED,
    RS_INTERNAL,
    RS_STATE_COUNT
} run_state_t;

const char *run_state_name(run_state_t s);
int         run_state_parse(const char *name, run_state_t *out);   /* 0 ok, -1 unknown */

/* All helpers handle EINTR and partial transfers. `timeout_ms` is the TOTAL
 * time budget for the whole call (not per recv), so a client that trickles
 * one byte at a time cannot hold a server thread forever. <= 0 means no limit. */
int send_all (int fd, const void *buf, size_t n);                       /* 0 ok, -1 error     */
int recv_all (int fd, void *buf, size_t n, int timeout_ms);             /* 0 ok, -1 err/EOF/timeout */
int recv_line(int fd, char *out, size_t cap, int timeout_ms);           /* length, or -1      */

long long monotonic_ms(void);

#endif
