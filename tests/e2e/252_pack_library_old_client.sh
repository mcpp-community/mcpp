#!/usr/bin/env bash
# requires:
# 252_pack_library_old_client.sh — an mcpp that predates library packaging must
# still BUILD against a package produced by one that has it.
#
# That claim is the reason the generated manifest introduces no section and no
# key: everything in it — `sources`, `include_dirs`, `[modules] exports`, a
# `cfg(...)` block per leg, `[[runtime.artifacts]]` — was already parsed before
# this feature existed. An older client reads the package and links it; what it
# does not do is run the gates, because it has no way to know that
# `provenance = "mcpp-pack …"` means anything.
#
# Two halves, because only one of them can run everywhere:
#
#   1. STATIC — the generated manifest's top-level sections are a subset of the
#      vocabulary that predates this feature. Portable, and it is the actual
#      invariant rather than a proxy for it.
#   2. REAL — consume the package with $MCPP_BOOT, the released mcpp each CI job
#      bootstraps from. Skipped with a loud note when that is not available,
#      never silently.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
PKG_HOST="$(host_path "$pkg")"

# ── 1. no section outside the pre-existing vocabulary ──────────────────
#
# Listed literally rather than derived: the point is that this set was frozen
# before the feature, so a new entry has to be added here deliberately — and
# adding one is exactly the moment to ask whether older clients can still read
# the package.
# `\[\[?` covers both a table and an array-of-tables header: `[[runtime.artifacts]]`
# is the same section as `[runtime]` for this purpose, and the first version of
# this pattern matched only the single-bracket form — so it flagged the very
# section the design deliberately reuses.
known='^\[\[?(package|build|modules|targets\.|target\.|dependencies|dev-dependencies|runtime|profile\.|features|lib|pack|workspace|indices|resources|xlings|capabilities|tools)'
bad="$(grep -E '^\[' "$pkg/mcpp.toml" | grep -Ev "$known" || true)"
[[ -z "$bad" ]] || {
    echo "FAIL: the generated manifest uses sections an older mcpp cannot read:"
    printf '%s\n' "$bad"
    echo "  Either express the fact with an existing key, or accept that packages"
    echo "  need a version floor — and say so in docs/12."
    exit 1; }

# ── 2. the released client actually builds against it ──────────────────
mkdir -p "$TMP/app/src"
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

# Baseline with the PR binary, so a failure below is attributable to the client
# and not to the package.
( cd "$TMP/app" && "$MCPP" run > new.log 2>&1 ) \
    || { cat "$TMP/app/new.log"; echo "the PR binary could not consume its own package"; exit 1; }
grep -q 'ok=42' "$TMP/app/new.log" || { cat "$TMP/app/new.log"; echo "wrong answer"; exit 1; }

# The boot entry each CI job bootstraps from is an xvm SHIM, and a shim
# resolves against the home it is asked in — under the e2e suite's environment
# it answers `xlings: 'mcpp' is not installed` and prints NOTHING for
# `--version`. The first version of this guard compared that empty string
# against the PR binary's version, found them "different", and concluded it had
# found an old client — then reported a compatibility failure against a package
# that is perfectly readable. So the guard demands a version-SHAPED answer;
# anything else means "no usable old binary here", which is a note, not a
# verdict.
boot_ver=""
new_ver="$("$MCPP" --version 2>/dev/null || true)"
if [[ -n "${MCPP_BOOT:-}" && -x "${MCPP_BOOT}" ]]; then
    boot_ver="$("$MCPP_BOOT" --version 2>/dev/null || true)"
fi
usable=0
case "$boot_ver" in
    mcpp\ [0-9]*) usable=1 ;;
esac

if [[ "$usable" == 1 && "$boot_ver" != "$new_ver" ]]; then
    echo "old client: $boot_ver"
    rm -rf "$TMP/app/target"
    ( cd "$TMP/app" && "$MCPP_BOOT" run > old.log 2>&1 ) || {
        cat "$TMP/app/old.log"
        echo "FAIL: the released mcpp cannot build against a package this one produced."
        echo "      The compatibility claim in docs/12 is then false: such packages"
        echo "      need a version floor, and publishing one without it bricks older"
        echo "      clients rather than degrading them."
        exit 1; }
    grep -q 'ok=42' "$TMP/app/old.log" || {
        cat "$TMP/app/old.log"; echo "the old client built it but ran it wrong"; exit 1; }
    echo "PASS: a released mcpp builds and runs against a package from this one"
else
    if [[ -n "${MCPP_BOOT:-}" && "$usable" != 1 ]]; then
        echo "NOTE: \$MCPP_BOOT=${MCPP_BOOT} does not answer --version with a version"
        echo "      (got: '${boot_ver}'), so it is not a usable old client here — a"
        echo "      shim resolves against the home it is asked in. The REAL old-client"
        echo "      check therefore did not run; run it with MCPP_BOOT pointing at a"
        echo "      released mcpp binary directly."
    else
        echo "NOTE: \$MCPP_BOOT is unset or identical to \$MCPP — the real old-client"
        echo "      check did not run here."
    fi
    echo "PASS: the generated manifest introduces no section an older mcpp cannot read"
fi
