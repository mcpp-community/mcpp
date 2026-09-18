#!/usr/bin/env bash
# requires: llvm unix-shell
# 741 -- the `[c-abi]` block a `mcpp:c-abi=<impl>` provider declares (design
# 2026-09-18, "C environment declared by the C library layer") reaches the
# target-side report, realises into the tokens `docs/22` documents, reaches
# `mcpp emit build-database`, is inferred off for a `mcpp:kernel-abi=<impl>`
# provider with no manifest change of its own, and its verification step
# refuses an unrealisable request rather than silently approximating it.
#
# WHAT THIS COVERS THAT THE UNIT TESTS CANNOT. `mcpp.toolchain.cenv::realise`
# is a pure function and its mapping table is asserted directly in
# tests/unit/test_cenv.cpp; `[c-abi]`/`c-environment` parsing, validation and
# the kernel-abi inference are asserted in tests/unit/test_manifest.cpp. What
# only a build can show is the WIRING: that a provider's block actually
# reaches the resolved target side, that the realised tokens reach the
# compile database of an ordinary package, that a kernel-abi provider is
# excluded from them WITHOUT declaring `c-environment` itself,
# that a GAS (.S) unit gets the SAME tokens a .c/.cpp unit in the same package
# does (a defect found by the openkal-musl spike after this test's first
# version: the substitution reached `f.cc`/`f.cxx` but not `f.as`, so a .c unit
# saw `_WIN32` undefined while a .S unit in the same package still saw it
# defined), and that a request this engine cannot realise is refused before
# anything compiles -- rather than compiled wrong and shipped.
#
# `x86_64-windows-gnu` is the flagship target (design's own motivating case),
# and this test compiles only -- it never links or runs the artifact, so it
# needs no mingw runtime and no Wine. `python3` reads compile_commands.json
# because grep across a JSON array of command strings is fragile the moment a
# path contains the token being searched for.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

PY=python3
command -v "$PY" >/dev/null 2>&1 || PY=python

mkdir -p fakemusl/src
cat > fakemusl/src/lib.c <<'EOF'
int fakemusl_marker(void) { return 0; }
EOF

mkdir -p openkalwin/src
cat > openkalwin/src/shim.c <<'EOF'
int openkalwin_marker(void) { return 0; }
EOF

mkdir -p src
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

# A third-party-style assembly unit, mirroring the class of file the
# coordinator's report named (openkal-musl's `okm_setjmp.S`, upstream
# libunwind's `assembly.h`): real GAS source that is preprocessed and reads
# the SAME environment macros a `.c`/`.cpp` unit does. Compiled only (never
# linked/run), so it needs no runtime -- one label is enough to be valid GAS.
cat > src/probe.S <<'EOF'
.text
.globl cabi_probe_asm_marker
cabi_probe_asm_marker:
    ret
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "cabi-probe"
version = "0.1.0"

[dependencies]
fakemusl  = { path = "fakemusl" }
openkalwin = { path = "openkalwin" }

[build]
allow_host_libs = true
EOF

