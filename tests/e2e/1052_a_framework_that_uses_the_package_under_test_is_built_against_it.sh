#!/usr/bin/env bash
# requires: gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1052 -- a test framework that itself uses the package under test is built against that package.
#
# mcpplibs.os is tested with mtest, and mtest uses mcpplibs.os. A second copy of the package from
# the framework's own edge would provide every module of the package twice in one program; the
# package being built stands for itself in its own graph (docs/05), whatever version the edge asked
# for, and its tests reach the framework without a cycle of packages.
#
# Criteria: the root's test, written with a framework whose library imports the root's module,
# builds and passes; the root's module is compiled once; a version the edge did not ask for is said.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/core/src" "$t/core/tests" "$t/tf/src"
cat > "$t/core/mcpp.toml" <<'EOF'
[package]
namespace = "acme"
name      = "core"
version   = "0.2.0"

[targets.core]
kind = "lib"

[dev-dependencies]
acme.tf = { path = "../tf" }
EOF
cat > "$t/core/src/core.cppm" <<'EOF'
export module acme.core;
export int answer() { return 42; }
EOF
cat > "$t/core/tests/uses_tf.cpp" <<'EOF'
import acme.core;
import acme.tf;
int main() { return tf::check(answer() == 42) ? 0 : 1; }
EOF
cat > "$t/tf/mcpp.toml" <<'EOF'
[package]
namespace = "acme"
name      = "tf"
version   = "0.1.0"

[targets.tf]
kind = "lib"

[dependencies]
acme.core = { path = "../core", version = "0.1.0" }
EOF
cat > "$t/tf/src/tf.cppm" <<'EOF'
export module acme.tf;
import acme.core;
export namespace tf { bool check(bool held) { return held && answer() == 42; } }
EOF
cd "$t/core"
out=$("$MCPP" test 2>&1) || fail "the test of the package under its own framework failed" "$out"
grep -q 'uses_tf \.\.\. ok' <<<"$out" || fail "the test did not pass" "$out"
grep -q 'it is the package being built, 0.2.0, which stands for itself' <<<"$out" \
    || fail "the version the edge did not ask for is not said" "$out"
n=$(find target -name 'core.m.o' -o -name 'core.o' | grep -c 'acme_core\|/core' || true)
[ "$n" -le 1 ] || fail "the root's module was compiled more than once ($n)" "$(find target -name 'core*.o')"
echo "  ok  the framework is built against the package under test, and the test passes"
echo "PASS: 1052 a framework that uses the package under test is built against it"
