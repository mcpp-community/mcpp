#!/usr/bin/env bash
# requires:
# 673_test_discover_names_where_tests_are.sh -- `[test] discover` (#634 A5).
#
# The test set was the literal pattern `tests/**/*.cpp`, one program per file.
# A project whose `tests/` belongs to another build system could not point
# discovery elsewhere. `discover` takes globs in the vocabulary of `[build]
# sources`, `!` exclusions included; a test's name is its path relative to the
# fixed directory of the glob that found it, so the default names every test
# as every earlier release did.
#
# Criteria:
#   1. negative direction first: without the key, `--list` names
#      `tests/a_ok.cpp` and `tests/unit/b_ok.cpp` as `a_ok` and `unit/b_ok`;
#   2. `discover = ["checks/**/*.cpp"]` runs `checks/c_ok.cpp` and not the
#      failing `tests/fails.cpp`, and names it `c_ok`;
#   3. a `!` glob excludes a file another glob found;
#   4. `discover = []` discovers nothing and says where it looked;
#   5. two files that map to one name are refused, naming both;
#   6. a value that is not an array of strings is refused, naming the key.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

"$MCPP" new disc > /dev/null
cd disc
rm -f tests/*.cpp
mkdir -p tests/unit checks/deep
printf 'int main() { return 0; }\n' > tests/a_ok.cpp
printf 'int main() { return 0; }\n' > tests/unit/b_ok.cpp
printf 'int main() { return 1; }\n' > tests/fails.cpp
printf 'int main() { return 0; }\n' > checks/c_ok.cpp
printf 'int main() { return 1; }\n' > checks/deep/skipped.cpp
cp mcpp.toml mcpp.toml.base

# ── 1. no key: today's names ───────────────────────────────────────────────
"$MCPP" test --list > l1.log 2>&1 || fail "mcpp test --list failed" l1.log
grep -qx "a_ok" l1.log || fail "the default does not name tests/a_ok.cpp 'a_ok'" l1.log
grep -qx "unit/b_ok" l1.log || fail "the default does not name tests/unit/b_ok.cpp 'unit/b_ok'" l1.log
grep -qx "c_ok" l1.log && fail "the default discovered a file outside tests/" l1.log
echo "without [test] the names are unchanged OK"

# ── 2 and 3. discover elsewhere, with an exclusion ────────────────────────
cat mcpp.toml.base > mcpp.toml
cat >> mcpp.toml <<'EOF'

[test]
discover = ["checks/**/*.cpp", "!checks/deep/**"]
EOF
"$MCPP" test --list > l2.log 2>&1 || fail "mcpp test --list with discover failed" l2.log
grep -qx "c_ok" l2.log || fail "discover did not name checks/c_ok.cpp 'c_ok'" l2.log
grep -q "fails\|a_ok" l2.log && fail "discover still discovered tests/" l2.log
grep -q "skipped" l2.log && fail "the ! glob did not exclude checks/deep/skipped.cpp" l2.log
"$MCPP" test > t2.log 2>&1 || fail "mcpp test with discover failed (a file outside the set ran?)" t2.log
grep -q "c_ok ... ok" t2.log || fail "checks/c_ok.cpp did not run" t2.log
grep -q "fails" t2.log && fail "tests/fails.cpp ran" t2.log
echo "discover runs checks/c_ok.cpp, excludes checks/deep, ignores tests/ OK"

# ── 4. an empty list discovers nothing, and says where it looked ──────────
cat mcpp.toml.base > mcpp.toml
printf '\n[test]\ndiscover = []\n' >> mcpp.toml
"$MCPP" test > t4.log 2>&1 || fail "mcpp test with discover = [] failed" t4.log
grep -q "no tests found (\[test\] discover = \[\])" t4.log \
    || fail "an empty discover did not say where it looked" t4.log
echo "discover = [] discovers nothing and names the key OK"

# ── 5. two files, one name ────────────────────────────────────────────────
cat mcpp.toml.base > mcpp.toml
printf '\n[test]\ndiscover = ["tests/*.cpp", "checks/*.cpp"]\n' >> mcpp.toml
printf 'int main() { return 0; }\n' > checks/a_ok.cpp
set +e
"$MCPP" test --list > t5.log 2>&1; rc=$?
set -e
[ "$rc" -ne 0 ] || fail "two files mapping to 'a_ok' were accepted" t5.log
grep -q "duplicate test name 'a_ok'" t5.log || fail "the refusal does not name the test" t5.log
grep -q "tests/a_ok.cpp" t5.log && grep -q "checks/a_ok.cpp" t5.log \
    || fail "the refusal does not name both files" t5.log
rm -f checks/a_ok.cpp
echo "two files mapping to one name are refused, naming both OK"

# ── 6. a wrong type ────────────────────────────────────────────────────────
cat mcpp.toml.base > mcpp.toml
printf '\n[test]\ndiscover = "tests/**/*.cpp"\n' >> mcpp.toml
# `mcpp test`, not `--list`: the listing is best effort over a manifest that
# does not load, so it lists the default set rather than failing.
set +e
"$MCPP" test > t6.log 2>&1; rc=$?
set -e
[ "$rc" -ne 0 ] || fail "a string discover was accepted" t6.log
grep -q "\[test\] discover must be an array" t6.log || fail "the refusal does not name the key" t6.log
echo "a discover that is not an array is refused OK"

echo "PASS: 673_test_discover_names_where_tests_are"
