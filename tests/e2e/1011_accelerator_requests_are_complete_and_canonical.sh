#!/usr/bin/env bash
# requires: gcc unix-shell python3
# Invalid accelerator syntax must not become a successful CPU build, and an
# implementation cannot occur twice with order-dependent constraint coverage.
set -e
MCPP="${MCPP:-mcpp}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
mkdir -p "$work/app/src"
cd "$work/app"
fail() { echo "FAIL: $*"; cat out.log 2>/dev/null; exit 1; }
manifest() {
    cat > mcpp.toml <<'TOML'
[package]
name = "accel-request"
version = "0.1.0"
[targets.accel-request]
kind = "bin"
main = "src/main.cpp"
TOML
}
manifest
echo 'int main(){return 0;}' > src/main.cpp

for request in 'sycl+{sm_89' 'sycl+{sm_89}garbage' 'sycl+{sm_89,,sm_90}' \
               'sycl+{sm_89},sycl+{sm_90}' 'sycl+{sm_90},sycl+{sm_89}'; do
    if "$MCPP" build --accel "$request" > out.log 2>&1; then
        fail "accepted malformed or repeated request: $request"
    fi
    grep -q 'invalid accelerator request at byte' out.log || fail "missing syntax location"
    grep -q -- '--accel' out.log || fail "missing request source"
done

cat >> mcpp.toml <<'TOML'
[build]
accel = "sycl+{sm_89"
TOML
if "$MCPP" build > out.log 2>&1; then fail 'accepted malformed manifest request'; fi
grep -q '\[build\] accel' out.log || fail 'missing manifest field'

manifest
cat >> mcpp.toml <<'TOML'
[build]
sources = [{ glob = "src/main.cpp", accel = "sycl+{sm_90" }]
TOML
if "$MCPP" build --no-accel > out.log 2>&1; then fail 'ignored malformed source constraint'; fi
grep -q "sources entry 'src/main.cpp'" out.log || fail 'missing source constraint'
grep -q 'invalid accelerator request' out.log || fail 'missing constraint syntax error'

manifest
cat >> mcpp.toml <<'TOML'
[build]
sources = [{ glob = "src/main.cpp", accel = "sycl+{sm_90}" }]
TOML
"$MCPP" build --accel 'sycl+{sm_90,sm_89},newmodel2+{opaque}' > out.log 2>&1 \
    || fail 'valid open-vocabulary request did not build'
before=$(find target -maxdepth 3 -name build.ninja | sort)
[ -n "$before" ] || fail 'no build graph was generated'
"$MCPP" build --accel 'newmodel2+{opaque},sycl+{sm_89,sm_90,sm_89}' > out.log 2>&1 \
    || fail 'equivalent request did not build'
after=$(find target -maxdepth 3 -name build.ninja | sort)
[ "$before" = "$after" ] || fail 'equivalent sets allocated different variants'
grep -q 'fingerprint changed' out.log && fail 'equivalent sets changed the fingerprint'
"$MCPP" run --accel 'sycl+{sm_89,sm_90},newmodel2+{opaque}' > out.log 2>&1 \
    || fail 'the canonical variant did not run'
echo 'PASS: accelerator requests are complete, unique and canonical'
