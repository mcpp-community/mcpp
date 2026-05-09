#!/usr/bin/env bash
# 32_semver_merge.sh — SemVer merge in the transitive walker:
#   * Two consumers of the same package with overlapping constraints
#     (one exact, one range) merge to a single satisfying version
#     instead of erroring out.
#   * Non-overlapping pins still hard-error (Level-1 mangling fallback
#     is a follow-up).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# Index is needed since we exercise version-source deps.
mkdir -p "$MCPP_HOME/registry/data"
if [[ -d "$HOME/.mcpp/registry/data/mcpp-index" ]]; then
    ln -sf "$HOME/.mcpp/registry/data/mcpp-index" \
        "$MCPP_HOME/registry/data/mcpp-index"
fi
# Pre-cached xpkg downloads so the test doesn't re-fetch the world.
if [[ -d "$HOME/.mcpp/registry/data/xpkgs" ]]; then
    [[ -e "$MCPP_HOME/registry/data/xpkgs" ]] \
        || ln -sf "$HOME/.mcpp/registry/data/xpkgs" \
            "$MCPP_HOME/registry/data/xpkgs"
fi

# ── 1. Compatible-merge case ────────────────────────────────────────────
#   mylib (path) pins cmdline to =0.0.1
#   app    (root) pins cmdline to >=0.0.1,<1   (resolves to 0.0.2)
#   The walker first sees app's broader range (→ 0.0.2), then mylib's
#   exact (=0.0.1) — strict equality would error; the merger AND-combines
#   the two constraints, picks 0.0.1, re-fetches that version, and
#   replaces the previously-pinned slot.

mkdir -p "$TMP/mylib" && cd "$TMP/mylib"
"$MCPP" new mylib > /dev/null
cd mylib
rm -f src/main.cpp
cat > src/mylib.cppm <<'EOF'
export module mylib;
export int mylib_answer() { return 1; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"
[targets.mylib]
kind = "lib"

[dependencies.mcpplibs]
cmdline = "=0.0.1"
EOF

mkdir -p "$TMP/app" && cd "$TMP/app"
"$MCPP" new app > /dev/null
cd app
cat > src/main.cpp <<'EOF'
import std;
import mylib;
int main() {
    std::println("ok={}", mylib_answer());
    return mylib_answer() == 1 ? 0 : 1;
}
EOF
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "$TMP/mylib/mylib" }

[dependencies.mcpplibs]
cmdline = ">=0.0.1, <1"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "compatible merge failed"; exit 1; }

# The resolver should announce the merge step so future debugging is
# obvious. Either trace style is fine — we just want proof the merger
# (not the strict-equality fallback) handled the conflict.
grep -qE 'Merged.*cmdline.*0\.0\.1|→ v0\.0\.1' build.log || {
    cat build.log
    echo "no merge trace in build log"; exit 1; }

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "ok=1" ]] || { echo "unexpected output: $out"; exit 1; }

# ── 2. Irreconcilable case ─────────────────────────────────────────────
#   Two non-overlapping exact pins (=0.0.1 vs =0.0.2). The merger fails
#   to find a satisfying version and the build hard-errors. The error
#   message must mention the package and both constraints so the user
#   can pick one. (Cross-major mangling fallback is a separate PR.)

mkdir -p "$TMP/mylib2" && cd "$TMP/mylib2"
"$MCPP" new mylib2 > /dev/null
cd mylib2
rm -f src/main.cpp
cat > src/mylib2.cppm <<'EOF'
export module mylib2;
export int mylib2_answer() { return 2; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "mylib2"
version = "0.1.0"
[targets.mylib2]
kind = "lib"

[dependencies.mcpplibs]
cmdline = "=0.0.1"
EOF

mkdir -p "$TMP/app2" && cd "$TMP/app2"
"$MCPP" new app2 > /dev/null
cd app2
cat > src/main.cpp <<'EOF'
import std;
import mylib2;
int main() { return mylib2_answer() == 2 ? 0 : 1; }
EOF
cat > mcpp.toml <<EOF
[package]
name    = "app2"
version = "0.1.0"

[dependencies]
mylib2 = { path = "$TMP/mylib2/mylib2" }

[dependencies.mcpplibs]
cmdline = "=0.0.2"
EOF

if "$MCPP" build > build-bad.log 2>&1; then
    cat build-bad.log
    echo "non-overlapping pins should have failed"; exit 1
fi
grep -q 'irreconcilable versions' build-bad.log \
    && grep -q 'cmdline' build-bad.log \
    || { cat build-bad.log
         echo "expected irreconcilable diagnostic missing"; exit 1; }

echo "OK"
