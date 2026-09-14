#!/usr/bin/env bash
# requires: macos
# 666 -- on the one host that produces a Mach-O program, `mcpp pack` stages
# the program and its declared files and hands the tree to a dispatched
# format (#630, item 3a). Until #634 A3 the tree arrived with
# `closure = not-walked`, because the Mach-O closure was never read, and
# `--format dir` and `--format tar` refused the program. The closure is read
# from the load commands now, so both directions changed: the built-in
# archive stages the program, and the dispatched format sees
# `closure = walked` with the OS's libraries stated as `platform`. A program
# with a dylib of its own is 668.
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

# The built-in archive stages the program: its closure is only the OS's.
"$MCPP" pack --format dir > dir.log 2>&1 || fail "--format dir of a Mach-O program failed" dir.log
D=$(find target/dist -mindepth 1 -maxdepth 1 -type d -name 'zapapp-0.1.0-*' | head -1)
[ -n "$D" ] || fail "--format dir staged no tree" dir.log
[ -x "$D/bin/zapapp" ] || fail "the staged tree has no bin/zapapp" dir.log
[ "$(sed -n '1p' "$D.stage-manifest")" = "closure = walked" ] \
    || fail "the dir manifest does not say closure = walked" "$D.stage-manifest"
echo "  --format dir stages a Mach-O program with closure = walked"

"$MCPP" pack --format zap > zap.log 2>&1 || fail "mcpp pack --format zap failed" zap.log
if grep -q 'staged without its dependency closure' zap.log; then
    fail "pack still reports the tree as staged without its closure" zap.log
fi
Z=$(find target -name 'app.zap' | head -1)
[ -n "$Z" ] || fail "--format zap produced nothing" zap.log
grep -qx './bin/zapapp' "$Z" || fail "the action's view of the tree lacks bin/zapapp" "$Z" zap.log
grep -qx './bin/data/notes.txt' "$Z" || fail "the action's view of the tree lacks the deployed file" "$Z" zap.log
grep -qx 'closure = walked' "$Z" || fail "the manifest does not say closure = walked" "$Z" zap.log
if grep -q '^reason = ' "$Z"; then
    fail "a walked closure carries a reason line" "$Z" zap.log
fi
echo "  the dispatched format sees the program, the deployed file, and closure = walked"
echo "PASS: 666_a_macho_program_reaches_a_dispatched_format_with_its_tree"
