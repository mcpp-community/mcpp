#!/usr/bin/env bash
# requires: python3
# 735 -- an offline plan that needs a package download reports
# MCPP_OFFLINE_DOWNLOAD_REQUIRED, and names the package (#648 L6).
#
# The package comes from a local path index, so the descriptor is present and
# the download is the only thing missing. Criteria:
#   A. offline, the envelope's code is MCPP_OFFLINE_DOWNLOAD_REQUIRED, its
#      message names the package and its version, and the exit status is 1;
#   B. a plan that fails for another reason keeps MCPP_BUILD_DATABASE_PLAN_FAILED.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

mkdir -p "$TMP/index/pkgs/w" "$TMP/app/src"
cat > "$TMP/index/pkgs/w/widget.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "acme",
    name = "widget",
    description = "a package whose archive is not installed",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
    mcpp = { language = "c++23", sources = { "src/*.cpp" }, targets = { ["widget"] = { kind = "lib" } } },
}
EOF
INDEX_HOST="$(host_path "$TMP/index")"
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[indices]
acme = { path = "$INDEX_HOST" }

[dependencies.acme]
widget = "1.0.0"
EOF
echo 'int main() { return 0; }' > "$TMP/app/src/main.cpp"
cd "$TMP/app"

code_of() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["diagnostics"][0]["code"] if d["diagnostics"] else ""); print(d["diagnostics"][0]["message"] if d["diagnostics"] else "")' "$1"; }

set +e
MCPP_OFFLINE=1 "$MCPP" emit build-database --format json > a.json 2> a.err
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "A: exit status $rc, expected 1" a.err
code_of a.json > a.txt
sed -n 1p a.txt | grep -qx MCPP_OFFLINE_DOWNLOAD_REQUIRED || fail "A: wrong code" a.txt
grep -q "widget" a.txt && grep -q "1.0.0" a.txt || fail "A: the message does not name the package and version" a.txt

# B. another failure keeps the general code: a dependency no index declares.
sed -i.bak 's/^widget = "1.0.0"/absent-package = "1.0.0"/' mcpp.toml
set +e
MCPP_OFFLINE=1 "$MCPP" emit build-database --format json > b.json 2> b.err
set -e
code_of b.json > b.txt
sed -n 1p b.txt | grep -qx MCPP_BUILD_DATABASE_PLAN_FAILED || fail "B: a selector error took the offline code" b.txt
echo "PASS: 735 an offline plan that needs a download has its own code"
