#!/usr/bin/env bash
# 66_runtime_provides.sh — [runtime] provides: a package that explicitly
# `provides` a capability wins provider selection over a package that merely
# lists it in `capabilities` (weak provider). Surfaced via resolution.json.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mk_dep() { # name, runtime-block
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "$1"
version = "0.1.0"

[targets.$1]
kind = "lib"

[runtime]
$2
EOF
    cat > "$1/src/lib.cppm" <<EOF
export module $1;
export int ${1}_id() { return 1; }
EOF
}

# weakdep only LISTS the capability; strongdep explicitly PROVIDES it.
mk_dep weakdep   'capabilities = ["test.cap.demo"]'
mk_dep strongdep 'capabilities = ["test.cap.demo"]
provides     = ["test.cap.demo"]'

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
weakdep   = { path = "../weakdep" }
strongdep = { path = "../strongdep" }
EOF
cat > app/src/main.cpp <<'EOF'
import std;
import weakdep;
import strongdep;
int main() { std::println("{}", weakdep_id() + strongdep_id()); return 0; }
EOF

cd app
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }

RES=$(find target -name resolution.json | head -1)
[[ -n "$RES" ]] || { echo "no resolution.json produced"; exit 1; }

# First provider entry for test.cap.demo must be the provides-declarer.
python3 - "$RES" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
caps = plan["runtime"]["capabilities"]
first = next(c for c in caps if c["capability"] == "test.cap.demo")
assert first["provider"] == "strongdep", \
    f"expected strong provider 'strongdep' first, got {first['provider']}"
PY

# Explicit override back to the weak provider must be honoured.
cat >> mcpp.toml <<'EOF'

[runtime."test.cap.demo"]
provider = "weakdep"
EOF
rm -rf target
"$MCPP" build > build2.log 2>&1 || { cat build2.log; echo "override build failed"; exit 1; }
RES=$(find target -name resolution.json | head -1)
python3 - "$RES" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
caps = plan["runtime"]["capabilities"]
first = next(c for c in caps if c["capability"] == "test.cap.demo")
assert first["provider"] == "weakdep", \
    f"override to weakdep not honoured, got {first['provider']}"
PY

echo "OK"
