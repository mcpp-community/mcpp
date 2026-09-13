#!/usr/bin/env bash
# requires: macos
# 666 -- on the one host that produces a Mach-O program, `mcpp pack` stages
# the program and its declared files and hands the tree to a dispatched
# format with `closure = not-walked` and a reason (#630, item 3a). Before
# this, the Mach-O refusal preceded staging, and a bundler reached its action
# with no tree at all. The negative direction stays: `--format dir` and
# `--format tar`, whose product is the closure, still refuse.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new zapapp > /dev/null
cd zapapp
mkdir -p share
printf 'notes\n' > share/notes.txt
cat >> mcpp.toml <<'EOF'

[runtime]
deploy = [ { from = "share/notes.txt", to = "data" } ]
EOF
cat > dist.sh <<'EOF'
set -e
stage="$1"; manifest="$2"; out="$3"
{
  echo "staged:"
  ( cd "$stage" && find . -type f | sort )
  echo "manifest:"
  sed -n '1,2p' "$manifest"
} > "$out"
EOF
chmod +x dist.sh
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string root  = mcpp::manifest_dir();
    const std::string stage = std::string("${mcpp.stage_dir}");
    const std::string out   = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id          = "zap";
    a.role        = "artifact";
    a.description = "zap";
    a.arg((root + "/dist.sh").c_str())
     .arg(stage.c_str())
     .arg((stage + ".stage-manifest").c_str())
     .arg(out.c_str())
     .input("${mcpp.target_file:zapapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

# The negative direction first: the built-in archive still refuses, since
# its product IS the closure.
if "$MCPP" pack --format dir > dir.log 2>&1; then
    fail "--format dir of a Mach-O program was accepted" dir.log
fi
grep -q 'Mach-O' dir.log || fail "the refusal does not name the format" dir.log
echo "  negative: --format dir still refuses a Mach-O program"

"$MCPP" pack --format zap > zap.log 2>&1 || fail "mcpp pack --format zap failed" zap.log
grep -q 'staged without its dependency closure' zap.log \
    || fail "pack did not report the tree as staged without its closure" zap.log
Z=$(find target -name 'app.zap' | head -1)
[ -n "$Z" ] || fail "--format zap produced nothing" zap.log
grep -qx './bin/zapapp' "$Z" || fail "the action's view of the tree lacks bin/zapapp" "$Z" zap.log
grep -qx './bin/data/notes.txt' "$Z" || fail "the action's view of the tree lacks the deployed file" "$Z" zap.log
grep -qx 'closure = not-walked' "$Z" || fail "the manifest does not say closure = not-walked" "$Z" zap.log
grep -q '^reason = ' "$Z" || fail "the manifest carries no reason line" "$Z" zap.log
echo "  positive: the dispatched format sees the program, the deployed file, and closure = not-walked"
echo "PASS: 666_a_macho_program_is_staged_without_its_closure"
