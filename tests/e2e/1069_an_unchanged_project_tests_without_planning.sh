#!/usr/bin/env bash
# requires:
# 1069 — the tests of an unchanged project run without planning (the test fast
# path, design 2026-11-10 supplement T7).
#
# A full `mcpp test` records what it planned for every test of the package
# beside `test.ninja`. The next one admits that record as `mcpp build` admits
# its own and runs the selected tests from it: it resolves no dev-dependency,
# so it prints no `Resolved mcpplibs.` line (the package's programs are built
# as `mcpp build` builds them, which on some hosts plans each time). An edited
# test, a test added or removed, and an edited source each take the full path
# again, and the results are theirs.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
rm -f tests/*.cpp
cat >> mcpp.toml <<'EOF'

[dev-dependencies]
mcpplibs.mtest = "0.5.2"
EOF
write_cases() {
    cat > tests/cases.cpp <<EOF
import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest one{"one", [] { expect(eq(1, 1)); }};
UTest two{"two", [] { expect(eq(2, $1)); }};
EOF
}
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f"; cat "$f"; done; exit 1; }
write_cases 2

"$MCPP" test > full.log 2>&1 || fail "the first run failed" full.log
grep -q 'Resolved mcpplibs\.' full.log || fail "the first run did not plan" full.log
"$MCPP" test > fast.log 2>&1 || fail "the second run failed" fast.log
grep -q 'Resolved mcpplibs\.' fast.log && fail "an unchanged project was planned again" fast.log
grep -q 'cases: 2 passed, 0 failed' fast.log || fail "the second run did not run both cases" fast.log

# A selection runs from the record.
"$MCPP" test cases --case two > case.log 2>&1 || fail "--case two failed" case.log
grep -q 'Resolved mcpplibs\.' case.log && fail "a selection was planned again" case.log
grep -q 'cases: 1 passed, 0 failed' case.log || fail "--case two did not run one case" case.log

# An edited test is built and its result is the edit's.
sleep 1
write_cases 3
"$MCPP" test > edited.log 2>&1 && fail "the edited test passed" edited.log
grep -q 'cases: 1 passed, 1 failed' edited.log || fail "the edit's failure is not reported" edited.log
"$MCPP" test > again.log 2>&1 && fail "the failing test passed from the record" again.log
grep -q 'Resolved mcpplibs\.' again.log && fail "an unchanged failing project was planned again" again.log
grep -q 'cases: 1 passed, 1 failed' again.log || fail "the record's run lost the failure" again.log

# A test added is planned and run; one removed is no longer run.
sleep 1
write_cases 2
printf 'int main() { return 0; }\n' > tests/extra.cpp
"$MCPP" test > added.log 2>&1 || fail "the run with an added test failed" added.log
grep -q 'extra ... ok' added.log || fail "the added test did not run" added.log
rm tests/extra.cpp
"$MCPP" test > removed.log 2>&1 || fail "the run after removing a test failed" removed.log
grep -q 'extra' removed.log && fail "a removed test ran" removed.log

# A build after the tests keeps its own graph.
"$MCPP" build > build.log 2>&1 || fail "mcpp build failed" build.log
grep -q 'Compiling' build.log && fail "mcpp build after mcpp test compiled again" build.log

echo "PASS: 1069 an unchanged project tests without planning"
