#!/usr/bin/env bash
# requires:
# 847 -- a module name is unique within one program, not within one build
# (mcpp#732).
#
# GCC and clang mangle a module's entities with its name and give the module
# one initializer named after it, so two modules of one name cannot be linked
# into one program (`multiple definition of value@common()`), and two programs
# may each have one. A build holds several programs -- a package and the
# programs it ships through `artifacts`, a workspace's members -- and mcpp
# refused a name two packages of one build provided, whichever programs they
# belonged to. An import is now resolved in the importer's closure, the two
# BMIs lie below their packages' directories, and each compile is told which
# one a name means.
#
# Criteria, with the default toolchain (GCC on Linux, clang on macOS and
# Windows):
#   A. An app and its `artifacts` updater each provide a different module `boost`:
#      the build succeeds, each program prints its own module's value, the
#      updater's BMI lies below its package's directory and the app's, the
#      root's, at its name (pack drive and selection design 2026-10-01, B1).
#   B. Editing the updater's `boost` rebuilds the updater and not the app.
#   C. Two independent members of one workspace, each with its own `boost`:
#      `--workspace` builds both, each with its own value.
#   D. The reported layout: one file listed by two packages through `..`, in
#      two programs. Each program has one `boost`, so it builds.
#   E. Two packages that one program links both provide `boost`: refused,
#      naming the program's package.
#   F. One file reached twice within one closure: refused, and named as one
#      file reached as two packages.
#   G. A build whose modules are all the root package's writes no module map
#      and no binding flag: its build directory is laid out as before.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
cd "$TMP"

# module_file <file> <value>: the module `boost`, whose value() is <value>.
# main_file <file>: a main that prints the value of the `boost` it imports.
# (`common`, the name in mcpp#732, is one of the top-level names mcpp's
# naming rule refuses; the reporter's own module is `boost`.)
module_file() { printf 'export module boost;\nexport int value() { return %s; }\n' "$2" > "$1"; }
main_file() {
    printf '#include <cstdio>\nimport boost;\nint main() { std::printf("%%d\\n", value()); }\n' > "$1"
}
bin_of() { find "$1" -path "*/bin/*" -name "$2$EXE" -type f | head -1; }

# ── A ──────────────────────────────────────────────────────────────────────
mkdir -p a/app/src a/updater/src
module_file a/app/src/boost.cppm 1
main_file a/app/src/main.cpp
module_file a/updater/src/boost.cppm 2
main_file a/updater/src/main.cpp
cat > a/updater/mcpp.toml <<'EOF'
[package]
name    = "updater"
version = "0.1.0"

[targets.updater]
kind = "bin"
main = "src/main.cpp"
EOF
cat > a/app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
updater = { path = "../updater", artifacts = ["updater"] }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
(cd a/app && "$MCPP" build > "$TMP/a.log" 2>&1) || fail "A: the build was refused" a.log
app=$(bin_of a/app/target app); upd=$(bin_of a/app/target updater)
[ -n "$app" ] && [ -n "$upd" ] || fail "A: a program is missing" a.log
[ "$("$app" | tr -d '\r')" = 1 ] || fail "A: the app does not print its own module's value"
[ "$("$upd" | tr -d '\r')" = 2 ] || fail "A: the updater does not print its own module's value"
nq=$(find a/app/target -path '*.cache/*updater/boost.*' -type f | wc -l)
nf=$(find a/app/target -path '*.cache/boost.*' -type f | wc -l)
[ "$nq" -eq 1 ] && [ "$nf" -eq 1 ] || { find a/app/target -name 'boost.*'; fail "A: expected the updater's BMI below its directory and the app's at its name, found $nq and $nf"; }
echo "ok: A, an app and its artifacts updater each have their own boost"

# ── B ──────────────────────────────────────────────────────────────────────
touch "$TMP/marker"
sleep 1
module_file a/updater/src/boost.cppm 3
(cd a/app && "$MCPP" build > "$TMP/b.log" 2>&1) || fail "B: the rebuild failed" b.log
[ "$("$upd" | tr -d '\r')" = 3 ] || fail "B: the updater did not take its edited module"
[ "$("$app" | tr -d '\r')" = 1 ] || fail "B: the app changed its value"
[ -z "$(find "$(dirname "$app")" -name "app$EXE" -newer "$TMP/marker")" ] \
    || fail "B: editing the updater's module relinked the app" b.log
echo "ok: B, an edit of one boost rebuilt its own program only"

# ── C ──────────────────────────────────────────────────────────────────────
mkdir -p c/one/src c/two/src
module_file c/one/src/boost.cppm 5; main_file c/one/src/main.cpp
module_file c/two/src/boost.cppm 6; main_file c/two/src/main.cpp
cat > c/mcpp.toml <<'EOF'
[workspace]
members = ["one", "two"]
EOF
for m in one two; do
    cat > c/$m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
