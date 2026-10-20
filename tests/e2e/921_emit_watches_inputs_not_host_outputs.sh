#!/usr/bin/env bash
# requires: python3
# #807: a version build dependency produces a real host lock; path-only
# libraries still contribute editable inputs, never synthesized host files.
set -e
source "$(dirname "$0")/_host_path.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
mkdir -p "$TMP/app/src" "$TMP/editable/src" "$TMP/index/pkgs/f"
INDEX_HOST=$(host_path "$TMP/index")
cat > "$TMP/index/pkgs/f/fixture.lockdep.lua" <<'LUA'
package = {
    spec = "1", namespace = "fixture", name = "fixture.lockdep",
    description = "Host lock fixture", licenses = {"MIT"}, type = "package",
    xpm = {
        linux = {["1.0.0"] = {url = "https://example.invalid/lockdep.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000"}},
        windows = {["1.0.0"] = {url = "https://example.invalid/lockdep.zip", sha256 = "0000000000000000000000000000000000000000000000000000000000000000"}},
        macosx = {["1.0.0"] = {url = "https://example.invalid/lockdep.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000"}},
    },
    mcpp = { language = "c++23", import_std = false,
             sources = {"src/dep.cpp"}, targets = {lockdep = {kind = "lib"}}, deps = {} },
}
LUA
# Seed both the project's legacy layout and the home-owned store; the host
# graph resolves the same version under its own working directory.
for base in "$TMP/app/.mcpp/.xlings/data/xpkgs" "$MCPP_HOME/xpkgs"; do
    mkdir -p "$base/fixture-x-lockdep/1.0.0/src"
    printf 'int lockdep_value() { return 42; }\n' > "$base/fixture-x-lockdep/1.0.0/src/dep.cpp"
done
printf '[package]\nname = "editable"\nversion = "0.1.0"\n[build]\nsources = ["src/*.cpp"]\n[targets.editable]\nkind = "lib"\n' > "$TMP/editable/mcpp.toml"
printf 'int editable_value() { return 7; }\n' > "$TMP/editable/src/lib.cpp"
cat > "$TMP/app/mcpp.toml" <<TOML
[package]
name = "watch-probe"
version = "0.1.0"
[indices]
fixture = { path = "$INDEX_HOST" }
[build-dependencies.fixture]
lockdep = "1.0.0"
[build-dependencies]
editable = { path = "../editable" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'int main() { return 0; }\n' > "$TMP/app/src/main.cpp"
printf '#include <cstdio>\nint main() { std::puts("cxxflag=-DHOST_PROBE=1"); }\n' > "$TMP/app/build.mcpp"
cd "$TMP/app"
"$MCPP" emit build-database --format json > first.json 2> first.err || fail "first emit" first.err
python3 - "$MCPP_HOME" <<'PY' || fail "host fixture must produce a nonempty lock"
import pathlib, sys, os, json
locks = list(pathlib.Path(sys.argv[1]).glob('cache/build-database/*/target/.build-mcpp/host/*/mcpp.lock'))
assert locks and all('[package.' in p.read_text() for p in locks), locks
snapshot = {}
for p in locks:
    os.utime(p, ns=(1000000000000000000, 1000000000000000000))
    snapshot[str(p)] = [p.stat().st_mtime_ns, p.read_text()]
pathlib.Path('snapshot.json').write_text(json.dumps(snapshot))
PY
"$MCPP" emit build-database --format json > second.json 2> second.err || fail "second emit" second.err
python3 - <<'PY' || fail "watch or host lock stability"
import json, pathlib
first, second = (json.load(open(p))['data'] for p in ('first.json', 'second.json'))
assert first['inputs-fingerprint'] == second['inputs-fingerprint']
watch = [p.replace('\\', '/') for p in second['watch']]
assert 'mcpp.lock' in watch and 'mcpp.toml' in watch and 'build.mcpp' in watch, watch
assert any(p.endswith('editable/mcpp.toml') for p in watch), watch
assert any(p.endswith('editable/src/lib.cpp') for p in watch), watch
assert not any('/.build-mcpp/host/' in p for p in watch), watch
assert not any('fixture-x-lockdep/' in p for p in watch), watch
for p, (mtime, content) in json.load(open('snapshot.json')).items():
    f = pathlib.Path(p)
    assert f.stat().st_mtime_ns == mtime and f.read_text() == content, p
PY
printf '\n// editable input changed\n' >> "$TMP/editable/src/lib.cpp"
"$MCPP" emit build-database --format json > third.json 2> third.err || fail "changed editable input" third.err
python3 - <<'PY' || fail "editable host input did not invalidate fingerprint"
import json
assert json.load(open('second.json'))['data']['inputs-fingerprint'] != json.load(open('third.json'))['data']['inputs-fingerprint']
PY
echo 'PASS: 921 emit watches real inputs; repeated host lock write is stable'
