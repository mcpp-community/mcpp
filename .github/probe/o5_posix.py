#!/usr/bin/env python3
# O5 (POSIX half): flock semantics the build-directory lock relies on, and
# replacing an executable that is running. Prints facts; never asserts.
import fcntl, os, shutil, signal, subprocess, sys, tempfile, time

d = tempfile.mkdtemp()
lock = os.path.join(d, "build.lock")
open(lock, "w").close()

def probe_lock():
    code = ("import fcntl,os,sys\n"
            f"fd=os.open({lock!r},os.O_RDWR)\n"
            "try:\n fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB); print('free')\n"
            "except BlockingIOError: print('held')\n")
    return subprocess.run([sys.executable, "-c", code], capture_output=True, text=True).stdout.strip()

def holder(cloexec, then):
    # A process takes the lock, then either execs (the `mcpp run` shape) or
    # spawns a long-lived child and exits.
    flags = os.O_RDWR | (os.O_CLOEXEC if cloexec else 0)
    code = ("import fcntl,os,subprocess,time\n"
            f"fd=os.open({lock!r},{flags})\n"
            "fcntl.flock(fd,fcntl.LOCK_EX)\n"
            + ("" if cloexec else "os.set_inheritable(fd,True)\n")
            + ("os.execv('/bin/sleep',['sleep','3'])\n" if then == "exec" else
               "subprocess.Popen(['sleep','3'],close_fds=False)\n"))
    return subprocess.Popen([sys.executable, "-c", code])

for cloexec in (True, False):
    for then in ("exec", "spawn-and-exit"):
        p = holder(cloexec, then)
        time.sleep(0.8)
        print(f"O5.flock cloexec={cloexec} holder={then}: lock is {probe_lock()} while the program runs")
        p.wait(); time.sleep(3)

p = subprocess.Popen([sys.executable, "-c",
    f"import fcntl,os,time\nfd=os.open({lock!r},os.O_RDWR)\nfcntl.flock(fd,fcntl.LOCK_EX)\ntime.sleep(30)\n"])
time.sleep(0.5)
before = probe_lock()
os.kill(p.pid, signal.SIGKILL); p.wait()
print(f"O5.flock SIGKILL holder: before={before} after={probe_lock()}")

# Replacing a running executable by rename, and exec of the new one.
prog = os.path.join(d, "prog")
shutil.copy("/bin/sleep", prog)
run = subprocess.Popen([prog, "5"])
time.sleep(0.3)
shutil.copy("/bin/echo", prog + ".tmp")
try:
    os.replace(prog + ".tmp", prog)
    out = subprocess.run([prog, "replaced"], capture_output=True, text=True).stdout.strip()
    print(f"O5.replace running exe by rename: ok; new program says {out!r}; old still running={run.poll() is None}")
except OSError as e:
    print(f"O5.replace running exe by rename: {e!r}")
run.kill()

# Writing in place over a running executable (what GNU ld's unlink-then-write
# avoids and a plain open(O_TRUNC) does not).
shutil.copy("/bin/sleep", prog)
run = subprocess.Popen([prog, "5"])
time.sleep(0.3)
try:
    with open(prog, "r+b") as f:
        f.write(b"\0")
    print("O5.write in place over running exe: allowed")
except OSError as e:
    print(f"O5.write in place over running exe: {e!r}")
run.kill()
print("platform:", sys.platform, os.uname().release)
