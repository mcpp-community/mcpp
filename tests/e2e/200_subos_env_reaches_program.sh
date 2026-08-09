#!/usr/bin/env bash
# requires: elf
# 200_subos_env_reaches_program.sh — one selected RuntimeBinding snapshot must
# reach full-path and cached `mcpp run` without an environment/CLI override.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Match the mcpp-managed default's real libc so payload probing remains
# truthful; the fixture changes only environment declarations.
default_manifest="$MCPP_HOME/registry/subos/default/.xlings.json"
[[ -f "$default_manifest" ]] || fail "mcpp default SubOS is missing"
default_subos=$(dirname "$default_manifest")
runtime=$(sed -n 's/.*"runtime"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$default_manifest" | head -1)
[[ -n "$runtime" ]] || fail "default SubOS has no runtime identity"

cd "$TMP"
"$MCPP" new hello >/dev/null
cd hello
cat >>mcpp.toml <<'EOF'

[xlings]
subos = "probe"
EOF
cat >src/main.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* v = std::getenv("MCPP_E2E_PROBE");
    std::printf("PROBE=%s\n", v ? v : "(unset)");
    return 0;
}
EOF

subos="$PWD/.mcpp/.xlings/subos/probe"
mkdir -p "$subos/usr/lib/dri"
# A named SubOS is a physical development-OS view, not only metadata. Reuse
# the managed default's exact runtime payload so this fixture varies only the
# selected environment declarations.
ln -s "$default_subos/lib" "$subos/lib"
write_contract() {
    local suffix=$1
    cat >"$subos/.xlings.json" <<EOF
{ "workspace": {},
  "subos_info": { "schema_version": 1, "runtime": "$runtime",
    "envs": { "probe@1": [
      { "var": "MCPP_E2E_PROBE", "op": "prepend",
        "value": "\${subosdir}/usr/lib/$suffix" } ] } } }
EOF
}
write_contract dri

# Full path consumes the selected named contract.
out=$("$MCPP" run 2>&1) || { echo "$out"; fail "first run"; }
grep -q "PROBE=$subos/usr/lib/dri" <<<"$out" \
    || { echo "$out"; fail "selected environment did not reach program"; }

# Second invocation consumes the serialized snapshot and really takes fast path.
cached=$("$MCPP" run 2>&1) || { echo "$cached"; fail "cached run"; }
grep -q 'Resolving toolchain' <<<"$cached" \
    && { echo "$cached"; fail "second run did not take fast path"; }
grep -q "PROBE=$subos/usr/lib/dri" <<<"$cached" \
    || { echo "$cached"; fail "fast path changed the runtime environment"; }

# MCPP_SUBOS_DIR was an undeclared second selection entrance. It is ignored:
# mcpp.toml remains the only project runtime selector.
evil="$TMP/evil"
mkdir -p "$evil"
cat >"$evil/.xlings.json" <<EOF
{ "workspace": {}, "subos_info": { "schema_version": 1,
  "runtime": "$runtime", "envs": { "evil@1": [
    { "var": "MCPP_E2E_PROBE", "op": "set", "value": "EVIL" } ] } } }
EOF
no_override=$(MCPP_SUBOS_DIR="$evil" "$MCPP" run 2>&1) \
    || { echo "$no_override"; fail "run with ignored legacy variable"; }
grep -q "PROBE=$subos/usr/lib/dri" <<<"$no_override" \
    || { echo "$no_override"; fail "MCPP_SUBOS_DIR overrode mcpp.toml"; }

# Contract mutation invalidates the fast path and creates a new fingerprint;
# this invocation uses one NEW snapshot consistently, rather than re-reading
# only at run time against objects from the old contract.
sleep 1
write_contract dri2
changed=$("$MCPP" run 2>&1) || { echo "$changed"; fail "changed contract run"; }
grep -q 'Resolving toolchain' <<<"$changed" \
    || { echo "$changed"; fail "contract mutation did not invalidate fast path"; }
grep -q "PROBE=$subos/usr/lib/dri2" <<<"$changed" \
    || { echo "$changed"; fail "new snapshot was not applied"; }

# Absence means the managed default, never active/current or the prior named
# environment. The default does not declare our synthetic probe variable.
sed -i '/^\[xlings\]/,$d' mcpp.toml
plain=$("$MCPP" run 2>&1) || { echo "$plain"; fail "McppDefault run"; }
grep -q 'PROBE=(unset)' <<<"$plain" \
    || { echo "$plain"; fail "named environment leaked into McppDefault"; }

# An old cache without the full serialized binding is a miss, not a run with a
# guessed/re-read SubOS. Re-select probe, create the cache, then age it.
cat >>mcpp.toml <<'EOF'

[xlings]
subos = "probe"
EOF
write_contract dri
"$MCPP" run >/dev/null 2>&1
cache="$PWD/target/.build_cache"
grep -q '^runtimeBinding=' "$cache" || fail "cache has no RuntimeBinding"
grep -v '^runtimeBinding=' "$cache" >"$cache.old" && mv "$cache.old" "$cache"
aged=$("$MCPP" run 2>&1) || { echo "$aged"; fail "aged-cache run"; }
grep -q 'Resolving toolchain' <<<"$aged" \
    || { echo "$aged"; fail "cache without RuntimeBinding was replayed"; }
grep -q "PROBE=$subos/usr/lib/dri" <<<"$aged" \
    || { echo "$aged"; fail "aged cache rebuild lost selected environment"; }
grep -q '^runtimeBinding=' "$cache" \
    || fail "rebuilt cache did not record RuntimeBinding"

# A selected named SubOS that cannot answer the contract is a hard error; it
# cannot fall back to default, active or compiler-baked state.
sed -i 's/subos = "probe"/subos = "bare"/' mcpp.toml
bare="$PWD/.mcpp/.xlings/subos/bare"
mkdir -p "$bare"
echo '{ "workspace": {} }' >"$bare/.xlings.json"
if "$MCPP" run >bare.log 2>&1; then
    cat bare.log
    fail "named SubOS without RuntimeBinding was accepted"
fi
grep -Eq 'RuntimeBinding|no `subos_info`|does not describe itself' bare.log \
    || { cat bare.log; fail "missing-contract diagnostic is not actionable"; }

echo "PASS: selected RuntimeBinding is root-local, immutable and cached"
