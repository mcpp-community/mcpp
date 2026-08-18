#!/usr/bin/env bash
# requires:
# (no capability: both assertions read mcpp's own output about a manifest key.)
#
# 263_lib_root_follows_the_extension.sh — the lib-root convention offers one
# candidate per DECLARED module extension, at every call site.
#
# ⚠️ WHY THIS EXISTS AS ITS OWN TEST. The previous round fixed the resolver the
# packer uses and left two other callers on the non-probing form, because the
# path under test was the only one anybody looked at. Both were reachable and
# both were wrong for an `.ixx` project:
#
#   validate.cppm   warned that the lib root was missing when it was right there
#   prepare.cppm    handed a host-module dependency a path to a file that does
#                   not exist
#
# Control-verified against the RELEASED 2026.8.18.2 binary on this fixture:
#
#   warning: src/mathkit.cppm: lib target without conventional lib root
#            'src/mathkit.cppm' (create the file or set [lib].path)
#
# "Fixed the path I was testing" is not "fixed the decision", and a test per
# CALL SITE is the only thing that distinguishes them.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── 1. the validator does not warn about a lib root that is present ─────
mkdir -p lib/src
cat > lib/src/mathkit.ixx <<'EOF'
export module mathkit;
export namespace mk { int answer() { return 42; } }
EOF
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.ixx"]
module_extensions = [".ixx"]
[targets.mathkit]
kind = "lib"
EOF
( cd lib && "$MCPP" build > build.log 2>&1 ) || { cat lib/build.log; echo "FAIL: build"; exit 1; }
grep -q 'conventional lib root' lib/build.log && {
    cat lib/build.log
    echo "FAIL: the validator looked for src/mathkit.cppm on a project whose"
    echo "      interface is src/mathkit.ixx. The convention has to offer one"
    echo "      candidate per DECLARED extension, not just the built-in one."
    exit 1; }

# The negative control: a project with NO lib root at all must still be warned
# about, or this test would pass against a validator that stopped checking.
mkdir -p noroot/src
cat > noroot/src/other.ixx <<'EOF'
export module other;
export namespace o { int v() { return 1; } }
EOF
cat > noroot/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.ixx"]
module_extensions = [".ixx"]
[targets.mathkit]
kind = "lib"
EOF
( cd noroot && "$MCPP" build > build.log 2>&1 ) || true
grep -q 'conventional lib root' noroot/build.log || {
    cat noroot/build.log
    echo "FAIL: a genuinely missing lib root produced no warning, so the check"
    echo "      above proves nothing — it would pass against a validator that"
    echo "      simply stopped looking."
    exit 1; }

# ── 2. a host-module dependency resolves its .ixx lib root ──────────────
#
# The second call site, which had no test of any kind. A build rule's interface
# is handed to build_program.cppm BY PATH, so a lib root resolved to a file that
# does not exist fails inside the build program rather than here.
mkdir -p rulepkg/src
cat > rulepkg/src/rulepkg.ixx <<'EOF'
export module rulepkg;
export namespace rp { int magic() { return 7; } }
EOF
cat > rulepkg/mcpp.toml <<'EOF'
[package]
name    = "rulepkg"
version = "0.1.0"
[build]
sources = ["src/*.ixx"]
module_extensions = [".ixx"]
[targets.rulepkg]
kind = "lib"
[modules]
exports = ["rulepkg"]
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
#ifndef RULE_MAGIC
#error "the host-module rule did not reach this compile"
#endif
int main(){ std::printf("host-mod=%d\n", RULE_MAGIC); return 0; }
EOF
cat > app/build.mcpp <<'EOF'
import std;
import rulepkg;
int main() {
    std::println("mcpp:rerun-if-changed=build.mcpp");
    // A real directive, so the assertion is on an EFFECT rather than on a log
    // line: the rule's return value has to reach the consumer's compile.
    std::println("mcpp:cfg=RULE_MAGIC={}", rp::magic());
    return 0;
}
EOF
# `[dependencies]`, not `[build-dependencies]`: `host-module = true` is what
# makes it build-time-only, and the key lives in the ordinary dependency table
# (docs/05 §2.14). The first version of this fixture put it under
# `[build-dependencies]` and failed identically for a `.cppm` package — i.e. the
# fixture was wrong, not the resolver. Checking the `.cppm` case first is what
# separated those two.
RULEPKG_HOST="$(host_path "$TMP/rulepkg")"
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
standard = "c++23"
[dependencies]
rulepkg = { path = "$RULEPKG_HOST", host-module = true }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
( cd app && "$MCPP" build > build.log 2>&1 ) || {
    cat app/build.log
    echo "FAIL: a host-module dependency whose interface is .ixx could not be"
    echo "      resolved. 'module rulepkg not found' means the lib root was"
    echo "      looked for as src/rulepkg.cppm, which does not exist."
    exit 1; }
# The `#error` above is the real assertion: if the rule module could not be
# imported, or its value never reached the compile, this file does not build.
grep -qE "warning:.*module_extensions declares" app/build.log && {
    cat app/build.log
    echo "FAIL: the rule package was warned about a dead module_extensions entry."
    echo "      Its sources are emptied on purpose (that is what keeps a build"
    echo "      rule out of the consumer's binary), so every declared extension"
    echo "      looks dead — a warning about a correct manifest, with nothing to fix."
    exit 1; }

echo "PASS: the lib-root convention follows the declared extension at every call site"
