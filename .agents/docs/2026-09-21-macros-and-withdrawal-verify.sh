#!/usr/bin/env bash
# Ecosystem verification for the 2026.9.21.2 wave against the PUBLISHED mcpp
# and index, run inside a SubOS sandbox with CN mirrors for xlings and mcpp.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-21-macros-and-withdrawal-verify.sh)
#   xlings subos use v920 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.21.2 bash /tmp/v.sh"
#
# RUN IT AGAINST THE PREVIOUS RELEASE FIRST (MCPP_VERIFY_VERSION=2026.9.21.1):
# every CHANGE section must FAIL there and pass here, and every GUARD section
# must pass on both. A CHANGE section green on both measured nothing.
#
# The sandbox's $HOME persists between runs of one SubOS, so each section
# clears its own directory. A section that cannot run says so and is listed
# again at the end: a run reporting only failures cannot be told from one that
# examined nothing.
#
# TWO RUNS AND THE READING FROM EACH (host dry run, 2026-09-21, the older one
# against the genuine published archive rather than a local build):
#
#   mcpp 2026.9.21.1 (published)   fails=2
#     B  the engine does not define the upper-case target macro
#     C  an unanswered requirement produced no note
#   mcpp 2026.9.21.2               fails=0
#
# C's SECOND LEG PASSES ON BOTH, and that is the point of having it: it asserts
# the note is ABSENT when the provider does state its list, so without it the
# first leg would pass against an engine that printed the line unconditionally.
# A negative control is not a hole in a CHANGE section.
#
# D and F needed `openkal-llvm-runtime@0.14.0`, registered after that dry run,
# and both reported NOT RUN rather than passing.
#
# THE 2026.9.21.3 WAVE, MEASURED IN SubOS `v920` WITH THE CN MIRROR, AGAINST
# THE PUBLISHED ARTEFACTS ON BOTH LEGS:
#
#   mcpp 2026.9.21.2 (published)   fails=2
#     G  the aarch64-macos build does not complete --- `-fno-builtin-
#        memset_pattern16` is accepted and ignored, so the link stops at
#        `undefined symbol: memset_pattern16`
#     H  `--no-run` does not exist
#   mcpp 2026.9.21.3 (published)   fails=0, nothing skipped
#
# B, C and D pass on BOTH, and that is correct rather than a hole: they are the
# previous wave's changes, and this file keeps them as guards once their own
# release has shipped. Only G and H are CHANGE sections for this one.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }
unset XLINGS_ACTIVE_SUBOS

root="$HOME/verify-9212"
rm -rf "$root"; mkdir -p "$root"

section "A. identity and mirror"
if [ ! -x "$STORE" ]; then
    skip "mcpp $VER is not in the store at $STORE"
    printf '\n-- summary --\nfails=%d\nnot run:%s\n' "$fails" "${skipped:-  (none)}"
    exit 1
fi
got="$("$STORE" --version 2>&1 | head -1)"
case "$got" in
    *"$VER"*) ok "mcpp $VER from $STORE" ;;
    *)        fail "the binary at $STORE reports '$got'" ;;
esac
"$STORE" self config --mirror CN >/dev/null 2>&1 \
  && ok "mcpp mirror set to CN" || fail "mcpp self config --mirror CN"

# ── CHANGE 1. The owned macros are spelt in upper case ──────────────────────
#
# BOTH DIRECTIONS IN ONE TRANSLATION UNIT, because either alone passes for the
# wrong reason: an engine defining NEITHER spelling satisfies "the lower-case
# one is gone", and one defining BOTH satisfies "the upper-case one is here".
section "B. __MCPP_TARGET_<OS>__ replaces __mcpp_target_<os>__ (CHANGE)"
b="$root/b"; rm -rf "$b"; mkdir -p "$b/src"
cat > "$b/src/main.c" <<'EOF'
#if !defined(__MCPP_TARGET_LINUX__)
#error "__MCPP_TARGET_LINUX__ is not defined"
#endif
#if defined(__mcpp_target_linux__)
#error "the lower-case spelling is still defined"
#endif
int main(void) { return 0; }
EOF
cat > "$b/mcpp.toml" <<'EOF'
[package]
name    = "macro-probe"
version = "0.1.0"

