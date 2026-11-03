#!/usr/bin/env python3
"""
Regression tests for Remote Code Runner.

Each test targets a specific bug of the original implementation (marked [BUG #n])
or a security / robustness property of the new one.

    make && python3 tests/test_all.py
"""
import glob
import os
import signal
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "server")
CLIENT = os.path.join(ROOT, "client")
PORT = 9000 + (os.getpid() % 900) + 50
MAX_CLIENTS = 4
READ_TIMEOUT = 2          # short, so the idle-client test is quick
IS_ROOT = os.geteuid() == 0

# ----------------------------------------------------------------- helpers
def request(lang, code, host="127.0.0.1", chunks=None, pause=0.0, timeout=40):
    """Speak the wire protocol. Returns (state, code, output, seconds) or ('ERR', reason, '', s)."""
    t0 = time.time()
    data = code.encode() if isinstance(code, str) else code
    hdr = f"RUN {lang} {len(data)}\n".encode()
    s = socket.create_connection((host, PORT), timeout=timeout)
    try:
        if chunks:                                   # trickle the request in tiny pieces
            payload = hdr + data
            for i in range(0, len(payload), chunks):
                s.sendall(payload[i:i + chunks])
                time.sleep(pause)
        else:                                        # header + code in ONE segment (coalesced)
            s.sendall(hdr + data)
        buf = b""
        while b"\n" not in buf:
            c = s.recv(4096)
            if not c:
                break
            buf += c
        line, _, rest = buf.partition(b"\n")
        parts = line.decode(errors="replace").split()
        if parts and parts[0] == "ERR":
            return ("ERR", parts[1] if len(parts) > 1 else "", "", time.time() - t0)
        state, rc, n = parts[1], int(parts[2]), int(parts[3])
        while len(rest) < n:
            c = s.recv(65536)
            if not c:
                break
            rest += c
        return (state, rc, rest[:n].decode(errors="replace"), time.time() - t0)
    finally:
        s.close()


def procs_matching(needle):
    """PIDs whose command line contains `needle` (excluding this process)."""
    found = []
    for p in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            pid = int(p.split("/")[2])
            if pid == os.getpid():
                continue
            if needle.encode() in open(p, "rb").read():
                found.append(pid)
        except OSError:
            pass
    return found


def job_dirs():
    return glob.glob("/tmp/rcr-*")


def alive(proc):
    return proc.poll() is None


results = []
def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f"  -- {detail}" if detail and not cond else ""))


def section(title):
    print(f"\n== {title}")


# ----------------------------------------------------------------- start server
def start_server():
    env = dict(os.environ, SECRET_TEST="leak-me")
    p = subprocess.Popen([SERVER, str(PORT), str(MAX_CLIENTS), str(READ_TIMEOUT)], cwd=ROOT, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    raise SystemExit("server did not start")


def main():
    srv = start_server()
    try:
        run_tests(srv)
    finally:
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=15)
        except subprocess.TimeoutExpired:
            srv.kill()

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    if failed:
        for n, _, d in failed:
            print(f"  FAILED: {n} {d}")
    sys.exit(1 if failed else 0)


