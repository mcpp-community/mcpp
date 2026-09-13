#!/usr/bin/env bash
# requires: gcc
# 661_one_identity_two_declarations.sh -- two declarations of the SAME
# dependency identity (T3, 2026-09-13-630 record §2.2). Before this, a
# second `git`/`path` declaration of an already-resolved identity carried
# no comparable reference at all -- `ResolvedRecord` held no path and no
# `gitRev` -- so the winner was silently whichever request the FIFO
# worklist happened to dequeue first, and a root/dependency KIND clash
# (`path` vs `git`) was an unconditional refusal even when the root itself
# was a party. Six cases:
#
#   1. root `git rev=A`, a library `git rev=B`         -> root wins, warned
#   2. root `path` (dirty working tree), library `git` -> root wins, warned
#   3. no root declaration, two libraries at B and C    -> first wins, warned
#   4. two libraries, one `path` one `git`              -> refused, "Pick one"
#   5. root pins a checkout below a library's SemVer floor -> refused
#   6. the SAME reference declared twice                -> silent, --strict OK
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() {
    local msg="$1"; shift
    echo "FAIL: $msg"
    for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done
    exit 1
}

# ── one shared git origin for "framework": commits A, B, C, then a dirty
# working tree. A `git clone` of a commit never sees the dirty edit made
# afterwards, so cases 1/3/4/6 (which clone a fixed rev) and case 2 (which
# reads the live directory as a `path` dep) can share this one repository. ──
FW_GIT="$TMP/framework-git"
FW_GIT_HOST="$(host_path "$FW_GIT")"
mkdir -p "$FW_GIT/src"
git init --quiet "$FW_GIT"
git -C "$FW_GIT" config user.email "test@local"
git -C "$FW_GIT" config user.name  "test"
cat > "$FW_GIT/mcpp.toml" <<'EOF'
[package]
name    = "framework"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.framework]
kind = "lib"
EOF

write_marker() {
    printf 'int framework_marker(void) { return %s; }\n' "$1" > "$FW_GIT/src/framework.c"
}

write_marker 101
git -C "$FW_GIT" add -A >/dev/null
git -C "$FW_GIT" commit --quiet -m "A"
REV_A=$(git -C "$FW_GIT" rev-parse HEAD)

write_marker 102
git -C "$FW_GIT" add -A >/dev/null
git -C "$FW_GIT" commit --quiet -m "B"
REV_B=$(git -C "$FW_GIT" rev-parse HEAD)

write_marker 103
git -C "$FW_GIT" add -A >/dev/null
git -C "$FW_GIT" commit --quiet -m "C"
REV_C=$(git -C "$FW_GIT" rev-parse HEAD)

# The dirty edit for case 2 -- never committed, so REV_A/B/C above are
# unaffected by it.
write_marker 199

# A second "framework", pinned by a `path` dep and carrying its own
# `[package] version` -- used by case 5's SemVer-against-checkout check.
FW_V020="$TMP/framework-v020"
mkdir -p "$FW_V020/src"
cat > "$FW_V020/mcpp.toml" <<'EOF'
[package]
name    = "framework"
version = "0.2.0"

[build]
sources = ["src/*.c"]

[targets.framework]
kind = "lib"
EOF
printf 'int framework_marker(void) { return 20; }\n' > "$FW_V020/src/framework.c"

# make_lib DIR NAME EXTRA_MANIFEST_LINES -- a tiny library whose one
# function calls framework_marker() and returns it, so a consumer can
# observe WHICH framework instance the library actually built against.
make_lib() {
    local dir="$1" name="$2" extra="$3"
    mkdir -p "$dir/src"
    {
        printf '[package]\nname    = "%s"\nversion = "0.1.0"\n\n' "$name"
        printf '[build]\nsources = ["src/*.c"]\n\n'
        printf '[targets.%s]\nkind = "lib"\n\n' "$name"
        printf '%s\n' "$extra"
    } > "$dir/mcpp.toml"
    printf 'extern int framework_marker(void);\nint %s_marker(void) { return framework_marker(); }\n' \
        "$name" > "$dir/src/$name.c"
}

# ═══════════════════════════════════════════════════════════════════════
# Case 1: root `git rev=A`, a library `git rev=B` -- the root wins.
# ═══════════════════════════════════════════════════════════════════════
C1="$TMP/case1"
make_lib "$C1/libb" "libb" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_B"
EOF
)"
mkdir -p "$C1/app/src"
cat > "$C1/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_A"

