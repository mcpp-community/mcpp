#!/usr/bin/env python3
# mcpp#874 re-measure: the areas of the issue's tables, read from a workspace's
# target/ and the global build cache after `mcpp test --workspace`. It prints
# and never asserts.
#
#   874-measure.py WORKSPACE SINCE_EPOCH [LABEL]
#   874-measure.py --snapshot WORKSPACE > lines.json
#
# With NINJA_BASE=lines.json, the edges of a run are the lines its .ninja_log
# files gained since the snapshot (an edge whose output restat kept counts too).
import collections, json, os, subprocess, sys, time

def ninja_logs(target):
    return [os.path.join(r, ".ninja_log") for r, _, fs in os.walk(target) if ".ninja_log" in fs]

if sys.argv[1] == "--snapshot":
    t = os.path.join(sys.argv[2], "target")
    print(json.dumps({os.path.relpath(l, t): sum(1 for _ in open(l, errors="replace")) for l in ninja_logs(t)}))
    sys.exit(0)

ws, since = sys.argv[1], float(sys.argv[2])
base = json.load(open(os.environ["NINJA_BASE"])) if os.environ.get("NINJA_BASE") else None
label = sys.argv[3] if len(sys.argv) > 3 else "run"
target = os.path.join(ws, "target")
home = os.environ.get("MCPP_HOME") or os.path.expanduser("~/.mcpp")
out = []
def say(s=""): out.append(s); print(s)

def du(p):
    total = 0
    seen = set()
    for root, _, files in os.walk(p):
        for f in files:
            try: st = os.lstat(os.path.join(root, f))
            except OSError: continue
            if (st.st_dev, st.st_ino) in seen: continue
            seen.add((st.st_dev, st.st_ino)); total += st.st_blocks * 512
    return total
def gb(n): return f"{n / 2**30:.2f} GiB" if n >= 2**30 else f"{n / 2**20:.0f} MiB"

def sections(p):
    """(name, size, flags) of each section; flags hold C when compressed."""
    rows = []
    for l in subprocess.run(["readelf", "-S", "-W", p], capture_output=True, text=True).stdout.splitlines():
        if "]" not in l or l.lstrip().startswith("[Nr]"): continue
        t = l.split("]", 1)[1].split()
        if len(t) >= 9: rows.append((t[0], int(t[4], 16), t[6] if len(t) >= 10 else ""))
    return rows

def is_elf(p):
    try:
        with open(p, "rb") as f: return f.read(4) == b"\x7fELF"
    except OSError: return False

say(f"## {label}")
say()
say("### 1. Areas of target/ (allocated bytes, hard links counted once)")
say()
say("| Where | Size |")
say("|---|---:|")
fps = []
for triple in sorted(os.listdir(target)) if os.path.isdir(target) else []:
    tdir = os.path.join(target, triple)
    if not os.path.isdir(tdir) or triple.startswith("."): continue
    for fp in sorted(os.listdir(tdir)):
        d = os.path.join(tdir, fp)
        if not os.path.isdir(d): continue
        fps.append(d)
        for area in ("obj", "gcm.cache", "pcm.cache", "bin", "lib"):
            if os.path.isdir(os.path.join(d, area)):
                say(f"| `{triple}/{fp[:8]}…/{area}/` | {gb(du(os.path.join(d, area)))} |")
for area in ("host", "out"):
    p = os.path.join(target, ".build-mcpp", area)
    if os.path.isdir(p): say(f"| `target/.build-mcpp/{area}/` | {gb(du(p))} |")
say(f"| **`target/` in all** | **{gb(du(target))}** |")
say()

say("### 2. bin/")
say()
for d in fps:
    b = os.path.join(d, "bin")
    if not os.path.isdir(b): continue
    tests, archives, shared, inodes = [], [], [], {}
    for root, _, files in os.walk(b):
        for f in files:
            p = os.path.join(root, f)
            try: st = os.lstat(p)
            except OSError: continue
            if f.endswith(".a"): archives.append((p, st))
            elif ".so" in f and not f.endswith(".json"): shared.append((p, st))
            elif os.access(p, os.X_OK) and is_elf(p) and ("test" in f or "/tests/" in p): tests.append((p, st))
    say(f"`{os.path.relpath(b, ws)}`")
    say()
    if tests:
        sizes = sorted(st.st_size for _, st in tests)
        compressed = 0
        sample = max(tests, key=lambda t: t[1].st_size)[0]
        for p, _ in tests[:400]:
            if any(n == ".debug_info" and "C" in fl or n.startswith(".zdebug") for n, _, fl in sections(p)):
                compressed += 1
        say(f"- test programs: {len(tests)}, {gb(sum(sizes))}; median {gb(sizes[len(sizes)//2])}, largest {gb(sizes[-1])} (`{os.path.basename(sample)}`); compressed debug sections in {compressed} of {min(len(tests), 400)} read")
        dbg = [(n, z, fl) for n, z, fl in sections(sample) if n.startswith((".debug_", ".zdebug"))]
        say(f"  - `{os.path.basename(sample)}`: debug sections {gb(sum(z for _, z, _ in dbg))} of {gb(os.path.getsize(sample))}" + (", compressed" if any("C" in fl for _, _, fl in dbg) else ""))
    if archives:
        members, distinct, thin = 0, set(), 0
        for p, st in archives:
            with open(p, "rb") as f: thin += f.read(8) == b"!<thin>\n"
            names = subprocess.run(["ar", "t", p], capture_output=True, text=True).stdout.split()
            members += len(names); distinct.update(names)
        big = sorted(archives, key=lambda a: -a[1].st_size)[:4]
        say(f"- member archives: {len(archives)}, {gb(sum(st.st_blocks * 512 for _, st in archives))} allocated; {members} members for {len(distinct)} distinct names; thin: {thin}")
        say("  - largest: " + ", ".join(f"`{os.path.basename(p)}` {gb(st.st_size)}" for p, st in big))
    if shared:
        uniq = {(st.st_dev, st.st_ino): st for _, st in shared}
        dirs = {os.path.dirname(p) for p, _ in shared}
        say(f"- staged shared objects: {len(shared)} files in {len(dirs)} directories, {len(uniq)} distinct inodes, {gb(sum(st.st_size for st in uniq.values()))} distinct, {gb(sum(st.st_size for _, st in shared))} as named")
    say()

