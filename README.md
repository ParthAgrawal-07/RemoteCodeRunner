# Remote Code Runner (enhanced)

A multi-threaded TCP server written in C that receives **C or Python** source code from
a client, runs it under resource limits in an isolated child process, and returns the
output and how the program ended.

```
 client ──TCP──► server: accept ─► thread ─► execute_code ─► fork ─► [uid nobody, rlimits, own process group]
                                                                         gcc ─► ./prog     |   python3 main.py
        ◄────── RESULT <STATE> <code> <n>\n<n bytes of output>  ◄─── pipe (stdout+stderr), per-request deadline
```

## Build, run, test

```bash
make                         # builds ./server and ./client (needs gcc, python3 on the server)
./server [port] [max_clients] [read_timeout_sec]   # defaults: 9000, 8, 10
./client 127.0.0.1                                  # interactive; finish the code with a line "."
./client 127.0.0.1 -l Python -f hello.py            # from a file
./client 192.168.1.10 -p 9000 -l C -f prog.c -t 30  # remote server, 30 s result timeout

make test                    # 47 regression tests
make asan && make test       # same tests under AddressSanitizer + UBSan
```

Run the server as **root** (or in a container) to get full isolation: each job then runs as
`nobody` with a process-count limit and, where the kernel allows, no network. As a normal
user it still works ("dev mode") but jobs share your uid.

Client exit status: `0` program exited 0 · `1` program failed / request rejected · `2` connection or protocol error.

## Protocol

TCP is a byte stream — it preserves order, **not message boundaries** — so every message carries its length.

```
Request   RUN <lang> <nbytes>\n<nbytes of source>
Response  RESULT <STATE> <code> <nbytes>\n<nbytes of output>      or      ERR <REASON>\n
```

| STATE | meaning | code |
|---|---|---|
| `EXITED` | ran to completion | exit status |
| `SIGNALED` | killed by a signal (e.g. 11 = SIGSEGV, 24 = CPU limit) | signal number |
| `TIMEOUT` | wall-clock limit (5 s) | 0 |
| `OUTPUT_LIMIT` | more than 64 KB of output | 0 |
| `COMPILE_ERROR` | compiler failed (output = compiler messages) | compiler status |
| `UNSUPPORTED` / `INTERNAL` | unknown language / server fault | 0 |

`ERR` reasons: `BAD_REQUEST`, `TOO_LARGE` (> 64 KB), `INCOMPLETE` (client stalled).

## Files

| File | Responsibility |
|---|---|
| `protocol.[ch]` | Shared framing: `send_all`, `recv_all`, `recv_line` (EINTR-safe, total-time deadlines, `MSG_NOSIGNAL`), result states |
| `server.c` | Accept loop, semaphore-bounded thread per connection, request parsing, graceful shutdown |
| `executor.c` | Per-job directory, compile + run stages, rlimits, privilege drop, process-group kill, cleanup |
| `logger.c` | Thread-safe logger; sanitises client-controlled text |
| `client.c` | CLI client (`getopt`, file or interactive input, exit codes) |
| `tests/test_all.py` | 47 regression tests, each aimed at a specific bug or security property |

## What was wrong in v1, and how it is fixed

Every row was reproduced against v1 before being fixed, and has a regression test.

| # | v1 problem | Fix |
|---|---|---|
| 1 | Timeout used a **global `child_pid` and `alarm()`/`SIGALRM`**, which are per-*process*. With two concurrent requests the second overwrote the first's PID and timer, so the first was never killed (a 9 s program ran to completion). | No signals at all. Each request `poll()`s its own pipe against its own deadline using only local variables. |
| 2 | **No message framing**: two bare `recv()` calls assumed one-`send()`-one-`recv()`. Back-to-back sends hung the server thread; programs larger than one TCP segment could be truncated. | Length-prefixed protocol; `recv_all()` loops until exactly *n* bytes arrive; header line is read byte-wise so the payload is never over-read. |
| 3 | A **`SIGCHLD` handler doing `waitpid(-1)`** could reap the child that another thread's `waitpid(pid)` was waiting for, leaving `status` uninitialised (a crash was reported as "no output"). | No handler. Each thread reaps exactly its own child; `status` is always initialised. |
| 4 | **Temp files leaked** on early-return paths (and `mkstemps` was implicitly declared, an error on GCC 14+). | One `mkdtemp` directory per job, removed on every path (`goto cleanup`). |
| 5 | **Killing only the direct child** left grandchildren alive; the child **inherited every server fd** (listening socket, other clients' sockets); no limit on forks, disk, or output; code ran as the server's user; `bash -c` shell string. | Own process group + `kill(-pgid, SIGKILL)`; all fds are `CLOEXEC`; `RLIMIT_NPROC/FSIZE/NOFILE/CORE`; output cap; drop to `nobody`; `NO_NEW_PRIVS`; best-effort network namespace; minimal environment; no shell (`execve` with an argv array). |
| 6 | **Idle clients held threads forever.** With a bounded pool this is a trivial denial of service. | Whole-request read deadline (default 10 s) enforced with `poll`, so trickling bytes does not help either. |
| 7 | A client disconnecting at the wrong moment could raise **SIGPIPE** and kill the process. | `MSG_NOSIGNAL` on every send, plus `SIGPIPE` ignored. |
| 8 | Log injection via client-supplied text; `localtime()` not thread-safe; log file opened/closed per line. | Control characters neutralised; `localtime_r`; file opened once with `O_CLOEXEC`. |
| 9 | Python output buffered in the pipe was lost when the job was killed. | `python3 -u` (unbuffered), so partial output survives a timeout. |
| 10 | Client: hard-coded IP, blank lines could not appear in code, single `recv()` for the reply, leaks on error paths. | `getopt` options, `.` terminator, framed reply, single cleanup path (found by LeakSanitizer). |

Smaller design points: compile and run are separate stages with separate limits (gcc needs far more than the 128 MB / 3 s given to a user program); the fork–exec child uses only async-signal-safe calls, since `fork()` in a multithreaded process only duplicates the calling thread; adding a language is one row in a table.

## Security model — read this before exposing it anywhere

This is **resource limiting plus basic isolation, not a hardened sandbox.**

What it does: bounded CPU / wall time / memory / output / disk / processes, a separate uid, a clean environment, no inherited descriptors, whole-process-tree termination, per-job scratch directory, optional network namespace.

What it does **not** do (all are the next steps for production):

- **Filesystem is not isolated.** Jobs can read any world-readable file on the host. Next: `chroot`/`pivot_root` + mount namespace, or run each job in a container.
- **No syscall filtering.** Next: seccomp-bpf allow-list.
- **Network isolation needs root and a permissive kernel** (the startup log says whether it is available).
- **`RLIMIT_NPROC` is per-uid, shared by concurrent jobs**, and is only applied when running as root.
- **`RLIMIT_AS` limits address space, not resident memory.** Next: cgroups v2 (`memory.max`, `pids.max`, `cpu.max`).
- No authentication, no TLS, no rate limiting per client.
- C programs that are killed lose unflushed stdio buffers (Python is unbuffered).

For real untrusted code: containers or microVMs (gVisor, Firecracker) with cgroups and seccomp.

## Adding a language

Add one row to `LANGS[]` in `executor.c` (source file name, optional compile argv, run argv) — nothing else changes.
