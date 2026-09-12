#!/usr/bin/env bash
# requires: elf
# 653_exceptions_is_the_second_abi_member.sh -- design 2026-09-12 (a UI
# framework on Android, iOS and Web), section 2.1, A1: `exceptions` joins
# `threads` as an `abi` member, and design section 2.6, A6: a dependency
# states the requirement with `requires_abi`.
#
# Same self-probe gate as tests/e2e/650_wasm_row_names_its_launcher.sh: `#
# requires: elf` only (there is no `emsdk` token in run_all.sh's KNOWN_CAPS,
# and 650 does not use one either), because whether THIS machine happens to
# have the emsdk payload already installed is not a portable capability --
# see 650's own comment for why. The script probes for the payload itself and
# exits PASS with a SKIP note when it is absent.
#
# MEASURED (this script's own reason to exist): a Web program that throws
# across an `import std` boundary WITHOUT `-fexceptions` builds and links
# without complaint -- Emscripten's compile-time exception support does not
# depend on the flag -- and fails only at RUN time, aborting with:
#
#   Aborted(Assertion failed: Exception thrown, but exception catching is not
#   enabled. Compile with -sNO_DISABLE_EXCEPTION_CATCHING or
#   -sEXCEPTION_CATCHING_ALLOWED=[..] to catch.)
#
# `mcpp run` exits non-zero and the marker this script looks for never
# prints. This is different from the `abi.threads` failure mode (647: a
# missing `-pthread` fails to LINK), which is exactly why A1 calls it out as
# its own criterion rather than folding it into 647.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1
done
if [ "$have_emsdk" -ne 1 ]; then
    echo "SKIP: 653 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten)"
    echo "PASS: 653 (skipped)"
    exit 0
fi

echo "== 653/1: a Linux build of a manifest carrying the table is byte-identical to one without =="

mkdir -p "$TMP/lx/src"
cd "$TMP/lx"
cat > mcpp.toml <<'TOML'
[package]
name    = "lx"
version = "0.1.0"

[targets.lx]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'import std;\nint main() { std::println("ok"); return 0; }\n' > src/main.cpp
"$MCPP" build > plain-build.log 2>&1 || fail "the plain Linux build failed" plain-build.log
plain_ninja=$(find target -name build.ninja | head -1)
cp "$plain_ninja" "$TMP/plain.ninja"
rm -rf target
cat >> mcpp.toml <<'TOML'

[target.'cfg(os = "emscripten")'.abi]
exceptions = true
TOML
"$MCPP" build > with-table.log 2>&1 || fail "the Linux build with the table failed" with-table.log
with_ninja=$(find target -name build.ninja | head -1)
diff -u "$TMP/plain.ninja" "$with_ninja" > "$TMP/ninja.diff" \
    || fail "a Linux build.ninja differs with the emscripten-only abi table present" "$TMP/ninja.diff"
echo "  ok: build.ninja is byte-identical"

echo "== 653/2: without exceptions, the build and link succeed but the run fails at the throw =="

mkdir -p "$TMP/excdep/src" "$TMP/app/src"
cat > "$TMP/excdep/mcpp.toml" <<'TOML'
[package]
name    = "excdep"
version = "0.1.0"

[targets.excdep]
kind = "lib"
TOML
printf 'export module excdep;\nimport std;\nexport void throwing() { throw std::runtime_error("boom"); }\n' \
    > "$TMP/excdep/src/excdep.cppm"

write_app() {   # $1 = trailing tables (the abi table, or empty)
    cat > "$TMP/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
excdep = { path = "../excdep" }

[targets.app]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]

$1
TOML
}
cat > "$TMP/app/src/main.cpp" <<'CPP'
import excdep;
import std;
int main() {
    try {
        throwing();
    } catch (std::exception const& e) {
        std::println("caught: {}", e.what());
        return 0;
    }
    return 1;
}
CPP

cd "$TMP/app"
write_app ''
"$MCPP" build --target wasm32-emscripten > build-noexc.log 2>&1 \
    || fail "the build without the abi table failed (expected: it succeeds; only the run fails)" build-noexc.log
plain_dir=$(dirname "$(ls -t $(find target/wasm32-emscripten -name build.ninja) | head -1)")
if "$MCPP" run --target wasm32-emscripten > run-noexc.log 2>&1; then
    fail "mcpp run succeeded without -fexceptions -- the throw should have aborted" run-noexc.log
