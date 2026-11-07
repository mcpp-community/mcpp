#!/usr/bin/env python3
"""Exercise run generations with real Ninja, signals and delayed state writers."""
import argparse
import fcntl
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import time


def until(predicate, description, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError(description)


def alive(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()
        return fields[0] != "Z"
    except FileNotFoundError:
        return False


def integer(path):
    try:
        return int(path.read_text().split()[0])
    except (FileNotFoundError, ValueError, IndexError):
        return 0


class Case:
    def __init__(self, root, name, binary, kind="module", linked=False, **extra):
        self.out = root / name / "target/host/fingerprint"
        if linked:
            cache = root / name / "cache-host"
            cache.mkdir(parents=True)
            self.out.parents[1].mkdir()
            self.out.parent.symlink_to(cache, target_is_directory=True)
        self.out.mkdir(parents=True)
        self.env = os.environ.copy()
        self.env.update(GTEST_FILTER="DetachLifetime.SubprocessDriver",
                        MCPP_TEST_LIFETIME_ROLE="root", MCPP_TEST_LIFETIME_KIND=kind,
                        MCPP_TEST_LIFETIME_OUTPUT=str(self.out))
        self.env.update(extra)
        self.binary = binary
        self.roots = []
        self.logs = []
        word = shlex.quote(str(binary))
        # A separate group protects the deliberate state-writer stall from
        # Ninja cancellation, while preserving the owner's native session.
        prefix = ("python3 -c 'import os,sys;os.setpgid(0,0);os.execvp(sys.argv[1],sys.argv[1:])' "
                  if extra.get("MCPP_TEST_LIFETIME_BLOCK") else "")
        (self.out / "compile.command").write_text(f"env MCPP_TEST_LIFETIME_ROLE=compiler {word}")
        if kind == "module":
            graph = (f"rule phase\n  command = {prefix}env MCPP_TEST_LIFETIME_ROLE=phase {word}\n  restat = 1\n"
                     f"rule await\n  command = env MCPP_TEST_LIFETIME_ROLE=await {word}\n"
                     "build published.gcm: phase source\nbuild module.o: await published.gcm\ndefault module.o\n")
        else:
            graph = (f"rule ordinary\n  command = env MCPP_TEST_LIFETIME_ROLE=ordinary {word}\n"
                     "build ordinary.o: ordinary source\ndefault ordinary.o\n")
        (self.out / "build.ninja").write_text(graph)
        (self.out / "source").write_text("input")

    def start(self, delay=20000, **extra):
        env = dict(self.env, MCPP_TEST_LIFETIME_DELAY=str(delay), **extra)
        log = (self.out / f"run-{len(self.roots)}.log").open("w")
        self.logs.append(log)
        child = subprocess.Popen([str(self.binary)], cwd=self.out, env=env,
                                 stdout=log, stderr=subprocess.STDOUT)
        self.roots.append(child)
        return child

    def ready(self, child):
        until(lambda: integer(self.out / "root.ready") == child.pid,
              f"new root did not cross generation barrier: {self.out.name}")

    def compiler(self):
        until(lambda: integer(self.out / "compiler.ready") != 0, "compiler did not start")
        return integer(self.out / "compiler.ready")

    def success(self, child):
        assert child.wait(timeout=8) == 0, "new generation failed: " + self.diagnostics()
        obj = "module.o" if self.env["MCPP_TEST_LIFETIME_KIND"] == "module" else "ordinary.o"
        assert (self.out / obj).read_text() == "complete"

    def diagnostics(self):
        for log in self.logs:
            log.flush()
        return "\n".join(path.read_text()[-3000:] for path in self.out.glob("run-*.log"))

    def close(self):
        # Only PIDs recorded by this private fixture may be ended here.
        for child in self.roots:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
        for name in ("compiler.ready", "writer.ready", "phase.pid"):
            pid = integer(self.out / name)
            if pid and alive(pid):
                os.kill(pid, signal.SIGKILL)
        for log in self.logs:
            log.close()


def cancellation(root, binary, sig, kind, linked=False):
    case = Case(root, f"cancel-{sig.name}-{kind}" + ("-linked" if linked else ""), binary, kind, linked)
    try:
        old = case.start()
        case.ready(old)
        if linked:
            assert (case.out.parents[2] / ".mcpp/compiler-runs").is_dir(), "runtime locks followed the target symlink"
        compiler = case.compiler()
        old.send_signal(sig)
        # Begin immediately: no waiting for old compiler death before launch.
        new = case.start(delay=30, MCPP_HOME=str(root / "changed-home"))
        case.ready(new)
        assert not alive(compiler), "new generation admitted a live old compiler"
        case.success(new)
        old.wait(timeout=5)
        calls = (case.out / "compiler.runs").read_text()
        obj = case.out / ("module.o" if kind == "module" else "ordinary.o")
        stamp = obj.stat().st_mtime_ns
        noop = case.start(delay=30)
        case.success(noop)
        assert (case.out / "compiler.runs").read_text() == calls, "generation caused no-op compilation"
        assert obj.stat().st_mtime_ns == stamp
        assert not list(case.out.glob("*.bak")), "old backup survived recovery"
        print(f"PASS: {kind} {sig.name}" + (" with linked target" if linked else "")
              + ", changed home, immediate restart, output recovery and unchanged no-op")
    finally:
        case.close()


def owner_closure(root, binary):
    case = Case(root, "owner-lock-closure", binary, MCPP_TEST_LIFETIME_CLOSE="1")
    try:
        child = case.start()
        case.ready(child)
        compiler = case.compiler()
        until(lambda: (case.out / "owner.closed").exists(), "scope did not close")
        assert alive(child.pid), "control did not retain the owner process"
        assert not alive(compiler), "owner file and live process overrode closed kernel lock"
        assert child.wait(timeout=5) != 0
        print("PASS: releasing owner lock revokes helpers while owner process is still alive")
    finally:
        case.close()


def blocked_writer(root, binary, restore):
    case = Case(root, "restore-barrier" if restore else "settle-barrier", binary,
                MCPP_TEST_LIFETIME_BLOCK="1", MCPP_TEST_LIFETIME_PUBLICATION_GATE="publish.release")
    descriptor=None
    lease_held=False
    previous_handler=signal.getsignal(signal.SIGIO)
    breaks=[]
    signal.signal(signal.SIGIO,lambda *_:breaks.append(time.monotonic()))
    try:
        (case.out / "published.gcm").write_text("same BMI")
        time.sleep(0.02)
        (case.out / "source").touch()
        old = case.start(delay=20, MCPP_TEST_LIFETIME_FAIL="19" if restore else "0")
        case.ready(old)
        compiler=case.compiler()
        baseline=next((case.out.parents[2]/".mcpp/compiler-runs").glob("*/run-*/attempts/*/baseline.bmi"))
        # A kernel write lease delays the real reader, including direct-syscall
        # OS I/O. No product hook or libc interception changes the state writer.
        descriptor=os.open(baseline,os.O_RDWR)
        fcntl.fcntl(descriptor,fcntl.F_SETOWN,os.getpid())
        fcntl.fcntl(descriptor,fcntl.F_SETLEASE,fcntl.F_WRLCK)
        lease_held=True
        (case.out/"publish.release").write_text("go")
        until(lambda:bool(breaks),"state writer did not reach the certified baseline")
        until(lambda:not alive(compiler),"compiler still alive at state-writer control")
        if not restore:
            old.send_signal(signal.SIGINT)
        new=case.start(delay=30,MCPP_TEST_LIFETIME_BLOCK="",MCPP_TEST_LIFETIME_PUBLICATION_GATE="")
        time.sleep(.15)
        assert integer(case.out/"root.ready")!=new.pid,"new generation bypassed a pending state writer"
        fcntl.fcntl(descriptor,fcntl.F_SETLEASE,fcntl.F_UNLCK)
        lease_held=False
        case.ready(new)
        case.success(new)
        assert old.wait(timeout=5)!=0
        assert not (case.out/"published.gcm.bak").exists()
        assert (case.out/"unit.rc").read_text().strip()=="0"
        print("PASS: prior "+("generation restore" if restore else "phase1 settle")
              +" is serialized before the next generation after compiler permit release",flush=True)
    finally:
        if descriptor is not None:
            try:
                if lease_held:fcntl.fcntl(descriptor,fcntl.F_SETLEASE,fcntl.F_UNLCK)
            finally:
                os.close(descriptor)
        signal.signal(signal.SIGIO,previous_handler)
        case.close()


def invalid_owner(root, binary):
    for identity in ("0 0", f"{os.getpid()} 0", f"{os.getpid()} 1"):
        case = Case(root, "invalid-" + identity.replace(" ", "-"), binary)
        try:
            generation = case.out / "forged-run"
            generation.mkdir()
            (generation / "owner").write_text("mcpp-compiler-run-v1\n" + identity + "\n")
            env = dict(case.env, MCPP_TEST_LIFETIME_ROLE="phase", MCPP_COMPILER_RUN=str(generation))
            with (generation / "owner.lock").open("a+") as lock:
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = subprocess.run([str(binary)], cwd=case.out, env=env, capture_output=True, timeout=5)
            assert result.returncode == 130, "invalid owner identity was accepted"
            assert not (case.out / "compiler.ready").exists()
            assert not (case.out / "unit.bmi").exists(), "late rejected phase1 wrote state"
        finally:
            case.close()
    print("PASS: missing birth and reused PID identities fail closed before state writes")


def late_registration(root, binary, supervisor):
    case = Case(root, "late-supervisor" if supervisor else "late-phase1", binary)
    delayed = None
    try:
        old = case.start()
        case.ready(old)
        case.compiler()
        generation = next((case.out.parents[2] / ".mcpp/compiler-runs").glob("*/run-*"))
        gate = case.out / "late.release"
        command = [str(binary)]
        if supervisor:
            command += ["bmi-supervise", "--slot", "unit", "--sem", "permits", "--cap", "1",
                        "--object", "module.o", "--command-file", "compile.command"]
        env = dict(case.env, MCPP_TEST_LIFETIME_ROLE="phase", MCPP_COMPILER_RUN=str(generation))
        waiter = ("import os,sys,time;from pathlib import Path;"
                  "gate=Path(sys.argv[1]);\nwhile not gate.exists():time.sleep(.01)\n"
                  "os.execv(sys.argv[2],sys.argv[2:])")
        delayed = subprocess.Popen(["python3", "-c", waiter, str(gate), *command],
                                   cwd=case.out, env=env, start_new_session=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        old.send_signal(signal.SIGINT)
        new = case.start(delay=30)
        case.ready(new)
        case.success(new)
        names = ("published.gcm", "module.o", "unit.rc", "unit.bmi", "compiler.runs")
        before = {name: ((case.out / name).read_bytes(), (case.out / name).stat().st_mtime_ns)
                  for name in names}
        gate.write_text("go")
        assert delayed.wait(timeout=5) == 130, "late old helper entered the new run"
        after = {name: ((case.out / name).read_bytes(), (case.out / name).stat().st_mtime_ns)
                 for name in names}
        assert before == after, "late helper changed new-generation artifacts or status"
        assert not (case.out / "published.gcm.bak").exists()
        print("PASS: unregistered old " + ("supervisor" if supervisor else "phase1")
              + " cannot mutate new generation")
    finally:
        if delayed is not None and delayed.poll() is None:
            delayed.kill()
            delayed.wait()
        case.close()



def record(path, tag):
    data=path.read_bytes()
    body,checksum=data[:-18],data[-17:-1]
    value=0xcbf29ce484222325
    for byte in body:value=((value^byte)*0x100000001b3)&((1<<64)-1)
    assert checksum==f"{value:016x}".encode()
    schema,count,body=body.split(b"\n",2)
    assert schema.decode()==tag
    fields=[]
    for _ in range(int(count)):
        size,body=body.split(b"\n",1);size=int(size)
        fields.append(body[:size].decode());body=body[size:]
    assert not body
    return fields


def hard_controller_loss(root, binary, kind):
    case=Case(root,"hard-controller-"+kind,binary,kind,
              MCPP_TEST_LIFETIME_DESCENDANT="1")
    try:
        if kind=="ordinary":
            word=shlex.quote(str(binary))
            (case.out/"start-second.py").write_text("from pathlib import Path\nimport time\nwhile not Path('writer.ready').exists():time.sleep(.01)\nPath('second.gate').touch()\n")
            with (case.out/"build.ninja").open("a") as graph:
                graph.write("rule gate\n  command = python3 start-second.py\n"
                            "build second.gate: gate\n"
                            "rule second\n  command = env MCPP_TEST_LIFETIME_ROLE=ordinary "
                            f"MCPP_TEST_LIFETIME_OBJECT=second.o {word}\n"
                            "build second.o: second source | second.gate\ndefault ordinary.o second.o\n")
        old=case.start(delay=1800);case.ready(old);case.compiler()
        until(lambda:integer(case.out/"writer.ready")!=0,"descendant did not start")
        writer=integer(case.out/"writer.ready")
        runtime=case.out.parents[2]/".mcpp/compiler-runs"
        def admitted():
            found=list(runtime.glob("*/run-*/attempts/*/admitted"))
            return next((p.parent for p in found if (p.parent/"unit").exists()),None)
        until(lambda:admitted() is not None,"unit was not admitted")
        attempt=admitted();unit=record(attempt/"unit","mcpp-unit-v1")
        controller=int(unit[1]);os.kill(controller,signal.SIGKILL)
        reservation=Path(record(attempt/"reservation","mcpp-reservation-pointer-v1")[0])
        lock=reservation.with_suffix(".lock")
        time.sleep(.15)
        with lock.open("a+") as handle:
            fcntl.lockf(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
        assert alive(writer),"descendant control ended too early"
        assert reservation.exists(),"live descendant lost its logical reservation"
        obj=case.out/("ordinary.o" if kind=="ordinary" else "module.o")
        assert obj.read_text()=="partial","output changed before writer completion"
        assert len((case.out/"compiler.runs").read_text().splitlines())==1,"cap1 admitted a replacement compiler"
        assert old.wait(timeout=8)!=0,"hard controller loss reported success"
        assert (case.out/"writer.finished").exists(),"counterexample descendant did not perform its late write"
        assert not obj.exists(),"quiet orphan result was not invalidated"
        assert not reservation.exists(),"quiet orphan kept its reservation"
        if kind=="module":assert not(case.out/"published.gcm").exists()
        # The actual helper is normal on retry; admission remains capped and
        # the late writer cannot overwrite this generation's result.
        case.env.pop("MCPP_TEST_LIFETIME_DESCENDANT")
        new=case.start(delay=30);case.ready(new);case.success(new)
        print("PASS:",kind,"hard controller loss retains cap1 while descendant writes; quiet recovery and retry",flush=True)
    finally:
        case.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    assert shutil.which("ninja"), "Ninja required"
    invalid_owner(args.fixture, args.binary)
    for kind in ("module","ordinary"):
        hard_controller_loss(args.fixture,args.binary,kind)
    for kind in ("module", "ordinary"):
        for sig in (signal.SIGINT, signal.SIGKILL):
            cancellation(args.fixture, args.binary, sig, kind)
    cancellation(args.fixture, args.binary, signal.SIGKILL, "module", linked=True)
    owner_closure(args.fixture, args.binary)
    for restore in (False, True):
        blocked_writer(args.fixture, args.binary, restore)
    for supervisor in (False, True):
        late_registration(args.fixture, args.binary, supervisor)


if __name__ == "__main__":
    main()
