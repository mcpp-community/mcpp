#!/usr/bin/env bash
# requires:
# Syntax errors must not prevent IDE configure from publishing a fresh CDB.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PROJECT="$TMP/app"
mkdir -p "$PROJECT/src"

cat >"$PROJECT/mcpp.toml" <<'EOF'
[package]
name = "ide-broken-source"
version = "1.0.0"
standard = "c++fly"
EOF

# 故意保留语法错误：configure 只能解析工程，不能要求普通 TU 编译成功。
cat >"$PROJECT/src/main.cpp" <<'EOF'
int main( {
    return missing_symbol;
}
EOF

set +e
(cd "$PROJECT" && "$MCPP" ide configure --format json) \
    >"$TMP/invalid-format.out" 2>"$TMP/invalid-format.err"
invalid_format_rc=$?
set -e
if [ "$invalid_format_rc" -ne 2 ]; then
    echo "FAIL: unsupported IDE format returned $invalid_format_rc, expected 2" >&2
    cat "$TMP/invalid-format.out" >&2
    cat "$TMP/invalid-format.err" >&2
    exit 1
fi
if [ -e "$PROJECT/compile_commands.json" ]; then
    echo "FAIL: unsupported IDE format published a CDB" >&2
    exit 1
fi

if ! (cd "$PROJECT" && "$MCPP" ide configure --format ndjson) \
    >"$TMP/events.ndjson" 2>"$TMP/stderr.log"; then
    cat "$TMP/stderr.log" >&2
    cat "$TMP/events.ndjson" >&2
    exit 1
fi

python3 - "$TMP/events.ndjson" "$PROJECT" <<'PY'
import json
import os
import sys

event_path, project = sys.argv[1:]
events = [json.loads(line) for line in open(event_path, encoding="utf-8") if line.strip()]
assert [event["type"] for event in events] == [
    "operation-started", "snapshot-published", "operation-finished"
]
published = events[1]
assert published["phase"] == "configured"
assert published["state"] == "configured"
assert published["compileCommandCount"] >= 1

snapshot_cdb = published["compileCommands"]
compat_cdb = published["compatibilityCompileCommands"]
assert os.path.isfile(snapshot_cdb), snapshot_cdb
assert os.path.realpath(compat_cdb) == os.path.realpath(
    os.path.join(project, "compile_commands.json")
), (compat_cdb, project)
assert os.path.isfile(compat_cdb), compat_cdb

cdb = json.load(open(snapshot_cdb, encoding="utf-8"))
entry = next(item for item in cdb if item["file"].endswith("/src/main.cpp"))
assert os.path.realpath(entry["directory"]) == os.path.realpath(project), (
    entry["directory"], project
)
assert entry["arguments"][0].endswith("clang++"), entry["arguments"][0]
assert any(arg.startswith("-std=c++") for arg in entry["arguments"]), entry["arguments"]
PY

cat >>"$PROJECT/mcpp.toml" <<'EOF'

[build]
cxxflags = ["-DIDE_SNAPSHOT_CHANGED=1"]
EOF

if ! (cd "$PROJECT" && "$MCPP" ide configure --format ndjson) \
    >"$TMP/events-changed.ndjson" 2>"$TMP/stderr-changed.log"; then
    cat "$TMP/stderr-changed.log" >&2
    cat "$TMP/events-changed.ndjson" >&2
    exit 1
fi

python3 - "$TMP/events.ndjson" "$TMP/events-changed.ndjson" <<'PY'
import json
import sys

before_path, after_path = sys.argv[1:]

def published(path):
    events = [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]
    return next(event for event in events if event["type"] == "snapshot-published")

before = published(before_path)
after = published(after_path)
assert before["configurationId"] == after["configurationId"], (before, after)
assert before["toolchainFingerprint"] == after["toolchainFingerprint"], (before, after)
assert before["snapshotId"] != after["snapshotId"], (before, after)

cdb = json.load(open(after["compileCommands"], encoding="utf-8"))
entry = next(item for item in cdb if item["file"].endswith("/src/main.cpp"))
assert "-DIDE_SNAPSHOT_CHANGED=1" in entry["arguments"], entry["arguments"]
PY

if find "$PROJECT/target" -type f \( -name '*.o' -o -name '*.obj' \) -print -quit 2>/dev/null | grep -q .; then
    echo "FAIL: ide configure compiled an ordinary object" >&2
    exit 1
fi
if find "$PROJECT/target" -type f -path '*/bin/*' -print -quit 2>/dev/null | grep -q .; then
    echo "FAIL: ide configure linked a final target" >&2
    exit 1
fi

echo "OK"
