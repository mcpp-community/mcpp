#!/usr/bin/env bash
# 660_an_action_names_the_engine_through_mcpp_self.sh -- `${mcpp.self}` in an
# action's argv is the engine's own executable, and `mcpp stage` through it is
# a portable copy (2026.9.13.1).
#
# An action's command is an argv with no shell assumed, so a build program had
# no portable way to copy a file: `cp` is absent on Windows, `cmd /c copy` is a
# shell and an 8191-character limit, and a copier carried by a package is a
# host-tool sub-build for one copy. The engine is the one program present
# wherever a build runs, and `mcpp stage --verify content --output <dst> <src>`
# is the copy every `stage_file` edge in build.ninja already performs: it
# creates the destination's parent and writes only when the bytes differ.
# `${mcpp.self}` is how an action names it, in the same substitution family as
# `${mcpp.out_dir}` and `${mcpp.target_file:}`.
#
# No `# requires:` line: the point is that this works on every shard, and the
# Windows one is the shard it exists for. The reverse leg -- under 2026.9.12.4
# the token stays literal in build.ninja and the edge fails -- was run once
# before merge and is recorded in the pull request.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cd app

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("SELF_COPY_OK"); }
EOF

cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    mcpp::action a;
    a.id          = "copy";
    a.role        = "artifact";
    a.description = "the linked program, copied by the engine";
    a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
     .arg("--output").arg("${mcpp.out_dir}/copied/deeper/app-copy")
     .arg("${mcpp.target_file:app}")
     .input("${mcpp.target_file:app}")
     .output("${mcpp.out_dir}/copied/deeper/app-copy")
     .submit();
}
EOF

"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *SELF_COPY_OK* ]] || { echo "FAIL: the program did not run: '$out'"; exit 1; }

NINJA=$(find target -name build.ninja -print -quit)
[ -n "$NINJA" ] || { echo "FAIL: no build.ninja"; exit 1; }

# The token is substituted, never emitted literally.
if grep -q 'mcpp\.self' "$NINJA"; then
    grep -n 'mcpp\.self' "$NINJA" | head -3
    echo "FAIL: \${mcpp.self} reached build.ninja unsubstituted"; exit 1
fi

# The copy exists, in a directory the action never created itself, and is the
# program byte for byte: the only way it got there is the engine's `stage`.
COPY=$(find target -path '*/copied/deeper/app-copy' -print -quit)
[ -n "$COPY" ] || { cat b1.log; echo "FAIL: the copy action produced nothing"; exit 1; }
BIN=$(find target -path '*/bin/app' -print -quit)
[ -n "$BIN" ] || BIN=$(find target -path '*/bin/app.exe' -print -quit)
[ -n "$BIN" ] || { echo "FAIL: no linked program under target/"; exit 1; }
cmp -s "$BIN" "$COPY" || { echo "FAIL: the copy differs from the program"; exit 1; }
echo "ok: the engine copied the program through \${mcpp.self}"

# A no-op rebuild leaves the copy alone: the edge is satisfied by its output,
# and `stage --verify content` would write nothing even if it ran.
before=$(stat -c %Y "$COPY" 2>/dev/null || stat -f %m "$COPY")
sleep 1
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: no-op rebuild failed"; exit 1; }
after=$(stat -c %Y "$COPY" 2>/dev/null || stat -f %m "$COPY")
[ "$before" = "$after" ] || { echo "FAIL: the copy was rewritten on a no-op rebuild ($before -> $after)"; exit 1; }
echo "ok: a no-op rebuild copied nothing"

echo "OK"
