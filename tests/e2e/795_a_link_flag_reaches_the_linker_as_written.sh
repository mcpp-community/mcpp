#!/usr/bin/env bash
# requires: elf unix-shell
# mcpp#703: a link flag reaches the linker as written.
#
# SPEC-004 §8 reads an element of a flag list into words, and each word
# reaches the tool verbatim whatever the host's command-line reader. Until
# 2026.9.26.2 that reading covered the compile flags only. A link-flag element
# was escaped for ninja and not quoted for the shell, so the `sh` that runs a
# POSIX link expanded a `$ORIGIN` the author wrote, and the program's run path
# held `/../lib`, which is the host's `/lib`.
#
# Five legs:
#   A. `[build] ldflags`: `$ORIGIN` reaches the program's run path.
#   B. a build program's `mcpp::link_flag`: the same.
#   C. a dependency's `ldflags`, propagated to the consumer: the same.
#   D. `mcpp::link_search` with a directory whose path holds a space: one
#      argument. Before, the shell split it, and the linker read the second
#      half as an input file.
#   E. an element escaped for ninja and the shell by hand is read as written,
#      and the first plan says what the linker receives now.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { [ -n "$2" ] && cat "$2"; echo "FAIL: $1"; exit 1; }

program() { find target -path '*/bin/*' -name "$1" -type f | head -1; }

# The run-path entries of a program, one per line.
run_path() {
    readelf -d "$1" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p' | tr ':' '\n'
}

# The run path names `$ORIGIN/<rest>` and no entry that ends in `/../<rest>`
# without it.
expect_origin() {
    local bin="$1" rest="$2" log="$3" entries
    entries="$(run_path "$bin")"
    printf '%s\n' "$entries" | grep -qxF "\$ORIGIN/$rest" \
        || { printf 'run path:\n%s\n' "$entries"; fail "\$ORIGIN/$rest is not in the run path" "$log"; }
    if printf '%s\n' "$entries" | grep -qxF "/$rest"; then
        printf 'run path:\n%s\n' "$entries"
        fail "the run path holds /$rest: \$ORIGIN was expanded by the shell" "$log"
    fi
}

write_main() {
    mkdir -p "$1/src"
    cat > "$1/src/main.cpp" <<'CPP'
#include <cstdio>
int main() { std::printf("LINK_FLAG_OK\n"); }
CPP
}

# ── A. [build] ldflags ──────────────────────────────────────────────────────
mkdir -p "$work/a"
write_main "$work/a"
cat > "$work/a/mcpp.toml" <<'TOML'
[package]
name    = "ldorigin"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,$ORIGIN/../lib"]
TOML
cd "$work/a"
"$MCPP" build > a.log 2>&1 || fail "the build failed" a.log
bin="$(program ldorigin)"
[ -n "$bin" ] || fail "no program was produced" a.log
expect_origin "$bin" "../lib" a.log
echo "  ok: \$ORIGIN in [build] ldflags reaches the run path"

# ── B. mcpp::link_flag ──────────────────────────────────────────────────────
mkdir -p "$work/b"
write_main "$work/b"
cat > "$work/b/mcpp.toml" <<'TOML'
[package]
name    = "linkflagorigin"
version = "0.1.0"
TOML
cat > "$work/b/build.mcpp" <<'CPP'
import mcpp;
int main() {
    mcpp::link_flag("-Wl,-rpath,$ORIGIN/../plugins");
}
CPP
cd "$work/b"
"$MCPP" build > b.log 2>&1 || fail "the build failed" b.log
bin="$(program linkflagorigin)"
[ -n "$bin" ] || fail "no program was produced" b.log
expect_origin "$bin" "../plugins" b.log
echo "  ok: \$ORIGIN in mcpp::link_flag reaches the run path"

# ── C. a dependency's ldflags ───────────────────────────────────────────────
mkdir -p "$work/c/dep/src" "$work/c/app"
cat > "$work/c/dep/mcpp.toml" <<'TOML'
[package]
name    = "originlib"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,$ORIGIN/../dep"]

[targets.originlib]
kind = "lib"
TOML
cat > "$work/c/dep/src/originlib.cppm" <<'CPP'
export module originlib;
export int originlib_value() { return 7; }
CPP
write_main "$work/c/app"
cat > "$work/c/app/src/main.cpp" <<'CPP'
#include <cstdio>
import originlib;
int main() { std::printf("LINK_FLAG_OK %d\n", originlib_value()); }
CPP
cat > "$work/c/app/mcpp.toml" <<'TOML'
[package]
name    = "originapp"
version = "0.1.0"

[dependencies]
originlib = { path = "../dep" }
TOML
cd "$work/c/app"
"$MCPP" build > c.log 2>&1 || fail "the build failed" c.log
bin="$(program originapp)"
[ -n "$bin" ] || fail "no program was produced" c.log
expect_origin "$bin" "../dep" c.log
echo "  ok: a dependency's \$ORIGIN reaches the consumer's run path"

# ── D. a search directory with a space ──────────────────────────────────────
mkdir -p "$work/d/my libs"
write_main "$work/d"
cat > "$work/d/mcpp.toml" <<'TOML'
[package]
name    = "spacedsearch"
version = "0.1.0"
TOML
cat > "$work/d/build.mcpp" <<'CPP'
import mcpp;
int main() {
    mcpp::link_search("my libs");
}
CPP
cd "$work/d"
"$MCPP" build > d.log 2>&1 \
    || fail "a search directory whose path holds a space broke the link" d.log
out="$("$MCPP" run 2>&1)" || fail "the program did not run: $out"
printf '%s\n' "$out" | grep -q LINK_FLAG_OK || fail "unexpected output: $out"
echo "  ok: a search directory whose path holds a space is one argument"

# ── E. an element escaped by hand ───────────────────────────────────────────
mkdir -p "$work/e"
write_main "$work/e"
cat > "$work/e/mcpp.toml" <<'TOML'
[package]
name    = "handescaped"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath='$$ORIGIN/../x'"]
TOML
cd "$work/e"
"$MCPP" build > e.log 2>&1 || fail "the build failed" e.log
grep -q "reaches the linker as \['-Wl,-rpath=\$\$ORIGIN/../x'\]" e.log \
    || fail "the first plan does not say what the linker receives now" e.log
"$MCPP" build > e2.log 2>&1 || fail "the second build failed" e2.log
if grep -q 'reaches the linker' e2.log; then
    fail "a build that repeats the plan repeated the note" e2.log
fi
echo "  ok: an element escaped by hand reads as written, and the first plan says so"

echo "PASS: a link flag reaches the linker as written"
