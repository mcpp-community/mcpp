#!/usr/bin/env bash
# requires: elf gcc
# mcpp#693, the POSIX half. build.ninja and compile_commands.json are UTF-8
# text, and a Linux file name is bytes that need not be UTF-8. Before the fix a
# project directory named in Latin-1 failed every build with
#
#     error: internal: unhandled exception: [json.exception.type_error.316]
#            invalid UTF-8 byte at index 123: 0x2F
#
# Four entry points, four answers:
#   1. a project directory with no UTF-8 spelling is refused before anything is
#      built, and the refusal names it with escapes;
#   2. a file with no UTF-8 spelling inside a project is skipped and reported by
#      its nearest nameable ancestor, and the rest of the project builds;
#   3. a build.mcpp directive whose text is not UTF-8 is refused by its key;
#   4. an MCPP_HOME with no UTF-8 spelling is refused before anything is written
#      into it.
# `elf` stands for Linux here: macOS refuses such a name at creation.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
BAD=$(printf 'caf\xe9')    # Latin-1 for U+00E9; not UTF-8

fail() { [ -n "$2" ] && cat "$2"; echo "FAIL: $1"; exit 1; }

# ── 1. the project directory ────────────────────────────────────────────────
mkdir -p "$TMP/$BAD/src"
cat > "$TMP/$BAD/mcpp.toml" <<'EOF'
[package]
name    = "latin1dir"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > "$TMP/$BAD/src/main.cpp"
cd "$TMP/$BAD"
if "$MCPP" build > "$TMP/b1.log" 2>&1; then
    fail "a project directory with no UTF-8 spelling built" "$TMP/b1.log"
fi
grep -q 'internal: unhandled exception' "$TMP/b1.log" \
    && fail "the directory still reaches a JSON writer" "$TMP/b1.log"
grep -q 'has no UTF-8 spelling' "$TMP/b1.log" \
    || fail "the refusal does not say what is wrong" "$TMP/b1.log"
grep -qF 'caf\xE9' "$TMP/b1.log" \
    || fail "the refusal does not name the directory" "$TMP/b1.log"
echo "  ok: the project directory is refused, by name"

# ── 2. a file inside an ordinary project ────────────────────────────────────
mkdir -p "$TMP/ok/src"
cat > "$TMP/ok/mcpp.toml" <<'EOF'
[package]
name    = "latin1file"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > "$TMP/ok/src/main.cpp"
# Matched by the default source glob, and never compiled: it defines main too.
printf 'int main() { return 7; }\n' > "$TMP/ok/src/$BAD.cpp"
cd "$TMP/ok"
"$MCPP" build > "$TMP/b2.log" 2>&1 \
    || fail "a project holding such a name must still build" "$TMP/b2.log"
grep -q 'contains names that have no UTF-8 spelling' "$TMP/b2.log" \
    || fail "the skipped file is not reported" "$TMP/b2.log"
grep -qF "'$TMP/ok/src'" "$TMP/b2.log" \
    || fail "the report does not name the nearest nameable directory" "$TMP/b2.log"
"$MCPP" run > "$TMP/r2.log" 2>&1 || fail "the program did not run" "$TMP/r2.log"
cdb=$(find "$TMP/ok" -name compile_commands.json | head -1)
[ -n "$cdb" ] || fail "no compile_commands.json was written" "$TMP/b2.log"
grep -q 'main.cpp' "$cdb" || fail "the compile database lost the ordinary source" "$cdb"
echo "  ok: the file is skipped and reported, and the rest builds and runs"

# ── 3. a build.mcpp directive ───────────────────────────────────────────────
mkdir -p "$TMP/bp/src"
cat > "$TMP/bp/mcpp.toml" <<'EOF'
[package]
name    = "latin1directive"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > "$TMP/bp/src/main.cpp"
cat > "$TMP/bp/build.mcpp" <<'EOF'
#include <cstdio>
int main() {
    std::printf("mcpp:cfg=FROM_BUILD_PROGRAM\n");
    std::printf("mcpp:include-dir=inc/caf\xE9\n");
    return 0;
}
EOF
cd "$TMP/bp"
if "$MCPP" build > "$TMP/b3.log" 2>&1; then
    fail "a directive with no UTF-8 spelling was applied" "$TMP/b3.log"
fi
grep -q 'internal: unhandled exception' "$TMP/b3.log" \
    && fail "the directive still reaches a JSON writer" "$TMP/b3.log"
grep -q 'whose text is not UTF-8: mcpp:include-dir' "$TMP/b3.log" \
    || fail "the refusal does not name the directive" "$TMP/b3.log"
echo "  ok: the directive is refused by its key"

# ── 4. the mcpp home ────────────────────────────────────────────────────────
cd "$TMP/ok"
if MCPP_HOME="$TMP/home-$BAD" "$MCPP" build > "$TMP/b4.log" 2>&1; then
    fail "an MCPP_HOME with no UTF-8 spelling was used" "$TMP/b4.log"
fi
grep -q 'the mcpp home .* has no UTF-8 spelling' "$TMP/b4.log" \
    || fail "the refusal does not name the home" "$TMP/b4.log"
[ ! -e "$TMP/home-$BAD/registry" ] \
    || fail "the refused home was written into" "$TMP/b4.log"
echo "  ok: the home is refused before anything is written into it"

echo "PASS: every path with no UTF-8 spelling is named where it enters"
