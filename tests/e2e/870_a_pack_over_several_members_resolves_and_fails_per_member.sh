#!/usr/bin/env bash
# requires: unix-shell
# 870 -- a pack over several members resolves a target name for the member an
# action is for, lets one member fail alone in the distribution step, and reports
# the members in `[workspace] members` order across configurations.
#
# mcpp#749 (K1, member selection design 2026-09-30, section 15.3). A pack of
# several members is one plan per configuration, so two members' targets of one
# name are in one plan, one ninja drive runs every member's distribution step,
# and the members are processed one configuration at a time.
#
# The fixture. `a`, `c` and `b`, in that order in `[workspace] members`. `a` and
# `b` each have a program target named `tool`, and `c`, which states another
# standard and is therefore a configuration of its own, has `ctool`. Each member's
# build program provides the format `zap` and submits an action that copies
# `${mcpp.target_file:<its target>}` to `<its OUT_DIR>/<package>.zap`, through
# `dist.sh`, which fails for a member whose `fail-<member>` file exists.
#
# Criteria:
#   A. Each member's distributable is its own program: `a.zap` prints `a` and
#      `b.zap` prints `b`, although both name the target `tool`.
#   B. The JSON envelope lists the members' stages in `[workspace] members`
#      order, `a`, `c`, `b`, although `a` and `b` are one configuration and are
#      packed before `c`.
#   C. When `b`'s distribution step fails, `a` and `c` are packed with files this
#      pack made, `b` is reported by name, and the command exits non-zero.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"

cat > mcpp.toml <<'EOF'
[workspace]
members = ["a", "c", "b"]
EOF

cat > dist.sh <<'EOF'
#!/usr/bin/env bash
set -e
here="$(cd "$(dirname "$0")" && pwd)"
[ -e "$here/fail-$3" ] && { echo "dist.sh: $3 is asked to fail" >&2; exit 1; }
cp "$1" "$2"
EOF
chmod +x dist.sh

member() {   # member <name> <target> [standard]
    local m=$1 t=$2 std=${3:-}
    mkdir -p $m/src
    {
        echo '[package]'
        echo "name    = \"$m\""
        echo 'version = "0.1.0"'
        [ -n "$std" ] && echo "standard = \"$std\""
        echo
        echo "[targets.$t]"
        echo 'kind = "bin"'
        echo 'main = "src/main.cpp"'
    } > $m/mcpp.toml
    printf '#include <cstdio>\nint main() { std::puts("%s"); return 0; }\n' $m > $m/src/main.cpp
    cat > $m/build.mcpp <<EOF
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string out  = mcpp::out_dir();
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = out + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str())
     .arg("\${mcpp.target_file:$t}")
     .arg(dist.c_str())
     .arg(mcpp::package_name())
     .input("\${mcpp.target_file:$t}")
     .output(dist.c_str())
     .submit();
    return 0;
}
EOF
}
member a tool
member b tool
member c ctool c++20

# A member's distributable, in its build-program output directory: one per
# configuration and package, below the workspace root (mcpp 2026.10.10.1+).
zap() { ls -d target/.build-mcpp/out/*/"$1"/"$1".zap 2>/dev/null | head -1; }

# ── A ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --workspace --format zap > a.log 2>&1 || fail "A: the pack failed" a.log
for m in a b c; do
    [ -x "$(zap $m)" ] || fail "A: no distributable for $m" a.log
    [ "$("$(zap $m)")" = "$m" ] || fail "A: $m's distributable runs '$("$(zap $m)")', not $m's program" a.log
done
echo "ok: A, each member's distributable is its own program, although a and b both name the target tool"

# ── B ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --workspace --format zap --message-format json > b.json 2> b.err \
    || fail "B: the JSON pack failed" b.err b.json
order=$(python3 -c 'import json,sys
d = json.load(open(sys.argv[1]))
print(" ".join(s["member"] for s in d["data"]["stages"]))' b.json) || fail "B: the envelope has no stages" b.json
[ "$order" = "a c b" ] || fail "B: the stages are listed as '$order', not in [workspace] members order (a c b)" b.json
echo "ok: B, the stages are listed in [workspace] members order across configurations"

# ── C ────────────────────────────────────────────────────────────────────────
touch fail-b
rc=0
"$MCPP" pack --workspace --format zap > c.log 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "C: the pack exited 0 although b's step failed" c.log
for m in a c; do
    [ -x "$(zap $m)" ] && [ "$("$(zap $m)")" = "$m" ] || fail "C: $m was not packed beside b's failure" c.log
    grep -qE "Packed .*$m\.zap" c.log || fail "C: $m's distributable was not reported" c.log
done
[ ! -e "$(zap b)" ] || fail "C: b's distributable of the previous pack is still in place" c.log
grep -qE "member 'b'|'b'" c.log || fail "C: the failure does not name b" c.log
echo "ok: C, a member whose distribution step fails fails alone"

echo "PASS: 870_a_pack_over_several_members_resolves_and_fails_per_member"