def run_tests(srv):
    section("Basic functionality")
    st, rc, out, _ = request("Python", 'print("hello")\n')
    check("python hello", st == "EXITED" and rc == 0 and out.strip() == "hello", f"{st} {rc} {out!r}")
    st, rc, out, _ = request("C", '#include <stdio.h>\nint main(){printf("hi\\n");return 3;}\n')
    check("C hello + exit status propagated", st == "EXITED" and rc == 3 and out.strip() == "hi", f"{st} {rc} {out!r}")
    st, rc, out, _ = request("py", 'print(1)\n')
    check("language alias 'py' accepted", st == "EXITED" and out.strip() == "1")
    st, rc, out, _ = request("C", 'int main(){ return x; }\n')
    check("compile error reported", st == "COMPILE_ERROR" and "error" in out, f"{st} {out[:80]!r}")
    check("compile error does not leak server temp paths", "/tmp/" not in out, out[:120])
    st, rc, out, _ = request("Rust", 'fn main(){}\n')
    check("unsupported language", st == "UNSUPPORTED", st)
    st, rc, out, _ = request("Python", 'import sys\nprint("err", file=sys.stderr)\nsys.exit(7)\n')
    check("stderr captured, exit code 7", st == "EXITED" and rc == 7 and "err" in out, f"{st} {rc} {out!r}")

    section("[BUG #2] message framing (TCP is a byte stream)")
    st, rc, out, _ = request("Python", 'print("coalesced")\n')
    check("header+code in one segment (used to hang the server thread)", st == "EXITED" and "coalesced" in out)
    st, rc, out, _ = request("Python", 'print("trickle")\n', chunks=3, pause=0.02)
    check("request delivered 3 bytes at a time", st == "EXITED" and "trickle" in out, f"{st} {out!r}")
    big = "x = 0\n" + "".join(f"# padding {i} " + "." * 40 + "\n" for i in range(1000)) + 'print("big ok")\n'
    assert 40_000 < len(big) < 64 * 1024
    st, rc, out, _ = request("Python", big, chunks=1400, pause=0.0)
    check(f"{len(big)}-byte program split across many segments (used to be truncated)", st == "EXITED" and "big ok" in out, f"{st} {out[:60]!r}")
    st, rc, out, _ = request("Python", "print('x')\n" + "#" * (64 * 1024))
    check("request over the size limit rejected", st == "ERR" and rc == "TOO_LARGE", f"{st} {rc}")
    s = socket.create_connection(("127.0.0.1", PORT)); s.sendall(b"HELLO WORLD\n")
    reply = s.recv(100); s.close()
    check("malformed header rejected", reply.startswith(b"ERR BAD_REQUEST"), reply)

    section("[BUG #1] per-request timeouts (original: global alarm() + child_pid)")
    st, rc, out, dt = request("Python", 'import time\nprint("before", flush=True)\ntime.sleep(30)\nprint("never")\n')
    check("sleeping program killed by wall-clock limit", st == "TIMEOUT" and 4 <= dt < 8, f"{st} {dt:.1f}s")
    check("output printed before the kill is preserved (python -u)", "before" in out, repr(out))
    st, rc, out, dt = request("Python", "while True: pass\n")
    check("CPU-bound infinite loop stopped", st in ("SIGNALED", "TIMEOUT") and dt < 8, f"{st} {rc} {dt:.1f}s")
    if st == "SIGNALED":
        check("  ...reported as SIGXCPU (signal 24), not 'no output'", rc == 24, f"{rc}")

    # THE concurrency scenario that failed originally: A (sleep 9s) was never killed
    # because B's alarm() replaced A's timer and B's PID replaced child_pid.
    res = {}
    def job(name, delay):
        time.sleep(delay)
        t = time.time()
        r = request("Python", 'import time\ntime.sleep(30)\nprint("finished normally")\n')
        res[name] = (r[0], time.time() - t)
    ta = threading.Thread(target=job, args=("A", 0)); tb = threading.Thread(target=job, args=("B", 2))
    ta.start(); tb.start(); ta.join(); tb.join()
    check("concurrent A+B: A still killed after its own 5s (was: ran to completion)",
          res["A"][0] == "TIMEOUT" and res["A"][1] < 8, f"A={res['A']}")
    check("concurrent A+B: B killed after its own 5s", res["B"][0] == "TIMEOUT" and res["B"][1] < 8, f"B={res['B']}")

    section("[BUG #3] wait/reap correctness")
    bad = 0
    for i in range(6):
        st, rc, out, _ = request("Python", f"import os\nprint({i})\nos._exit({i % 4})\n")
        if not (st == "EXITED" and rc == i % 4):
            bad += 1
    check("exit status always correct (no SIGCHLD reaper race)", bad == 0, f"{bad}/6 wrong")
    st, rc, out, _ = request("C", '#include <signal.h>\n#include <unistd.h>\nint main(){ raise(SIGSEGV); return 0; }\n')
    check("crash reported as SIGNALED/11 (original printed 'no output')", st == "SIGNALED" and rc == 11, f"{st} {rc}")

    section("[BUG #5] process-tree cleanup and resource limits")
    code = 'import subprocess, time\nsubprocess.Popen(["sleep", "61234"])\ntime.sleep(30)\n'
    st, rc, out, dt = request("Python", code)
    time.sleep(0.3)
    check("child's child (sleep 61234) is killed with the job", st == "TIMEOUT" and not procs_matching("61234"),
          f"{st} stragglers={procs_matching('61234')}")
    code = 'import subprocess\nsubprocess.Popen(["sleep", "61235"])\nprint("parent exits immediately")\n'
    st, rc, out, dt = request("Python", code)
    time.sleep(0.3)
    check("background straggler does not stall the reply", st == "EXITED" and dt < 2.5, f"{st} {dt:.1f}s")
    check("...and is killed too", not procs_matching("61235"), procs_matching("61235"))
    st, rc, out, dt = request("Python", 'while True: print("x" * 1000)\n')
    check("output flood stopped at the cap", st == "OUTPUT_LIMIT" and dt < 3 and len(out) <= 64 * 1024, f"{st} len={len(out)} {dt:.1f}s")
    st, rc, out, _ = request("Python", 'x = bytearray(600*1024*1024)\nprint("allocated")\n')
    check("memory limit enforced", "MemoryError" in out or st == "SIGNALED", f"{st} {out[-80:]!r}")
    st, rc, out, _ = request("C", '#include <stdio.h>\n#include <stdlib.h>\nint main(){ void*p=malloc(600UL<<20); printf("%s\\n", p?"got it":"refused"); return 0; }\n')
    check("C malloc(600MB) refused", "refused" in out, f"{st} {out!r}")
    code = ('import os, time\nn = 0\nfor _ in range(300):\n    try:\n        if os.fork() == 0:\n'
            '            time.sleep(30); os._exit(0)\n        n += 1\n    except OSError:\n        break\nprint("forked", n, flush=True)\ntime.sleep(30)\n')
    st, rc, out, dt = request("Python", code)
    time.sleep(0.5)
    n = int(out.split("forked")[1].split()[0]) if "forked" in out else -1
    check("fork bomb contained: every child killed with the job", not procs_matching("main.py"), f"leftovers={procs_matching('main.py')}")
    if IS_ROOT:
        check("  ...RLIMIT_NPROC capped the fork count (<=70 of 300)", 0 <= n <= 70, f"forked {n}")
    else:
        print(f"  [info] not root: no RLIMIT_NPROC, forked {n} (all killed at the deadline)")

    section("Isolation of the job")
    st, rc, out, _ = request("Python", 'import os\nprint(sorted(os.listdir("/proc/self/fd")))\n')
    fds = set(eval(out.strip())) if out.strip().startswith("[") else {"?"}
    check("no inherited file descriptors (only 0,1,2 + listdir's own)", fds <= {"0", "1", "2", "3"}, f"{fds}")
    st, rc, out, _ = request("Python", 'import os\nprint(os.environ.get("SECRET_TEST"), sorted(os.environ))\n')
    check("server environment is not leaked to jobs", out.startswith("None"), out)
    st, rc, out, _ = request("Python", 'import os\nprint(os.getuid())\n')
    expected = "65534" if IS_ROOT else str(os.geteuid())
    check(f"job runs as uid {expected}", out.strip() == expected, out)
    st, rc, out, _ = request("Python", f'import socket\ntry:\n    socket.create_connection(("127.0.0.1", {PORT}), timeout=2); print("CONNECTED")\nexcept Exception as e:\n    print("blocked:", type(e).__name__)\n')
    print(f"  [info] job -> server loopback connection: {out.strip()}  "
          f"({'network namespace active' if 'blocked' in out else 'network NOT isolated'})")
    st, rc, out, _ = request("Python", 'import os\nprint(os.listdir("."))\nopen("junk.bin","wb").write(b"x"*1000)\nos.mkdir("sub"); open("sub/f","w").write("1")\n')
    check("job may write inside its own scratch dir", st == "EXITED", f"{st} {out!r}")
    st, rc, out, _ = request("Python", 'f = open("big.bin","wb")\ntry:\n    f.write(b"x" * (50*1024*1024)); f.flush(); print("wrote 50MB")\nexcept Exception as e:\n    print("blocked:", type(e).__name__)\n')
    check("disk-fill blocked by RLIMIT_FSIZE", "wrote 50MB" not in out, f"{st} {out!r}")

    section("[BUG #4] temp-file cleanup")
    time.sleep(0.5)
    check("no scratch directories left after all the abuse above", job_dirs() == [], job_dirs())

    section("Robustness of the server")
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(b"RUN Python 5000\n"); s.sendall(b"print(1)")      # promises 5000 bytes, sends 8
    t0 = time.time(); reply = s.recv(100); s.close()
    check("client that stalls mid-request is dropped at the deadline", reply.startswith(b"ERR INCOMPLETE") and time.time() - t0 < READ_TIMEOUT + 2, reply)

    s = socket.create_connection(("127.0.0.1", PORT)); s.sendall(b"RUN Python 1000000000\n")
    check("absurd length rejected without allocating", s.recv(100).startswith(b"ERR TOO_LARGE")); s.close()

    # original: a client closing right after its request killed the whole server via SIGPIPE
    for _ in range(5):
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall(b'RUN Python 38\nfor i in range(3000): print("x"*50)\n')
        s.close()
    time.sleep(1.5)
    check("clients that vanish mid-request do not kill the server (SIGPIPE)", alive(srv))
    st, rc, out, _ = request("Python", 'print("still serving")\n')
    check("...and the server still serves", out.strip() == "still serving", f"{st} {out!r}")

    # original: idle connections held threads forever (and with bounded slots, starved everyone)
    idle = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(MAX_CLIENTS)]
    time.sleep(0.3)
    t0 = time.time()
    st, rc, out, _ = request("Python", 'print("served after idle clients timed out")\n')
    dt = time.time() - t0
    check(f"{MAX_CLIENTS} idle connections hold ALL slots, yet a real client is served within the deadline",
          st == "EXITED" and dt < READ_TIMEOUT + 3, f"{st} {dt:.1f}s")
    for s in idle:
        s.close()

    # concurrency + isolation between jobs
    outs = {}
    def worker(i):
        outs[i] = request("Python", f'import time\ntime.sleep(0.3)\nprint("job-{i}")\n')[2].strip()
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(12)]
    [t.start() for t in ts]; [t.join() for t in ts]
    check("12 concurrent jobs over 4 slots all get THEIR OWN output",
          all(outs.get(i) == f"job-{i}" for i in range(12)), outs)
    check("no scratch directories left after the concurrent batch", job_dirs() == [], job_dirs())

    section("Client program")
    srcf = "/tmp/rcr_test_prog.py"
    open(srcf, "w").write('print("from file")\n')
    r = subprocess.run([CLIENT, "127.0.0.1", "-p", str(PORT), "-l", "Python", "-f", srcf], capture_output=True, text=True)
    check("client -f works, exit status 0", r.returncode == 0 and "from file" in r.stdout, r.stdout + r.stderr)
    r = subprocess.run([CLIENT, "127.0.0.1", "-p", str(PORT)], input="Python\nprint(1)\n\nprint(2)\n.\n", capture_output=True, text=True)
    check("interactive mode allows blank lines inside the code", r.returncode == 0 and "1\n2" in r.stdout, r.stdout + r.stderr)
    open(srcf, "w").write("import sys\nsys.exit(5)\n")
    r = subprocess.run([CLIENT, "127.0.0.1", "-p", str(PORT), "-l", "Python", "-f", srcf], capture_output=True, text=True)
    check("client exit status 1 when the program fails", r.returncode == 1 and "exit code 5" in r.stdout, r.stdout)
    r = subprocess.run([CLIENT, "127.0.0.1", "-p", "1", "-l", "C", "-f", srcf], capture_output=True, text=True)
    check("client exit status 2 when the server is unreachable", r.returncode == 2, r.returncode)
    os.remove(srcf)

    section("Log hygiene and shutdown")
    request("Python\x01FAKE", "print(1)\n") if False else None
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(b"RUN EVIL\r[FORGED] 2099 admin logged in 5\nprint(1)"); s.close()
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "logs", "run.log"), errors="replace").read()
    check("control characters in client input cannot forge log lines",
          not any(l.startswith("[FORGED]") for l in log.splitlines()))
    srv.send_signal(signal.SIGTERM)
    try:
        rc = srv.wait(timeout=15)
        check("SIGTERM -> graceful shutdown, exit status 0", rc == 0, f"rc={rc}")
    except subprocess.TimeoutExpired:
        check("SIGTERM -> graceful shutdown", False, "server did not exit")


if __name__ == "__main__":
    main()
