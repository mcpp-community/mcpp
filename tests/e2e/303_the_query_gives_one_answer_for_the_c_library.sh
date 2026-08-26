#!/usr/bin/env bash
# requires: unix-shell jq
# `mcpp why toolchain --format json` answers "which C library" once.
#
# ⚠️⚠️ IT USED TO ANSWER TWICE AND DIFFERENTLY, IN ONE DOCUMENT.
#
# Measured on 2026.8.26.1 over an openkal project:
#
#     "cLibrary": { "origin": "payload", "path": "…/xim-x-glibc/2.44/lib64" }
#     "layers":   [ { "layer": "c-abi", "interface": "musl",
#                     "impl": "openkal-musl@0.3.5", "origin": "graph" } ]
#
# The artifact settles it — statically linked, no interpreter, no `DT_NEEDED`,
# eleven openkal symbols — so glibc is not in it. Both fields were accurate
# about different questions: `cLibrary` describes the PAYLOAD's link model,
# `layers[].c-abi` describes the BUILD. A consumer had no way to tell which one
# governed, which is a machine interface contradicting itself.
#
# ⭐ ONE FIELD ADDED, NONE CHANGED. docs/11 §6 promises that fields are added
# and never removed and that a field's meaning never changes; renaming
# `cLibrary` or widening `mode` would break that for a document whose whole
# point is to be depended on.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf 'extern "C" int main(int, char**, char**) { return 0; }\n' > src/main.cpp

probe() {   # → "<cabi-origin> <suppliesTarget>"
    "$MCPP" why toolchain --format json 2>/dev/null | tr -d '\r' \
      | jq -r '[(.data.layers[] | select(.layer=="c-abi") | .origin),
                (.data.cLibrary.suppliesTarget | tostring)] | @tsv'
}

# ── Half one: the field exists at all ────────────────────────────────────
#
# ⚠️ A `null` HERE IS NOT A FAILING ASSERTION, IT IS AN ABSENT ONE. Without
# this check the comparisons below would read "null" on both sides and agree.
printf '[package]\nname    = "clibprobe"\nversion = "0.1.0"\n' > mcpp.toml
raw="$("$MCPP" why toolchain --format json 2>/dev/null | tr -d '\r' \
       | jq -r '.data.cLibrary.suppliesTarget // "MISSING"')"
if [ "$raw" = MISSING ]; then
    echo "FAIL: cLibrary.suppliesTarget is absent — the document still cannot say"
    echo "      which of its two C-library answers governs"
    exit 1
fi
echo "  ok  the query says which of its two C-library answers governs"

# ── Half two: with no graph, the payload governs ─────────────────────────
read -r origin supplies <<EOF
$(probe)
EOF
if [ "$origin" = payload ] && [ "$supplies" = true ]; then
    echo "  ok  with no dependency graph the payload supplies the C library"
else
    echo "FAIL: no graph, yet c-abi origin '$origin' and suppliesTarget '$supplies'"
    exit 1
fi

# ── Half three: with a graph, it says so ─────────────────────────────────
#
# ⭐ THE ROW IS BUILT FROM A LOCAL PACKAGE, NOT FROM THE INDEX. A test whose
# subject is "the document agrees with itself" must not also depend on a
# network fetch: the skip that produces is indistinguishable from a pass.
mkdir -p "$work/libc/src"
printf '[package]\nname     = "fake-libc"\nversion  = "0.1.0"\nprovides = ["mcpp:c-abi=musl"]\n' \
    > "$work/libc/mcpp.toml"
printf 'export module fake_libc;\nexport int fl() { return 0; }\n' \
    > "$work/libc/src/fake_libc.cppm"
# ⚠️ `libc`, NOT `../libc`. The root package IS `$work`, so `../libc` points
# outside the fixture — measured, the dependency did not resolve, `data.layers`
# came back null, and the skip below reported "no contradiction to check".
printf '[package]\nname    = "clibprobe"\nversion = "0.1.0"\n\n[dependencies]\nfake-libc = { path = "libc" }\n' \
    > mcpp.toml

# ⚠️⚠️ A REFUSAL MUST NOT BE READ AS "NOTHING TO CHECK". The first draft went
# straight to the skip when `origin2` was empty — and empty is what a FAILED
# query produces, not only an inapplicable one. A criterion whose "no" and whose
# "could not measure" print the same line asserts nothing.
reason2="$("$MCPP" why toolchain --format json 2>/dev/null | tr -d '\r' \
           | jq -r '.data.reason // "QUERY-FAILED"')"
if [ "$reason2" != none ]; then
    echo "FAIL: the query refused ('$reason2') where it had to answer"
    "$MCPP" why toolchain --format json 2>/dev/null | tr -d '\r' \
      | jq -r '.diagnostics[].message' | sed 's/^/        /'
    exit 1
fi
read -r origin2 supplies2 <<EOF
$(probe)
EOF
if [ "$origin2" != graph ]; then
    echo "FAIL: a package declaring \`provides = [\"mcpp:c-abi=musl\"]\` did not"
    echo "      take the c-abi layer (origin '$origin2')"
    exit 1
fi
if [ "$supplies2" = false ]; then
    echo "  ok  and when the graph supplies it, the payload model says it does not"
else
    echo "FAIL: c-abi comes from the graph, yet cLibrary claims to supply the target"
    "$MCPP" why toolchain --format json 2>/dev/null | tr -d '\r' \
      | jq -c '{cLibrary:.data.cLibrary,
                cabi:(.data.layers[]|select(.layer=="c-abi"))}' | sed 's/^/        /'
    exit 1
fi

echo "OK: the query gives one answer for the C library"
