#!/usr/bin/env bash
# requires: gcc
# 1037 -- `mcpp new` scaffolds a program or a library whose test is mtest's, and a test file that
# defines no `main` begins at the entry a dependency offers (docs/08, docs/01).
#
# Criteria:
#   A. `--template lib` writes the module, a partition it exports and the partition's implementation,
#      named after the package (`acme.my-lib` is the module `acme.my_lib`); the library builds and its
#      test, a file of mtest's cases with no `main`, passes as one protocol case.
#   B. The program's skeleton test passes as one protocol case.
#   C. An entry with `test-main = true` begins a dependent's test that defines no `main`; a test that
#      defines one keeps it; the package's own entry begins its own tests.
#   D. Two offers are refused, naming both; `[test] main` chooses one.
#   E. A library whose name has a part that does not begin with a letter is refused.
#   F. In one package, a test that imports a framework's module begins at that framework's entry,
#      and a test that imports none begins at a dev-dependency's source that defines `main`.
#   G. `mcpp test --guide` names each test framework with its guide (`test-guide`), and a failed
#      run points to it.
#   H. A test that begins at an entry links no source that defines `main` beneath its
#      dev-dependency (gtest_main.cc of compat.gtest beneath a GoogleTest adapter).
#   I. A test links a dev-dependency that is a shared library (mtest's C ABI) by its library.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"

# A
"$MCPP" new acme.my-lib --template lib > a.log 2>&1 || fail "A: --template lib" a.log
for f in mcpp.toml src/my-lib.cppm src/hello.cppm src/hello.cpp tests/hello.cpp; do
    [ -f "acme.my-lib/$f" ] || fail "A: no $f" a.log
done
grep -q '^export module acme.my_lib;' acme.my-lib/src/my-lib.cppm || fail "A: the module's name" acme.my-lib/src/my-lib.cppm
grep -q '^export module acme.my_lib:hello;' acme.my-lib/src/hello.cppm || fail "A: the partition" acme.my-lib/src/hello.cppm
grep -q 'namespace acme::my_lib' acme.my-lib/src/hello.cpp || fail "A: the namespace" acme.my-lib/src/hello.cpp
grep -q 'int main' acme.my-lib/tests/hello.cpp && fail "A: the test defines main" acme.my-lib/tests/hello.cpp
(cd acme.my-lib && "$MCPP" build > ../a-build.log 2>&1) || fail "A: the library does not build" a-build.log
(cd acme.my-lib && "$MCPP" test > ../a-test.log 2>&1) || fail "A: the library's test failed" a-test.log
grep -q 'hello \.\.\. ok (.*1 case)' a-test.log || fail "A: the test is not one protocol case" a-test.log

# B
"$MCPP" new prog > b.log 2>&1 || fail "B: mcpp new" b.log
(cd prog && "$MCPP" test > ../b-test.log 2>&1) || fail "B: the program's test failed" b-test.log
grep -q 'test_smoke \.\.\. ok (.*1 case)' b-test.log || fail "B: the smoke test is not one protocol case" b-test.log

# C: a framework whose entry offers to begin tests, and its own tests.
mkdir -p fw/src fw/tests app/tests
cat > fw/mcpp.toml <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true
TOML
printf 'import std;\nextern "C++" int run_cases();\nint main() { std::println("fw main"); return run_cases(); }\n' > fw/src/entry.cpp
printf 'extern "C++" int run_cases() { return 0; }\n' > fw/tests/own.cpp
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dev-dependencies]
fw = { path = "../fw" }
TOML
printf 'extern "C++" int run_cases() { return 0; }\n' > app/tests/begun.cpp
printf 'import std;\nint main() { std::println("own main"); return 0; }\n' > app/tests/own.cpp
(cd app && "$MCPP" test > ../c.log 2>&1) || fail "C: the dependent's tests" c.log
grep -q 'begun \.\.\. ok' c.log && grep -q 'own \.\.\. ok' c.log || fail "C: a test did not run" c.log
(cd fw && "$MCPP" test > ../c-own.log 2>&1) || fail "C: the framework's own test" c-own.log

# D: a second framework offers too.
cp -R fw fw2
sed -i.bak 's/name    = "fw"/name    = "fw2"/' fw2/mcpp.toml
printf 'fw2 = { path = "../fw2" }\n' >> app/mcpp.toml
(cd app && "$MCPP" test > ../d.log 2>&1) && fail "D: two offers were taken" d.log
grep -q "fw/main" d.log && grep -q "fw2/main" d.log || fail "D: the refusal does not name both" d.log
rm app/tests/own.cpp   # under [test] main every test begins at the entry
printf '\n[test]\nmain = { from = "fw2/main" }\n' >> app/mcpp.toml
(cd app && "$MCPP" test > ../d2.log 2>&1) || fail "D: [test] main did not choose" d2.log

