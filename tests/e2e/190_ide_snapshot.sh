#!/usr/bin/env bash
# requires:
# Verify the read-only IDE snapshot CLI contract.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

assert_json() {
    local file=$1 mode=$2
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$file" "$mode" <<'PY'
import json
import sys

path, mode = sys.argv[1:]
raw = open(path, encoding="utf-8").read()
decoder = json.JSONDecoder()
value, end = decoder.raw_decode(raw.lstrip())
assert not raw.lstrip()[end:].strip(), "stdout contains more than one JSON value"
assert isinstance(value, dict)
assert value["schemaVersion"] == 1
assert value["kind"] == "mcpp.ide.snapshot"

if mode == "partial":
    assert value["state"] == "partial"
    assert [member["name"] for member in value["workspace"]["members"]] == ["app"]
    assert value["workspace"]["selectedMembers"] == ["app"]
    assert value["artifacts"]["compileCommands"][0]["state"] == "missing"
elif mode == "stale":
    assert value["state"] == "stale"
    assert value["artifacts"]["compileCommands"][0]["state"] == "stale"
    assert value["state"] != "ready"
elif mode == "all":
    assert value["workspace"]["selectedMembers"] == ["one", "two"]
elif mode == "one":
    assert value["workspace"]["selectedMembers"] == ["one"]
else:
    expected = {
        "unknown": "MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND",
        "invalid": "MCPP_IDE_MANIFEST_INVALID",
        "format": "MCPP_IDE_UNSUPPORTED_FORMAT",
    }[mode]
    assert value["state"] == "unavailable"
    assert expected in [item["code"] for item in value["diagnostics"]]
PY
        return
    fi

    grep -q '"schemaVersion": 1' "$file"
    grep -q '"kind": "mcpp.ide.snapshot"' "$file"
    case "$mode" in
        partial) grep -q '"state": "partial"' "$file"; grep -q '"state": "missing"' "$file" ;;
        stale) grep -q '"state": "stale"' "$file"; ! grep -q '"state": "ready"' "$file" ;;
        all) grep -q '"one"' "$file"; grep -q '"two"' "$file" ;;
        one) grep -q '"one"' "$file" ;;
        unknown) grep -q 'MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND' "$file" ;;
        invalid) grep -q 'MCPP_IDE_MANIFEST_INVALID' "$file" ;;
        format) grep -q 'MCPP_IDE_UNSUPPORTED_FORMAT' "$file" ;;
    esac
}

run_success() {
    local dir=$1 mode=$2
    shift 2
    (cd "$dir" && "$MCPP" ide snapshot "$@") >"$TMP/out.json" 2>"$TMP/err.log"
    [[ ! -s "$TMP/err.log" ]] || { cat "$TMP/err.log"; exit 1; }
    ! LC_ALL=C grep -q $'\033' "$TMP/out.json" || { echo "ANSI bytes on stdout"; exit 1; }
    assert_json "$TMP/out.json" "$mode"
}

run_failure() {
    local dir=$1 mode=$2
    shift 2
    local rc=0
    (cd "$dir" && "$MCPP" ide snapshot "$@") >"$TMP/out.json" 2>"$TMP/err.log" || rc=$?
    [[ $rc -eq 3 ]] || { cat "$TMP/err.log"; echo "expected exit 3, got $rc"; exit 1; }
    [[ ! -s "$TMP/err.log" ]] || { cat "$TMP/err.log"; exit 1; }
    assert_json "$TMP/out.json" "$mode"
}

mkdir -p "$TMP/app"
cat >"$TMP/app/mcpp.toml" <<'EOF'
[package]
name = "app"
version = "1.0.0"
EOF
run_success "$TMP/app" partial --format json
printf '[]\n' >"$TMP/app/compile_commands.json"
run_success "$TMP/app" stale

mkdir -p "$TMP/workspace/one" "$TMP/workspace/two"
cat >"$TMP/workspace/mcpp.toml" <<'EOF'
[workspace]
members = ["one", "two"]
EOF
cat >"$TMP/workspace/one/mcpp.toml" <<'EOF'
[package]
name = "one"
version = "1.0.0"
EOF
cat >"$TMP/workspace/two/mcpp.toml" <<'EOF'
[package]
name = "two"
version = "1.0.0"
EOF
run_success "$TMP/workspace" all
run_success "$TMP/workspace" one -p one
run_success "$TMP/workspace" all --workspace
run_failure "$TMP/workspace" unknown -p missing

mkdir -p "$TMP/invalid"
printf '[package\nname = "bad"\n' >"$TMP/invalid/mcpp.toml"
run_failure "$TMP/invalid" invalid
run_failure "$TMP/app" format --format yaml

echo "OK"
