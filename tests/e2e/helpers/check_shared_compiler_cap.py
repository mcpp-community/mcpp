#!/usr/bin/env python3
"""Check actual GCC/Ninja mixed compilation and the shared permit lifetime."""
import argparse
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


def settled(path, semaphore):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not state(path)["active"] and not list(semaphore.iterdir()):
            return
        time.sleep(0.02)
    raise AssertionError("compilers or permits survived the completed workload")


def check_mixed(mcpp, root, cap, ninja):
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
def update(start):
 with counter.with_suffix(".lock").open("a") as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  record=json.loads(counter.read_text()) if counter.exists() else {"active":[],"peak":0,"calls":0}
  if start:
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
        command = [ninja, "-j", str(cap * 6)]
        checked(command, build, log, env)
        semaphore = build / ".mcpp-sched"
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
        print(f"PASS: mixed GCC modules/C++/C cap={cap}, joined without deadlock, no-op and both failure recoveries")


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
    process = subprocess.Popen([*command, *child], cwd=root)
    pid = None
    try:
        deadline = time.monotonic() + 5
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert pid_file.exists(), "controlled compiler did not start"
        pid = int(pid_file.read_text())
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=5) != 0
        deadline = time.monotonic() + 5
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not alive(pid), "cancellation left the compiler alive"
        assert not list(semaphore.iterdir()), "cancellation retained its compiler permit"
        result = subprocess.run([*command, sys.executable, "-c", "raise SystemExit(7)"], cwd=root)
        assert result.returncode == 7 and not list(semaphore.iterdir()), "failure lost its status or leaked a permit"
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
    args = parser.parse_args()
    args.fixture.mkdir(parents=True)
    ninja = shutil.which("ninja")
    assert ninja, "Ninja is required"
    for cap in (1, 2):
        check_mixed(str(args.mcpp.resolve()), args.fixture.resolve(), cap, ninja)
    check_cancellation(str(args.mcpp.resolve()), args.fixture.resolve())


if __name__ == "__main__":
    main()
