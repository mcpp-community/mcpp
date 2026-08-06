#!/usr/bin/env bash
# requires: gcc
# 194_build_mcpp_glob_input.sh — #359: an input that is a SET of files.
#
# `build.mcpp`'s re-run key was built from declared FILES and environment
# variables only, both fingerprinted by content. A program that globs its
# inputs therefore could not express "re-run me when a file appears": adding a
# .proto changed no declared file's hash, so the program did not re-run and the
# new file was silently never generated. Measured before the fix:
# `Finished dev in 0.01s`, zero artifacts. That is worse than requiring the
# author to list names, which is why grpc-m chose the explicit list.
#
# Two layers have to agree, and the second is easy to miss: the build.mcpp
# cache AND the project-level fast path, which skips prepare entirely when no
# source is newer than build.ninja. A new .proto moves no existing mtime, so
# the fast path is exactly where this silently did nothing.
#
# Covered here:
#   1. adding a matching file re-runs the program and changes the artifact
#   2. removing one re-runs it too (the set shrank)
#   3. editing a matched file's CONTENT does not re-run on the glob's account
#      — content is what an ordinary rerun-if-changed entry is for
#   4. the program's own outputs live inside the project and must never be part
#      of the set, or the build would re-run forever
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src app/inputs
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int count_inputs();
int main() { std::printf("COUNT=%d\n", count_inputs()); }
EOF
# The program watches a directory of ".in" files and generates one function
# returning how many it found. It never names them: that is the point.
cat > app/build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    namespace fs = std::filesystem;
    mcpp::rerun_if_changed_glob("inputs/**/*.in");
    std::string root = mcpp::manifest_dir();
    int n = 0;
    std::error_code ec;
    for (auto const& e : fs::recursive_directory_iterator(root + "/inputs", ec))
        if (e.path().extension() == ".in") ++n;
    std::string out = std::string(mcpp::out_dir()) + "/count.cpp";
    { std::ofstream os(out); os << "int count_inputs() { return " << n << "; }\n"; }
    // Written into the project tree on purpose: a real rule does this, and it
    // is what would make a naive glob re-run forever.
    { std::ofstream os(root + "/marker.txt"); os << n; }
    mcpp::generated(out.c_str());
    return 0;
}
EOF

cd app
# Logs live OUTSIDE the project: the wide-glob case below asserts that the
# build's own outputs do not perturb the set, and a redirect creating b6.log
# inside the tree would perturb it for real — the test would then be checking
# the shell, not mcpp.
LOGS="$TMP/logs"; mkdir -p "$LOGS"
printf 'a\n' > inputs/a.in

ran()    { grep -q "build.mcpp running" "$1"; }
cached() { grep -q "build.mcpp.*cached" "$1"; }

"$MCPP" build > "$LOGS/b1.log" 2>&1 || { cat "$LOGS/b1.log"; echo "FAIL: first build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^COUNT=' | tail -1)"
[[ "$out" == "COUNT=1" ]] || { echo "FAIL: expected COUNT=1, got '$out'"; exit 1; }

# ── 1. a new matching file re-runs the program ──────────────────────────────
printf 'b\n' > inputs/b.in
"$MCPP" build > "$LOGS/b2.log" 2>&1 || { cat "$LOGS/b2.log"; echo "FAIL: build after adding a file failed"; exit 1; }
ran "$LOGS/b2.log" || {
    cat "$LOGS/b2.log"
    echo "FAIL: adding a matching file did not re-run build.mcpp"
    exit 1
}
out="$("$MCPP" run 2>&1 | grep '^COUNT=' | tail -1)"
[[ "$out" == "COUNT=2" ]] || { echo "FAIL: expected COUNT=2, got '$out'"; exit 1; }

# ── 2. removing one re-runs it as well ─────────────────────────────────────
rm inputs/b.in
"$MCPP" build > "$LOGS/b3.log" 2>&1 || { cat "$LOGS/b3.log"; echo "FAIL: build after removing a file failed"; exit 1; }
ran "$LOGS/b3.log" || { cat "$LOGS/b3.log"; echo "FAIL: removing a matching file did not re-run build.mcpp"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^COUNT=' | tail -1)"
[[ "$out" == "COUNT=1" ]] || { echo "FAIL: expected COUNT=1 after removal, got '$out'"; exit 1; }

# ── 3. editing content does not re-run on the glob's account ───────────────
# The set is unchanged, so the glob has nothing to say. (A program that cares
# about contents declares the file with rerun_if_changed, which hashes them.)
printf 'a totally different body\n' > inputs/a.in
"$MCPP" build > "$LOGS/b4.log" 2>&1 || { cat "$LOGS/b4.log"; echo "FAIL: build after an edit failed"; exit 1; }
ran "$LOGS/b4.log" && {
    cat "$LOGS/b4.log"
    echo "FAIL: editing a matched file's content re-ran the program via the glob"
    exit 1
}

# ── 4. no re-run loop from the program's own outputs ───────────────────────
# marker.txt is written into the project by the program itself, and target/
# holds everything it generated. With a deliberately maximal pattern, a second
# build with nothing changed must still be a no-op; if the output tree were
# part of the set it never would be.
sed 's|"inputs/\*\*/\*\.in"|"**"|' build.mcpp > build.mcpp.new
mv build.mcpp.new build.mcpp
"$MCPP" build > "$LOGS/b5.log" 2>&1 || { cat "$LOGS/b5.log"; echo "FAIL: wide-glob build failed"; exit 1; }
ran "$LOGS/b5.log" || { cat "$LOGS/b5.log"; echo "FAIL: the edited program did not run"; exit 1; }
"$MCPP" build > "$LOGS/b6.log" 2>&1 || { cat "$LOGS/b6.log"; echo "FAIL: second wide-glob build failed"; exit 1; }
ran "$LOGS/b6.log" && {
    cat "$LOGS/b6.log"
    echo "FAIL: a wide glob re-runs forever — the build output tree is in its set"
    exit 1
}

echo "PASS: 194_build_mcpp_glob_input"
