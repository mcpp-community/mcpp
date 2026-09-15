#!/usr/bin/env bash
# requires: elf python3
# 701 -- the symbol-provision check reports what the build did not intend
# (#646 F3), read with the default toolchain.
#
# A program over a C++ shared library, both importing `std`, reported symbols
# "provided twice" that nothing in the project had done wrong:
#
#   * 882 to 900 libstdc++ symbols under the default contracts: the program
#     carried a static libstdc++ beside the library's libstdc++.so.6 (#646
#     F3a, now one runtime);
#   * under one runtime, the `std` module initialiser (`_ZGIW3std`), which
#     every C++ image importing `std` links from one `std.o`, and which GCC 16's
#     libstdc++.so.6 exports as well;
#   * seven STB_GNU_UNIQUE objects, which the loader unifies by design.
#
# Every finding is a degradation, so `--strict` failed the build. This asserts
# the default shape is clean in the record, passes `--strict` and runs; the
# real duplicate the check exists for is 307's second half.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

PYTHON="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON" ]] || { echo "skip: no python for JSON assertions"; exit 0; }

cd "$TMP"
mkdir -p lib/src app/src
cat > lib/mcpp.toml <<'TOML'
[package]
name    = "lib"
version = "0.1.0"

[targets.lib]
kind = "shared"
TOML
cat > lib/src/lib.cppm <<'CPP'
export module lib;
import std;
export [[gnu::visibility("default")]] std::string lib_greet(int n);
CPP
cat > lib/src/lib.cpp <<'CPP'
module lib;
import std;
std::string lib_greet(int n) { return std::format("lib-{}", n); }
CPP
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }
TOML
cat > app/src/main.cpp <<'CPP'
import std;
import lib;
int main() { std::println("{}", lib_greet(3)); }
CPP

cd app
"$MCPP" build --strict > strict.log 2>&1 \
    || fail "a program over a C++ shared library failed --strict" strict.log
grep -q 'also provided by a library it loads' strict.log \
    && fail "the check reported the shape the build creates by construction" strict.log
dir=$(ls -d target/*/*/ | head -1)
out=$("$dir/bin/app" 2>&1) || fail "the program exited non-zero: $out"
[ "$out" = "lib-3" ] || fail "expected lib-3, got: $out"

verdict=$("$PYTHON" - "$dir/resolution.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
entries = doc.get("runtime", {}).get("symbol_provision") or []
app = [e for e in entries if e["path"].endswith("bin/app")]
if not app:
    print("NO-APP-ENTRY")
else:
    e = app[0]
    print(e.get("status"), e.get("exported", 0), e.get("dynamic_symbols", 0),
          len(e.get("conflicts", [])))
PY
)
set -- $verdict
[ "$1" = "clean" ] || fail "the record says '$verdict', expected a clean verdict" "$dir/resolution.json"
# Measured, not skipped: the program does export what the library binds to, and
# the denominator is real. "clean" must not be the reading of a check that did
# not look.
[ "${2:-0}" -ge 1 ] || fail "the program exports nothing, so the check did not measure the shape" "$dir/resolution.json"
[ "${3:-0}" -gt "${2:-0}" ] || fail "implausible denominator: $verdict" "$dir/resolution.json"
echo "ok: the default shape is clean in the record and passes --strict"

echo "PASS: 701 symbol provision does not report what the build shares by construction"
