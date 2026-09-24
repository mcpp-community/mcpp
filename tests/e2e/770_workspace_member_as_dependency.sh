#!/usr/bin/env bash
# requires: gcc python3
# 770 -- a workspace member is compiled the same way whichever position it
# holds in the graph, and however it was fetched (#690).
#
#   A. `[workspace.build]` reaches a member exactly once, as the selected root
#      (`-p lib`) and as a sibling's `path` dependency (`-p app`): every
#      workspace word occurs once in the member's C++ and C entries, and
#      precedes the member's own word. Before #690 the sibling position lost
#      `defines` (0 occurrences) and the root position carried `cflags` and
#      `cxxflags` twice.
#   B. A member reached as a dependency resolves its own
#      `x.workspace = true` entries. Before #690 the entry reached resolution
#      with neither version nor path.
#   C. `defines` is a set keyed by macro name: a member that restates an
#      inherited name produces one `-D` word with its own value, and `!NAME`
#      removes an inherited name.
#   D. A member of a git-hosted workspace, consumed through `git`, receives its
#      repository's `[workspace.build]`, as its own checkout does.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

# count <cdb> <file-suffix> <word>: occurrences of <word> in that entry.
count() {
    python3 - "$1" "$2" "$3" <<'EOF'
import json, sys
cdb, suffix, word = sys.argv[1], sys.argv[2], sys.argv[3]
for e in json.load(open(cdb)):
    if e["file"].replace("\\", "/").endswith(suffix):
        print(e["arguments"].count(word))
        sys.exit(0)
print("missing")
EOF
}
# before <cdb> <file-suffix> <a> <b>: the first <a> precedes the first <b>.
before() {
    python3 - "$1" "$2" "$3" "$4" <<'EOF'
import json, sys
cdb, suffix, a, b = sys.argv[1:5]
for e in json.load(open(cdb)):
    if e["file"].replace("\\", "/").endswith(suffix):
        args = e["arguments"]
        print("yes" if a in args and b in args and args.index(a) < args.index(b) else "no")
        sys.exit(0)
print("missing")
EOF
}

# ── A, B, C: one workspace, three members ───────────────────────────────────
mkdir -p "$TMP/ws" && cd "$TMP/ws"
mkdir -p lib app/src util
cat > mcpp.toml <<'EOF'
[workspace]
members = ["lib", "app", "util"]

[workspace.package]
version = "0.1.0"

[workspace.dependencies]
util = { path = "util" }

[workspace.build]
defines  = ["WS_DEF=1", "LEVEL=1", "WS_ONLY=1"]
cflags   = ["-DWS_CFLAG=1"]
cxxflags = ["-DWS_CXXFLAG=1"]
EOF
cat > util/mcpp.toml <<'EOF'
[package]
name = "util"

[targets.util]
kind = "lib"

[build]
sources = ["u.cpp"]
EOF
echo 'int util_v() { return 5; }' > util/u.cpp
cat > lib/mcpp.toml <<'EOF'
[package]
name = "lib"

[dependencies]
util.workspace = true

[targets.lib]
kind = "lib"

