#!/usr/bin/env bash
# requires: python3
# #816 control: visible direct imports already determine a transitive module
# map. No include expansion, hidden import or compatibility fallback is used.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do cat "$f" 2>/dev/null; done; exit 1; }
mkdir -p "$TMP/probe/src" "$TMP/dependency/src"
cat > "$TMP/dependency/mcpp.toml" <<'TOML'
[package]
name = "dependency"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = false
[modules]
sources = ["src/*.cppm"]
exports = ["dependency"]
[targets.dependency]
kind = "lib"
TOML
printf 'export module dependency;\nexport int dep_value() { return 42; }\n' > "$TMP/dependency/src/dependency.cppm"
cat > "$TMP/probe/mcpp.toml" <<'TOML'
[package]
name = "probe"
version = "0.1.0"
[language]
standard = "c++23"
modules = true
import_std = false
[modules]
sources = ["src/*.cppm", "src/*.cpp"]
exports = ["probe"]
[dependencies]
dependency = {path = "../dependency"}
[targets.probe]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'export module probe;\nimport dependency;\nexport int value() { return dep_value(); }\n' > "$TMP/probe/src/probe.cppm"
printf 'import probe;\nint main() { return value() == 42 ? 0 : 1; }\n' > "$TMP/probe/src/main.cpp"
cd "$TMP/probe"
"$MCPP" build > first.log 2>&1 || fail 'visible transitive module build' first.log
"$MCPP" run > run.log 2>&1 || fail 'transitive module program' run.log
python3 - <<'PY' || fail 'consumer map does not reach dependency' first.log
import pathlib
maps = list(pathlib.Path('target').glob('**/modmap/probe-*.map'))
assert any('dependency ' in p.read_text() and 'probe ' in p.read_text() for p in maps), maps
PY
"$MCPP" build > second.log 2>&1 || fail 'warm transitive module build' second.log
echo 'PASS: visible imports resolve the transitive module on the active compiler'
