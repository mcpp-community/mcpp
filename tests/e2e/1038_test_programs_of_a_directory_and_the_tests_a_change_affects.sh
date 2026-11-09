#!/usr/bin/env bash
# requires: gcc
# 1038 -- the files of one directory are one test program, and `mcpp test` runs the tests a change
# affects (docs/08 "Where tests are", "The tests a change affects").
#
# Criteria:
#   A. `[test] programs = "directory"`: the files of a directory below `tests/` are one program,
#      named by the directory, whose cases are all of theirs, run as a process per file of its cases
#      and, under `split = "none"`, once; a directory whose file defines `main`
#      begins there; two files of one directory that define `main` are refused, naming both.
#   B. A program built from a file of plain `main` that links a framework's library (it carries the
#      protocol's marker and speaks none) is one case, judged by its exit status, run and listed.
#   C. `--changed PATH` runs the tests built from PATH (a module a test imports, transitively) and
#      not the others; a file no test is built from or names affects none; a file a test names
#      affects it; the manifest affects every test. `--since COMMIT` reads the change from git.
#   D. Switching between `programs = "directory"` and the default builds without a stale product
#      in the way.
#   E. Under a shipping profile, `mcpp build` and `mcpp test` are one configuration: the test
#      compiles none of the code under test again, which is trimmed as the build trims it, and a
#      test program's own file keeps its path (`__FILE__` finds the test's directory).
#   F. A case a listed case declares (protocol P3.4) is selected by its name alone; a pattern below
#      the listed case runs it whole.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app
mkdir -p tests/unit tests/e2e tests/data
cat > src/calc.cppm <<'CPP'
export module app.calc;
export int twice(int x) { return 2 * x; }
CPP
cat > tests/unit/a.cpp <<'CPP'
import mcpplibs.mtest;
import app.calc;
using namespace mcpplibs::mtest;
UTest a{"doubles", [] { expect(eq(twice(2), 4)); }};
CPP
cat > tests/unit/b.cpp <<'CPP'
import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest b{"reads its data", [] { expect(eq(std::string_view("tests/data/f.txt").size(), 16u)); }};
CPP
printf 'import std;\nint main() { return 0; }\n' > tests/e2e/plain.cpp
printf 'data\n' > tests/data/f.txt

# A
printf '\n[test]\nprograms = "directory"\n' >> mcpp.toml
"$MCPP" test > a.log 2>&1 || fail "A: the directory programs" a.log
# A program that speaks the protocol runs as a process per file of its cases (`split = "file"`).
grep -q 'unit/a \.\.\. ok (.*1 case)' a.log && grep -q 'unit/b \.\.\. ok (.*1 case)' a.log \
    || fail "A: unit does not run as a process per file" a.log
printf 'split = "none"\n' >> mcpp.toml
"$MCPP" test > a.log 2>&1 || fail "A: the directory programs, run once" a.log
grep -q 'unit \.\.\. ok (.*2 cases)' a.log || fail "A: unit is not one program of two cases" a.log
grep -q '^e2e \.\.\. ok' a.log || fail "A: the directory of a file with main" a.log
"$MCPP" test --cases > a-cases.log 2>&1 || fail "A: --cases" a-cases.log
grep -q 'doubles' a-cases.log && grep -q 'reads its data' a-cases.log || fail "A: the program's cases" a-cases.log
# B: a file of plain main beside the library's marker.
grep -q 'e2e: one case, its exit status' a-cases.log || fail "B: a program of plain main is listed as one case" a-cases.log
printf 'int main() { return 0; }\n' > tests/e2e/second.cpp
"$MCPP" test > a2.log 2>&1 && fail "A: two files of one directory that define main were taken" a2.log
grep -q "plain.cpp" a2.log && grep -q "second.cpp" a2.log || fail "A: the refusal does not name both" a2.log
rm tests/e2e/second.cpp