[build]
sources  = ["lib.cpp", "c.c"]
defines  = ["MEMBER_DEF=1", "LEVEL=2", "!WS_ONLY"]
cflags   = ["-DMEMBER_CFLAG=1"]
cxxflags = ["-DMEMBER_CXXFLAG=1"]
EOF
cat > lib/lib.cpp <<'EOF'
#if !defined(WS_DEF) || !defined(WS_CXXFLAG) || !defined(MEMBER_DEF)
#error "lib.cpp: a workspace or member definition is missing"
#endif
#if LEVEL != 2
#error "lib.cpp: the member's LEVEL must replace the workspace's"
#endif
#ifdef WS_ONLY
#error "lib.cpp: !WS_ONLY must remove the inherited name"
#endif
int util_v();
extern "C" int lib_c();
int lib_v() { return util_v() + lib_c(); }
EOF
cat > lib/c.c <<'EOF'
#if !defined(WS_DEF) || !defined(WS_CFLAG) || !defined(MEMBER_DEF)
#error "c.c: a workspace or member definition is missing"
#endif
int lib_c(void) { return LEVEL; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name = "app"

[dependencies]
lib = { path = "../lib" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int lib_v();\nint main() { return lib_v() == 7 ? 0 : 1; }\n' > app/src/main.cpp

# Each position is asserted right after its own build, so the root position's
# counts are read even when the sibling position does not build.
for position in lib app; do
    "$MCPP" build -p "$position" > "build-$position.log" 2>&1 \
        || fail "A/B: -p $position" "build-$position.log"
    cdb="$position/compile_commands.json"
    [ -f "$cdb" ] || fail "A: no $cdb"
    for word in -DWS_DEF=1 -DWS_CXXFLAG=1 -DMEMBER_CXXFLAG=1 -DMEMBER_DEF=1 -DLEVEL=2; do
        n=$(count "$cdb" lib/lib.cpp "$word")
        [ "$n" = 1 ] || fail "A: $word occurs $n times in lib.cpp (-p $position)"
    done
    for word in -DWS_DEF=1 -DWS_CFLAG=1 -DMEMBER_CFLAG=1 -DMEMBER_DEF=1 -DLEVEL=2; do
        n=$(count "$cdb" lib/c.c "$word")
        [ "$n" = 1 ] || fail "A: $word occurs $n times in c.c (-p $position)"
    done
    for word in -DLEVEL=1 -DWS_ONLY=1; do
        n=$(count "$cdb" lib/lib.cpp "$word")
        [ "$n" = 0 ] || fail "C: $word occurs $n times in lib.cpp (-p $position)"
    done
    [ "$(before "$cdb" lib/lib.cpp -DWS_CXXFLAG=1 -DMEMBER_CXXFLAG=1)" = yes ] \
        || fail "A: the workspace cxxflags must precede the member's (-p $position)"
    [ "$(before "$cdb" lib/c.c -DWS_CFLAG=1 -DMEMBER_CFLAG=1)" = yes ] \
        || fail "A: the workspace cflags must precede the member's (-p $position)"
done
"$MCPP" run -p app > run.log 2>&1 || fail "A/B: the program did not return 0" run.log
echo "ok: A, B, C -- one definition per name in both positions, sibling workspace dependency resolved"

# ── D: a member of a git-hosted workspace ───────────────────────────────────
mkdir -p "$TMP/repo/glib" && cd "$TMP/repo"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["glib"]

[workspace.package]
version = "0.2.0"

[workspace.build]
cxxflags = ["-DREPO_FLAG=1"]
EOF
cat > glib/mcpp.toml <<'EOF'
[package]
namespace = "probe770"
name = "glib"

[targets.glib]
kind = "lib"

[build]
sources = ["g.cpp"]
EOF
printf '#ifndef REPO_FLAG\n#error "the repository [workspace.build] did not reach its git-consumed member"\n#endif\nint g_v() { return REPO_FLAG; }\n' > glib/g.cpp
git init -q -b main . && git add -A \
    && git -c user.email=e2e@mcpp -c user.name=e2e commit -qm init

mkdir -p "$TMP/consumer/src" && cd "$TMP/consumer"
cat > mcpp.toml <<EOF
[package]
name = "consumer"
version = "0.1.0"

[dependencies]
"probe770.glib" = { git = "file://$TMP/repo", branch = "main" }

[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int g_v();\nint main() { return g_v() == 1 ? 0 : 1; }\n' > src/main.cpp
"$MCPP" build > git.log 2>&1 || fail "D: consumer of a git-hosted member" git.log
n=$(count compile_commands.json glib/g.cpp -DREPO_FLAG=1)
[ "$n" = 1 ] || fail "D: -DREPO_FLAG=1 occurs $n times in the git member's g.cpp"
echo "ok: D -- the git-consumed member compiles as its repository does"

echo "PASS: 770_workspace_member_as_dependency"