done
(cd c && "$MCPP" build --workspace > "$TMP/c.log" 2>&1) || fail "C: the workspace build was refused" c.log
one=$(bin_of c/target one); two=$(bin_of c/target two)
[ -n "$one" ] && [ -n "$two" ] || fail "C: a member's program is missing" c.log
[ "$("$one" | tr -d '\r')" = 5 ] && [ "$("$two" | tr -d '\r')" = 6 ] \
    || fail "C: a member does not print its own module's value"
echo "ok: C, two workspace members each have their own boost"

# ── D ──────────────────────────────────────────────────────────────────────
mkdir -p d/shared d/gui/src d/upd/src
module_file d/shared/boost.cppm 9
main_file d/gui/src/main.cpp; main_file d/upd/src/main.cpp
cat > d/upd/mcpp.toml <<'EOF'
[package]
name    = "upd"
version = "0.1.0"

[build]
sources = ["../shared/boost.cppm"]

[targets.upd]
kind = "bin"
main = "src/main.cpp"
EOF
cat > d/gui/mcpp.toml <<'EOF'
[package]
name    = "gui"
version = "0.1.0"

[dependencies]
upd = { path = "../upd", artifacts = ["upd"] }

[build]
sources = ["../shared/boost.cppm"]

[targets.gui]
kind = "bin"
main = "src/main.cpp"
EOF
(cd d/gui && "$MCPP" build > "$TMP/d.log" 2>&1) || fail "D: one file in two programs was refused" d.log
[ "$("$(bin_of d/gui/target gui)" | tr -d '\r')" = 9 ] && [ "$("$(bin_of d/gui/target upd)" | tr -d '\r')" = 9 ] \
    || fail "D: a program does not run" d.log
echo "ok: D, one file compiled in two programs builds"

# ── E ──────────────────────────────────────────────────────────────────────
mkdir -p e/lib1/src e/lib2/src e/prog/src
module_file e/lib1/src/boost.cppm 1; module_file e/lib2/src/boost.cppm 2
printf '#include <cstdio>\nint main() { std::printf("x\\n"); }\n' > e/prog/src/main.cpp
for l in lib1 lib2; do
    printf '[package]\nname    = "%s"\nversion = "0.1.0"\n' "$l" > e/$l/mcpp.toml
done
cat > e/prog/mcpp.toml <<'EOF'
[package]
name    = "prog"
version = "0.1.0"

[dependencies]
lib1 = { path = "../lib1" }
lib2 = { path = "../lib2" }

[targets.prog]
kind = "bin"
main = "src/main.cpp"
EOF
if (cd e/prog && "$MCPP" build > "$TMP/e.log" 2>&1); then fail "E: two providers in one program were accepted" e.log; fi
grep -q "module 'boost' is provided by package" e.log && grep -q "closure of package 'prog'" e.log \
    || fail "E: the refusal does not name the program's package" e.log
echo "ok: E, two providers in one program are refused"

# ── F ──────────────────────────────────────────────────────────────────────
mkdir -p f/shared f/lib3 f/lib4 f/prog/src
module_file f/shared/boost.cppm 1
printf '#include <cstdio>\nint main() { std::printf("x\\n"); }\n' > f/prog/src/main.cpp
for l in lib3 lib4; do
    printf '[package]\nname    = "%s"\nversion = "0.1.0"\n\n[build]\nsources = ["../shared/boost.cppm"]\n' "$l" > f/$l/mcpp.toml
done
cat > f/prog/mcpp.toml <<'EOF'
[package]
name    = "prog"
version = "0.1.0"

[dependencies]
lib3 = { path = "../lib3" }
lib4 = { path = "../lib4" }

[targets.prog]
kind = "bin"
main = "src/main.cpp"
EOF
if (cd f/prog && "$MCPP" build > "$TMP/f.log" 2>&1); then fail "F: one file twice in one program was accepted" f.log; fi
grep -q "one file is reached as two packages" f.log \
    || fail "F: the refusal does not say that one file is reached as two packages" f.log
echo "ok: F, one file reached twice in one program is refused as such"

# ── G ──────────────────────────────────────────────────────────────────────
(cd a/updater && "$MCPP" build > "$TMP/g.log" 2>&1) || fail "G: a plain build failed" g.log
[ -z "$(find a/updater/target -type d -name modmap)" ] || fail "G: a plan of the root's own modules wrote a module map"
ninja=$(find a/updater/target -name build.ninja | head -1)
[ -n "$ninja" ] || fail "G: no build.ninja"
if grep -q 'module-map\|fmodule-mapper\|cache/[^ ]*/boost\.' "$ninja"; then
    fail "G: a plan of the root's own modules binds a module name" "$ninja"
fi
echo "ok: G, a plan of the root's own modules is laid out as before"

echo "PASS: 847_a_module_name_is_unique_within_a_program"