[dependencies.libb]
path = "../libb"
EOF
cat > "$C1/app/src/main.cpp" <<'EOF'
#include <cstdio>
extern "C" int libb_marker(void);
int main() {
    std::printf("libb=%d\n", libb_marker());
    return 0;
}
EOF
(
    cd "$C1/app"
    ec=0
    "$MCPP" run > run.log 2>&1 || ec=$?
    [[ $ec -eq 0 ]] || fail "case 1: exit $ec" run.log
    grep -q "app" run.log || fail "case 1: warning does not name the root" run.log
    grep -q "libb" run.log || fail "case 1: warning does not name the library" run.log
    grep -q "$REV_A" run.log || fail "case 1: warning does not name rev A" run.log
    grep -q "$REV_B" run.log || fail "case 1: warning does not name rev B" run.log
    grep -q "libb=101" run.log || fail "case 1: program did not print A's marker" run.log
    grep -q "$REV_A" mcpp.lock || fail "case 1: mcpp.lock does not record A" mcpp.lock
)
echo "ok: case 1 -- root git rev wins over a library's, warned, locked, built against A"

# ═══════════════════════════════════════════════════════════════════════
# Case 2: root `path` (dirty working tree), a library `git` -- root wins,
# and what the library actually sees is the WORKING TREE, not a commit.
# ═══════════════════════════════════════════════════════════════════════
C2="$TMP/case2"
make_lib "$C2/libg" "libg" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_B"
EOF
)"
mkdir -p "$C2/app/src"
cat > "$C2/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.framework]
path = "$FW_GIT_HOST"

[dependencies.libg]
path = "../libg"
EOF
cat > "$C2/app/src/main.cpp" <<'EOF'
#include <cstdio>
extern "C" int libg_marker(void);
int main() {
    std::printf("libg=%d\n", libg_marker());
    return 0;
}
EOF
(
    cd "$C2/app"
    ec=0
    "$MCPP" run > run.log 2>&1 || ec=$?
    [[ $ec -eq 0 ]] || fail "case 2: exit $ec" run.log
    grep -q "libg" run.log || fail "case 2: warning does not name the library" run.log
    grep -q "libg=199" run.log \
        || fail "case 2: program did not print the working tree's marker" run.log
)
echo "ok: case 2 -- root path wins over a library's git rev, program sees the working tree"

# ═══════════════════════════════════════════════════════════════════════
# Case 3: no root declaration, two libraries at B and C -- the first one
# dequeued (libb, alphabetically before libc) wins, and BOTH edges are
# redirected to it -- libc_marker() must also read B, not C.
# ═══════════════════════════════════════════════════════════════════════
C3="$TMP/case3"
make_lib "$C3/libb" "libb" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_B"
EOF
)"
make_lib "$C3/libc" "libc" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_C"
EOF
)"
mkdir -p "$C3/app/src"
cat > "$C3/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.libb]
path = "../libb"

[dependencies.libc]
path = "../libc"
EOF
cat > "$C3/app/src/main.cpp" <<'EOF'
#include <cstdio>
extern "C" int libb_marker(void);
extern "C" int libc_marker(void);
int main() {
    std::printf("libb=%d libc=%d\n", libb_marker(), libc_marker());
    return 0;
}
EOF
(
    cd "$C3/app"
    ec=0
    "$MCPP" run > run.log 2>&1 || ec=$?
    [[ $ec -eq 0 ]] || fail "case 3: exit $ec" run.log
    grep -q "libb" run.log || fail "case 3: warning does not name libb" run.log
    grep -q "libc" run.log || fail "case 3: warning does not name libc" run.log
    grep -q "libb=102 libc=102" run.log \
        || fail "case 3: both libraries did not converge on the first-resolved framework (B)" run.log
)
echo "ok: case 3 -- with no root opinion, the first-dequeued declaration wins for both edges"

# ═══════════════════════════════════════════════════════════════════════
# Case 4: two libraries, one `path` one `git` -- neither is the root, so
# this is refused exactly as a KIND clash always was, plus the new hint.
# ═══════════════════════════════════════════════════════════════════════
C4="$TMP/case4"
FW_V020_HOST="$(host_path "$FW_V020")"
make_lib "$C4/libd" "libd" "$(cat <<EOF
[dependencies.framework]
path = "$FW_V020_HOST"
EOF
)"
make_lib "$C4/libe" "libe" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_B"
EOF
)"
mkdir -p "$C4/app/src"
cat > "$C4/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.libd]
path = "../libd"

