#!/usr/bin/env bash
# requires: llvm python3
# 951 -- `<arch>-openkal` needs no declaration: the target's platform supplies
# the C library, the C++ run-time, the tools and the runner (SPEC-013; design
# 2026-10-28 §7, §10).
#
# The target table pins `openkal-platform` for both ok-image rows. A project
# that names nothing but its sources builds a C++ image with `import std`, runs
# it, tests it and packs it; what the platform added is reported as the
# platform's and locked as such; the overrides take their place by the rules
# of SPEC-013 §4.3-§4.5.
#
# Criteria:
#   A. `mcpp new`-shaped project, `mcpp run --target <host-arch>-openkal`: the
#      image runs through okrun, with exceptions and the standard library.
#   B. The report names the platform and its source, marks the layers it added
#      `platform`, and the toolchain line says the toolchain is the platform's.
#   C. mcpp.lock records the packages the platform added, and those only they
#      reach, with `origin = "platform"`; a build for this machine leaves the
#      file as it is.
#   D. `mcpp test` builds and runs a test image; `mcpp pack --mode
#      self-contained` stages the image's tree with its runner, and the
#      launcher runs with nothing installed.
#   E. `mcpp self env --target … --format json` names the platform, its fields'
#      sources and the runner's program; `mcpp toolchain list --format json`
#      gives the row's platform.
#   F. A package of the graph that provides a layer takes the platform's place
#      (reported `graph`); a version of the family the platform's group does not
#      name is refused, naming the group and both remedies.
#   G. `--platform none` selects no platform for the target.
#   H. After `mcpp toolchain install --target`, a project for the target
#      builds and runs with `--offline`.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
T="$HOST_ARCH-openkal"

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char** argv) {
    try { throw std::runtime_error("caught"); }
    catch (const std::exception& e) { std::println("image: {} ({} args)", e.what(), argc); }
    return 0;
}
CPP
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }
CPP

# A
"$MCPP" run --target "$T" -- one two > run.log 2>&1 || fail "A: the image did not build or run" run.log
grep -q "image: caught (3 args)" run.log || fail "A: the image's output" run.log
echo "ok: a project that declares nothing runs as an ok image"

# B
"$MCPP" build --target "$T" -v > build.log 2>&1 || fail "B: rebuild" build.log
rm -rf target
"$MCPP" build --target "$T" > fresh.log 2>&1 || fail "B: fresh build" fresh.log
# The source is the target table's column, or the variable a run before the
# platform's release selects it with.
grep -qE "Platform +openkal-platform [0-9.]+ \((built-in|MCPP_TARGET_[A-Z0-9_]+_PLATFORM)\)" fresh.log || fail "B: no Platform line naming the source" fresh.log
grep -qE "c-abi +musl +\(openkal-musl@[0-9.]+, platform\)" fresh.log || fail "B: the C library is not reported as the platform's" fresh.log
grep -qE "c\+\+-abi +libc\+\+ +\(openkal-llvm-runtime@[0-9.]+, platform\)" fresh.log || fail "B: the C++ run-time is not the platform's" fresh.log
grep -qE "tools +okld, okrun" fresh.log || fail "B: the tools are not reported" fresh.log
echo "ok: the report names the platform and what it added"

# C
grep -A5 '"openkal-musl"' mcpp.lock | grep -q 'origin  = "platform"' || fail "C: openkal-musl is not locked as the platform's" mcpp.lock
grep -A5 '"openkal-llvm-runtime"' mcpp.lock | grep -q 'origin  = "platform"' || fail "C: openkal-llvm-runtime is not locked as the platform's" mcpp.lock
grep -A5 '"openkal-tools"' mcpp.lock | grep -q 'origin  = "platform"' || fail "C: the platform's tools are not locked as the platform's" mcpp.lock
cp mcpp.lock lock.image
"$MCPP" build > c-host.log 2>&1 || fail "C: the build for this machine" c-host.log
cmp -s lock.image mcpp.lock || { diff lock.image mcpp.lock > c-lock.diff; fail "C: the build for this machine rewrote the lock" c-lock.diff; }
echo "ok: the lock says which packages the platform added, whichever configuration wrote it last"

