#!/usr/bin/env bash
# requires: elf
# 650_wasm_row_names_its_launcher.sh -- #622 A5: `artifact_naming` on
# `wasm32-emscripten` names the executable `<name>.js` on every host (not the
# host's own convention), the link edge declares `<name>.wasm` as an implicit
# output, `mcpp pack` stages the whole stem family, and `kind = "shared"` is
# refused by name rather than produced as a file that does not work.
#
# Part 1 (always runs, no emsdk needed): the `shared` refusal is reached
# before any toolchain is resolved (`MCPP_NO_AUTO_INSTALL=1`), so an offline
# host gets the sentence instead of downloading the SDK first.
#
# Part 2 (skips honestly where no emsdk payload is installed): an actual
# build, run and pack of a `1-2-3` program for `--target wasm32-emscripten`.
# The fixture declares its own `[target.wasm32-emscripten] runner = ["node"]`
# rather than relying on the payload's `.mcpp-toolchain.json` descriptor,
# because whether an ALREADY-INSTALLED payload on this machine carries that
# descriptor is an index-freshness question this script is not testing (see
# .agents/docs/2026-09-12-a-verified-web-run-that-asked-the-host-for-node.md
# and the dedicated `wasm32-emscripten runs through its payload's runner, not
# PATH` step in ci-target-matrix.yml, which IS that test).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

echo "== 650/1: kind = \"shared\" is refused on wasm32-emscripten, before any toolchain is resolved =="

mkdir -p "$TMP/shared/src"
cat > "$TMP/shared/mcpp.toml" <<'TOML'
[package]
name    = "sh"
version = "0.1.0"

[targets.sh]
kind = "shared"
main = "src/lib.cpp"
TOML
printf 'extern "C" int foo() { return 1; }\n' > "$TMP/shared/src/lib.cpp"

out=$(cd "$TMP/shared" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target wasm32-emscripten 2>&1) && rc=0 || rc=$?
[ "$rc" -ne 0 ] || fail "kind = shared built instead of being refused" <(echo "$out")
want='[targets.sh] kind = "shared" is not supported on wasm32-emscripten: a side module needs -sSIDE_MODULE, which mcpp does not render'
grep -qF "$want" <<<"$out" || fail "the refusal does not carry the exact sentence" <(echo "$out")
echo "  ok: the exact refusal line is printed"

# The negative direction of the SAME criterion: an ordinary `bin` on the same
# row is NOT refused by this check -- the predicate is about the target's
# `kind`, not about the row.
mkdir -p "$TMP/notshared/src"
cat > "$TMP/notshared/mcpp.toml" <<'TOML'
[package]
name = "ns"
version = "0.1.0"

[targets.ns]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'import std;\nint main() { std::println("ok"); return 0; }\n' > "$TMP/notshared/src/main.cpp"
have_emsdk=0
for d in "${MCPP_HOME:-$HOME/.mcpp}"/registry/data/xpkgs/xim-x-emsdk/*/emscripten \
         "$HOME"/.xlings/data/xpkgs/xim-x-emsdk/*/emscripten; do
    [[ -x "$d/em++" ]] && have_emsdk=1 && EMSDK_DIR="$d"
done
if [ "$have_emsdk" -eq 1 ]; then
    out=$(cd "$TMP/notshared" && "$MCPP" build --target wasm32-emscripten 2>&1) \
        || fail "an ordinary bin on wasm32-emscripten was refused" <(echo "$out")
    grep -qF 'kind = "shared" is not supported' <<<"$out" \
        && fail "the shared refusal fired on a bin target" <(echo "$out")
    echo "  ok: an ordinary bin target is not refused by this check"
else
    echo "  (skipping the negative-direction build; no emsdk payload -- rerun with MCPP_NO_AUTO_INSTALL=1 unset and network to build it)"
fi

if [ "$have_emsdk" -ne 1 ]; then
    echo "SKIP: 650/2 -- no emsdk payload installed (looked under xim-x-emsdk/*/emscripten)"
    echo "PASS: 650 (part 1 only)"
    exit 0
fi

echo "== 650/2: build, run and pack a 1-2-3 program for wasm32-emscripten =="

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[target.wasm32-emscripten]
runner = ["node"]
TOML
printf 'import std;\nint main() { std::println("1-2-3"); return 0; }\n' > src/main.cpp

"$MCPP" build --target wasm32-emscripten > build.log 2>&1 \
    || fail "mcpp build --target wasm32-emscripten failed" build.log

bindir=$(dirname "$(find target/wasm32-emscripten -name 'app.js' | head -1)")
[ -n "$bindir" ] || fail "no bin/app.js under target/wasm32-emscripten" build.log
[ -f "$bindir/app.js" ]   || fail "bin/app.js is missing" build.log
[ -f "$bindir/app.wasm" ] || fail "bin/app.wasm is missing" build.log
[ -f "$bindir/app" ]      && fail "a bare bin/app (no extension) exists alongside app.js -- the fallback naming leaked" build.log
echo "  ok: bin/app.js and bin/app.wasm exist, and no bare bin/app does"

"$MCPP" run --target wasm32-emscripten > run.log 2>&1 \
    || fail "mcpp run --target wasm32-emscripten failed" run.log
grep -q '^1-2-3$' run.log || fail "mcpp run did not print 1-2-3" run.log
echo "  ok: mcpp run prints 1-2-3"

"$MCPP" pack --target wasm32-emscripten --format dir > pack.log 2>&1 \
    || fail "mcpp pack --format dir failed" pack.log
staged=$(find target/dist -maxdepth 1 -name 'app-0.1.0-wasm32-emscripten' -type d | head -1)
[ -n "$staged" ] || fail "no staged directory under target/dist" pack.log
[ -f "$staged/bin/app.js" ]   || fail "the staged tree has no bin/app.js" pack.log
[ -f "$staged/bin/app.wasm" ] || fail "the staged tree has no bin/app.wasm" pack.log
manifest="target/dist/$(basename "$staged").stage-manifest"
[ -f "$manifest" ] || fail "no .stage-manifest sibling of the staged tree" pack.log
grep -q 'bin/app.js'   "$manifest" || fail "the stage manifest does not list bin/app.js" "$manifest"
grep -q 'bin/app.wasm' "$manifest" || fail "the stage manifest does not list bin/app.wasm" "$manifest"
echo "  ok: mcpp pack --format dir stages both files and the manifest lists both"

# `--no-entry` reaches the link line as an ordinary ldflag on this row -- no
# engine mechanism checks for a `main` symbol (docs/21, docs/22).
mkdir -p "$TMP/noent/src"
cd "$TMP/noent"
cat > mcpp.toml <<'TOML'
[package]
name = "noent"
version = "0.1.0"

[targets.noent]
kind = "bin"
main = "src/lib.cpp"

[target.'cfg(os = "emscripten")'.build]
ldflags = ["--no-entry"]
TOML
printf 'extern "C" int mcpp_factory() { return 42; }\n' > src/lib.cpp
"$MCPP" build --target wasm32-emscripten > build.log 2>&1 \
    || fail "a main()-less bin with --no-entry did not link" build.log
ninjafile=$(find target/wasm32-emscripten -name 'build.ninja' | head -1)
grep -q -- '--no-entry' "$ninjafile" || fail "--no-entry did not reach build.ninja" "$ninjafile"
echo "  ok: --no-entry reaches the link line as an ordinary ldflag"

echo "PASS: 650"
