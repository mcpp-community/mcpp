#!/usr/bin/env bash
# requires: elf gcc
# 205_root_local_subos.sh — root/workspace-root owns the local development OS;
# member/dependency declarations are non-transitive and active shell state is
# never a selector.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

default_manifest="$MCPP_HOME/registry/subos/default/.xlings.json"
[[ -f "$default_manifest" ]] || fail "mcpp default SubOS is missing"
default_subos=$(dirname "$default_manifest")
runtime=$(sed -n 's/.*"runtime"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$default_manifest" | head -1)
[[ -n "$runtime" ]] || fail "default SubOS has no runtime identity"

write_contract() {
    local dir=$1 marker=$2
    mkdir -p "$dir"
    # Selection tests still need a truthful physical RuntimeBinding. Reuse the
    # same libc/loader payload while varying only root ownership and metadata.
    ln -s "$default_subos/lib" "$dir/lib"
    cat >"$dir/.xlings.json" <<EOF
{ "workspace": {}, "subos_info": { "schema_version": 1,
  "runtime": "$runtime", "envs": { "$marker@1": [
    { "var": "MCPP_ROOT_LOCAL_MARKER", "op": "set", "value": "$marker" }
  ] } } }
EOF
}

field11() {
    sed -n 's/^[[:space:]]*\[11\][[:space:]]*//p' | tail -1
}

# Absence is always McppDefault. A hostile/missing active SubOS name cannot
# change either success or fingerprint.
mkdir -p "$TMP/default/src"
cat >"$TMP/default/mcpp.toml" <<'EOF'
[package]
name = "default-app"
version = "0.1.0"
EOF
echo 'int main() { return 0; }' >"$TMP/default/src/main.cpp"
cd "$TMP/default"
plain=$($MCPP build --print-fingerprint 2>&1) || { echo "$plain"; fail "default build"; }
active=$(XLINGS_ACTIVE_SUBOS=definitely-missing \
    $MCPP build --print-fingerprint 2>&1) \
    || { echo "$active"; fail "active state influenced default build"; }
plain_hash=$(field11 <<<"$plain")
active_hash=$(field11 <<<"$active")
[[ -n "$plain_hash" && "$plain_hash" == "$active_hash" ]] \
    || fail "McppDefault fingerprint changed with XLINGS_ACTIVE_SUBOS"

# Explicit `default` is NamedSubos("default"), not absence; it intentionally
# carries a distinct selection/provenance identity even though the physical
# SubOS is the same managed default.
cat >>mcpp.toml <<'EOF'

[xlings]
subos = "default"
EOF
explicit=$($MCPP build --print-fingerprint 2>&1) \
    || { echo "$explicit"; fail "explicit default build"; }
explicit_hash=$(field11 <<<"$explicit")
[[ -n "$explicit_hash" && "$explicit_hash" != "$plain_hash" ]] \
    || fail "explicit default collapsed into absence"

# Workspace root wins before member substitution. The member deliberately
# names a missing SubOS: if its declaration leaks into selection, the build
# fails before compiling. Root materialization must also stay at the root.
ws="$TMP/workspace"
mkdir -p "$ws/app/src"
cat >"$ws/mcpp.toml" <<'EOF'
[workspace]
members = ["app"]

[xlings]
subos = "root-el8"

[xlings.envs]
ROOT_ONLY = "1"
EOF
cat >"$ws/app/mcpp.toml" <<'EOF'
[package]
name = "workspace-app"
version = "0.1.0"

[xlings]
subos = "missing-member-subos"
EOF
echo 'int main() { return 0; }' >"$ws/app/src/main.cpp"
write_contract "$ws/.mcpp/.xlings/subos/root-el8" root
cd "$ws"
workspace_out=$($MCPP build -p app --print-fingerprint 2>&1) \
    || { echo "$workspace_out"; fail "workspace root did not own SubOS"; }
workspace_hash=$(field11 <<<"$workspace_out")
[[ -n "$workspace_hash" ]] || fail "workspace binding hash missing"
grep -q '"subos": "root-el8"' "$ws/.mcpp/.xlings.json" \
    || { cat "$ws/.mcpp/.xlings.json"; fail "workspace root xlings config not materialized"; }
[[ ! -f "$ws/app/.mcpp/.xlings.json" ]] \
    || fail "member xlings config was materialized during workspace build"

# The same member is authoritative when it becomes an independent root.
standalone="$TMP/standalone"
cp -R "$ws/app" "$standalone"
write_contract "$standalone/.mcpp/.xlings/subos/missing-member-subos" member
cd "$standalone"
standalone_out=$($MCPP build --print-fingerprint 2>&1) \
    || { echo "$standalone_out"; fail "standalone member selection"; }
standalone_hash=$(field11 <<<"$standalone_out")
[[ -n "$standalone_hash" && "$standalone_hash" != "$workspace_hash" ]] \
    || fail "standalone member reused workspace RuntimeBinding"

# A dependency's own SubOS declaration is not consulted. It remains source
# distributed and is built inside the consumer root's selected environment.
dep="$TMP/dep"
app="$TMP/consumer"
mkdir -p "$dep/src" "$app/src"
cat >"$dep/mcpp.toml" <<'EOF'
[package]
name = "localdep"
version = "0.1.0"

[modules]
sources = ["src/**/*.cppm"]

[targets.localdep]
kind = "lib"

[xlings]
subos = "missing-dependency-subos"
EOF
cat >"$dep/src/value.cppm" <<'EOF'
export module localdep.value;
export int local_value() { return 7; }
EOF
cat >"$app/mcpp.toml" <<EOF
[package]
name = "consumer"
version = "0.1.0"

[dependencies.localdep]
path = "$dep"
EOF
cat >"$app/src/main.cpp" <<'EOF'
import localdep.value;
int main() { return local_value() == 7 ? 0 : 1; }
EOF
cd "$app"
dep_out=$($MCPP run 2>&1) \
    || { echo "$dep_out"; fail "dependency SubOS leaked into consumer"; }

echo "PASS: SubOS selection is root-local, workspace-owned and non-transitive"
