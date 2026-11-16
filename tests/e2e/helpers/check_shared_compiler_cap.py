#!/usr/bin/env python3
"""Check actual GCC/Ninja mixed compilation and the shared permit lifetime."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time


def checked(command, cwd, log, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    if result.returncode:
        log.flush()
        raise AssertionError(f"command failed ({result.returncode}): "
                             + Path(log.name).read_text(errors="replace")[-5000:])


def state(path):
    return json.loads(path.read_text()) if path.exists() else {"active": [], "peak": 0, "calls": 0}


def process_birth(pid):
    try:
        fields=Path(f"/proc/{pid}/stat").read_text().split(")",1)[1].split()
        return int(fields[19]) if fields[0]!="Z" else None
    except (FileNotFoundError,ProcessLookupError):
        return None


def active_originals(record):
    births=record.get("births",{})
    if any(str(pid) not in births for pid in record["active"]):
        raise AssertionError("counter has a PID without its original birth identity")
    return [pid for pid in record["active"] if process_birth(pid)==births[str(pid)]]


def permits_free(semaphore):
    for entry in semaphore.iterdir():
        if not entry.is_file() or entry.name.endswith(".reservation"):
            return False
        try:
            with entry.open("r+b") as lock:
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
    return True


def settled(path, semaphore):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not active_originals(state(path)) and permits_free(semaphore):
            return
        time.sleep(0.02)
    details={"counter":state(path),"pid_alive":{str(pid):alive(pid) for pid in state(path)["active"]},
             "permit_files":[entry.name for entry in semaphore.iterdir()],"kernel_locks_free":permits_free(semaphore)}
    path.with_name("unsettled-evidence.json").write_text(json.dumps(details,indent=2)+"\n")
    raise AssertionError("compilers or permits survived the completed workload: "+json.dumps(details))


def check_mixed(mcpp, root, cap, ninja, owner_driver=None):
    project = root / f"mixed-{cap}"
    (project / "src").mkdir(parents=True)
    (project / "mcpp.toml").write_text('''[package]
name = "cap_probe"
version = "0.1.0"
[toolchain]
default = "gcc@16.1.0"
[build]
bmi_schedule = "on"
''')
    (project / "src/core.cppm").write_text("export module cap.core;\nexport int core_value() { return 7; }\n")
    (project / "src/mid.cppm").write_text("export module cap.mid;\nimport cap.core;\nexport int value() { return core_value(); }\n")
    (project / "src/main.cpp").write_text("import cap.mid;\nint main() { return value() == 7 ? 0 : 1; }\n")
    for index in range(6):
        (project / f"src/ordinary_{index}.cpp").write_text(f"int ordinary_{index}() {{ return {index}; }}\n")
        (project / f"src/c_{index}.c").write_text(f"int c_{index}(void) {{ return {index}; }}\n")
    with (project / "build.log").open("w") as log:
        checked([mcpp, "build", "--release", "--configure-only", "-j", str(cap)], project, log)
        graphs = list(project.glob("target/*/*/build.ninja"))
        assert len(graphs) == 1
        graph = graphs[0]
        build = graph.parent
        counter = project / "counter.json"
        wrapper = project / "compiler-driver"
        wrapper.write_text("#!" + sys.executable + r'''
import fcntl,json,os,subprocess,sys,time
from pathlib import Path
counter=Path(os.environ["MCPP_CAP_COUNTER"])
def born(pid):
 try:
  fields=Path(f"/proc/{pid}/stat").read_text().split(")",1)[1].split()
  return int(fields[19]) if fields[0]!="Z" else None
 except (FileNotFoundError,ProcessLookupError):return None
def update(start):
 with counter.with_suffix(".lock").open("a") as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  record=json.loads(counter.read_text()) if counter.exists() else {"active":[],"peak":0,"calls":0}
  births=record.setdefault("births",{})
  if any(str(pid) not in births for pid in record["active"]):raise RuntimeError("counter birth missing")
  expired=[pid for pid in record["active"] if born(pid)!=births[str(pid)]]
  record.setdefault("ended_without_finally",[]).extend({"pid":pid,"birth":births[str(pid)]} for pid in expired)
  record["active"]=[pid for pid in record["active"] if pid not in expired]
  if start:
   births[str(os.getpid())]=born(os.getpid())
   record["active"].append(os.getpid());record["calls"]+=1
   record["peak"]=max(record["peak"],len(record["active"]))
  else:record["active"].remove(os.getpid())
  temporary=counter.with_suffix(".tmp");temporary.write_text(json.dumps(record));temporary.replace(counter)
update(True)
try:
 result=subprocess.run(sys.argv[1:]).returncode
 time.sleep(0.15) # keep the driver alive after BMI publication
 failed=os.environ.get("MCPP_CAP_TEST_FAIL")
 if failed and "-c" in sys.argv and any(arg.endswith("/"+failed) for arg in sys.argv):
  print("controlled compiler failure",file=sys.stderr);result=17
finally:update(False)
sys.exit(result)
''')
        wrapper.chmod(0o755)
        text = graph.read_text()
        word = shlex.quote(str(wrapper)).replace("$", "$$")
        text = re.sub(r"^(cxx|cc|cxx_driver)\s*= (.+)$", lambda match:
                      match[1] + " = " + word + " " + match[2], text, flags=re.MULTILINE)
        graph.write_text(text)
        env = os.environ.copy()
        env["MCPP_CAP_COUNTER"] = str(counter)
        env["MCPP_COMPILER_CAP"] = str(cap)  # build.ninja names no cap; outside a generation it is this
        command = [ninja, "-j", str(cap * 6)]
        if owner_driver:
            env.update(MTEST_PROTOCOL="",
                       MCPP_TEST_LIFETIME_ROLE="root", MCPP_TEST_LIFETIME_OUTPUT=str(build),
                       MCPP_TEST_LIFETIME_NINJA_JOBS=str(cap*6), MCPP_TEST_LIFETIME_CAP=str(cap))
            command=[str(owner_driver)]
        checked(command, build, log, env)
        semaphore = next((project/".mcpp/compiler-runs").glob("*/slots")) if owner_driver else build/".mcpp-sched"
        settled(counter, semaphore)
        measured = state(counter)
        assert measured["peak"] == cap, f"actual compiler driver peak {measured['peak']} differs from cap {cap}"
        checked([str(next(build.glob("bin/cap_probe")))], build, log, env)
        checked(command, build, log, env)
        assert state(counter)["calls"] == measured["calls"], "no-op build launched compilers"
        for failed in ("ordinary_0.cpp", "core.cppm"):
            checked([ninja, "-t", "clean"], build, log, env)
            env["MCPP_CAP_TEST_FAIL"] = failed
            result = subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
            assert result.returncode != 0, "compiler failure was swallowed"
            log.flush()
            assert "controlled compiler failure" in Path(log.name).read_text(), "compiler diagnostic was lost"
            settled(counter, semaphore)
            del env["MCPP_CAP_TEST_FAIL"]
            checked(command, build, log, env)
            settled(counter, semaphore)
            assert state(counter)["peak"] <= cap, "failure recovery exceeded the compiler cap"
        mode="owned-unit generation" if owner_driver else "standalone permits"
        (root/f"cap-{cap}.json").write_text(json.dumps({"mode":mode,"cap":cap,"ninja_jobs":cap*6,
            "peak":state(counter)["peak"],"calls":state(counter)["calls"],"active":state(counter)["active"],
            "normal_and_late_failure_recovery":True,"unchanged_noop":True},indent=2)+"\n")
        print(f"PASS: mixed GCC modules/C++/C cap={cap} ({mode}), joined without deadlock, no-op and both failure recoveries")


def alive(pid):
    try:
        os.kill(pid, 0)
        status = Path(f"/proc/{pid}/stat")
        return not status.exists() or status.read_text().split(")", 1)[1].split()[0] != "Z"
    except ProcessLookupError:
        return False


def check_cancellation(mcpp, root):
    semaphore = root / "cancel-semaphore"
    semaphore.mkdir()
    pid_file = root / "child.pid"
    command = [mcpp, "__compiler-cap", "--sem", str(semaphore), "--cap", "1", "--"]
    child = [sys.executable, "-c", "import os,time;from pathlib import Path;"
             f"Path({str(pid_file)!r}).write_text(str(os.getpid()));time.sleep(60)"]
    wrapper=root/"mixed-1/compiler-driver"
    counter=root/"cancel-counter.json"
    env=dict(os.environ,MCPP_CAP_COUNTER=str(counter))
    process = subprocess.Popen([*command,str(wrapper),*child],cwd=root,env=env)
    pid = None
    try:
        deadline = time.monotonic() + 5
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert pid_file.exists(), "controlled compiler did not start"
        pid = int(pid_file.read_text())
        original=state(counter)
        assert len(original["active"])==1
        wrapper_pid=original["active"][0];wrapper_birth=original["births"][str(wrapper_pid)]
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=5) != 0
        deadline = time.monotonic() + 5
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not alive(pid), "cancellation left the compiler alive"
        settled(counter,semaphore)
        assert permits_free(semaphore), "cancellation retained its compiler permit"
        result = subprocess.run([*command,str(wrapper),sys.executable,"-c","raise SystemExit(7)"],cwd=root,env=env)
        assert result.returncode == 7 and permits_free(semaphore), "failure lost its status or leaked a permit"
        record=state(counter)
        assert {"pid":wrapper_pid,"birth":wrapper_birth} in record["ended_without_finally"], "hard-killed counter entry was not retained as history"
        assert not active_originals(record) and record["peak"]==1, "dead counter history inflated live compiler capacity"
        words = ["space value", 'literal "quote"', "back\\slash", "$literal", "", "Ω",
                 "--system", "--home", "--deny", "--toolchain", "-j200", "--sem", "--cap"]
        result = subprocess.run([*command, sys.executable, "-c", "import json,sys;print(json.dumps(sys.argv[1:]))", *words],
                                cwd=root, capture_output=True, text=True)
        assert result.returncode == 0 and json.loads(result.stdout) == words, "compiler argument words changed"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if pid is not None and alive(pid):
            os.kill(pid, signal.SIGKILL)
    print("PASS: compiler cancellation ends its child and releases the permit; argv and failure status retained")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mcpp", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--owner-driver",type=Path,
                        help="fresh schedule unit-test driver for owned-generation coverage")
    args = parser.parse_args()
    args.fixture.mkdir(parents=True)
    ninja = shutil.which("ninja")
    assert ninja, "Ninja is required"
    for cap in (1, 2):
        check_mixed(str(args.mcpp.resolve()), args.fixture.resolve(), cap, ninja,
                    args.owner_driver.resolve() if args.owner_driver else None)
    check_cancellation(str(args.mcpp.resolve()), args.fixture.resolve())


if __name__ == "__main__":
    main()
