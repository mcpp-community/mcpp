#!/usr/bin/env bash
# requires: gcc unix-shell
# 991 -- a file `[build] ldflags` names by absolute path is an input of the link
# (design 2026-10-30 part 2 §25.3 C1).
#
# The clangd probe linked clangd's main over prebuilt static libraries named in
# `[build] ldflags`. A changed library did not relink the program: the words
# reached the linker but were no input of the link edge, so ninja saw nothing
# out of date, and the probe deleted its image to take a new library.
#
# Criteria: the program runs with the archive's first contents; after the
# archive is replaced, `mcpp build` relinks and the program runs with the new
# contents; a further build with nothing changed links nothing.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The prebuilt archive, made by mcpp from a library project.
mkdir -p "$work/val/src" "$work/prebuilt"
cat > "$work/val/mcpp.toml" <<'TOML'
[package]
name    = "val"
version = "0.1.0"

[targets.val]
kind = "lib"
TOML
make_archive() {  # make_archive <value>
    printf 'extern "C" int val() { return %s; }\n' "$1" > "$work/val/src/val.cpp"
    (cd "$work/val" && "$MCPP" build > "$work/val.log" 2>&1) || { cat "$work/val.log"; echo "FAIL: the library did not build"; exit 1; }
    local a
    a="$(find "$work/val/target" -name 'libval*.a' -type f | head -1)"
    [[ -n "$a" ]] || { find "$work/val/target" -type f | head -40; echo "FAIL: no archive of the library"; exit 1; }
    cp "$a" "$work/prebuilt/libval.a"
}
make_archive 41

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[build]
ldflags = ["$work/prebuilt/libval.a"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
TOML
cat > "$work/app/src/main.cpp" <<'CPP'
import std;
extern "C" int val();
int main() { std::println("val {}", val()); }
CPP
cd "$work/app"

"$MCPP" run > run1.log 2>&1 || { cat run1.log; echo "FAIL: the first run"; exit 1; }
grep -q 'val 41' run1.log || { cat run1.log; echo "FAIL: the first run did not print val 41"; exit 1; }

make_archive 42
"$MCPP" run > run2.log 2>&1 || { cat run2.log; echo "FAIL: the second run"; exit 1; }
grep -q 'val 42' run2.log || {
    cat run2.log
    echo "FAIL: a replaced archive named in [build] ldflags did not relink the program"
    exit 1
}

"$MCPP" build -v > build3.log 2>&1 || { cat build3.log; echo "FAIL: the third build"; exit 1; }
# `-v` prints each step ninja runs (`[1/1] … -o bin/app.tmp`); none may link.
if grep -qE '^\[[0-9]+/[0-9]+\] .*bin/app' build3.log; then
    cat build3.log
    echo "FAIL: a build with nothing changed linked the program again"
    exit 1
fi

echo "OK"
