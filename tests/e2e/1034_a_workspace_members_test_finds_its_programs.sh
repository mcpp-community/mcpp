#!/usr/bin/env bash
# requires: gcc
# 1034 -- a workspace member's test finds the member's programs under `--workspace` as under `-p`.
#
# `mcpp test` builds a package's programs before its tests and names each to them as MCPP_BIN_<NAME>
# (docs/08). A workspace's test plan holds a member's test programs only, so under `--workspace` the
# member's programs were not built and its tests found no MCPP_BIN_<NAME>; `-p <member>` found them.
# (Found by mtest's conformance suite, whose tests run the member's own fixture.)
#
# Criteria: both members' tests pass under `--workspace` and under `-p`; each finds its own program
# of one shared name.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
printf '[workspace]\nmembers = ["a", "b"]\n' > mcpp.toml
for m in a b; do
    mkdir -p "$m/src" "$m/tests"
    cat > "$m/mcpp.toml" <<TOML
[package]
name    = "$m"
version = "0.1.0"

[targets.tool]
kind = "bin"
main = "src/tool.cpp"
TOML
    printf 'import std;\nint main() { std::println("%s"); return 0; }\n' "$m" > "$m/src/tool.cpp"
    cat > "$m/tests/finds.cpp" <<CPP
import std;
int main() {
    const char* tool = std::getenv("MCPP_BIN_TOOL");
    if (!tool || !std::filesystem::is_regular_file(tool)) { std::println("no MCPP_BIN_TOOL"); return 1; }
    const std::string path = tool;
    return path.find("$m") != std::string::npos || std::filesystem::path(path).parent_path().string().find("$m") != std::string::npos ? 0 : 2;
}
CPP
done
"$MCPP" test --workspace > ws.log 2>&1 || fail "--workspace: a member's test did not find its program" ws.log
"$MCPP" test -p a > a.log 2>&1 || fail "-p a" a.log
grep -q 'finds \.\.\. ok' ws.log || fail "the tests did not run under --workspace" ws.log
echo "PASS: 1034 a workspace member's test finds its programs"
