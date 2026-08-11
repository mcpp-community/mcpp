#!/usr/bin/env bash
# requires: gcc
# 217_module_extensions.sh — `[build] module_extensions`: a project declares
# which extensions its module INTERFACES use, and mcpp treats them as such
# everywhere.
#
# What each part protects against:
#
#  * opt-in       — `.ccm`/`.cxxm`/`.ixx` are NOT built in. If they were, the
#                   default source glob would widen with them and a published
#                   package with a vendored MSVC-only `.ixx` under src/ would
#                   start compiling it on the next mcpp upgrade — a break its
#                   author cannot fix, because that version's tarball shipped.
#                   Part 1 pins the un-configured behaviour.
#  * rule routing — mcpp#272 fixed link-object collection while `pick_rule`
#                   stayed keyed on the extension, so a `.ixx` was routed to
#                   `cxx_object`: the edge still DECLARED a BMI output (that
#                   line reads providesModule) while the command line lost
#                   `-fmodule-output=`. GCC's gcm.cache made it look fine.
#  * clang leg    — and that is why this test must not be GCC-only. Clang does
#                   not recognize `.ixx` at all: it hands the file to the
#                   LINKER, warns, and exits 0 having produced no BMI. A
#                   GCC-only test would report green for a build that cannot
#                   work anywhere else.
#  * object names — giving all four module extensions the `.m` prefix (as
#                   mcpp#272 proposed) makes `foo.cppm` and `foo.ccm` both
#                   `foo.m.o`. Part 2 pins one object per source.
set -e

# Resolve before cd'ing: $0 is relative and every leg runs from $TMP.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fixture() {   # $1 = dir, $2 = extra mcpp.toml lines
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "extfix"
version = "0.1.0"
$2
EOF
    printf 'export module extfix.a;\nimport std;\nexport auto a() -> void { std::println("from .cppm"); }\n' > "$1/src/a.cppm"
    printf 'export module extfix.b;\nimport std;\nexport auto b() -> void { std::println("from .ccm"); }\n'  > "$1/src/b.ccm"
    printf 'export module extfix.c;\nimport std;\nexport auto c() -> void { std::println("from .cxxm"); }\n' > "$1/src/c.cxxm"
    printf 'export module extfix.d;\nimport std;\nexport auto d() -> void { std::println("from .ixx"); }\n'  > "$1/src/d.ixx"
}

# ── Part 1: NOT configured ⇒ only .cppm is a module interface ──────────────
#
# Reverse assertion. Without it, someone "simplifying" the built-in table into
# all four extensions would make every test above pass and break every
# published package that ships a stray .ixx.
fixture nocfg ""
printf 'import std;\nimport extfix.a;\nint main(){ a(); std::println("NOCFG OK"); }\n' > nocfg/src/main.cpp
cd nocfg
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: default build broke"; exit 1; }
grep -q 'sources \[src/\*\*/\*\.{cppm,cpp,cc,c,S,s,asm}\]' b.log || {
    cat b.log; echo "FAIL: default source glob changed"; exit 1; }
nj="$(find target -name build.ninja | head -1)"
for stray in b.ccm c.cxxm d.ixx; do
    grep -q "$stray" "$nj" && { echo "FAIL: $stray compiled without opt-in"; exit 1; }
done
out="$("$MCPP" run 2>&1)"
[[ "$out" == *"NOCFG OK"* ]] || { echo "FAIL: $out"; exit 1; }
echo "  ok: un-configured build ignores .ccm/.cxxm/.ixx"
cd "$TMP"

# ── Part 2: configured ⇒ all four build, link and RUN ──────────────────────
run_leg() {   # $1 = leg name, $2 = extra [toolchain] lines
    local dir="leg_$1"
    fixture "$dir" "
[build]
module_extensions = [\".ccm\", \".cxxm\", \".ixx\"]
$2"
    printf 'import std;\nimport extfix.a;\nimport extfix.b;\nimport extfix.c;\nimport extfix.d;\nint main(){ a(); b(); c(); d(); std::println("ALL OK"); }\n' > "$dir/src/main.cpp"
    cd "$dir"

    "$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL[$1]: build"; exit 1; }

    # The declared extensions must reach the default glob, or the key would
    # change how files are TREATED without changing whether they are FOUND.
    grep -q 'sources \[src/\*\*/\*\.{cppm,ccm,cxxm,ixx,cpp,cc,c,S,s,asm}\]' b.log || {
        cat b.log; echo "FAIL[$1]: declared extensions missing from default glob"; exit 1; }

    out="$("$MCPP" run 2>&1)"
    for want in "from .cppm" "from .ccm" "from .cxxm" "from .ixx" "ALL OK"; do
        [[ "$out" == *"$want"* ]] || { echo "FAIL[$1]: missing '$want' in: $out"; exit 1; }
    done

    local nj; nj="$(find target -name build.ninja | head -1)"

    # Every module interface must use the MODULE rule. Asserting on the rule —
    # not on the BMI path — is the point: the BMI output line is emitted from
    # providesModule and stayed correct while the rule was wrong.
    for src in a.cppm b.ccm c.cxxm d.ixx; do
        grep -qE "^build [^:]*: cxx_module .*${src}\$|^build [^:]*: cxx_module .*${src} " "$nj" \
          || grep -q ": cxx_module .*${src}" "$nj" \
          || { echo "FAIL[$1]: $src did not use the cxx_module rule"; exit 1; }
    done

    # One object per source, no two alike.
    local objs
    objs="$(grep -oE 'obj/[A-Za-z0-9_.]+\.o' "$nj" | sort -u)"
    for want in "obj/a.m.o" "obj/b.ccm.o" "obj/c.cxxm.o" "obj/d.ixx.o"; do
        grep -qx "$want" <<< "$objs" || {
            echo "FAIL[$1]: expected object $want; got:"; echo "$objs"; exit 1; }
    done

    echo "  ok[$1]: four extensions build, link and run"
    cd "$TMP"
}

run_leg gcc ""

# ── Part 3: the same, on Clang ─────────────────────────────────────────────
#
# Not optional coverage. `.ixx` is the extension Clang does not know, so this
# leg is the one that would have caught the pick_rule defect. Skipped only
# when no LLVM payload is installed.
source "$SCRIPT_DIR/_llvm_env.sh"
if [[ -d "$LLVM_ROOT" ]]; then
    run_leg "clang" "
[toolchain]
default = \"llvm@${LLVM_VERSION}\""
else
    echo "  skip: no LLVM payload installed — clang leg not run"
fi

echo "OK"