# E
"$MCPP" new 3d --template lib > e.log 2>&1 && fail "E: a name beginning with a digit was taken" e.log
grep -q "does not begin with a letter" e.log || fail "E: the refusal" e.log
# F
mkdir -p fwm/src hdr/src mixed/tests
cat > fwm/mcpp.toml <<'TOML'
[package]
name    = "fwm"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true
TOML
printf 'export module fwm;\nexport int fwm_cases() { return 0; }\n' > fwm/src/fwm.cppm
printf 'import std;\nimport fwm;\nint main() { std::println("began at fwm"); return fwm_cases(); }\n' > fwm/src/entry.cpp
printf '[package]\nname    = "hdr"\nversion = "0.1.0"\n' > hdr/mcpp.toml
printf 'import std;\nint main() { std::println("began at hdr"); return 0; }\n' > hdr/src/hdr_main.cpp
printf 'int hdr_marker() { return 0; }\n' > hdr/src/hdr.cpp
cat > mixed/mcpp.toml <<'TOML'
[package]
name    = "mixed"
version = "0.1.0"

[dev-dependencies]
fwm = { path = "../fwm" }
hdr = { path = "../hdr" }
TOML
printf 'import fwm;\nint uses_fwm() { return fwm_cases(); }\n' > mixed/tests/modular.cpp
printf 'int uses_headers() { return 0; }\n' > mixed/tests/headers.cpp
(cd mixed && "$MCPP" test > ../f.log 2>&1) || fail "F: the mixed package's tests" f.log
grep -A2 'modular \.\.\. ok' f.log | grep -q "began at fwm" || fail "F: the modular test did not begin at fwm" f.log
grep -A2 'headers \.\.\. ok' f.log | grep -q "began at hdr" || fail "F: the header test did not begin at hdr" f.log

# G
printf 'test-guide = "GUIDE.md"\n' >> fwm/mcpp.toml
printf '# Writing a test with fwm\n' > fwm/GUIDE.md
(cd mixed && "$MCPP" test --guide > ../g.log 2>&1) || fail "G: --guide" g.log
grep -q "fwm 0.1.0" g.log && grep -q "fwm/GUIDE.md" g.log || fail "G: the framework and its guide" g.log
(cd mixed && "$MCPP" test --guide --message-format json > ../g.json 2>/dev/null) || fail "G: --guide json" g.json
grep -q '"guide":"[^"]*fwm/GUIDE.md"' g.json || fail "G: the guide in JSON" g.json
printf 'import fwm;\nint uses_fwm() { return fwm_cases(); }\nint broken = missing_name;\n' > mixed/tests/modular.cpp
(cd mixed && "$MCPP" test > ../g-fail.log 2>&1) && fail "G: a test that does not compile passed" g-fail.log
grep -q "how these tests are written: mcpp test --guide" g-fail.log || fail "G: the failure does not point to the guide" g-fail.log

# H
mkdir -p fwh/src deep/tests
cat > fwh/mcpp.toml <<'TOML'
[package]
name    = "fwh"
version = "0.1.0"

[targets.main]
kind      = "entry"
main      = "src/entry.cpp"
test-main = true

[dependencies]
hdr = { path = "../hdr" }
TOML
printf 'import std;\nint main() { std::println("began at fwh"); return 0; }\n' > fwh/src/entry.cpp
printf 'int fwh_marker() { return 0; }\n' > fwh/src/fwh.cpp
printf '[package]\nname    = "deep"\nversion = "0.1.0"\n\n[dev-dependencies]\nfwh = { path = "../fwh" }\n' > deep/mcpp.toml
printf 'int uses_fwh() { return 0; }\n' > deep/tests/one.cpp
(cd deep && "$MCPP" test > ../h.log 2>&1) || fail "H: a main beneath the dev-dependency was linked" h.log
grep -q "began at fwh" h.log || fail "H: the test did not begin at fwh" h.log

# I
mkdir -p shlib/src shapp/tests
printf '[package]\nname    = "shlib"\nversion = "0.1.0"\n\n[targets.shlib]\nkind = "shared"\n' > shlib/mcpp.toml
printf 'extern "C" int shlib_answer() { return 42; }\n' > shlib/src/shlib.cpp
printf '[package]\nname    = "shapp"\nversion = "0.1.0"\n\n[dev-dependencies]\nshlib = { path = "../shlib" }\n' > shapp/mcpp.toml
printf 'extern "C" int shlib_answer();\nint main() { return shlib_answer() == 42 ? 0 : 1; }\n' > shapp/tests/links.cpp
(cd shapp && "$MCPP" test > ../i.log 2>&1) || fail "I: a shared dev-dependency is not linked" i.log

echo "PASS: 1037 a skeleton tests with mtest, and a test needs no main"
