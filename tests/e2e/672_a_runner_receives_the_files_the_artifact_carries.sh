#!/usr/bin/env bash
# requires: unix-shell
# 672_a_runner_receives_the_files_the_artifact_carries.sh -- every runner of
# `mcpp run` and `mcpp test` receives MCPP_RUNTIME_FILES (#634 A6).
#
# A runner receives the artifact's path and nothing else. A runner that moves
# the artifact -- `adb-run` pushes the program to a device -- moved only the
# program, and a test reading its deployed data failed on an API 34 emulator
# with `open failed: /data/local/tmp/data/data.txt` while it passed on the host.
#
# The variable names a file with one line per file the artifact reads or loads
# from its own directory: the destination relative to the artifact's
# directory, a TAB, and the absolute path of the staged file. It lists the
# `[runtime] deploy` entries and the shared libraries the plan links.
#
# No device is needed and none is used: the runner is a script that records
# the variable and the list it names, then executes the program.
#
# Criteria:
#   1. `mcpp run` through a runner: the list names `data/data.txt` and the
#      dependency's shared library, each with the absolute path of an existing
#      file in the output tree;
#   2. `mcpp test` through the same runner: one list per test program, and a
#      test discovered in a subdirectory names its files with `../` (in a
#      project without a shared dependency: a test in a subdirectory does not
#      load a graph-built shared library on 2026.9.14.1 either, because the
#      consumer's `$ORIGIN` rpath names the subdirectory);
#   3. a project that deploys nothing and links no shared library still
#      receives the variable, naming an existing empty file;
#   4. negative direction: `--no-runner` executes the program without the
#      variable, and a distributable run with `--format` receives an empty
#      list, because a distributable holds its own files.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p rec fw/src app/src app/share app/tests

cat > "$TMP/runner.sh" <<EOF
#!/bin/sh
name=\$(basename "\$1")
if [ -n "\${MCPP_RUNTIME_FILES+x}" ]; then
    { printf 'LIST=%s\n' "\$MCPP_RUNTIME_FILES"; cat "\$MCPP_RUNTIME_FILES"; } > "$TMP/rec/\$name.rec"
else
    echo "UNSET" > "$TMP/rec/\$name.rec"
fi
exec "\$@"
EOF
chmod +x "$TMP/runner.sh"

cat > fw/mcpp.toml <<'EOF'
[package]
name    = "fw"
version = "0.1.0"

