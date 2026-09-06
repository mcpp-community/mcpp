#!/usr/bin/env bash
# requires: elf gcc
# An import no dependency provides is refused BY NAME, before the compiler is
# reached.
#
# Left to the compiler the message is
#
#     mcpp.rules.spirv: error: failed to read compiled module: No such file or
#     directory
#     mcpp.rules.spirv: note: imports must be built before being imported
#
# which is true and names neither the package that would provide the module nor
# the key that would make it importable. `host-module = true` and the section a
# dependency is written in are separate axes -- the section says whether the
# package reaches the target, `host-module` whether its module is compiled for
# the build program -- and forgetting the first while getting the second right
# is the ordinary mistake.
#
# THE SET OF NAMES THAT CAN COMPILE HERE IS CLOSED (`std`, `std.compat`, the
# bundled `mcpp`, and the importable host modules), so a name outside it cannot
# become valid later and is refused rather than warned about.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p rule/src
cat > rule/src/rule.cppm <<'EOF'
export module rule;
export namespace testrule { inline int answer() { return 42; } }
EOF
cat > rule/mcpp.toml <<'EOF'
[package]
name    = "rule"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[build]
sources = ["src/rule.cppm"]
[targets.rule]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/build.mcpp <<'EOF'
import std;
import mcpp;
import rule;
int main() { return testrule::answer() == 42 ? 0 : 1; }
EOF
# Declared, and declared in the right section -- only `host-module` is absent.
# That is the point: the manifest looks correct.
cat > app/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[build-dependencies]
rule = { path = "../rule" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

cd app
out=$("$MCPP" build 2>&1) && { echo "FAIL: the build succeeded with an unprovided import"; exit 1; }
echo "$out" | grep -q "imports 'rule'" || { echo "FAIL: the refusal does not name the module"; echo "$out" | tail -6; exit 1; }
echo "$out" | grep -q 'host-module = true' || { echo "FAIL: the refusal does not name the key"; echo "$out" | tail -6; exit 1; }
echo "$out" | grep -q 'rule (in \[build-dependencies\])' || { echo "FAIL: the refusal does not name the candidate dependency"; echo "$out" | tail -6; exit 1; }
echo "$out" | grep -qi 'failed to read compiled module' && { echo "FAIL: the compiler was reached; the check must run first"; exit 1; }
echo "ok: refused by name, before the compiler"

# The same project with the key added must build -- otherwise the check could
# be refusing something that was always going to work.
sed -i 's|rule = { path = "../rule" }|rule = { path = "../rule", host-module = true }|' mcpp.toml
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: adding host-module = true did not make it build"; exit 1; }
echo "ok: the fix the message names is the fix that works"

echo "PASS: an import no dependency provides is refused by name, and the named fix works"
