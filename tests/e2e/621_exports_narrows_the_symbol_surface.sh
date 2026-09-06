#!/usr/bin/env bash
# requires: elf gcc
# A shared library publishes what `exports` names, and everything otherwise.
#
# The default on both platforms is "everything": ELF gives symbols default
# visibility, and PE gets an auto-generated .def listing every symbol. Narrowing
# it is what a runtime with a stable ABI needs, and what a plugin loaded beside
# its rivals needs -- an ICD exporting its internals collides with the loader
# and with the other ICDs in the same process.
#
# THE CRITERION IS TWO-SIDED, AND BOTH SIDES ARE ASSERTED. Checking only that
# the public symbol is present would pass for a library exporting everything,
# which is the state before this feature. Checking only that the internal one is
# absent cannot tell "correctly hidden" from "never linked at all" -- so the
# same source is built twice, once with the key and once without, and the two
# readings must differ.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

mkdir -p src abi
cat > src/lib.cpp <<'CPP'
extern "C" int mcpp_e2e_621_public(int x)   { return x + 1; }
extern "C" int mcpp_e2e_621_internal(int x) { return x + 2; }
CPP

cat > abi/lib.exports <<'EXPORTS'
# One symbol pattern per line; `#` starts a comment.
mcpp_e2e_621_public
EXPORTS

emit_manifest() {
cat > mcpp.toml <<TOML
[package]
name    = "expsurface"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.expsurface]
kind   = "shared"
soname = "libexpsurface.so.1"
$1
TOML
}

dynsyms() {
  local so
  so=$(find target -name 'libexpsurface*.so*' -type f | head -1)
  [ -n "$so" ] || { echo "FAIL: no shared library was produced"; exit 1; }
  nm -D --defined-only "$so" 2>/dev/null | awk '{print $NF}' | grep '^mcpp_e2e_621' | sort
}

# leg 1: no `exports` -- both symbols are published
emit_manifest ""
rm -rf target
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: the default build failed"; exit 1; }
before=$(dynsyms)
echo "default:  $(echo "$before" | tr '\n' ' ')"
case "$before" in
  *mcpp_e2e_621_public*) ;;
  *) echo "FAIL: the default build did not publish the public symbol"; exit 1 ;;
esac
case "$before" in
  *mcpp_e2e_621_internal*) ;;
  *) echo "FAIL: the default is supposed to publish everything and did not."
     echo "      Without this leg the second one proves nothing."; exit 1 ;;
esac

# leg 2: with `exports` -- only the named symbol
emit_manifest 'exports = "abi/lib.exports"'
rm -rf target
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: the build with exports failed"; exit 1; }
after=$(dynsyms)
echo "exports:  $(echo "$after" | tr '\n' ' ')"
case "$after" in
  *mcpp_e2e_621_public*) ;;
  *) echo "FAIL: the declared symbol is not published"; exit 1 ;;
esac
case "$after" in
  *mcpp_e2e_621_internal*) echo "FAIL: an undeclared symbol is still published"; exit 1 ;;
esac
[ "$before" != "$after" ] || { echo "FAIL: the two legs read identically"; exit 1; }

# the inline form is the same statement
emit_manifest 'exports = ["mcpp_e2e_621_public"]'
rm -rf target
"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: the inline form failed to build"; exit 1; }
inline=$(dynsyms)
[ "$inline" = "$after" ] || {
  echo "FAIL: the inline list and the file disagree"
  echo "  file:   $after"
  echo "  inline: $inline"; exit 1; }

echo "PASS: exports narrows the published symbol set, and the two forms agree"