fi
grep -q '^caught: boom$' run-noexc.log \
    && fail "the marker printed without -fexceptions" run-noexc.log
grep -qF 'exception catching is not enabled' run-noexc.log \
    || fail "the run did not fail with the expected Emscripten abort" run-noexc.log
echo "  ok: builds and links without -fexceptions; the run aborts at the throw, and the marker never prints"

echo "== 653/3: with exceptions, the marker prints, the flags reach both compiles, and the fingerprint moves =="

write_app $'[target.\'cfg(os = "emscripten")\'.abi]\nexceptions = true\n'
"$MCPP" build --target wasm32-emscripten > build-exc.log 2>&1 \
    || fail "the build with the abi table failed" build-exc.log
exc_dir=$(dirname "$(ls -t $(find target/wasm32-emscripten -name build.ninja) | head -1)")
[ "$exc_dir" != "$plain_dir" ] \
    || fail "the build with exceptions reused the fingerprint directory of the build without" build-exc.log

"$MCPP" run --target wasm32-emscripten > run-exc.log 2>&1 \
    || fail "the program did not run with the abi table present" run-exc.log
grep -q '^caught: boom$' run-exc.log || fail "mcpp run did not print the marker" run-exc.log
echo "  ok: mcpp run prints 'caught: boom' under node"

ninjafile="$exc_dir/build.ninja"
# The dependency's own compile command, in build.ninja: the top-level
# `cxxflags` variable every cxx_module/cxx_object edge references.
grep -E '^cxxflags' "$ninjafile" | grep -q -- '-fexceptions' \
    || fail "-fexceptions is not in build.ninja's cxxflags (the dependency's compile line)" "$ninjafile"
echo "  ok: -fexceptions reaches the dependency's compile command (build.ninja's cxxflags)"

# The std module prebuild command: build.ninja stages std.pcm from the
# persistent, content-addressed build cache (`stage_file`) rather than
# compiling it inline, so the actual em++ invocation is not literally a line
# of THIS build.ninja -- it is recorded once, beside the cached .pcm, in
# std-module.json. Follow the path build.ninja itself names.
stdcache_pcm=$(grep -oE '/[^ ]*pcm\.cache/std\.pcm' "$ninjafile" | grep build-cache | head -1)
[ -n "$stdcache_pcm" ] || fail "build.ninja does not name a cached std.pcm to stage" "$ninjafile"
stdcache_dir=$(dirname "$(dirname "$stdcache_pcm")")
stdmodule_json="$stdcache_dir/std-module.json"
[ -f "$stdmodule_json" ] || fail "no std-module.json beside the cached std.pcm build.ninja names" "$ninjafile"
grep -qF -- '-fexceptions' "$stdmodule_json" \
    || fail "-fexceptions is not recorded on the std module's cached prebuild command" "$stdmodule_json"
echo "  ok: -fexceptions reached the std module's prebuild command (recorded in the build cache build.ninja stages from)"

echo "== 653/4: a dependency requiring exceptions is refused on the Web build without the table =="

mkdir -p "$TMP/excreq/src" "$TMP/app3/src"
cat > "$TMP/excreq/mcpp.toml" <<'TOML'
[package]
name         = "excreq"
version      = "0.1.0"
requires_abi = { exceptions = true }

[targets.excreq]
kind = "lib"
TOML
printf 'export module excreq;\nexport int v() { return 1; }\n' > "$TMP/excreq/src/excreq.cppm"
cat > "$TMP/app3/mcpp.toml" <<'TOML'
[package]
name    = "app3"
version = "0.1.0"

[dependencies]
excreq = { path = "../excreq" }

[targets.app3]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]
TOML
printf 'import excreq;\nint main() { return v() == 1 ? 0 : 1; }\n' > "$TMP/app3/src/main.cpp"
cd "$TMP/app3"
if "$MCPP" build --target wasm32-emscripten > refuse-exc.log 2>&1; then
    fail "a dependency requiring exceptions was built for the Web without the root's table" refuse-exc.log
fi
grep -qF "\`excreq\` requires the artefact's ABI to have exceptions (the package)" refuse-exc.log \
    || fail "the refusal does not name 'exceptions'" refuse-exc.log
echo "  ok: the dependency's requirement is refused, naming exceptions"

echo "PASS: 653"