[targets.macro-probe]
kind = "bin"
main = "src/main.c"
EOF
if (cd "$b" && "$STORE" build >/dev/null 2>&1); then
    ok "the upper-case target macro is defined and the lower-case one is not"
else
    out=$(cd "$b" && "$STORE" build 2>&1)
    case "$out" in
      *"__MCPP_TARGET_LINUX__ is not defined"*)
        fail "the engine does not define the upper-case target macro" ;;
      *"lower-case spelling is still defined"*)
        fail "the engine still defines the lower-case target macro" ;;
      *) fail "the probe did not build, and for neither of the two reasons" ;;
    esac
fi

# THE SPELLING IS DERIVED, NOT ENUMERATED. A second target with a different
# `os` says the engine reads the triple rather than carrying a table of names.
b2="$root/b2"; rm -rf "$b2"; mkdir -p "$b2/src"
cat > "$b2/src/main.c" <<'EOF'
#if !defined(__MCPP_TARGET_NONE__)
#error "__MCPP_TARGET_NONE__ is not defined for a freestanding target"
#endif
void _start(void) {}
EOF
cat > "$b2/mcpp.toml" <<'EOF'
[package]
name    = "macro-probe-bare"
version = "0.1.0"

[targets.macro-probe-bare]
kind = "bin"
main = "src/main.c"

[build]
ldflags = ["-nostdlib", "-nostartfiles", "-static"]
EOF
if (cd "$b2" && "$STORE" build --target riscv64-none-elf >/dev/null 2>&1); then
    ok "a freestanding target spells its own macro from the triple"
else
    skip "the freestanding toolchain did not resolve in this sandbox"
fi

# ── CHANGE 2. A requirement nobody answered is named ────────────────────────
#
# Three situations exist and two build. Without this line the first and the
# third produce identical output, so a consumer cannot tell "checked and
# agreed" from "never asked". Both legs, because a note printed
# unconditionally would satisfy the first one alone.
section "C. an unanswered requirement is named (CHANGE)"
c="$root/c"; rm -rf "$c"; mkdir -p "$c/impl/src" "$c/src"
printf 'int fake_kernel_marker(void){return 0;}\n' > "$c/impl/src/lib.c"
printf 'int main(void){return 0;}\n'               > "$c/src/main.c"
mk_impl() {   # $1 = provides-interfaces body, or empty
  if [ -z "$1" ]; then
    cat > "$c/impl/mcpp.toml" <<'EOF'
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]
EOF
  else
    cat > "$c/impl/mcpp.toml" <<EOF
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]

[kernel-abi]
provides-interfaces = [$1]
EOF
  fi
}
cat > "$c/mcpp.toml" <<'EOF'
[package]
name    = "iface-probe"
version = "0.1.0"

[dependencies]
fakekernel = { path = "impl" }

[build]
allow_host_libs = true

[kernel-abi]
requires-interfaces = ["openkal.fs", "openkal.net"]
EOF
mk_impl ''
out=$(cd "$c" && "$STORE" build 2>&1)
if [ $? -ne 0 ]; then
    fail "a provider that states nothing must not be refused"
else
    case "$out" in
      *"kernel-abi interfaces"*"states none"*"2 requirements unchecked"*)
        ok "the note names the implementation and how many went unchecked" ;;
      *"kernel-abi interfaces"*)
        fail "the note is printed but does not carry the count" ;;
      *) fail "an unanswered requirement produced no note" ;;
    esac
fi
rm -rf "$c/target"
mk_impl '"openkal.fs", "openkal.net", "openkal.abort"'
out=$(cd "$c" && "$STORE" build 2>&1)
if [ $? -ne 0 ]; then
    fail "a graph whose provider states every requirement must build"
else
    case "$out" in
      *"kernel-abi interfaces"*)
        fail "the note appeared for a graph in which everything WAS checked" ;;
      *) ok "a provider that states its list draws no note" ;;
    esac
fi