[dependencies.libe]
path = "../libe"
EOF
cat > "$C4/app/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
(
    cd "$C4/app"
    "$MCPP" build > build.log 2>&1 && ec=0 || ec=$?
    [[ $ec -ne 0 ]] || fail "case 4: expected a refusal but the build succeeded" build.log
    grep -q "Pick one" build.log || fail "case 4: refusal message missing 'Pick one'" build.log
    grep -q "in the root to settle it" build.log \
        || fail "case 4: refusal is missing the new root hint" build.log
    grep -q "libd" build.log || fail "case 4: refusal does not name libd" build.log
    grep -q "libe" build.log || fail "case 4: refusal does not name libe" build.log
)
echo "ok: case 4 -- a kind clash between two non-root requesters is still refused, with the hint"

# ═══════════════════════════════════════════════════════════════════════
# Case 5: the root pins a checkout below a library's SemVer floor -- the
# root still wins the reference, but the floor is checked against the
# checkout's OWN `[package] version` and a violation is refused.
# ═══════════════════════════════════════════════════════════════════════
C5="$TMP/case5"
INDEX_DIR="$C5/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/f"
cat > "$INDEX_DIR/pkgs/f/framework.lua" <<'EOF'
package = {
    spec = "1",
    name = "framework",
    description = "SemVer side of the 661 fixture -- never actually fetched",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.5.0"] = {
                url = "https://example.invalid/framework-0.5.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/*.c" },
        targets = { ["framework"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
make_lib "$C5/libv" "libv" "$(cat <<'EOF'
[dependencies]
framework = ">=0.3"
EOF
)"
mkdir -p "$C5/app/src"
cat > "$C5/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[indices]
default = { path = "$INDEX_DIR_HOST" }

[dependencies.framework]
path = "$FW_V020_HOST"

[dependencies.libv]
path = "../libv"
EOF
cat > "$C5/app/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
(
    cd "$C5/app"
    "$MCPP" build > build.log 2>&1 && ec=0 || ec=$?
    [[ $ec -ne 0 ]] || fail "case 5: expected a refusal but the build succeeded" build.log
    grep -q "0.2.0" build.log || fail "case 5: refusal does not name the checkout's version" build.log
    grep -q ">=0.3" build.log || fail "case 5: refusal does not name the requirement" build.log
    grep -q "app" build.log || fail "case 5: refusal does not name the root" build.log
    grep -q "libv" build.log || fail "case 5: refusal does not name the library" build.log
)
echo "ok: case 5 -- a root pin below a library's SemVer floor is refused, naming both"

# ═══════════════════════════════════════════════════════════════════════
# Case 6 (negative): the SAME reference declared twice must warn about
# NOTHING, and must not trip `--strict` -- without this row, a fixture
# that warns unconditionally would still pass every row above.
# ═══════════════════════════════════════════════════════════════════════
C6="$TMP/case6"
make_lib "$C6/libf" "libf" "$(cat <<EOF
[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_A"
EOF
)"
mkdir -p "$C6/app/src"
cat > "$C6/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.framework]
git = "$FW_GIT_HOST"
rev = "$REV_A"

[dependencies.libf]
path = "../libf"
EOF
cat > "$C6/app/src/main.cpp" <<'EOF'
#include <cstdio>
extern "C" int libf_marker(void);
int main() {
    std::printf("libf=%d\n", libf_marker());
    return 0;
}
EOF
(
    cd "$C6/app"
    ec=0
    "$MCPP" build --strict > build.log 2>&1 || ec=$?
    [[ $ec -eq 0 ]] || fail "case 6: --strict refused an identical, repeated declaration" build.log
    # `grep -q ... && fail ...` would make a NON-match (the success path here)
    # the last command's exit status, which -- under `set -e`, outside any
    # conditional -- would abort the whole script silently. `if` is what
    # keeps a passing check from reading as a script failure.
    if grep -q "wins" build.log; then
        fail "case 6: an identical reference declared twice still produced an override warning" build.log
    fi
)
echo "ok: case 6 -- the same reference declared twice is silent, and --strict stays green"

echo "OK"
