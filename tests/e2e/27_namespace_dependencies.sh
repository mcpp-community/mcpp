#!/usr/bin/env bash
# Namespaced dependencies: `[dependencies.<ns>] name = { path = "..." }`
# is parsed correctly and the dep is actually picked up by the build.
# Also verifies that the legacy `"<ns>.<name>" = "..."` quoted form still
# round-trips through the manifest parser.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# ── 1. Sibling lib package (acme:util). Pure-modular C++23. ─────────────
mkdir -p "$TMP/util-pkg"
cd "$TMP/util-pkg"
"$MCPP" new util > /dev/null
cd util
rm -f src/main.cpp
cat > src/util.cppm <<'EOF'
export module acme.util;
import std;
export int answer() { return 42; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name        = "util"
version     = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.util]
kind = "lib"
EOF

# ── 2. Consumer that pulls util via the new namespaced subtable form. ───
mkdir -p "$TMP/app"
cd "$TMP/app"
"$MCPP" new app > /dev/null
cd app
cat > src/main.cpp <<'EOF'
import std;
import acme.util;
int main() { std::println("answer = {}", answer()); return answer() == 42 ? 0 : 1; }
EOF
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies.acme]
util = { path = "$TMP/util-pkg/util" }
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
out=$("$MCPP" run 2>&1 | tail -1)
[[ "$out" == "answer = 42" ]] || { echo "unexpected output: $out"; exit 1; }

# ── 3. Same consumer, legacy quoted-dotted form. Must still parse. ──────
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
"acme.util" = { path = "$TMP/util-pkg/util" }
EOF
rm -rf target
"$MCPP" build > build-legacy.log 2>&1 || { cat build-legacy.log; echo "legacy form build failed"; exit 1; }
out=$("$MCPP" run 2>&1 | tail -1)
[[ "$out" == "answer = 42" ]] || { echo "legacy form unexpected output: $out"; exit 1; }

# ── 4. Default-namespace flat form keeps working with no quotes. ────────
mkdir -p "$TMP/util2-pkg"
cd "$TMP/util2-pkg"
"$MCPP" new util2 > /dev/null
cd util2
rm -f src/main.cpp
cat > src/util2.cppm <<'EOF'
export module util2;
import std;
export int two() { return 2; }
EOF
cat > mcpp.toml <<'EOF'
[package]
name        = "util2"
version     = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.util2]
kind = "lib"
EOF

cd "$TMP/app/app"
cat > src/main.cpp <<'EOF'
import std;
import util2;
int main() { std::println("two = {}", two()); return two() == 2 ? 0 : 1; }
EOF
cat > mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
util2 = { path = "$TMP/util2-pkg/util2" }
EOF
rm -rf target
"$MCPP" build > build-flat.log 2>&1 || { cat build-flat.log; echo "flat form build failed"; exit 1; }
out=$("$MCPP" run 2>&1 | tail -1)
[[ "$out" == "two = 2" ]] || { echo "flat form unexpected output: $out"; exit 1; }

echo "OK"
