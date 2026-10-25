#!/usr/bin/env python3
# D22 / O5: what a process that starts a program sees while a linker rewrites
# it, per linker and per publication strategy. Prints counts; never asserts.
#
#   in-place   the linker writes the output name directly (today)
#   tmp+rename the linker writes <out>.tmp, then rename over <out>   (POSIX D22)
#   aside      rename <out> to <out>.old-N, then link to <out>      (PE D22)
import os, shutil, subprocess, sys, tempfile, threading, time

win = sys.platform == "win32"
d = tempfile.mkdtemp()
os.chdir(d)
n = 6000
with open("big.cpp", "w") as f:
    for i in range(n):
        f.write(f"int f{i}(int x) {{ return x * {i} + {i % 7}; }}\n")
    f.write("int main(int argc, char**) { int s = 0;\n")
    for i in range(0, n, 3):
        f.write(f"  s += f{i}(argc);\n")
    f.write("  return s == 12345 ? 1 : 0; }\n")

linkers = []
if win:
    subprocess.run(["cl", "/nologo", "/c", "/Od", "big.cpp"], check=True, stdout=subprocess.DEVNULL)
    linkers.append(("link.exe", lambda out: ["link", "/nologo", "/DEBUG", f"/OUT:{out}", "big.obj"]))
    if shutil.which("lld-link"):
        linkers.append(("lld-link", lambda out: ["lld-link", "/nologo", "/DEBUG", f"/OUT:{out}", "big.obj"]))
else:
    cxx = os.environ.get("CXX", "c++")
    subprocess.run([cxx, "-c", "-O0", "-g", "big.cpp", "-o", "big.o"], check=True)
    if sys.platform == "darwin":
        linkers.append(("ld64", lambda out: [cxx, "big.o", "-o", out]))
    else:
        linkers.append(("bfd", lambda out: [cxx, "-fuse-ld=bfd", "big.o", "-o", out]))
    for ld in ("lld", "gold", "mold"):
        if subprocess.run([cxx, f"-fuse-ld={ld}", "big.o", "-o", "probe-ld"], capture_output=True).returncode == 0:
            linkers.append((ld, lambda out, ld=ld: [cxx, f"-fuse-ld={ld}", "big.o", "-o", out]))

exe = "app.exe" if win else "./app"

def publish(strategy, cmd, k):
    if strategy == "in-place":
        subprocess.run(cmd(exe), capture_output=True)
    elif strategy == "tmp+rename":
        tmp = exe + ".tmp" + (".exe" if win else "")
        subprocess.run(cmd(tmp), capture_output=True)
        try:
            os.replace(tmp, exe)
        except OSError as e:
            errors["publish:" + type(e).__name__ + str(getattr(e, "winerror", ""))] = errors.get("publish:" + type(e).__name__ + str(getattr(e, "winerror", "")), 0) + 1
    else:  # aside
        try:
            if os.path.exists(exe):
                os.replace(exe, f"{exe}.old-{k}")
        except OSError as e:
            errors["aside:" + type(e).__name__] = errors.get("aside:" + type(e).__name__, 0) + 1
        subprocess.run(cmd(exe), capture_output=True)

for name, cmd in linkers:
    for strategy in ("in-place", "tmp+rename", "aside"):
        for f in os.listdir("."):
            if f.startswith("app"):
                try: os.remove(f)
                except OSError: pass
        subprocess.run(cmd(exe), capture_output=True)
        errors, runs, stop = {}, [0], [False]
        def spawner():
            while not stop[0]:
                try:
                    subprocess.run([exe], capture_output=True, timeout=10)
                    runs[0] += 1
                except OSError as e:
                    key = f"{type(e).__name__}:{getattr(e, 'winerror', None) or e.errno}"
                    errors[key] = errors.get(key, 0) + 1
        ths = [threading.Thread(target=spawner) for _ in range(4)]
        for t in ths: t.start()
        t0 = time.time()
        for k in range(25):
            publish(strategy, cmd, k)
        stop[0] = True
        for t in ths: t.join()
        print(f"D22 {sys.platform} linker={name:8} strategy={strategy:10} links=25 ({time.time()-t0:.1f}s) "
              f"runs ok={runs[0]} spawn errors={errors or 'none'}")
