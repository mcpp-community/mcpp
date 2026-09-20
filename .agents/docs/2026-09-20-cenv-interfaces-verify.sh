#!/usr/bin/env bash
# Ecosystem verification for the 2026.9.20.1 wave against the PUBLISHED mcpp
# and index, run inside a SubOS sandbox with CN mirrors for xlings and mcpp.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-20-cenv-interfaces-verify.sh)
#   xlings subos new v920
#   xlings subos use v920 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.20.1 bash /tmp/v.sh"
#
# Run it once against the PREVIOUS release first
# (MCPP_VERIFY_VERSION=2026.9.18.3): every section marked CHANGE must fail there
# and pass here; every section marked GUARD must pass on both. A section that
# passes on both releases in a CHANGE section measured nothing.
#
# The sandbox's $HOME persists between runs of one SubOS, so each section clears
# its own directory. A section that cannot run says so and is listed again at
# the end, because a run that reports only failures cannot be told from one that
# examined nothing.
# TWO RUNS, AND THE READING FROM EACH (host dry run, 2026-09-20):
#
#   mcpp 2026.9.17.1 (published)   fails=3
#     B  a requirement the implementation does not provide must be refused
#     C  a declared absence with a known shape must be accepted
#     C  the refusal does not name the shapes -- "[c-abi] has no member 'absent'"
#   mcpp 2026.9.20.1               fails=0
#
# B's FIRST leg passes on both, and that is the documented behaviour rather
# than a hole: `[kernel-abi]` is an unknown top-level table to an older engine
# and is ignored, so a graph that satisfies its requirements builds either way.
# The leg that distinguishes the releases is the refusal. C fails outright on
# the older engine because `[c-abi-absent]` is a new key inside a table it
# knows, where an unrecognised key is a parse error -- the asymmetry docs/22
# records.
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

root="$HOME/verify-920"
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

# ── CHANGE 1. A package states which interfaces of the layer it requires ─────
#
# The engine knows no interface name: it compares two sets and refuses before
# anything is compiled. Both legs are here because only the second says the
# comparison happened -- a build that succeeds proves nothing about a check
# that never ran.
section "B. requires-interfaces is answered at resolution (CHANGE)"
b="$root/b"; rm -rf "$b"; mkdir -p "$b/impl/src" "$b/src"
printf 'int fake_kernel_marker(void){return 0;}\n' > "$b/impl/src/lib.c"
printf 'int main(void){return 0;}\n'               > "$b/src/main.c"
cat > "$b/impl/mcpp.toml" <<'EOF'
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]

[kernel-abi]
provides-interfaces = ["openkal.abort", "openkal.stream", "openkal.memory"]
EOF
mk_root() {  # $1 = requires-interfaces body
    cat > "$b/mcpp.toml" <<EOF
[package]
name    = "iface-probe"
version = "0.1.0"

[dependencies]
fakekernel = { path = "impl" }

[build]
allow_host_libs = true

[kernel-abi]
requires-interfaces = [$1]
EOF
}
mk_root '"openkal.stream"'
rm -rf "$b/target"
if (cd "$b" && "$STORE" build >/dev/null 2>&1); then
    ok "a requirement the implementation provides builds"
else
    fail "a requirement the implementation provides must build"
fi
mk_root '"openkal.stream", "openkal.net"'
rm -rf "$b/target"
out="$(cd "$b" && "$STORE" build 2>&1)"
if [ $? -eq 0 ]; then
    fail "a requirement the implementation does not provide must be refused"
else
    case "$out" in
        *openkal.net*iface-probe*|*iface-probe*openkal.net*)
            ok "the refusal names the interface and the package" ;;
        *) fail "the refusal does not name both: $(printf '%s' "$out" | head -3 | tr '\n' ' ')" ;;
    esac
    if [ -d "$b/target" ] && find "$b/target" -name '*.o' -print -quit 2>/dev/null | grep -q .; then
        fail "the refusal arrived after something was compiled"
    else
        ok "nothing was compiled before the refusal"
    fi
fi

# ── CHANGE 2. A C library states what it does not supply ────────────────────
section "C. [c-abi-absent] is read, and a bad shape is refused (CHANGE)"
c="$root/c"; rm -rf "$c"; mkdir -p "$c/libc/src" "$c/src"
printf 'int fake_libc_marker(void){return 0;}\n' > "$c/libc/src/lib.c"
printf 'int main(void){return 0;}\n'             > "$c/src/main.c"
cat > "$c/mcpp.toml" <<'EOF'
[package]
name    = "absent-probe"
version = "0.1.0"

[dependencies]
fakelibc = { path = "libc" }

[build]
allow_host_libs = true
EOF
mk_libc() {  # $1 = the form value
    cat > "$c/libc/mcpp.toml" <<EOF
[package]
name     = "fakelibc"
version  = "0.1.0"
provides = ["mcpp:c-abi=musl"]

[targets.fakelibc]
kind    = "lib"
sources = ["src/*.c"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32

[c-abi-absent]
fork = { form = "$1", note = "no process image duplication" }
EOF
}
mk_libc link
rm -rf "$c/target"
if (cd "$c" && "$STORE" build >/dev/null 2>&1); then
    ok "a declared absence with a known shape is accepted"
else
    fail "a declared absence with a known shape must be accepted"
fi
mk_libc sometimes
rm -rf "$c/target"
out="$(cd "$c" && "$STORE" build 2>&1)"
if [ $? -eq 0 ]; then
    fail "an absence with an unknown shape must be refused"
else
    case "$out" in
        *accepted-no-effect*) ok "the refusal names the shapes that exist" ;;
        *) fail "the refusal does not name the shapes: $(printf '%s' "$out" | head -2 | tr '\n' ' ')" ;;
    esac
fi

# ── GUARD. A package that declares neither key is untouched ─────────────────
section "D. a package declaring neither key is unchanged (GUARD)"
d="$root/d"; rm -rf "$d"; mkdir -p "$d/src"
printf '#include <cstdio>\nint main(){std::puts("plain");return 0;}\n' > "$d/src/main.cpp"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name    = "plain"
version = "0.1.0"
EOF
if (cd "$d" && "$STORE" build >/dev/null 2>&1) \
   && "$d"/target/*/*/bin/plain 2>/dev/null | grep -q plain; then
    ok "a package declaring nothing builds and runs"
else
    fail "a package declaring nothing must build and run unchanged"
fi

# ── GUARD. openkal from the published index ─────────────────────────────────
section "E. an openkal program from the published index (GUARD)"
e="$root/e"; rm -rf "$e"; mkdir -p "$e/src"
printf '#include <cstdio>\nint main(){std::puts("openkal ok");return 0;}\n' > "$e/src/main.cpp"
cat > "$e/mcpp.toml" <<'EOF'
[package]
name    = "openkal-hello"
version = "0.1.0"

[dependencies]
openkal-llvm-runtime = "0.12.0"
EOF
if (cd "$e" && "$STORE" build >/dev/null 2>&1); then
    if "$e"/target/*/*/bin/openkal-hello 2>/dev/null | grep -q "openkal ok"; then
        ok "an openkal program builds and runs from the published index"
    else
        fail "the openkal program built and did not run"
    fi
else
    skip "openkal-llvm-runtime 0.12.0 did not resolve from the index"
fi

printf '\n-- summary --\nfails=%d\nnot run:%s\n' "$fails" "${skipped:-
  (none)}"
[ "$fails" -eq 0 ]