# `fakemusl_declares <wchar>` rewrites the provider's manifest with the given
# `wchar` value so the "deliberately wrong" leg below can ask for a value this
# engine has no way to realise, without duplicating the whole manifest.
fakemusl_declares() {
    cat > fakemusl/mcpp.toml <<EOF
[package]
name     = "fakemusl"
version  = "1.0.0"
provides = ["mcpp:c-abi=fakemusl"]

[targets.fakemusl]
kind    = "lib"
sources = ["src/*.c"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = $1
builtins   = "iso"
EOF
    rm -rf target
}

# NO explicit `c-environment` here -- that is the point of this leg
# (coordinator revision, openkal-musl spike): a `mcpp:kernel-abi=<impl>`
# provider is inferred into the platform boundary from `provides` alone,
# because it is BY DEFINITION the package that speaks the platform's own
# ABI and can never want the graph's presented [c-abi] environment. Before
# this revision this manifest carried an explicit `c-environment =
# "platform"` line; it is gone on purpose, to prove the DEFAULT, not the
# opt-out key (`tests/unit/test_manifest.cpp`'s
# `CEnvironmentAcceptsOnlyPlatform` already covers the explicit key).
cat > openkalwin/mcpp.toml <<'EOF'
[package]
name          = "openkalwin"
version       = "1.0.0"
provides      = ["mcpp:kernel-abi=openkal"]

[targets.openkalwin]
kind    = "lib"
sources = ["src/*.c"]
EOF

# ── A. the realisable request reaches the report and the compile database ──
fakemusl_declares 32
out=$("$MCPP" build --target x86_64-windows-gnu --toolchain llvm@22.1.8 2>&1) || {
    echo "FAIL: a realisable [c-abi] request must not fail the build" >&2
    echo "$out" >&2; exit 1
}

echo "$out" | grep -q 'c-abi  *fakemusl' || {
    echo "FAIL: the target-side report must name the c-abi provider" >&2
    echo "$out" >&2; exit 1
}

"$MCPP" emit build-database --target x86_64-windows-gnu --toolchain llvm@22.1.8 \
    --format json > db.json 2> db.err || {
    echo "FAIL: emit build-database must succeed on the realised graph" >&2
    cat db.err >&2; exit 1
}

"$PY" - "$TMP/db.json" <<'PYEOF'
import json, sys
doc = json.load(open(sys.argv[1]))
sets = doc["data"]["database"]["sets"]

def args_for(source_contains):
    for s in sets:
        for tu in s.get("translation-units", []):
            if source_contains in tu.get("source", ""):
                yield tu["arguments"]

def joined(argv_iter):
    return " ".join(" ".join(a) for a in argv_iter)

# The ordinary consumer (main.cpp, package "cabi-probe") is target-side and
# does NOT declare c-environment: it must carry the Cygwin-flavoured tokens.
#
# `__CYGWIN__`/`__CYGWIN32__` are NOT undefined (design revision from the
# openkal-musl spike: third-party portable code that needs to know the
# object format -- not the C environment, not the platform API -- has no
# other name for "PE format, POSIX-presenting environment", and such code
# cannot be patched the way this ecosystem's own packages can; a trade-off
# for the 30-member measurement to settle, not a settled fact) -- so this
# asserts `-U__CYGWIN__` is ABSENT from the command line, the opposite of an
# earlier version of this test.
consumer = joined(args_for("main.cpp"))
missing = [tok for tok in ("--target=x86_64-pc-cygwin", "-fno-short-wchar")
           if tok not in consumer]
if missing:
    print(f"FAIL: ordinary package is missing realised tokens {missing}\n  args: {consumer}")
    sys.exit(1)
present = [tok for tok in ("-U__CYGWIN__", "-U__CYGWIN32__") if tok in consumer]
if present:
    print(f"FAIL: __CYGWIN__/__CYGWIN32__ must stay defined, but found {present}\n  args: {consumer}")
    sys.exit(1)

# fakemusl's OWN units get them too -- the environment applies to the C
# library itself, not only to its consumers.
libc = joined(args_for("fakemusl/src/lib.c"))
if "--target=x86_64-pc-cygwin" not in libc:
    print(f"FAIL: the c-abi provider's own unit is missing the realised triple\n  args: {libc}")
    sys.exit(1)

# The GAS (.S) unit must carry the SAME environment tokens as the C/C++ units
# of the SAME package -- the defect this leg pins (coordinator report,
# openkal-musl spike): the substitution used to reach C/C++ compiles only, so
# a `.c` unit in a package saw `_WIN32` undefined while a `.S` unit in the
# SAME package still saw it defined, because `--target=`/`-fno-short-wchar`
# never reached the assembler's command line at all. `--target=` is asserted
# by full-string match (assembly's flag string is independently assembled --
# `mcpp.build.flags::f.as`, not `f.cc` -- so a match here proves the token
# actually reached that channel, not merely that it exists somewhere in the
# database). `-fno-short-wchar` is meaningless to GAS (no wchar_t in
# assembly) and is asserted too regardless, on the coordinator's own
# instruction: "whatever the C units get for the environment, the assembler
# units should get too, minus anything meaningless to the assembler" --
# clang accepts the flag for `.S` input (measured, does not error), so
# nothing here justifies dropping it just because assembly has no use for it.
asm_args = list(args_for("probe.S"))
if not asm_args:
    print("FAIL: could not find the .S unit's compile command at all")
    sys.exit(1)
asm_joined = joined(iter(asm_args))
missing = [tok for tok in ("--target=x86_64-pc-cygwin", "-fno-short-wchar")
           if tok not in asm_joined]
if missing:
    print(f"FAIL: the assembly unit is missing realised tokens {missing} "
          f"-- the [c-abi] substitution must reach .S the same as .c/.cpp\n"
          f"  args: {asm_joined}")
    sys.exit(1)
present = [tok for tok in ("-U__CYGWIN__", "-U__CYGWIN32__") if tok in asm_joined]
if present:
    print(f"FAIL: __CYGWIN__/__CYGWIN32__ must stay defined on assembly too, "
          f"but found {present}\n  args: {asm_joined}")
    sys.exit(1)

# openkalwin provides `mcpp:kernel-abi=openkal` and declares NO
# `c-environment` of its own -- the platform boundary is INFERRED from that
# alone (coordinator revision), so it must NOT see the substituted triple:
# it needs the real Windows identity to include platform declarations, the
# same as if it had written `c-environment = "platform"` by hand.
shim = joined(args_for("shim.c"))
if not shim:
    print("FAIL: could not find openkalwin's compiled unit at all")
    sys.exit(1)
if "--target=x86_64-pc-cygwin" in shim:
    print(f"FAIL: a kernel-abi provider must be INFERRED into the platform "
          f"boundary and must not receive the realised triple, even though "
          f"it declares no c-environment itself\n  args: {shim}")
    sys.exit(1)

print("OK: A")
PYEOF

# ── B. an unrealisable request is refused, naming target/request/missing ───
#
# `data-model = "llp64"` together with `presents = "posix"` has no
# realisation this engine knows (docs/22's table has exactly one Windows row,
# and it delivers LP64): the "deliberately wrong declaration" the design
# calls for. `mcpp.toolchain.cenv::realise` refuses it before anything
# compiles -- this is the request-cannot-be-realised leg; the "declared vs
# measured" probe (§3.2) is exercised by every REALISABLE request above,
# which only builds because the probe found no mismatch.
cat > fakemusl/mcpp.toml <<'EOF'
[package]
name     = "fakemusl"
version  = "1.0.0"
provides = ["mcpp:c-abi=fakemusl"]

[targets.fakemusl]
kind    = "lib"
sources = ["src/*.c"]

[c-abi]
presents   = "posix"
data-model = "llp64"
wchar      = 32
EOF
rm -rf target

out=$("$MCPP" build --target x86_64-windows-gnu --toolchain llvm@22.1.8 2>&1) && {
    echo "FAIL: an unrealisable [c-abi] request must refuse the build" >&2
    echo "$out" >&2; exit 1
}
echo "$out" | grep -q 'x86_64-windows-gnu\|windows' || {
    echo "FAIL: the refusal must name the target" >&2
    echo "$out" >&2; exit 1
}
echo "$out" | grep -qi 'llp64' || {
    echo "FAIL: the refusal must name the request" >&2
    echo "$out" >&2; exit 1
}
echo "OK: B"

echo "OK"
