#!/usr/bin/env bash
# requires: import-std-libcxx
# Syntax errors must not prevent IDE configure from publishing a fresh CDB.
set -euo pipefail

OS="$(uname -s)"
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    echo "SKIP: libc++ std modules are unavailable on Windows"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PROJECT="$TMP/app"
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
mkdir -p "$PROJECT/src"

cat >"$PROJECT/mcpp.toml" <<EOF
[package]
name = "ide-broken-source"
version = "1.0.0"
standard = "c++fly"

[toolchain]
linux = "llvm@${LLVM_VERSION}"
macosx = "llvm@${LLVM_VERSION}"
EOF

# 故意保留语法错误：configure 只能解析工程，不能要求普通 TU 编译成功。
cat >"$PROJECT/src/main.cpp" <<'EOF'
import std;

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
operation_ids = {event["operationId"] for event in events}
assert len(operation_ids) == 1, events
assert next(iter(operation_ids)).startswith("operation-fnv1a64:"), events
published = events[1]
assert published["phase"] == "configured"
assert published["state"] == "configured"
assert published["compileCommandCount"] >= 1
std_module = published["stdModule"]
assert std_module["state"] == "ready", std_module
assert os.path.isfile(std_module["path"]), std_module

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
std_bmi = next(
    arg.removeprefix("-fmodule-file=std=")
    for arg in entry["arguments"]
    if arg.startswith("-fmodule-file=std=")
)
std_compat_bmi = next(
    arg.removeprefix("-fmodule-file=std.compat=")
    for arg in entry["arguments"]
    if arg.startswith("-fmodule-file=std.compat=")
)
assert os.path.realpath(std_bmi) == os.path.realpath(std_module["path"]), (
    std_bmi, std_module
)
assert os.path.isfile(std_bmi), std_bmi
assert os.path.isfile(std_compat_bmi), std_compat_bmi
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
import os
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
assert before["compileCommands"] != after["compileCommands"], (before, after)
assert os.path.isfile(before["compileCommands"]), before
assert ":" not in os.path.basename(after["compileCommands"]), after

cdb = json.load(open(after["compileCommands"], encoding="utf-8"))
entry = next(item for item in cdb if item["file"].endswith("/src/main.cpp"))
assert "-DIDE_SNAPSHOT_CHANGED=1" in entry["arguments"], entry["arguments"]
PY

# 用一个确定耗时的 build.mcpp 证明 started 在 prepare 完成前已经 flush。
# 若只在进程退出后读取整个文件，事件即使被错误地移到末尾也会假通过。
cat >"$PROJECT/build.mcpp" <<'EOF'
import std;

int main() {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}
EOF

LIVE_FIFO="$TMP/live-events.fifo"
mkfifo "$LIVE_FIFO"
(
    cd "$PROJECT"
    "$MCPP" ide configure --format ndjson >"$LIVE_FIFO" 2>"$TMP/live-stderr.log"
) &
live_pid=$!
exec 3<"$LIVE_FIFO"
if ! IFS= read -r -t 10 first_event <&3; then
    echo "FAIL: operation-started was not visible while configure was running" >&2
    kill "$live_pid" 2>/dev/null || true
    wait "$live_pid" 2>/dev/null || true
    cat "$TMP/live-stderr.log" >&2
    exit 1
fi
if ! kill -0 "$live_pid" 2>/dev/null; then
    echo "FAIL: first event became visible only after configure exited" >&2
    wait "$live_pid" 2>/dev/null || true
    exit 1
fi
printf '%s\n' "$first_event" >"$TMP/live-events.ndjson"
cat <&3 >>"$TMP/live-events.ndjson"
exec 3<&-
if ! wait "$live_pid"; then
    cat "$TMP/live-stderr.log" >&2
    cat "$TMP/live-events.ndjson" >&2
    exit 1
fi
python3 - "$TMP/live-events.ndjson" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
assert [event["type"] for event in events] == [
    "operation-started", "snapshot-published", "operation-finished"
], events
assert len({event["operationId"] for event in events}) == 1, events
PY

# 真实 CLI 失败路径必须闭合生命周期，不能只输出 diagnostic 后退出。
NO_MANIFEST="$TMP/no-manifest"
mkdir -p "$NO_MANIFEST"
set +e
(cd "$NO_MANIFEST" && "$MCPP" ide configure --format ndjson) \
    >"$TMP/failure-events.ndjson" 2>"$TMP/failure-stderr.log"
failure_rc=$?
set -e
if [ "$failure_rc" -ne 3 ]; then
    echo "FAIL: failed configure returned $failure_rc, expected 3" >&2
    cat "$TMP/failure-events.ndjson" >&2
    cat "$TMP/failure-stderr.log" >&2
    exit 1
fi
python3 - "$TMP/failure-events.ndjson" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
assert [event["type"] for event in events] == [
    "operation-started", "diagnostic", "operation-finished"
], events
assert [event["seq"] for event in events] == [1, 2, 3], events
assert len({event["operationId"] for event in events}) == 1, events
assert events[1]["diagnostic"]["code"] == "MCPP_IDE_CONFIGURE_FAILED", events
assert events[2]["status"] == "failed", events
PY

# `df4f75e` 允许依赖在 prepare 中构建 host tool。即使用户请求该子构建的
# verbose 输出，IDE stdout 仍必须保持纯 NDJSON，详细日志只能留给人类命令。
TOOL_PACKAGE="$TMP/tool-package"
VERBOSE_PROJECT="$TMP/verbose-project"
mkdir -p "$TOOL_PACKAGE/src" "$VERBOSE_PROJECT/src"
cat >"$TOOL_PACKAGE/mcpp.toml" <<'EOF'
[package]
name = "ide-tool-package"
version = "1.0.0"

[targets.codegen]
kind = "bin"
main = "src/codegen.cpp"
EOF
cat >"$TOOL_PACKAGE/src/codegen.cpp" <<'EOF'
int main() { return 0; }
EOF
cat >"$VERBOSE_PROJECT/mcpp.toml" <<EOF
[package]
name = "ide-tool-consumer"
version = "1.0.0"

[dependencies]
ide-tool-package = { path = "../tool-package", tools = ["codegen"] }

[toolchain]
linux = "llvm@${LLVM_VERSION}"
macosx = "llvm@${LLVM_VERSION}"
EOF
cat >"$VERBOSE_PROJECT/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
if ! (cd "$VERBOSE_PROJECT" && MCPP_TOOL_BUILD_VERBOSE=1 \
    "$MCPP" ide configure --format ndjson) \
    >"$TMP/verbose-events.ndjson" 2>"$TMP/verbose-stderr.log"; then
    cat "$TMP/verbose-stderr.log" >&2
    cat "$TMP/verbose-events.ndjson" >&2
    exit 1
fi
python3 - "$TMP/verbose-events.ndjson" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
assert [event["type"] for event in events] == [
    "operation-started", "snapshot-published", "operation-finished"
], events
PY

if find "$PROJECT/target" -type f \( -name '*.o' -o -name '*.obj' \) \
    ! -path '*/.build-mcpp/*' -print -quit 2>/dev/null | grep -q .; then
    echo "FAIL: ide configure compiled an ordinary object" >&2
    exit 1
fi
if find "$PROJECT/target" -type f -path '*/bin/*' ! -path '*/.build-mcpp/*' \
    -print -quit 2>/dev/null | grep -q .; then
    echo "FAIL: ide configure linked a final target" >&2
    exit 1
fi

echo "OK"
