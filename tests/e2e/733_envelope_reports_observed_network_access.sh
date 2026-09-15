#!/usr/bin/env bash
# requires: unix-shell python3
# 733 -- the envelope's `effects` report the network access that happened, and
# an offline plan that needs the index has its own code (#648 L4, L6).
#
# docs/50 defines `effects` as what running the command did. A home with no
# package index plans a registry dependency: online, the plan refreshes the
# index (the xlings stub answers at once and fetches nothing); offline, it does
# not. Criteria:
#   A. online, the failure envelope lists `network`;
#   B. offline, it does not, and its code is MCPP_OFFLINE_DOWNLOAD_REQUIRED.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
rm -rf "$MCPP_HOME/registry/data/mcpplibs"

mkdir -p "$TMP/bin"
cat > "$TMP/bin/xlings" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "${STUB_LOG:?}"
exit 0
EOF
chmod +x "$TMP/bin/xlings"
STUB_HOST="$(host_path "$TMP/bin/xlings")"
{
    grep -v '^binary' "$MCPP_HOME/config.toml" 2>/dev/null | sed '/^\[xlings\]/d'
    printf '\n[xlings]\nbinary = "%s"\n' "$STUB_HOST"
} > "$TMP/config.toml"
mv "$TMP/config.toml" "$MCPP_HOME/config.toml"

mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies.mcpplibs]
probe-not-published = "1.0.0"
EOF
echo 'int main() { return 0; }' > "$TMP/app/src/main.cpp"
cd "$TMP/app"

effects() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(" ".join(d["effects"])); print(" ".join(x["code"] for x in d["diagnostics"]))' "$1"; }

STUB_LOG="$TMP/online.log" "$MCPP" emit build-database --format json > online.json 2> online.err || true
[ -s online.json ] || fail "A: no envelope" online.err
grep -q '^update' "$TMP/online.log" 2>/dev/null || fail "A: the plan did not refresh the index" online.err
effects online.json > online.txt
head -1 online.txt | grep -qw network || fail "A: a plan that refreshed the index reports no network effect" online.txt

STUB_LOG="$TMP/offline.log" MCPP_OFFLINE=1 "$MCPP" emit build-database --format json > offline.json 2> offline.err || true
[ -s offline.json ] || fail "B: no envelope" offline.err
effects offline.json > offline.txt
if head -1 offline.txt | grep -qw network; then fail "B: an offline plan reports a network effect" offline.txt; fi
sed -n 2p offline.txt | grep -qw MCPP_OFFLINE_DOWNLOAD_REQUIRED \
    || fail "B: an offline plan with no index is not MCPP_OFFLINE_DOWNLOAD_REQUIRED" offline.txt
echo "PASS: 733 the envelope reports observed network access"
