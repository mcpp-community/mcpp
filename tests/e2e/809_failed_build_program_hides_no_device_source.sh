#!/usr/bin/env bash
# requires: python3
# 809 -- a package that declares a device source and whose build.mcpp does not
# compile is described by `emit build-database` with its one real diagnostic,
# `MCPP_BUILD_DATABASE_PROGRAM_FAILED`, not with the device-source orphan
# refusal (mcpp#724 side finding A, design 2026-09-27 §4.2, SPEC-005 R5.2
# amended).
#
# `rules` declares a feature whose `device_extensions`/`rule_module` classify
# `.dev` as a device source when active; `app809` activates it and lists a
# `.dev` file in `[build] sources`, and its own `build.mcpp` is invalid C++.
# The build program never runs, so it applies none of its directives — no
# action claims the device source, which used to read as an orphan and fail
# the whole member (a check whose premise is the program's directives ran
# anyway). Criteria:
#   A. exit status 1.
#   B. `data` is present, and `app809` is described (its manifest, toolchain
#      and module graph do not depend on the failed program).
#   C. the one diagnostic is `MCPP_BUILD_DATABASE_PROGRAM_FAILED`, severity
#      error, `path` "build.mcpp".
#   D. that diagnostic does not mention the device source or the orphan text.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

mkdir -p "$TMP/rules/src" "$TMP/app/src/kernels"

cat > "$TMP/rules/mcpp.toml" <<'EOF'
[package]
name        = "rules"
namespace   = "t809"
version     = "0.1.0"

[features]
default = []

[features.x]
sources           = ["src/rules.cppm"]
rule_module       = "t809.rules"
device_extensions = [".dev"]
EOF
cat > "$TMP/rules/src/rules.cppm" <<'EOF'
export module t809.rules;
export namespace t809::rules {
inline bool noop() { return true; }
}
EOF

cd "$TMP/app"
cat > mcpp.toml <<'EOF'
[package]
name    = "app809"
version = "0.1.0"

[language]
standard = "c++23"

[build-dependencies]
t809.rules = { path = "../rules", features = ["x"] }

[build]
sources = ["src/*.cpp", "src/kernels/*.dev"]
EOF
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
echo "not a real kernel" > src/kernels/k.dev
# Invalid C++: the build program never runs, and applies none of its
# directives -- the same shape 789 uses for its `nocompile` leg.
cat > build.mcpp <<'EOF'
int main() { this is not valid c++ }
EOF

set +e
"$MCPP" emit build-database --format json > out.json 2> out.err
rc=$?
set -e
[ "$rc" = 1 ] || fail "A: exit status $rc, expected 1" out.err out.json
echo "ok: A, exit status 1"

"$PY" - out.json <<'EOF' || fail "B/C/D: the envelope" out.json
import json, sys
e = json.load(open(sys.argv[1]))
assert "data" in e, e
sets = {s["name"]: s for s in e["data"]["database"]["sets"]}
assert "app809" in sets, sets
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED", diag
assert diag["severity"] == "error", diag
assert diag["path"] == "build.mcpp", diag
assert "k.dev" not in diag["message"], diag["message"]
assert "device sources that no action compiles" not in diag["message"], diag["message"]
EOF
echo "ok: B, the package is described; C, the one diagnostic is PROGRAM_FAILED with path build.mcpp; D, it mentions no device source"

echo "PASS: 809 a failed build program hides no device source"
