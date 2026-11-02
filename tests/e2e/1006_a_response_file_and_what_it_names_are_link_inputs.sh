#!/usr/bin/env bash
# requires: gcc unix-shell
# 1006 -- a response file a link flag names (`@<file>`), and every file it
# names, are inputs of the link.
#
# clangd's ok image lists the archives a `prepare` action makes in one
# response file, named by the build program (`link_flag("@<file>")`). When the
# action rebuilt an archive the response file stayed as it was, and the link,
# which depended on neither, stayed up to date over the new archive.
#
# Criteria: the program runs with the archive's first contents; after the
# archive the response file names is replaced (the response file unchanged),
# `mcpp build` relinks and the program runs with the new contents; a further
# build with nothing changed links nothing; a response file that names the
# archive in quotes is read the same way.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

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
printf '"%s"\n' "$work/prebuilt/libval.a" > "$work/prebuilt/libs.rsp"

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[build]
ldflags = ["@$work/prebuilt/libs.rsp"]

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
    echo "FAIL: a replaced archive a response file names did not relink the program"
    exit 1
}

"$MCPP" build -v > build3.log 2>&1 || { cat build3.log; echo "FAIL: the third build"; exit 1; }
if grep -qE '^\[[0-9]+/[0-9]+\] .*bin/app' build3.log; then
    cat build3.log
    echo "FAIL: a build with nothing changed linked the program again"
    exit 1
fi

echo "OK"
