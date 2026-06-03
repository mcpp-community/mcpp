#!/usr/bin/env bash
# 68_profile_passthrough.sh — build profiles: built-in release/dev/dist knobs
# plus the [profile.<name>] passthrough escape hatch (cflags/cxxflags/ldflags).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new profapp > /dev/null
cd profapp
cat >> mcpp.toml <<'EOF'

[profile.dist]
opt      = 3
strip    = true
cxxflags = ["-fno-plt"]
EOF

# Built-in release default: -O2, no -g.
"$MCPP" build --verbose > rel.log 2>&1 || { cat rel.log; echo "release build failed"; exit 1; }
grep -q -- "-O2" rel.log || { echo "release missing -O2"; exit 1; }

# dev: -O0 -g.
rm -rf target
"$MCPP" build --profile dev --verbose > dev.log 2>&1 || { cat dev.log; echo "dev build failed"; exit 1; }
grep -q -- "-O0" dev.log || { echo "dev missing -O0"; exit 1; }
grep -q -- "-g"  dev.log || { echo "dev missing -g"; exit 1; }

# dist from [profile.dist]: -O3 -flto + passthrough cxxflag, stripped binary.
rm -rf target
"$MCPP" build --profile dist --verbose > dist.log 2>&1 || { cat dist.log; echo "dist build failed"; exit 1; }
grep -q -- "-O3"      dist.log || { echo "dist missing -O3"; exit 1; }
grep -q -- "-fno-plt" dist.log || { echo "dist missing passthrough -fno-plt"; exit 1; }

binary=$(find target \( -name profapp -o -name profapp.exe \) -type f | head -1)
[[ -n "$binary" ]] || { echo "no binary"; exit 1; }
"$binary" > /dev/null || { echo "dist binary does not run"; exit 1; }

echo "OK"
