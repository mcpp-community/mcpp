#!/usr/bin/env bash
# requires: gcc
# 1035 -- a test program that implements the test protocol reports its cases (docs/08 "Cases").
#
# mcpp runs the runner's side of the mtest test protocol, version 1: a program whose entry states
# `test-protocol = 1`, or whose file carries the marker `mtest-protocol=1`, is told the protocol's
# variables, lists its cases, runs a selection and reports each case; every other program is one
# case judged by its exit status and is told none of the variables.
#
# Criteria:
#   A. A run's record names the protocol and each case's result, a failure with its place; a case a
#      crash left without an end failed (P5.4); the human report names the failed cases.
#   B. `--case PATTERN` runs the matching cases of a protocol program and no other program.
#   C. `--cases` lists each protocol program's cases and runs none.
#   D. A program that implements no protocol receives none of the variables, though mcpp's own
#      environment has them (P1.2).
#   E. A program found by its marker is a protocol program.
#   F. `--no-run --message-format json` records each built test with its program and sources.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

cd "$TMP"
printf '[workspace]\nmembers = ["fw", "app", "plain"]\n' > mcpp.toml

# fw: a framework whose entry implements the protocol; a test file defines `cases()`.
mkdir -p fw/src
cat > fw/mcpp.toml <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind          = "entry"
main          = "src/main.cpp"
test-protocol = 1
TOML
cat > fw/src/main.cpp <<'CPP'
import std;
struct test_case { std::string id; int line; int (*body)(); };
std::vector<test_case> cases();
int main() {
    const char* mode = std::getenv("MTEST_MODE");
    const char* path = std::getenv("MTEST_REPORT");
    std::ofstream report;
    if (path) report.open(path, std::ios::app);
    auto event = [&](const std::string& body) { if (report) { report << "{" << body << "}\n"; report.flush(); } };
    event(R"("e":"program","protocol":1,"level":1,"framework":"fw","version":"0.1.0")");
    std::set<std::string> selected;
    bool selecting = false;
    if (const char* s = std::getenv("MTEST_SELECT")) {
        selecting = true;
        std::ifstream in(s);
        for (std::string line; std::getline(in, line);) if (!line.empty()) selected.insert(line);
    }
    int failed = 0;
    for (const auto& c : cases()) {
        if (mode && std::string(mode) == "list") {
            event(std::format(R"("e":"case","id":"{}","name":"{}","file":"t.cpp","line":{})", c.id, c.id, c.line));
            continue;
        }
        if (selecting && !selected.contains(c.id)) continue;
        event(std::format(R"("e":"begin","id":"{}","t":0)", c.id));
        const int r = c.body();
        if (r != 0) {
            ++failed;
            event(std::format(R"("e":"failure","id":"{}","file":"t.cpp","line":{},"message":"{} != 0")", c.id, c.line, r));
        }
        event(std::format(R"("e":"end","id":"{}","status":"{}","ms":1)", c.id, r ? "fail" : "pass"));
    }
    event(std::format(R"("e":"done","status":"{}","counts":{{}})", failed ? "fail" : "pass"));
    return failed ? 1 : 0;
}
CPP

# app: its tests begin at fw's entry.
mkdir -p app/src app/tests
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dev-dependencies]
fw = { path = "../fw" }

[test]
main = { from = "fw/main" }
TOML
printf 'export module app;\nexport int answer() { return 42; }\n' > app/src/app.cppm
cat > app/tests/math.cpp <<'CPP'
import std;
import app;
struct test_case { std::string id; int line; int (*body)(); };
std::vector<test_case> cases() {
    return {
        {"math/adds", 4, [] { return answer() == 42 ? 0 : 1; }},
        {"math/fails", 5, [] { return 7; }},
        {"math/crashes", 6, [] { std::exit(3); return 0; }},
    };
}
CPP

