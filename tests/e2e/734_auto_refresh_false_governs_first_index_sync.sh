#!/usr/bin/env bash
# requires: unix-shell
# 734 -- `[index] auto_refresh = false` governs the implicit refreshes that used
# to bypass the refresh policy (#648 L5).
#
# docs/05: auto_refresh = false means the index is never refreshed
# automatically. A project whose custom index has never been synced used to be
# synced on its first build regardless. Criteria, with an xlings stub that logs:
#   A. the build refuses before syncing, and names `mcpp index update`;
#   B. the stub was never asked to `update`.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
source "$(dirname "$0")/_host_path.sh"

mkdir -p "$TMP/bin"
cat > "$TMP/bin/xlings" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "${STUB_LOG:?}"
exit 0
EOF
chmod +x "$TMP/bin/xlings"
STUB_HOST="$(host_path "$TMP/bin/xlings")"
{
    grep -v '^binary\|^auto_refresh' "$MCPP_HOME/config.toml" 2>/dev/null \
        | sed '/^\[xlings\]/d; /^\[index\]$/d'
    printf '\n[xlings]\nbinary = "%s"\n\n[index]\nauto_refresh = false\n' "$STUB_HOST"
} > "$TMP/config.toml"
mv "$TMP/config.toml" "$MCPP_HOME/config.toml"

mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[indices]
acme = { url = "https://example.invalid/acme-index.git" }

[dependencies.acme]
widget = "1.0.0"
EOF
echo 'int main() { return 0; }' > "$TMP/app/src/main.cpp"
cd "$TMP/app"

set +e
STUB_LOG="$TMP/stub.log" "$MCPP" build > build.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "A: the build succeeded with an unsynced custom index" build.log
grep -q "mcpp index update" build.log || fail "A: the refusal does not name mcpp index update" build.log
grep -q "auto_refresh = false" build.log || fail "A: the refusal does not name the setting" build.log
if grep -q '^update' "$TMP/stub.log" 2>/dev/null; then
    fail "B: the custom index was synced although auto_refresh = false" "$TMP/stub.log"
fi
echo "PASS: 734 auto_refresh = false governs the first custom index sync"
