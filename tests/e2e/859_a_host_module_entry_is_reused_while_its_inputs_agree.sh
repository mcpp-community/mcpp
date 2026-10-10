#!/usr/bin/env bash
# requires:
# 859 -- a build program's library is compiled again exactly when what it is
# compiled from changed, and the program never runs on a stale compile.
#
# #748 (B1), mcpp#797 (R6). A library of a path dependency can change without
# its name and version changing, so a program's identity holds the content
# digest of that package's tree: an included header beside the interface is
# part of what was compiled. Hashing the interface file alone left an edited
# header unseen, and the program was neither compiled nor run again (R6).
#
# Criteria:
#   A. A second run of a stale program (its declared input changed, its
#      graph did not) compiles nothing.
#   B. An edit to the interface compiles the library and the program again.
#   C. An edit to a header the interface includes, and not to the interface
#      itself, does too, and the program reads the new value.
#   D. A record that disagrees with the identity the program would have
#      (here, edited in place) is a miss: the program is compiled again and
#      the record replaced.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/app/src" "$TMP/rules/src"
cd "$TMP/app"

cat > "$TMP/rules/mcpp.toml" <<'EOF'
[package]
name    = "rule859"
version = "0.1.0"

[targets.rule859]
kind = "lib"
EOF
printf '#define T859_VALUE 1\n' > "$TMP/rules/src/value.h"
cat > "$TMP/rules/src/rule859.cppm" <<'EOF'
module;
#include "value.h"
export module t859.rules;
import mcpp;
export int rule_value() { return T859_VALUE; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "app859"
version = "0.1.0"

[build-dependencies]
rule859 = { path = "../rules", host-module = true }

[targets.app859]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
cat > build.mcpp <<'EOF'
import mcpp;
import t859.rules;
int main() {
    mcpp::rerun_if_changed("marker.txt");
    // Said on every run, so the log states which value the BMI carried.
    mcpp::warning(rule_value() == 1 ? "T859 value one"
                : rule_value() == 2 ? "T859 value two"
                :                     "T859 value other");
    return 0;
}
EOF
echo 0 > marker.txt

HOST=target/.build-mcpp/host
lib_object() { find $HOST -name 'rule859.m.o' | head -1; }
stamp() { stat -c %Y.%s "$(lib_object)" 2>/dev/null || stat -f %m.%z "$(lib_object)"; }
compiled() { grep -cE "build-program: app859: identity [0-9a-f]+ -> [0-9a-f]+, compiled" "$1" || true; }
# A stale program: its declared input changed, the sources of its imports did not.
rerun() { echo "$1" > marker.txt; MCPP_VERBOSE=1 "$MCPP" build > "$2" 2>&1 || fail "the build failed ($2)" "$2"; }

MCPP_VERBOSE=1 "$MCPP" build > first.log 2>&1 || fail "the first build failed" first.log
[ -n "$(lib_object)" ] || fail "the first build compiled no library object" first.log
grep -q "T859 value one" first.log || fail "the program did not read the library's value" first.log
s0=$(stamp)

# A
sleep 1
rerun 1 a.log
[ "$(compiled a.log)" = 0 ] || fail "A: a stale program was compiled again" a.log
[ "$(stamp)" = "$s0" ] || fail "A: the library was compiled again" a.log
grep -q "T859 value one" a.log || fail "A: the program gave another value" a.log

# B
sleep 1
cat > "$TMP/rules/src/rule859.cppm" <<'EOF'
module;
#include "value.h"
export module t859.rules;
import mcpp;
export int rule_value() { return T859_VALUE + 0; }
EOF
rerun 2 b.log
[ "$(compiled b.log)" = 1 ] || fail "B: an edit to the interface did not compile the program again" b.log
[ "$(stamp)" != "$s0" ] || fail "B: an edit to the interface did not compile the library" b.log
s1=$(stamp)

# C
sleep 1
printf '#define T859_VALUE 2\n' > "$TMP/rules/src/value.h"
rerun 3 c.log
[ "$(compiled c.log)" = 1 ] || fail "C: an edit to an included header did not compile the program again" c.log
[ "$(stamp)" != "$s1" ] || fail "C: an edit to an included header did not compile the library" c.log
grep -q "T859 value two" c.log || fail "C: the program took the compile made before the header changed" c.log

# D
rec=$(ls $HOST/*/programs.json | head -1)
sed 's/"identity": "[0-9a-f]*"/"identity": "0000000000000000"/' "$rec" > "$rec.edited" && mv "$rec.edited" "$rec"
rerun 4 d.log
[ "$(compiled d.log)" = 1 ] || fail "D: a record that disagrees was used without compiling" d.log
grep -q '"identity": "0000000000000000"' "$rec" && fail "D: the record that disagreed was not replaced" "$rec"
grep -q "T859 value two" d.log || fail "D: the replaced compile gave another value" d.log

echo "PASS: 859_a_host_module_entry_is_reused_while_its_inputs_agree"
