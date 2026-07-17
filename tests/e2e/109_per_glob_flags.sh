#!/usr/bin/env bash
# requires: gcc
# G4: [build] flags = [{ glob = "...", ... }] — per-glob compile flags.
# Matched TUs get the flags (probed via -D), unmatched TUs don't, later
# entries override earlier ones ("last flag wins"), zero-hit globs warn.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new globf > /dev/null
cd globf

cat > src/tagged.cpp <<'EOF'
#ifndef TAG
#error "per-glob define did not reach tagged.cpp"
#endif
extern "C" int tag_value() { return TAG; }
EOF

cat > src/plain.cpp <<'EOF'
#ifdef TAG
#error "per-glob define leaked into plain.cpp"
#endif
extern "C" int plain_ok() { return 1; }
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int tag_value();
extern "C" int plain_ok();
int main() {
    std::println("tag = {}", tag_value() + plain_ok());
    return 0;
}
EOF

# Two overlapping globs: the later, more specific entry must win (-DTAG=2
# appended after -DTAG=1; GNU last-wins → 2). Plus a zero-hit glob → warning.
cat > mcpp.toml <<'EOF'
[package]
name    = "globf"
version = "0.1.0"

[build]
flags = [
  { glob = "src/tag*.cpp", cxxflags = ["-DTAG=1"] },
  { glob = "src/tagged.cpp", cxxflags = ["-DTAG=2"] },
  { glob = "src/never/**", cxxflags = ["-DNOPE"] },
]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
grep -q "matched no source file" build.log || {
    cat build.log; echo "missing zero-hit glob warning"; exit 1; }
out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "tag = 3" ]] || { echo "unexpected output: $out (want later-entry override → 2+1)"; exit 1; }

echo "OK"
