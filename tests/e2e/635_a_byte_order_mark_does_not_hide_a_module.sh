#!/usr/bin/env bash
# requires: gcc
# A UTF-8 BYTE-ORDER MARK DOES NOT HIDE A MODULE DECLARATION.
#
# MSVC writes UTF-8 with a BOM by default, so a `.cppm` whose first line is
# `export module foo;` commonly arrives as `EF BB BF e x p o r t ...`. Every
# compiler skips the mark. mcpp's scanner did not: `trim` uses isspace, which is
# false for all three bytes, so the mark stayed attached to the first token and
# the declaration was never seen.
#
# WHAT THAT COST IS THE POINT. The unit provided nothing, so nothing failed at
# the file that was wrong -- the consumer failed instead, with `module 'bom' not
# found`, which sends a reader to the import rather than to the encoding.
#
# THE MARK IS WRITTEN HERE RATHER THAN COMMITTED. A fixture file carrying a BOM
# is exactly the kind of thing an editor, a linter or a checkout filter quietly
# normalises; writing the bytes in the script means the test cannot be disarmed
# by a tool that never mentions it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src

printf '\xEF\xBB\xBF' > src/bom.cppm
cat >> src/bom.cppm <<'EOF'
export module bom;
export int bom() { return 42; }
EOF

cat > src/main.cpp <<'EOF'
import bom;
int main() { return bom() == 42 ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "bomtest"
version = "0.1.0"
EOF

# The fixture must actually carry the mark, or everything below asserts nothing.
head -c 3 src/bom.cppm | od -An -tx1 | tr -d ' \n' | grep -qx 'efbbbf' || {
    echo "FAIL: the fixture lost its byte-order mark; this test would pass vacuously"
    exit 1; }

"${MCPP:-mcpp}" build > build.log 2>&1 || {
    echo "FAIL: a BOM'd module interface did not build"
    cat build.log
    exit 1; }

# The symptom was reported at the CONSUMER, so it is named here explicitly: a
# future regression that reintroduces it must not read as a generic build error.
grep -q "imported but not provided" build.log && {
    echo "FAIL: the module declaration behind the BOM was not seen"
    cat build.log
    exit 1; }

bin=$(find target -type f -name bomtest -perm -u+x | head -1)
[ -n "$bin" ] || { echo "FAIL: no binary"; exit 1; }
"$bin" || { echo "FAIL: the program did not return 42"; exit 1; }

# A manifest authored on the same editor carries the same mark. It used to
# produce `1:1: error: expected key`, a true statement about the token that
# says nothing about the file.
printf '\xEF\xBB\xBF' > mcpp2.toml
cat >> mcpp2.toml <<'EOF'
[package]
name    = "bomtest"
version = "0.1.0"
EOF
mv mcpp2.toml mcpp.toml
rm -rf target
"${MCPP:-mcpp}" build > build2.log 2>&1 || {
    echo "FAIL: a BOM'd mcpp.toml was not read"
    cat build2.log
    exit 1; }

echo "ok: the mark is consumed by both readers"
