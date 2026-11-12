#!/usr/bin/env bash
# 1033 -- an ok image runs on the system that built it, on every system mcpp is released for.
#
# docs/21 calls a row `verified` when CI builds AND runs its artefact. The two
# `*-openkal` rows were built on every host, but run by mcpp's own CI only on
# Linux: 942 and its neighbours declare `llvm`, which run_all.sh grants on Linux
# alone, so macOS and Windows skipped them. This test needs nothing the
# platform does not bring, and tests/e2e/host-requirements.tsv requires it to
# pass on Linux, macOS and Windows: an image of this machine's architecture,
# with the platform's C library and C++ run-time (`import std`, as the
# ecosystem's applications use them), is built, run by okrun and tested; the
# other architecture's row builds too.
#
# Criteria:
#   A. `mcpp run --target <arch>-openkal` runs the image: its arguments and its
#      environment arrive, and std::format and a std::thread work.
#   B. `mcpp test --target <arch>-openkal` builds and runs a test image.
#   C. The other architecture's row builds an image (without the platform's
#      layers, so that the test stays within one bound).
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64;  OTHER_ARCH=aarch64 ;;
    aarch64|arm64) HOST_ARCH=aarch64; OTHER_ARCH=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 77 ;;
esac

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"
TOML
cat > src/main.cpp <<'CPP'
import std;

int main(int argc, char** argv) {
    std::string args;
    for (int i = 1; i < argc; ++i) args += std::format("[{}]", argv[i]);
    const char* v = std::getenv("MCPP_E2E_1033");
    int sum = 0;
    std::thread t([&] { for (int i = 1; i <= 10; ++i) sum += i; });
    t.join();
    std::println("args={} env={} sum={}", args, v ? v : "(none)", sum);
    return sum == 55 ? 0 : 1;
}
CPP
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { return std::format("{}-{}", 1, 2) == "1-2" ? 0 : 1; }
CPP

MCPP_E2E_1033=here "$MCPP" run --target "$HOST_ARCH-openkal" -- one "two words" > run.log 2>&1 \
    || fail "A: mcpp run --target $HOST_ARCH-openkal" run.log
grep -q 'args=\[one\]\[two words\] env=here sum=55' run.log || fail "A: the image's output" run.log
echo "ok: A, the $HOST_ARCH-openkal image runs here"

"$MCPP" test --target "$HOST_ARCH-openkal" > test.log 2>&1 || fail "B: mcpp test --target $HOST_ARCH-openkal" test.log
grep -q 'smoke \.\.\. ok' test.log || fail "B: the test image did not pass" test.log
echo "ok: B, a test image runs here"

mkdir -p "$TMP/other/src"
cd "$TMP/other"
cat > mcpp.toml <<'TOML'
[package]
name    = "other"
version = "0.1.0"

[targets.other]
kind = "bin"
main = "src/main.c"

[target.'cfg(platform = "openkal")'.layers]
c-abi            = false
"c++-abi"        = false
compiler-runtime = false
TOML
printf 'int main(void) { return 0; }\n' > src/main.c
"$MCPP" build --target "$OTHER_ARCH-openkal" > other.log 2>&1 || fail "C: mcpp build --target $OTHER_ARCH-openkal" other.log
ls target/"$OTHER_ARCH"-openkal/*/bin/other.oke > /dev/null 2>&1 || fail "C: no image for $OTHER_ARCH-openkal" other.log
echo "ok: C, the $OTHER_ARCH-openkal row builds here"
echo "PASS: 1033 an ok image runs on the system that built it"
