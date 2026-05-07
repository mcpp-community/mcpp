#!/usr/bin/env bash
# emit xpkg: produce valid Lua entry from mcpp.toml
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new emit-test > /dev/null
cd emit-test

cat > src/foo.cppm <<'EOF'
export module emit-test;
import std;
export int answer() { return 42; }
EOF

# Wait — module name "emit-test" has a hyphen which is fine, but "emit-test" is treated
# as one logical module. Manifest's package name "emit-test" has no '.' so it's treated
# as local-flat. So the module need not be prefixed. Good.

out=$("$MCPP" emit xpkg)
[[ "$out" == *"AUTO-GENERATED"*       ]] || { echo "missing AUTO-GENERATED header: $out"; exit 1; }
[[ "$out" == *"name = \"emit-test\""* ]] || { echo "missing name: $out"; exit 1; }
[[ "$out" == *"\"emit-test\""*        ]] || { echo "missing module entry: $out"; exit 1; }
[[ "$out" == *"manifest = \"mcpp.toml\""* ]] || { echo "missing manifest pointer"; exit 1; }
[[ "$out" == *"import_std = true"*    ]] || { echo "missing import_std flag: $out"; exit 1; }

# --output flag
"$MCPP" emit xpkg --output myout.lua > /dev/null
[[ -f myout.lua ]] || { echo "--output didn't write file"; exit 1; }

# Bare Lua syntax check via lua interpreter if present
if command -v lua >/dev/null 2>&1; then
    lua -e "loadfile('myout.lua')()" || { echo "Lua failed to load output"; exit 1; }
fi

echo "OK"
