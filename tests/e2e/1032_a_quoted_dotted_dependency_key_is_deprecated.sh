#!/usr/bin/env bash
# 1032 -- a quoted dotted dependency key is deprecated, and the warning names the unquoted key.
#
# `"acme.lib" = ...` is one TOML key whose name contains a dot; mcpp reads it as
# the selector `acme.lib`, as it did before, and said nothing, so a project
# never learned the recommended spelling (mcpp#904). It is now the deprecated
# reading `quoted-dotted-dependency-key` (docs/51).
#
# Criterion: the quoted key still resolves and builds; the build warns, naming
# the key, the releases and the unquoted spelling; `mcpp why compat`
# lists the use; the unquoted key builds with no such warning.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/lib/src" "$TMP/app/src"
cat > "$TMP/lib/mcpp.toml" <<'TOML'
[package]
namespace = "acme"
name      = "lib"
version   = "0.1.0"
TOML
printf 'export module acme.lib;\nexport int answer() { return 42; }\n' > "$TMP/lib/src/lib.cppm"
printf 'import acme.lib;\nint main() { return answer() == 42 ? 0 : 1; }\n' > "$TMP/app/src/main.cpp"

cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\n"acme.lib" = { path = "../lib" }\n' > mcpp.toml
"$MCPP" build > quoted.log 2>&1 || fail "the quoted key no longer builds" quoted.log
grep -q '"acme.lib" is deprecated since mcpp 2026.11.8.1 and removed from mcpp 2027.2' quoted.log \
    || fail "no deprecation warning for the quoted key" quoted.log
grep -q 'mcpplibs.cmdline = "0.0.2"' quoted.log || fail "the warning does not show the unquoted spelling" quoted.log
"$MCPP" why compat > why.log 2>&1 || true
grep -q 'quoted-dotted-dependency-key: \[dependencies\] "acme.lib"' why.log || fail "mcpp why compat does not list the use" why.log

printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\nacme.lib = { path = "../lib" }\n' > mcpp.toml
"$MCPP" build > unquoted.log 2>&1 || fail "the unquoted key does not build" unquoted.log
grep -q 'quoted-dotted-dependency-key\|is deprecated since' unquoted.log && fail "the unquoted key is warned about" unquoted.log
echo "PASS: 1032 a quoted dotted dependency key is deprecated"
