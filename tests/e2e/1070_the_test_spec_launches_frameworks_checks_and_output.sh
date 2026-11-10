#!/usr/bin/env bash
# requires: gcc
# 1070 — the mcpp test spec's B5, B6, O3 and O8 (docs/specs/test.md).
#
#   B5  a record of a program built and not run carries how it starts
#       (`launch`: argv, env, cwd, via), the arguments after `--` included;
#   B6  a framework's entry states the capabilities it implements and its
#       session's command, and `mcpp test --guide` reports both;
#   O3  a failure's `source` and `fatal`, and a case's `checks`, reach the
#       record and the kept run as the program reported them;
#   O8  a case's own output: the program's `output` events, or all that a
#       program which ran one case wrote; several cases are not divided.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f"; cat "$f"; done; exit 1; }

# The framework: an entry that begins a test and states the spec's keys.
mkdir -p fw/src app/tests
cat > fw/mcpp.toml <<'EOF'
[package]
name    = "fw"
version = "0.1.0"

[targets.main]
kind              = "entry"
main              = "src/main.cpp"
test-protocol     = 1
test-main         = true
test-capabilities = ["check-details", "output"]
test-session      = ["fw-serve", "--stdio"]
EOF
cat > fw/src/main.cpp <<'CPP'
import std;
extern bool cases(std::ofstream& report, bool list, const std::set<std::string>& selected);
int main() {
    const char* path = std::getenv("MTEST_REPORT");
    if (!path) return 4;
    std::ofstream report(path, std::ios::app);
    const std::string mode = std::getenv("MTEST_MODE") ? std::getenv("MTEST_MODE") : "run";
    std::set<std::string> selected;
    if (const char* s = std::getenv("MTEST_SELECT")) {
        std::ifstream in(s);
        for (std::string line; std::getline(in, line);) selected.insert(line);
    }
    report << R"({"e":"program","protocol":1,"level":1,"framework":"fw","version":"0.1.0"})" << "\n";
    const bool failed = cases(report, mode == "list", selected);
    report << (failed ? R"({"e":"done","status":"fail","counts":{}})" : R"({"e":"done","status":"pass","counts":{}})") << "\n";
    report.flush();
    return failed ? 1 : 0;
}
CPP

cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dev-dependencies]
fw = { path = "$TMP/fw" }
EOF
cat > app/tests/t.cpp <<'CPP'
import std;
bool cases(std::ofstream& r, bool list, const std::set<std::string>& selected) {
    auto on = [&](std::string_view id) { return selected.empty() || selected.contains(std::string(id)); };
    if (list) {
        r << R"({"e":"case","id":"stated","name":"stated","file":"t.cpp","line":3})" << "\n";
        r << R"({"e":"case","id":"plain","name":"plain","file":"t.cpp","line":4})" << "\n";
        return false;
    }
    if (on("stated")) {
        r << R"({"e":"begin","id":"stated","t":0})" << "\n";
        r << R"({"e":"output","id":"stated","stream":"out","text":"said by the case\n"})" << "\n";
        r << R"({"e":"failure","id":"stated","file":"t.cpp","line":3,"message":"a == b","expected":"2","actual":"3","source":"expect(eq(a, b));","fatal":true})" << "\n";
        r << R"({"e":"end","id":"stated","status":"fail","ms":0,"checks":{"held":4,"failed":1}})" << "\n";
    }
    if (on("plain")) {
        std::println("written by plain");
        r << R"({"e":"begin","id":"plain","t":0})" << "\n";
        r << R"({"e":"end","id":"plain","status":"pass","ms":0})" << "\n";
    }
    return on("stated");
}
CPP
cd app

record() {  # record LOG TEST
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
field() { $PY -c "import json,sys; r=json.loads(sys.argv[1]); print(eval(sys.argv[2]))" "$1" "$2"; }

# B6
"$MCPP" test --guide --message-format json > guide.json 2> guide.err || fail "B6: --guide" guide.json guide.err
$PY - guide.json <<'PY' || fail "B6: the framework's capabilities and session" guide.json
import json, sys
g = json.loads([l for l in open(sys.argv[1]) if l.startswith("{")][0])["guide"]
f = [x for x in g["frameworks"] if x["entry"] == "fw/main"][0]
assert f["capabilities"] == ["check-details", "output"], f
assert f["session"] == ["fw-serve", "--stdio"], f
PY
"$MCPP" test --guide > guide.log 2>&1 || fail "B6: --guide (human)" guide.log
grep -q 'implements  check-details, output' guide.log || fail "B6: the human guide does not name the capabilities" guide.log
grep -q 'session  fw-serve --stdio' guide.log || fail "B6: the human guide does not name the session" guide.log

# B5
"$MCPP" test --no-run --message-format json -- --flag > norun.json 2> norun.err || fail "B5: --no-run" norun.json norun.err
rec=$(record norun.json t)
[ "$(field "$rec" "r['status']")" = built ] || fail "B5: the program is not reported built" norun.json
[ "$(field "$rec" "r['launch']['argv'][-1]")" = --flag ] || fail "B5: the launch omits the arguments after --" norun.json
[ "$(field "$rec" "r['launch']['argv'][0] == r['program'] or r['launch']['argv'][0].replace('\\\\', '/') == r['program']")" = True ] \
    || fail "B5: the launch does not start the program" norun.json
[ "$(field "$rec" "'MTEST_PROTOCOL' in r['launch']['env']")" = False ] || fail "B5: the launch carries the protocol's variables" norun.json
[ -n "$(field "$rec" "r['launch']['cwd']")" ] || fail "B5: the launch names no directory" norun.json

# O3, O8: both cases
"$MCPP" test --message-format json > all.json 2> all.err && fail "O3: the failing case passed" all.json all.err
rec=$(record all.json t)
[ "$(field "$rec" "r['cases'][0]['failures'][0]['source']")" = 'expect(eq(a, b));' ] || fail "O3: the failure's source" all.json
[ "$(field "$rec" "r['cases'][0]['failures'][0]['fatal']")" = True ] || fail "O3: the failure's fatal" all.json
[ "$(field "$rec" "(r['cases'][0]['checks']['held'], r['cases'][0]['checks']['failed'])")" = "(4, 1)" ] || fail "O3: the case's checks" all.json
[ "$(field "$rec" "r['cases'][0]['output']")" = 'said by the case' ] || fail "O8: the case's stated output" all.json
[ "$(field "$rec" "'output' in r['cases'][1]")" = False ] || fail "O8: the output of a program of two cases was divided" all.json
grep -q '"checks":{"failed":1,"held":4}\|"checks":{"held":4,"failed":1}' target/mtest/runs/*/cases.ndjson \
    || fail "O3: the kept run has no checks" target/mtest/runs/*/cases.ndjson

# O8: one case run alone
"$MCPP" test --case plain --message-format json > one.json 2> one.err || fail "O8: --case plain" one.json one.err
rec=$(record one.json t)
field "$rec" "r['cases'][0]['output']" | grep -q 'written by plain' || fail "O8: the output of a program that ran one case" one.json

echo "PASS: 1070 the test spec launches, frameworks, checks and output"