[targets.fw]
kind = "shared"
EOF
cat > fw/src/fw.cppm <<'EOF'
export module fw;
export int fw_value() { return 42; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
fw = { path = "../fw" }

[runtime]
deploy = [ { from = "share/data.txt", to = "data" } ]
EOF
printf 'payload\n' > app/share/data.txt
cat > app/src/main.cpp <<'EOF'
import std;
import fw;
int main() {
    std::println("APP-RAN {}", fw_value());
    std::println("RUNTIME_FILES={}", std::getenv("MCPP_RUNTIME_FILES") ? "set" : "unset");
    return 0;
}
EOF
cat > app/tests/reads.cpp <<'EOF'
import std;
import fw;
int main() { return fw_value() == 42 ? 0 : 1; }
EOF

cd app
"$MCPP" build > b0.log 2>&1 || fail "the initial build failed" b0.log
HOST=$(ls target | head -1)
[ -n "$HOST" ] || fail "could not determine the host triple from target/" b0.log
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml

# field <record> <destination> -> the absolute path the record pairs with it
field() { awk -F'\t' -v d="$2" '$1 == d { print $2 }' "$1"; }

# ── 1. mcpp run ────────────────────────────────────────────────────────────
"$MCPP" run > r1.log 2>&1 || fail "mcpp run through the runner failed" r1.log
grep -q "APP-RAN 42" r1.log || fail "the program did not run through the runner" r1.log
rec="$TMP/rec/app.rec"
[ -f "$rec" ] || fail "the runner recorded nothing" r1.log
grep -q '^UNSET$' "$rec" && fail "the runner did not receive MCPP_RUNTIME_FILES" "$rec"
list=$(sed -n 's/^LIST=//p' "$rec")
[ -f "$list" ] || fail "MCPP_RUNTIME_FILES does not name an existing file" "$rec"
data=$(field "$rec" "data/data.txt")
[ -n "$data" ] || fail "the list does not name data/data.txt" "$rec"
case "$data" in /*) ;; *) fail "the source of data/data.txt is not absolute: $data" "$rec" ;; esac
[ "$(cat "$data")" = "payload" ] || fail "data/data.txt pairs with a file that is not the deployed one" "$rec"
lib=$(awk -F'\t' '$1 ~ /^libfw\.(so|dylib)$/ { print $2 }' "$rec")
[ -n "$lib" ] && [ -f "$lib" ] || fail "the list does not name the dependency's shared library" "$rec"
tabs=$(awk -F'\t' 'NF != 2' "$rec" | grep -vc '^LIST=' || true)
[ "$tabs" -eq 0 ] || fail "a line of the list is not <destination><TAB><source>" "$rec"
grep -q "RUNTIME_FILES=set" r1.log || fail "the program under the runner did not inherit the variable" r1.log
echo "mcpp run hands the runner data/data.txt and libfw OK"

# ── 2a. mcpp test ──────────────────────────────────────────────────────────
rm -f "$TMP"/rec/*.rec
"$MCPP" test > t2.log 2>&1 || fail "mcpp test through the runner failed" t2.log
[ -f "$TMP/rec/reads.rec" ] || fail "the runner recorded nothing for test reads" t2.log
grep -q '^UNSET$' "$TMP/rec/reads.rec" && fail "test reads' runner did not receive the variable" "$TMP/rec/reads.rec"
[ -n "$(field "$TMP/rec/reads.rec" "data/data.txt")" ] \
    || fail "test reads does not carry data/data.txt" "$TMP/rec/reads.rec"
awk -F'\t' '$1 ~ /^libfw\.(so|dylib)$/' "$TMP/rec/reads.rec" | grep -q . \
    || fail "test reads does not carry the shared library" "$TMP/rec/reads.rec"
echo "mcpp test hands the test program its list OK"

# ── 4a. --no-runner: no variable ───────────────────────────────────────────
"$MCPP" run --no-runner > r4.log 2>&1 || fail "mcpp run --no-runner failed" r4.log
grep -q "RUNTIME_FILES=unset" r4.log || fail "a run without a runner received the variable" r4.log
echo "--no-runner executes the program without the variable OK"

# ── 2b. a test in a subdirectory names its files with ../ ─────────────────
cd "$TMP"
"$MCPP" new deep > /dev/null
cd deep
mkdir -p share tests/unit
printf 'payload\n' > share/data.txt
printf '\n[runtime]\ndeploy = [ { from = "share/data.txt", to = "data" } ]\n' >> mcpp.toml
printf 'int main() { return 0; }\n' > tests/unit/nested.cpp
"$MCPP" build > b2b.log 2>&1 || fail "the project with a nested test did not build" b2b.log
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml
rm -f "$TMP"/rec/*.rec
"$MCPP" test > t2b.log 2>&1 || fail "mcpp test of the nested test failed" t2b.log
for t in nested test_smoke; do
    [ -f "$TMP/rec/$t.rec" ] && ! grep -q '^UNSET$' "$TMP/rec/$t.rec" \
        || fail "test $t's runner did not receive the variable" t2b.log "$TMP/rec/$t.rec"
done
nested_data=$(field "$TMP/rec/nested.rec" "../data/data.txt")
[ -n "$nested_data" ] && [ -f "$nested_data" ] \
    || fail "test unit/nested does not name its data with ../" "$TMP/rec/nested.rec"
[ -n "$(field "$TMP/rec/test_smoke.rec" "data/data.txt")" ] \
    || fail "the top-level test does not name its data without ../" "$TMP/rec/test_smoke.rec"
[ "$(sed -n 's/^LIST=//p' "$TMP/rec/nested.rec")" != "$(sed -n 's/^LIST=//p' "$TMP/rec/test_smoke.rec")" ] \
    || fail "two test programs share one list" "$TMP/rec/nested.rec" "$TMP/rec/test_smoke.rec"
echo "a test in a subdirectory names its files with ../, one list per test OK"

# ── 3. nothing to carry: an empty list, still named ────────────────────────
cd "$TMP"
"$MCPP" new lone > /dev/null
cd lone
"$MCPP" build > b3.log 2>&1 || fail "the plain project did not build" b3.log
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml
"$MCPP" run > r3.log 2>&1 || fail "mcpp run of the plain project failed" r3.log
rec="$TMP/rec/lone.rec"
[ -f "$rec" ] && ! grep -q '^UNSET$' "$rec" || fail "the plain project's runner did not receive the variable" "$rec"
list=$(sed -n 's/^LIST=//p' "$rec")
[ -f "$list" ] && [ ! -s "$list" ] || fail "the plain project's list is not an existing empty file" "$rec"
echo "a project with nothing to carry receives an empty list OK"

# ── 4b. a distributable holds its own files: an empty list ────────────────
cat > copy.sh <<'EOF'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
chmod +x "$2"
EOF
chmod +x copy.sh
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/lone.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:lone}")
     .arg(out.c_str())
     .input("${mcpp.target_file:lone}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF
"$MCPP" run --format blob > r5.log 2>&1 || fail "mcpp run --format blob failed" r5.log
rec="$TMP/rec/lone.blob.rec"
[ -f "$rec" ] && ! grep -q '^UNSET$' "$rec" || fail "the distributable's runner did not receive the variable" r5.log "$rec"
list=$(sed -n 's/^LIST=//p' "$rec")
[ -f "$list" ] && [ ! -s "$list" ] || fail "the distributable's list is not an existing empty file" "$rec"
echo "a distributable run with --format receives an empty list OK"

echo "PASS: 672_a_runner_receives_the_files_the_artifact_carries"