# D
"$MCPP" test --target "$T" > test.log 2>&1 || fail "D: mcpp test" test.log
grep -q "smoke ... ok" test.log || fail "D: the test image did not pass" test.log
"$MCPP" pack --target "$T" --mode self-contained --pkg-format dir -o m3 > m3.log 2>&1 || fail "D: pack" m3.log
[ -x target/dist/m3/bin/app ] || fail "D: the package has no launcher" m3.log
env -i HOME="$HOME" PATH=/usr/bin:/bin target/dist/m3/bin/app x > m3run.log 2>&1 || fail "D: the launcher did not run" m3run.log
grep -q "image: caught (2 args)" m3run.log || fail "D: the package's image" m3run.log
echo "ok: test and pack work with no declaration"

# E
"$MCPP" self env --target "$T" --format json > env.json 2> env.err || fail "E: self env" env.err env.json
python3 - env.json <<'PY' || fail "E: self env --format json" env.json
import json, sys
d = json.load(open(sys.argv[1]))["data"]
p = d["platform"]
assert p and p["name"].endswith("openkal-platform"), p
assert p["source"] == "built-in" or p["source"].startswith("MCPP_TARGET_"), p["source"]
assert p["fields"]["runner"]["source"] == "platform"
assert p["fields"]["layers.c-abi"]["source"] == "platform"
assert d["runner"]["program"] and d["runner"]["program"].replace("\\", "/").split("/")[-1].startswith("okrun"), d["runner"]
PY
"$MCPP" toolchain list --format json > list.json 2>/dev/null || fail "E: toolchain list" list.json
python3 - list.json "$T" <<'PY' || fail "E: toolchain list's platform column" list.json
import json, sys
rows = [r for r in json.load(open(sys.argv[1]))["data"]["targets"] if r["target"] == sys.argv[2]]
assert rows and rows[0]["platform"].split("@")[0].endswith("openkal-platform"), rows
PY
echo "ok: self env and toolchain list state the platform"

# F
mkdir -p "$TMP/own/src"
cd "$TMP/own"
cat > mcpp.toml <<'TOML'
[package]
name    = "own"
version = "0.1.0"

[dependencies]
openkal-musl = { version = "0.22.2", default-features = false, features = ["posix"] }
TOML
cp "$TMP/app/src/main.cpp" src/main.cpp
if "$MCPP" build --target "$T" > group.log 2>&1; then
    fail "F: a version outside the platform's group was built" group.log
fi
grep -q "version group" group.log || fail "F: the refusal does not name the group" group.log
grep -qE "names openkal-musl [0-9]+\.[0-9]+\.[0-9]+" group.log || fail "F: the refusal does not name the group's version" group.log
echo "ok: a version outside the platform's group is refused"

# G
"$MCPP" self env --target "$T" --platform none --format json > none.json 2> none.err || fail "G: self env --platform none" none.err
python3 - none.json <<'PY' || fail "G: --platform none still selected a platform" none.json
import json, sys
d = json.load(open(sys.argv[1]))["data"]
assert d["platform"] is None, d["platform"]
PY
echo "ok: --platform none selects no platform"

# H
mkdir -p "$TMP/off/src"
cd "$TMP/off"
printf '[package]\nname    = "off"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("offline image"); }\n' > src/main.cpp
"$MCPP" toolchain install --target "$T" > install.log 2>&1 || fail "H: toolchain install --target" install.log
grep -q "Installed openkal-platform" install.log || fail "H: the install does not name the platform" install.log
"$MCPP" run --offline --target "$T" > off.log 2>&1 || fail "H: run --offline after the install" off.log
grep -q "offline image" off.log || fail "H: the offline run's output" off.log
echo "ok: toolchain install --target prepares the target for --offline"

echo "PASS: 951 an ok image needs no declaration"