# ── CHANGE 3. The borrowed name is withdrawn ────────────────────────────────
#
# `__CYGWIN__` was left defined so that code needing "PE object format with a
# POSIX-presenting C environment" had a name. A 30-member measurement found
# four members reading it as "Win32 is available" and reaching windows.h.
# This needs a real openkal graph, so it skips rather than failing when the
# sandbox cannot reach one.
section "D. __CYGWIN__ is withdrawn on a Windows target presenting POSIX (CHANGE)"
d="$root/d"; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/src/main.cpp" <<'EOF'
#if defined(__CYGWIN__) || defined(__CYGWIN32__)
#error "the borrowed name is still defined"
#endif
#if !defined(__MCPP_TARGET_WINDOWS__)
#error "mcpp's own name for the target is missing"
#endif
#if defined(_WIN32)
#error "presents = posix must suppress _WIN32"
#endif
#if !defined(__OPENKAL__)
#error "__OPENKAL__ is not defined over a resolved openkal layer"
#endif
#if defined(__openkal__)
#error "the lower-case spelling is still defined"
#endif
int main() { return 0; }
EOF
cat > "$d/mcpp.toml" <<'EOF'
[package]
name    = "withdrawal-probe"
version = "0.1.0"

[dependencies]
openkal-llvm-runtime = "0.14.0"
EOF
out=$(cd "$d" && "$STORE" build --target x86_64-windows-gnu 2>&1)
rc=$?
case "$out" in
  *"openkal-llvm-runtime"*"not found"*|*"did not resolve"*)
    skip "openkal-llvm-runtime 0.14.0 did not resolve from the index" ;;
  *)
    if [ $rc -eq 0 ]; then
        ok "the borrowed name is gone, mcpp's own name is there, over openkal"
    else
        case "$out" in
          *"borrowed name is still defined"*) fail "__CYGWIN__ is still defined" ;;
          *"own name for the target is missing"*) fail "__MCPP_TARGET_WINDOWS__ is missing" ;;
          *"__OPENKAL__ is not defined"*) fail "__OPENKAL__ is missing over openkal" ;;
          *"lower-case spelling is still defined"*) fail "__openkal__ is still defined" ;;
          *"presents = posix"*) fail "_WIN32 survived the substitution" ;;
          *) skip "the openkal Windows graph did not build in this sandbox" ;;
        esac
    fi ;;
esac

# ── GUARD. A package declaring nothing is untouched ─────────────────────────
section "E. a package declaring neither key builds and runs (GUARD)"
e="$root/e"; rm -rf "$e"; mkdir -p "$e/src"
printf '#include <cstdio>\nint main(){std::puts("plain ok");return 0;}\n' > "$e/src/main.cpp"
cat > "$e/mcpp.toml" <<'EOF'
[package]
name    = "plain"
version = "0.1.0"
EOF
if (cd "$e" && "$STORE" build >/dev/null 2>&1) \
   && "$e"/target/*/*/bin/plain 2>/dev/null | grep -q "plain ok"; then
    ok "a package that declares nothing builds and runs"
else
    fail "a package that declares nothing must be untouched by this release"
fi

# ── GUARD. An openkal program from the published index ──────────────────────
section "F. an openkal program from the published index (GUARD)"
f="$root/f"; rm -rf "$f"; mkdir -p "$f/src"
printf '#include <cstdio>\nint main(){std::puts("openkal ok");return 0;}\n' > "$f/src/main.cpp"
cat > "$f/mcpp.toml" <<'EOF'
[package]
name    = "openkal-hello"
version = "0.1.0"

[dependencies]
openkal-llvm-runtime = "0.14.0"
EOF
if (cd "$f" && "$STORE" build >/dev/null 2>&1); then
    if "$f"/target/*/*/bin/openkal-hello 2>/dev/null | grep -q "openkal ok"; then
        ok "an openkal program builds and runs from the published index"
    else
        fail "the openkal program built and did not run"
    fi
else
    skip "openkal-llvm-runtime 0.14.0 did not resolve from the index"
fi

