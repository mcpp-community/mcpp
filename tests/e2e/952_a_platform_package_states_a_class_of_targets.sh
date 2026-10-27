#!/usr/bin/env bash
# requires: unix-shell python3
# 952 -- a platform package that has nothing to do with openkal states the
# configuration of a class of targets, and the engine carries every group of it
# (SPEC-013 §4; design 2026-10-28 §12.1, the board-support row).
#
# The platform here is a local directory for this machine's own triples. It
# states compile flags, the name of a program, a seal step and a runner; the
# project selects it by path for its system and overrides one field under
# `cfg(platform = "demo")`.
#
# Criteria:
#   A. The platform's compile flag reaches the project's units; the program is
#      named by the platform's pattern; the seal step ran on it before it was
#      published; `mcpp run` starts it through the platform's runner.
#   B. The report names the platform and the source of its selection.
#   C. An override under `cfg(platform = "demo")` takes the field's place
#      (`link.exe`), and the report's JSON says where each field came from.
#   D. `--platform none` builds the project without the platform: the default
#      name, no flag, no seal, no runner.
#   E. A platform package without `[platform]` is refused, naming the package.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/plat" "$TMP/app/src"
cat > "$TMP/plat/mcpp.toml" <<'TOML'
[package]
name    = "demo-platform"
version = "0.1.0"

[platform]
targets = ["x86_64-linux-gnu", "aarch64-linux-gnu", "aarch64-macos", "x86_64-macos"]
runner  = ["env", "DEMO_PLATFORM_RUNNER=yes", "{}"]

[platform.build]
cxxflags = ["-DDEMO_PLATFORM=1"]

[platform.link]
exe  = "{name}.demo"
seal = [["sh", "-c", "printf DEMO-SEALED >> \"$0\"", "{}"]]
TOML

cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[target.'cfg(any(linux, macos))']
platform = { path = "../plat" }
TOML
cat > src/main.cpp <<'CPP'
#include <cstdio>
#include <cstdlib>
int main() {
#ifdef DEMO_PLATFORM
    const char* flag = "flag";
#else
    const char* flag = "no-flag";
#endif
    const char* runner = std::getenv("DEMO_PLATFORM_RUNNER");
    std::printf("demo: %s, runner %s\n", flag, runner ? runner : "none");
    return 0;
}
CPP

# A
"$MCPP" build > build.log 2>&1 || fail "A: build" build.log
prog=$(ls target/*/*/bin/app.demo 2>/dev/null | head -1)
[ -n "$prog" ] || fail "A: no program named by the platform's pattern (app.demo)" build.log
[ "$(tail -c 11 "$prog")" = "DEMO-SEALED" ] || fail "A: the seal step did not run before publishing" build.log
"$MCPP" run > run.log 2>&1 || fail "A: run" run.log
grep -q "demo: flag, runner yes" run.log || fail "A: the flag or the runner did not reach the program" run.log
echo "ok: the platform's flag, name, seal and runner reach the program"

# B
grep -qE "Platform +demo-platform 0\.1\.0 \(mcpp\.toml" build.log || fail "B: the report does not name the platform and its source" build.log
echo "ok: the report names the platform and where it was selected"

# C
cat >> mcpp.toml <<'TOML'

[target.'cfg(platform = "demo")'.link]
exe = "{name}.over"
TOML
"$MCPP" build > over.log 2>&1 || fail "C: build with the override" over.log
ls target/*/*/bin/app.over > /dev/null 2>&1 || fail "C: the override did not take the field's place" over.log
host=$(python3 - <<'PY'
import platform
m = platform.machine().lower()
arch = "aarch64" if m in ("arm64", "aarch64") else "x86_64"
print(f"{arch}-macos" if platform.system() == "Darwin" else f"{arch}-linux-gnu")
PY
)
"$MCPP" self env --target "$host" --format json > env.json 2> env.err || fail "C: self env" env.err env.json
python3 - env.json <<'PY' || fail "C: the JSON's sources" env.json
import json, sys
p = json.load(open(sys.argv[1]))["data"]["platform"]
assert p and p["name"].endswith("demo-platform"), p
f = p["fields"]
assert f["link.exe"]["source"].startswith("mcpp.toml"), f["link.exe"]
assert f["runner"]["source"] == "platform", f["runner"]
PY
echo "ok: an override under cfg(platform) takes the field's place, and the JSON says where"

# D
"$MCPP" run --platform none > none.log 2>&1 || fail "D: run --platform none" none.log
grep -q "demo: no-flag, runner none" none.log || fail "D: --platform none kept a field of the platform" none.log
ls target/*/*/bin/app > /dev/null 2>&1 || fail "D: the program is not under its default name" none.log
echo "ok: --platform none builds without the platform"

# E
mkdir -p "$TMP/notplat"
printf '[package]\nname = "notplat"\nversion = "0.1.0"\n' > "$TMP/notplat/mcpp.toml"
if "$MCPP" build --platform "path:$TMP/notplat" > notplat.log 2>&1; then
    fail "E: a package without [platform] was accepted as a platform" notplat.log
fi
grep -q "has no \[platform\] section" notplat.log || fail "E: the refusal does not say why" notplat.log
grep -q "notplat" notplat.log || fail "E: the refusal does not name the package" notplat.log
echo "ok: a package without [platform] is refused as a platform"

echo "PASS: 952 a platform package states a class of targets"
