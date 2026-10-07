#!/usr/bin/env bash
# requires: unix-shell
# 890_a_toolchain_declared_for_an_unservable_target_is_still_installed.sh — mcpp#782.
#
# A target no payload on this host produces (a linux-gnu guest on a macOS
# host, a hosted guest on any host) sets the engine's carried
# `unservedTargetDiagnosis`. The diagnosis is HELD, not fatal: whether the
# dependency graph supplies the target's system side is not knowable until
# the graph is resolved, and the graph-supplied arrangement (openkal's rows)
# is exactly "retargetable clang + a package implementing the system".
#
# The regression this test pins: the toolchain install between the two
# decisions once read the held diagnosis as "skip the install" (mcpp#782).
# When the suite had installed the pinned toolchain out of band, the skip
# never fired; the moment the line moved and the out-of-band install was
# gone, `resolve_xpkg_path(autoInstall=false)` returned nothing and the held
# diagnosis fired BEFORE the graph release could run — a buildable project
# made unbuildable by an unrelated version move.
#
# The arrangement this test needs — a graph package implementing
# x86_64-linux-gnu's system — is what the openkal stack ships, and test 738
# already covers its build. What this test adds is the NARROW engine
# contract that failed:
#   1. on a host that cannot serve the target, a DECLARED toolchain for it
#      is still resolved and installed;
#   2. the held diagnosis stays silent while the graph can still speak.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf $TMP' EXIT
cd "$TMP"

# A cold registry proves installation rather than an out-of-band cache hit.
export MCPP_HOME="$TMP/mcpp-home"
"$MCPP" self config --mirror "${MCPP_E2E_MIRROR:-GLOBAL}"

# The guest triple must be one no payload here serves. `x86_64-linux-gnu`
# qualifies on macOS and Windows; on Linux the row is servable natively, so
# the contract it pins is vacuous there and the test exits 0 — which is why
# its CI home is the macOS leg of the target matrix.
case "$(uname -s)" in
    Darwin|MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "SKIP: hosted-guest refusal semantics only bite on a non-linux host"; exit 0 ;;
esac

mkdir -p app/src
cat > app/mcpp.toml <<TOML
[package]
name    = "declared-cross"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION}"
TOML
cat > app/src/main.cpp <<'CPP'
int main() { return 0; }
CPP

cd app
out=$("$MCPP" build --target x86_64-linux-gnu 2>&1) && { echo "FAIL: an empty project built a linux-gnu guest with no system supplier"; exit 1; } || true
# 1. THE INSTALL RAN. The resolution line names the declared toolchain, and
#    the payload is in the registry — not skipped behind the held diagnosis.
echo "$out" | grep -q "Resolved llvm@${LLVM_VERSION}" \
    || { echo "FAIL: the declared toolchain did not resolve"; echo "$out"; exit 1; }
store="${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-llvm/${LLVM_VERSION}"
[[ -x "$store/bin/clang++" ]] \
    || { echo "FAIL: llvm@${LLVM_VERSION} not installed — the held diagnosis skipped it"; echo "$out"; exit 1; }
# 2. THE DIAGNOSIS IS THE REFUSAL ONLY AFTER THE INSTALL FAILS. It names the
#    target and the graph remedy, not the skipped install.
echo "$out" | grep -q "cannot be built on this host" \
    || { echo "FAIL: expected the hosted-guest refusal once the empty graph cannot supply the system"; echo "$out"; exit 1; }
echo "$out" | grep -qE "depend on a package that implements|implement the target's system" \
    || { echo "FAIL: the refusal does not name the graph remedy"; echo "$out"; exit 1; }

echo "PASS: a declared toolchain for an unserved target still installs; the refusal waits for the graph"
