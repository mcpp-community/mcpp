#!/usr/bin/env bash
# requires: elf
# 940 -- a `kernel-abi` package over the platform's C library is linked into a
# shared image that reaches it, like any static package.
#
# THE DEFECT. Every package that provides an `mcpp:` layer was a boundary of
# static placement (#646 F1): never placed into a shared image, its objects
# went into the programs only. That is right for a C library or a C++ runtime,
# whose contract decides where they go. It is not right for openkal-linux
# (-macos, -windows) on a hosted target: there the C library is the system's,
# and the kernel-abi package is a static library over it that `mcpplibs.os`
# depends on like any other. A shared image reaching it was left with `kal_*`
# undefined. ELF linked it, and a host that had not linked the package itself
# could not load it -- measured with CPython importing an extension module
# built on `mcpplibs.os`:
#
#     ImportError: .../_openmui.abi3.so: undefined symbol: kal_process_close
#
# and Mach-O and PE refuse the link outright.
#
# Two legs, both a program over a dependency's shared image:
#   A  the program reaches the kernel-abi package only through the image: the
#      image defines its symbols and does not import them, as it does for the
#      ordinary static package beside it.
#   B  the program reaches the package itself too: it links and runs as
#      before.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p kernel/src sys/src ext/src app/src

cat > kernel/src/kernel.c <<'EOF'
int kal_fake_answer(void) { return 42; }
EOF
cat > kernel/mcpp.toml <<'EOF'
[package]
name     = "fakekernel"
version  = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]

[targets.fakekernel]
kind    = "lib"
sources = ["src/*.c"]
EOF

cat > sys/src/sys.c <<'EOF'
extern int kal_fake_answer(void);
int sys_answer(void) { return kal_fake_answer(); }
EOF
cat > sys/mcpp.toml <<'EOF'
[package]
name    = "fakesys"
version = "0.1.0"

[targets.fakesys]
kind    = "lib"
sources = ["src/*.c"]

[dependencies]
fakekernel = { path = "../kernel" }
EOF

cat > ext/src/ext.c <<'EOF'
extern int sys_answer(void);
int ext_answer(void) { return sys_answer(); }
EOF
cat > ext/mcpp.toml <<'EOF'
[package]
name    = "ext"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.ext]
kind = "shared"

[dependencies]
fakesys = { path = "../sys" }
EOF

write_app() {  # $1 = an extra dependency line, $2 = what main checks
    cat > app/src/main.c <<EOF
extern int ext_answer(void);
extern int kal_fake_answer(void);
int main(void) { return $2 ? 0 : 1; }
EOF
    cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.c"]
allow_host_libs = true

[targets.app]
kind = "bin"
main = "src/main.c"

[dependencies]
ext = { path = "../ext" }
$1
EOF
}

build_app() {  # $1 = the leg
    rm -rf app/target
    out=$(cd app && "$MCPP" build 2>&1) || { echo "$out" >&2; echo "FAIL: $1: the program must build" >&2; exit 1; }
    so="$(find app/target -name 'libext.so' -type f | head -1)"
    bin="$(find app/target -path '*/bin/app' -type f | head -1)"
    if [ -z "$so" ] || [ -z "$bin" ]; then
        echo "$out" >&2; echo "FAIL: $1: libext.so or bin/app was not produced" >&2; exit 1
    fi
    "$bin" || { echo "FAIL: $1: the program ran and answered $?" >&2; exit 1; }
}

# ── A. the image is the package's one home ────────────────────────────────
write_app "" "ext_answer() == 42"
build_app A
nm --defined-only "$so" | grep -qw sys_answer || {
    echo "FAIL: A: the shared image does not define sys_answer (its static dependency)" >&2; exit 1; }
if nm -D --undefined-only "$so" | grep -qw kal_fake_answer; then
    nm -D --undefined-only "$so" >&2
    echo "FAIL: A: the shared image imports kal_fake_answer from the program; a host" \
         "that did not link fakekernel cannot load it" >&2
    exit 1
fi
nm --defined-only "$so" | grep -qw kal_fake_answer || {
    echo "FAIL: A: the shared image does not define kal_fake_answer" >&2; exit 1; }
echo "OK: A (the shared image defines the kernel-abi package's symbols)"

# ── B. the program reaches the package itself ─────────────────────────────
write_app 'fakekernel = { path = "../kernel" }' "ext_answer() == 42 && kal_fake_answer() == 42"
build_app B
echo "OK: B (a program reaching the package links and runs)"