# C
changed() {  # changed LOG PATH...: the tests run
    local log=$1; shift
    local args=()
    for p in "$@"; do args+=(--changed "$p"); done
    "$MCPP" test "${args[@]}" > "$log" 2>&1 || fail "C: mcpp test ${args[*]}" "$log"
}
changed c1.log src/calc.cppm
grep -q 'Affected 1 of 3 tests' c1.log && grep -q 'unit \.\.\. ok' c1.log || fail "C: the change of an imported module" c1.log
grep -q 'test_smoke \.\.\.' c1.log && fail "C: a test not built from the change ran" c1.log
changed c2.log README.md
grep -q 'Affected 0 of 3 tests' c2.log || fail "C: a file nothing reads" c2.log
changed c3.log tests/data/f.txt
grep -q 'Affected 1 of 3 tests' c3.log && grep -q 'unit \.\.\. ok' c3.log || fail "C: a file a test names" c3.log
changed c4.log mcpp.toml
grep -q 'Affected 3 of 3 tests' c4.log || fail "C: the manifest affects every test" c4.log
git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -qm base
printf '\nexport int thrice(int x) { return 3 * x; }\n' >> src/calc.cppm
"$MCPP" test --since HEAD > c5.log 2>&1 || fail "C: --since" c5.log
grep -q 'Affected 1 of 3 tests' c5.log || fail "C: --since reads the working tree's change" c5.log

# D
sed -i.bak '/^\[test\]/,$d' mcpp.toml
"$MCPP" test > d.log 2>&1 || fail "D: back to a program per file" d.log
grep -q 'unit/a \.\.\. ok' d.log && grep -q 'e2e/plain \.\.\. ok' d.log || fail "D: the programs per file" d.log
printf '\n[test]\nprograms = "directory"\n' >> mcpp.toml
"$MCPP" test > d2.log 2>&1 || fail "D: and back to the directories" d2.log

# E
cd "$TMP"
"$MCPP" new rel > /dev/null 2>&1
cd rel
cat > src/where.cppm <<'CPP'
export module rel.where;
import std;
export std::string library_file() { return std::source_location::current().file_name(); }
CPP
cat > tests/where.cpp <<'CPP'
import std;
import rel.where;
int main() {
    const std::string here = std::source_location::current().file_name();
    std::println("test {}", here);
    std::println("library {}", library_file());
    return std::filesystem::exists(std::filesystem::path(here).parent_path() / "test_smoke.cpp") ? 0 : 1;
}
CPP
"$MCPP" build --release > e.log 2>&1 || fail "E: the release build" e.log
"$MCPP" test --profile release where > e-test.log 2>&1 || fail "E: the release test (its file did not find its directory)" e-test.log
set -- target/*/*/build.ninja
[ $# = 1 ] || fail "E: build and test are $# configurations" e-test.log
grep -q "library rel@0.1.0/src/where.cppm" e-test.log || fail "E: the code under test is not trimmed as built" e-test.log
[ "$(grep -cE 'obj/where\.m\.(o|obj)	' "$(dirname "$1")/.ninja_log")" = 1 ] || fail "E: the test compiled the library again" "$(dirname "$1")/.ninja_log"

# F
cd "$TMP"
"$MCPP" new grp > /dev/null 2>&1
cd grp
cat > tests/test_smoke.cpp <<'CPP'
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest g{"g", [] {
    "one"_test = [] { expect(true); };
    "two"_test = [] { expect(true); };
}};
CPP
"$MCPP" test --cases > f-cases.log 2>&1 || fail "F: --cases" f-cases.log
grep -q 'test_smoke: 1 case' f-cases.log || fail "F: the object is the one listed case" f-cases.log
"$MCPP" test --case 'g/one' > f1.log 2>&1 || fail "F: --case g/one" f1.log
grep -q 'test_smoke \.\.\. ok (.*1 case)' f1.log || fail "F: a declared case is not selected alone" f1.log
"$MCPP" test --case 'g/t*' > f2.log 2>&1 || fail "F: --case g/t*" f2.log
grep -q 'test_smoke \.\.\. ok (.*3 cases)' f2.log || fail "F: a pattern below the listed case does not run it whole" f2.log

echo "PASS: 1038 test programs of a directory, and the tests a change affects"
