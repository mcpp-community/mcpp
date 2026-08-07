#!/usr/bin/env bash
# requires: windows msvc
# Windows 不依赖 libc++ std modules，也必须能在源码语法错误时发布可用 CDB。
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PROJECT="$TMP/app"
mkdir -p "$PROJECT/src"

cat >"$PROJECT/mcpp.toml" <<'EOF'
[package]
name = "ide-windows-broken-source"
version = "1.0.0"

[toolchain]
windows = "msvc@system"
EOF

cat >"$PROJECT/src/main.cpp" <<'EOF'
int main( {
    return missing_symbol;
}
EOF

configure() {
    local output=$1
    if ! (cd "$PROJECT" && "$MCPP" ide configure --format ndjson) \
        >"$output" 2>"$output.stderr"; then
        cat "$output.stderr" >&2
        cat "$output" >&2
        return 1
    fi
}

configure "$TMP/first.ndjson"

# 第二次发布必须替换根目录兼容 CDB，同时保留按内容寻址的旧快照。
cat >>"$PROJECT/mcpp.toml" <<'EOF'

[build]
cxxflags = ["/DIDE_WINDOWS_CHANGED=1"]
EOF
configure "$TMP/second.ndjson"

python3 - "$TMP/first.ndjson" "$TMP/second.ndjson" "$PROJECT" <<'PY'
import json
import os
import sys

first_path, second_path, project = sys.argv[1:]


def read_events(path):
    with open(path, encoding="utf-8") as stream:
        events = [json.loads(line) for line in stream if line.strip()]
    assert [event["type"] for event in events] == [
        "operation-started", "snapshot-published", "operation-finished"
    ], events
    assert [event["seq"] for event in events] == [1, 2, 3], events
    assert len({event["operationId"] for event in events}) == 1, events
    return events[1]


first = read_events(first_path)
second = read_events(second_path)
assert first["configurationId"] == second["configurationId"], (first, second)
assert first["snapshotId"] != second["snapshotId"], (first, second)
assert first["compileCommands"] != second["compileCommands"], (first, second)
assert os.path.isfile(first["compileCommands"]), first
assert os.path.isfile(second["compileCommands"]), second

compatibility_cdb = os.path.join(project, "compile_commands.json")
assert os.path.realpath(second["compatibilityCompileCommands"]) == os.path.realpath(
    compatibility_cdb
), second
with open(compatibility_cdb, encoding="utf-8") as stream:
    cdb = json.load(stream)
entry = next(item for item in cdb if item["file"].replace("\\", "/").endswith("/src/main.cpp"))
assert "/DIDE_WINDOWS_CHANGED=1" in entry["arguments"], entry
PY

echo "PASS: Windows IDE configure publishes and replaces CDB before compilation"
