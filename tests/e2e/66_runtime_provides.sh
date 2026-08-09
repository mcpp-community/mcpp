#!/usr/bin/env bash
# 66_runtime_provides.sh — [runtime] requirements and providers are disjoint:
# only an explicit `provides` claim can own a capability. Merely requiring it
# through legacy `capabilities` never turns the requester into its own provider.
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

# weakdep only REQUIRES the capability; strongdep explicitly PROVIDES it.
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

# The only provider entry must be the provides-declarer, with canonical ID.
python3 - "$RES" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
providers = [p for p in plan["runtime"]["providers"]
             if p["capability"] == "test.cap.demo"]
assert len(providers) == 1, providers
provider = providers[0]["provider"]
assert provider["canonical"] == "mcpplibs.strongdep@0.1.0", provider
requirements = plan["runtime"]["requirements"]
weak = [r for r in requirements
        if r["value"] == "test.cap.demo"
        and r["requester"]["canonical"] == "mcpplibs.weakdep@0.1.0"]
assert len(weak) == 1, requirements
PY

# An override cannot promote the requester into a provider.
cat >> mcpp.toml <<'EOF'

[runtime."test.cap.demo"]
provider = "weakdep"
EOF
rm -rf target
if "$MCPP" build > build2.log 2>&1; then
    cat build2.log
    echo "weak requester was incorrectly promoted to provider"
    exit 1
fi
grep -q 'does not name a provider' build2.log || {
    cat build2.log
    echo "missing exact provider diagnostic"
    exit 1
}

echo "OK"