# ── CHANGE. `builtins = "iso"` withdraws the Apple pattern fill ─────────────
section "G. builtins = \"iso\" emits -fno-builtin (CHANGE)"
# A CODE-GENERATION PROPERTY, so the reading is an object file rather than a
# `-dM` dump — which is exactly why the token this asserts was a silent no-op
# for the whole of its first life. `2026.9.21.2` and earlier emit
# `-fno-builtin-memset_pattern16`; clang matches `-fno-builtin-<fn>` against
# its builtin table, `memset_pattern16` is an LLVM TargetLibraryInfo libfunc
# and not in it, so the flag is accepted in silence and the call survives. On
# those versions the aarch64-macos link fails with
#
#     ld64.lld: error: undefined symbol: memset_pattern16
#
# so a failed build here IS the negative reading rather than an absent one.
g="$root/g"; rm -rf "$g"; mkdir -p "$g/src"
cat > "$g/src/main.cpp" <<'EOF'
extern "C" void fill(int* a, long n) {
    for (long i = 0; i < n; ++i) a[i] = 0x01020304;
}
int main() { static int buf[64]; fill(buf, 64); return buf[0] == 0x01020304 ? 0 : 1; }
EOF
# `-O2` per package rather than --release: the idiom pass does not run at the
# dev profile's -O0, and a release build would compile the runtime a second
# time in a second profile for no reading.
cat > "$g/mcpp.toml" <<'EOF'
[package]
name    = "builtins-probe"
version = "0.1.0"

[build]
cxxflags = ["-O2"]

[dependencies]
openkal-llvm-runtime = "0.15.0"

[toolchain]
default = "llvm@22.1.8"
EOF
gnm=$(ls "$HOME"/.xlings/data/xpkgs/xim-x-llvm/22.1.8/bin/llvm-nm 2>/dev/null | head -1)
if [ -z "$gnm" ]; then
    skip "G: no llvm-nm in the payload to read the object with"
elif (cd "$g" && "$STORE" build --target aarch64-macos >/dev/null 2>&1); then
    gobj=$(find "$g/target" -name 'main.o' 2>/dev/null | head -1)
    if [ -z "$gobj" ]; then
        fail "G: the build reported success and produced no object"
    elif [ "$("$gnm" -u "$gobj" 2>/dev/null | grep -c memset_pattern16)" = 0 ]; then
        ok "builtins = \"iso\" leaves no memset_pattern16 in the object"
    else
        fail "builtins = \"iso\" did not withdraw memset_pattern16"
    fi
else
    fail "G: the aarch64-macos build did not complete (the old token's signature)"
fi

# ── CHANGE. `mcpp test --no-run` ────────────────────────────────────────────
section "H. mcpp test --no-run builds the tests and says so (CHANGE)"
# Before this release the flag does not exist and the command exits non-zero
# with "unknown option: --no-run". The runner named here is a program that
# does not exist, which produces "tests built, nothing run" on every host for
# the native target with nothing installed.
h="$root/h"; rm -rf "$h"; mkdir -p "$h/tests"
printf 'int main() { return 0; }\n' > "$h/tests/alpha.cpp"
printf 'int main() { return 0; }\n' > "$h/tests/beta.cpp"
printf '[package]\nname = "norun"\nversion = "0.1.0"\n' > "$h/mcpp.toml"
hhost=$("$STORE" --print-target 2>/dev/null || true)
if [ -z "$hhost" ]; then
    (cd "$h" && "$STORE" build >/dev/null 2>&1) || true
    hhost=$(ls "$h/target" 2>/dev/null | grep -v '^\.' | head -1)
fi
if [ -z "$hhost" ]; then
    skip "H: could not determine the host triple"
else
    cat > "$h/mcpp.toml" <<EOF
[package]
name    = "norun"
version = "0.1.0"

[target.$hhost]
runner = ["mcpp-no-such-runner-exists"]
EOF
    hout=$( (cd "$h" && "$STORE" test --target "$hhost" --no-run 2>&1) )
    hrc=$?
    case "$hout" in
        *"2 built, not run"*)
            if [ "$hrc" = 0 ]; then
                ok "--no-run builds the tests and exits 0"
            else
                fail "--no-run reported the built tests and exited $hrc"
            fi ;;
        *) fail "--no-run did not report two tests as built" ;;
    esac
fi


printf '\n-- summary --\nfails=%d\nnot run:%s\n' "$fails" "${skipped:-
  (none)}"
[ "$fails" -eq 0 ]
