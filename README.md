# Remote Code Runner — Console-Based OS Project

> **Type:** Console-Based Client-Server Application  
> **Language:** C (POSIX / Linux)

---

## Project Overview

This project implements a **Remote Code Execution System** — a client-server application where a client sends source code (C or Python) over a TCP socket to a server, which compiles and/or executes it in a sandboxed subprocess and returns the output back to the client.

The system demonstrates core OS and systems programming concepts including:
- TCP socket programming
- Multi-threaded server design with POSIX threads
- Process creation and management using `fork()` / `exec()`
- Inter-process communication using pipes
- Resource limiting with `setrlimit()`
- Signal handling (`SIGCHLD`, `SIGALRM`)
- Thread-safe logging with mutex locks

---

## Architecture

```
┌─────────────┐       TCP Socket (Port 9000)      ┌──────────────────────────────┐
│             │  ──── lang + code ──────────────►  │                              │
│   client.c  │                                    │   server.c  (main loop)      │
│  (Client)   │  ◄─── output ───────────────────   │   + executor.c (fork/exec)   │
│             │                                    │   + logger.c  (log thread)   │
└─────────────┘                                    └──────────────────────────────┘
```

The server spawns a new **POSIX thread** per client connection. Each thread calls the executor, which `fork()`s a child process to run the code and streams output back through a pipe.

---

## File Structure

```
.
├── server.h       # Shared header: macros, includes, function declarations
├── server.c       # TCP server — accepts connections, spawns threads per client
├── client.c       # TCP client — sends language + code, prints output
├── executor.c     # Code execution engine — fork/exec with sandboxing
└── logger.c       # Thread-safe file logger
```

---

## File Descriptions

### `server.h` — Shared Header
Defines constants and declares shared symbols used across all modules:
- `PORT 9000` — the TCP port the server listens on
- `BUFFER_SIZE 8192` — max code/output buffer size
- `execute_code()` and `log_event()` function declarations
- `log_mutex` extern declaration for thread-safe logging

---

### `server.c` — TCP Server
The entry point of the server application.

**Responsibilities:**
- Creates a TCP socket and binds it to `PORT 9000`
- Listens for incoming client connections (backlog: 16)
- Accepts connections in an infinite loop
- Spawns a detached POSIX thread (`pthread_create` + `pthread_detach`) for each client
- Each thread receives the language identifier and source code from the client, calls `execute_code()`, and sends the output back

**Key OS Concepts Used:**
- `socket()`, `bind()`, `listen()`, `accept()` — BSD socket API
- `pthread_create()` / `pthread_detach()` — POSIX threads
- `SIGCHLD` handler with `waitpid(..., WNOHANG)` to reap zombie child processes
- `SO_REUSEADDR` socket option to allow quick server restarts

---

### `client.c` — TCP Client
The console-based client that the user interacts with.

**Responsibilities:**
- Connects to the server at a given IP (default: `172.20.10.4`) and port `9000`
- Prompts the user to enter a language (`C` or `Python`)
- Reads multi-line source code from stdin (terminated by an empty line)
- Sends language and code to the server over the socket
- Receives and prints the execution output

**Usage:**
```
./client [server_ip]
```
If no IP is provided, it defaults to `172.20.10.4`.

**Input Flow:**
```
Enter Language (C/Python): Python
Enter code. End by entering an empty line:
print("Hello from remote!")

--- Execution Output ---
Hello from remote!
--- End ---
```

---

### `executor.c` — Code Execution Engine
The core of the project. Handles safe, sandboxed execution of submitted code.

**Responsibilities:**
- Writes the received source code to a unique temp file (`/tmp/codeXXXXXX.c` or `.py`) using `mkstemps()`
- `fork()`s a child process to compile (for C) and/or run the code
- Redirects child's `stdout` and `stderr` to a pipe for output capture
- Reads output from the pipe and returns it as a dynamically allocated string
- Kills the child and cleans up temp files after execution

**Sandbox / Safety Features:**

| Feature | Implementation |
|---|---|
| CPU time limit | `setrlimit(RLIMIT_CPU, 3s)` |
| Memory limit | `setrlimit(RLIMIT_AS, 64 MB)` |
| Execution timeout | `alarm()` + `SIGALRM` handler kills child via `SIGKILL` |
| Temp file isolation | `mkstemps()` creates unique files; `unlink()` removes them after use |
| Output capture | `dup2()` redirects child stdout/stderr into a pipe |

**C Execution Flow:**
1. Write code to `/tmp/codeXXXXXX.c`
2. Compile: `gcc <src> -o /tmp/exe_<pid>`
3. Run: `<exe_path>`
4. Both compile errors and runtime output are captured together

**Python Execution Flow:**
1. Write code to `/tmp/codeXXXXXX.py`
2. Run: `python3 <src_path>`

---

### `logger.c` — Thread-Safe Logger
Provides timestamped event logging to `logs/run.log`.

**Responsibilities:**
- Implements `log_event(fmt, ...)` — a variadic logging function
- Uses `pthread_mutex_lock` / `pthread_mutex_unlock` to prevent concurrent write corruption
- Appends log entries with timestamp in `YYYY-MM-DD HH:MM:SS` format
- Automatically creates the `logs/` directory on server startup (via `mkdir()` in `server.c`)

**Sample Log Output:**
```
[2026-03-30 18:45:01] Server started on port 9000
[2026-03-30 18:45:10] Accepted connection from 192.168.1.5:54321 fd=4
[2026-03-30 18:45:10] Received request (fd=4) lang=Python size=28
[2026-03-30 18:45:11] Closed connection fd=4
```

---

## How to Build and Run

### Prerequisites
- GCC compiler
- Python 3 installed on the server machine
- POSIX-compliant Linux system
- `pthread` library

### Compilation

```bash
# Compile the server
gcc server.c executor.c logger.c -o server -lpthread

# Compile the client
gcc client.c -o client
```

### Running

**Start the server (on host machine):**
```bash
./server
```
Output: `[Server] Listening on port 9000`

**Run the client (on same or different machine):**
```bash
./client <server_ip>
# Example:
./client 172.20.10.4
```

---

## Concepts Demonstrated

| Concept | Where Used |
|---|---|
| TCP Socket Programming | `server.c`, `client.c` |
| Multi-threading (pthreads) | `server.c` — one thread per client |
| Process Creation (`fork`) | `executor.c` |
| `exec` family (`execlp`) | `executor.c` — runs gcc / python3 |
| Pipes (IPC) | `executor.c` — capturing child output |
| Signal Handling | `server.c` (SIGCHLD), `executor.c` (SIGALRM) |
| Resource Limits (`setrlimit`) | `executor.c` — CPU & memory caps |
| Mutex Locks | `logger.c` — thread-safe logging |
| File I/O (temp files) | `executor.c` — mkstemps, unlink |

---

## Limitations & Notes

- The client sends language and code in **two separate `send()` calls** — on fast loopback networks this works fine, but on slower/lossy links a framing protocol (e.g., length-prefixed messages) would be more robust.
- The server default IP in `client.c` (`172.20.10.4`) is hardcoded for the project setup — pass the correct server IP as a command-line argument when running on a different machine.
- Only **C** and **Python** are supported. Other language strings return `"Unsupported language."`.
- The system uses `bash -c "gcc ... && ./exe"` for C, so `bash` must be present on the server.