# plain: a program that implements no protocol, and one found by its marker.
mkdir -p plain/tests
printf '[package]\nname    = "plain"\nversion = "0.1.0"\n' > plain/mcpp.toml
cat > plain/tests/bare.cpp <<'CPP'
import std;
int main() {
    for (const char* v : {"MTEST_PROTOCOL", "MTEST_MODE", "MTEST_REPORT"})
        if (std::getenv(v)) { std::println("{} reached a program that implements no protocol", v); return 3; }
    return 0;
}
CPP
cat > plain/tests/marked.cpp <<'CPP'
import std;
constinit const char marker[] = "mtest-protocol=1";
int main() {
    [[maybe_unused]] volatile char keep = marker[0];
    const char* path = std::getenv("MTEST_REPORT");
    if (!path) return 4;
    std::ofstream report(path, std::ios::app);
    const std::string mode = std::getenv("MTEST_MODE") ? std::getenv("MTEST_MODE") : "run";
    report << R"({"e":"program","protocol":1,"level":1,"framework":"marked","version":"1"})" << "\n";
    if (mode == "list") report << R"({"e":"case","id":"only","name":"only","file":"marked.cpp","line":1})" << "\n";
    else report << R"({"e":"begin","id":"only","t":0})" << "\n" << R"({"e":"end","id":"only","status":"pass","ms":0})" << "\n";
    report << R"({"e":"done","status":"pass","counts":{}})" << "\n";
    return 0;
}
CPP

record() {  # record LOG TEST: the test's record
    $PY - "$1" "$2" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("{"): continue
    try: r = json.loads(line)
    except ValueError: continue
    if r.get("test") == sys.argv[2]: print(json.dumps(r)); break
PY
}
field() {  # field JSON EXPRESSION
    $PY -c "import json,sys; r=json.loads(sys.argv[1]); print(eval(sys.argv[2]))" "$1" "$2"
}

# A
"$MCPP" test -p app --message-format json > a.json 2> a.err && fail "A: a failing case passed the run" a.json a.err
rec=$(record a.json math)
[ -n "$rec" ] || fail "A: no record of the test" a.json a.err
[ "$(field "$rec" "r['protocol']")" = 1 ] || fail "A: the record does not name the protocol" a.json
[ "$(field "$rec" "[c['id']+':'+c['status'] for c in r['cases']]")" = "['math/adds:pass', 'math/fails:fail', 'math/crashes:fail']" ] \
    || fail "A: the cases' results" a.json
[ "$(field "$rec" "r['cases'][1]['failures'][0]['line']")" = 5 ] || fail "A: the failure's place" a.json
field "$rec" "r['cases'][2]['reason']" | grep -q "status 3 before the case ended" || fail "A: a crashed case's reason (P5.4)" a.json
"$MCPP" test -p app > a.log 2>&1 && fail "A: the human run passed" a.log
grep -q "case math/fails failed at t.cpp:5" a.log || fail "A: the human report does not name the failed case" a.log

# B
"$MCPP" test --case math/adds --message-format json > b.json 2> b.err || fail "B: the selected case failed" b.json b.err
rec=$(record b.json math)
[ "$(field "$rec" "[c['id'] for c in r['cases']]")" = "['math/adds']" ] || fail "B: the selection" b.json
[ -z "$(record b.json bare)" ] || fail "B: a program with no protocol ran under --case" b.json

# C
"$MCPP" test --cases > c.log 2>&1 || fail "C: --cases" c.log
grep -q "math: 3 cases" c.log || fail "C: the listing of math" c.log
grep -q "math/fails  t.cpp:5" c.log || fail "C: a listed case's place" c.log
grep -q "bare: one case" c.log || fail "C: a program with no protocol" c.log

# D, E
MTEST_PROTOCOL=1 MTEST_MODE=list MTEST_REPORT="$TMP/outer.ndjson" "$MCPP" test -p plain --message-format json > d.json 2> d.err \
    || fail "D: the plain member's tests" d.json d.err
[ "$(field "$(record d.json bare)" "r['status']")" = pass ] || fail "D: the protocol's variables reached a program with none (P1.2)" d.json
[ "$(field "$(record d.json marked)" "r['protocol']")" = 1 ] || fail "E: the marker" d.json
[ "$(field "$(record d.json marked)" "[c['id'] for c in r['cases']]")" = "['only']" ] || fail "E: the marked program's case" d.json

# F
"$MCPP" test -p app --no-run --message-format json > f.json 2> f.err || fail "F: --no-run" f.json f.err
rec=$(record f.json math)
[ "$(field "$rec" "r['status']")" = built ] || fail "F: the built record" f.json
field "$rec" "r['program']" | grep -q math || fail "F: the program" f.json
field "$rec" "any(s.endswith('app/tests/math.cpp') for s in r['sources']) and any(s.endswith('app/src/app.cppm') for s in r['sources'])" \
    | grep -q True || fail "F: the sources" f.json

echo "PASS: 1035 a test program reports its cases"
