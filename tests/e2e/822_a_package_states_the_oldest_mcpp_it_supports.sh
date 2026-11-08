#!/usr/bin/env bash
# requires: unix-shell
# resources: machine
# 822_a_package_states_the_oldest_mcpp_it_supports.sh -- #734 E9 and E12.
#
# `[package] mcpp = ">=V"` states the oldest mcpp release a package supports.
# An engine below the floor stops before any other phase, naming the package,
# the floor, its own release and the way to upgrade. The floor is written with
# `>=` only: a bare release means "exactly" elsewhere in mcpp, so it is refused
# with the spelling that is meant. `[workspace.package] mcpp` is inherited by
# members. `[lib]` reports an unknown key, as `[build]` and `[package]` do.
#
#   F1  a floor above this release stops the build with the hint;
#   F2  a floor at or below this release builds;
#   F3  a bare release and a non-release are refused, each with its reason;
#   F4  a workspace floor reaches a member built with -p;
#   F5  `[lib]` names an unknown key.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
HAVE=$("$MCPP" --version | awk '{print $2}')

mkdir -p p/src
cd p
printf 'int main() { return 0; }\n' > src/main.cpp
manifest() { printf '[package]\nname = "floor822"\nversion = "0.1.0"\nmcpp = %s\n' "$1" > mcpp.toml; }

# F1
manifest '">=2999.1.1"'
if "$MCPP" build > f1.log 2>&1; then fail "F1: a floor above $HAVE built" f1.log; fi
grep -q "requires mcpp >= 2999.1.1; this is mcpp $HAVE" f1.log || fail "F1: the message does not name the floor and this release" f1.log
grep -q "xlings install mcpp@2999.1.1" f1.log || fail "F1: the message does not say how to upgrade" f1.log

# F2
manifest "\">=$HAVE\""
"$MCPP" build > f2.log 2>&1 || fail "F2: a floor equal to this release did not build" f2.log
manifest '">=2026.1.1"'
"$MCPP" build > f2b.log 2>&1 || fail "F2: a floor below this release did not build" f2b.log

# F3
manifest "\"$HAVE\""
if "$MCPP" build > f3.log 2>&1; then fail "F3: a bare release was accepted" f3.log; fi
grep -q "names one release exactly; a floor is written \">=$HAVE\"" f3.log || fail "F3: the bare release is not explained" f3.log
manifest '">=banana"'
if "$MCPP" build > f3b.log 2>&1; then fail "F3: a non-release was accepted" f3b.log; fi
grep -q "'banana' is not an mcpp release" f3b.log || fail "F3: the non-release is not explained" f3b.log

# F4
cd "$TMP"
mkdir -p ws/app/src
cat > ws/mcpp.toml <<'EOF'
[workspace]
members = ["app"]

[workspace.package]
version = "0.1.0"
mcpp    = ">=2999.1.1"
EOF
printf '[package]\nname = "app822"\n' > ws/app/mcpp.toml
printf 'int main() { return 0; }\n' > ws/app/src/main.cpp
cd ws
if "$MCPP" build -p app > f4.log 2>&1; then fail "F4: the workspace floor did not reach the member" f4.log; fi
grep -q "package 'app822'.*requires mcpp >= 2999.1.1" f4.log || fail "F4: the member is not named with the inherited floor" f4.log

# F5
cd "$TMP/p"
printf '[package]\nname = "floor822"\nversion = "0.1.0"\n\n[lib]\npth = "src/x.cppm"\n' > mcpp.toml
"$MCPP" build > f5.log 2>&1 || fail "F5: the build failed" f5.log
grep -q "\[lib\] has unsupported key 'pth' (ignored). Supported keys: path." f5.log || fail "F5: [lib] did not name the unknown key" f5.log

echo "OK"