say("### 3. Build programs")
say()
host = os.path.join(target, ".build-mcpp", "host")
if os.path.isdir(host):
    nested = [r for r, ds, _ in os.walk(host) if r.endswith(os.path.join("target", ".build-mcpp", "host"))]
    progs = []
    for root, _, files in os.walk(host):
        for f in files:
            if f in ("build-program", "build.mcpp.bin") or f.startswith("build-program"):
                p = os.path.join(root, f)
                if is_elf(p): progs.append((os.path.relpath(p, host), os.path.getsize(p)))
    say(f"- groups under `target/.build-mcpp/host/`: {len([x for x in os.listdir(host) if os.path.isdir(os.path.join(host, x))])}")
    say(f"- nested `target/.build-mcpp/host` inside a group: {len(nested)}" + ("".join(f"\n  - `{os.path.relpath(n, target)}`" for n in nested[:5])))
    say(f"- build-program executables: {len(progs)}, {gb(sum(s for _, s in progs))}")
outd = os.path.join(target, ".build-mcpp", "out")
if os.path.isdir(outd):
    say(f"- generated-source trees under `target/.build-mcpp/out/`: {len(os.listdir(outd))}")
say()

say("### 4. Edges of this run (.ninja_log)")
say()
count, ms, hashes = collections.Counter(), collections.Counter(), collections.defaultdict(set)
logs = ninja_logs(target)
for log in logs:
    lines = open(log, errors="replace").read().split("\n")
    skip = base.get(os.path.relpath(log, target), 0) if base is not None else 1
    for line in lines[max(skip, 1):]:
        if line.count("\t") != 4: continue
        start, end, mtime, o, h = line.split("\t")
        t = int(mtime) / 1e9 if int(mtime) > 1e12 else int(mtime)
        if base is not None or t >= since:
            key = os.path.join(os.path.dirname(log), o)
            count[key] += 1; ms[key] += int(end) - int(start); hashes[key].add(h)
again = [o for o, c in count.items() if c > 1]
say(f"- logs: {len(logs)}; edges ran: {sum(count.values())} for {len(count)} distinct outputs")
say(f"- outputs built more than once: {len(again)}; repeated minutes: {round(sum(ms[o] * (count[o] - 1) / count[o] for o in again) / 60000, 1)} of {round(sum(ms.values()) / 60000, 1)}")
for o in sorted(again, key=lambda o: -count[o])[:8]:
    say(f"  - `{os.path.relpath(o, target)}`: {count[o]} runs, {len(hashes[o])} command hashes")
say()

say("### 5. Host tools in the global cache")
say()
tool = os.path.join(home, "build-cache", "v1", "tool")
if os.path.isdir(tool):
    keys = collections.defaultdict(list)
    for root, ds, files in os.walk(tool):
        if "entry.json" in files:
            rel = os.path.relpath(root, tool).split(os.sep)
            keys[os.sep.join(rel[:-1])].append(root)
    path_keys = [k for k in keys if "+path." in k]
    say(f"- `build-cache/v1/tool/`: {gb(du(tool))}; {len(keys)} keys, {sum(len(v) for v in keys.values())} variants; path-package keys: {len(path_keys)}")
    for k, v in sorted(keys.items(), key=lambda kv: -len(kv[1]))[:8]:
        say(f"  - `{k}`: {len(v)} variant(s)")
std = os.path.join(home, "build-cache", "v1", "std")
if os.path.isdir(std): say(f"- `build-cache/v1/std/`: {len(os.listdir(std))} builds, {gb(du(std))}")
say()

with open(os.environ.get("GITHUB_STEP_SUMMARY", os.devnull), "a") as f: f.write("\n".join(out) + "\n\n")
