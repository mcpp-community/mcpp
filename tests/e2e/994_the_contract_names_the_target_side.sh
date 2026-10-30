#!/usr/bin/env bash
# requires: llvm
# 994 -- the build's contract (`${mcpp.contract}`, docs/30) states the target
# side every unit receives: for an ok image (`<arch>-openkal`), the platform's
# layers' headers (the C library's, the C++ runtime's), the engine's
# predefines (`__OPENKAL__`) and the realised C environment. A foreign build
# compiles for the target with the contract alone (the clangd ok image's
# LLVM build, design 2026-10-30 part 2 §25.3 C2).
#
# Criteria:
#   A. The contract's C++ words name `-D__OPENKAL__` and an `-isystem`
#      directory holding the C library's <stdio.h>.
#   B. A C++ unit that includes <vector> and <cstdio> compiles with the
#      contract's C++ program and words (and a standard of its own) alone.
#   C. A C unit that includes <stdio.h> compiles with the C program and words.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac

cd "$TMP"
mkdir -p app/src
printf '[package]\nname    = "ct"\nversion = "0.1.0"\n' > app/mcpp.toml
printf 'int main() { return 0; }\n' > app/src/main.cpp
(cd app && "$MCPP" build --target "$ARCH-openkal") > b.log 2>&1 || fail "the build" b.log
contract=$(find app/target/"$ARCH"-openkal -maxdepth 2 -name mcpp-contract.json | head -1)
[ -n "$contract" ] || fail "no contract" b.log

# A
python3 - "$contract" <<'EOF' || fail "A: the target side is not in the contract" "$contract"
import json, os, sys
d = json.load(open(sys.argv[1]))
cxx = d["compile"]["cxx"]["args"]
assert "-D__OPENKAL__" in cxx, cxx
dirs = [cxx[i + 1] for i, w in enumerate(cxx[:-1]) if w == "-isystem"]
assert any(os.path.isfile(os.path.join(p, "stdio.h")) for p in dirs), dirs
EOF
echo "ok: the contract names the target side"

# B, C
printf '#include <vector>\n#include <cstdio>\nint f() { std::vector<int> v{1, 2}; std::printf("%%zu", v.size()); return 0; }\n' > u.cpp
printf '#include <stdio.h>\nint g(void) { return puts("x"); }\n' > u.c
python3 - "$contract" > run.log 2>&1 <<'EOF' || fail "B/C: a unit did not compile with the contract alone" run.log
import json, subprocess, sys
d = json.load(open(sys.argv[1]))
for lang, unit, std in (("cxx", "u.cpp", "-std=c++20"), ("c", "u.c", "-std=c11")):
    t = d["compile"][lang]
    argv = [t["program"], *t["args"], std, "-c", unit, "-o", unit + ".o"]
    r = subprocess.run(argv, capture_output=True, text=True)
    print(lang, r.returncode, r.stderr[-2000:])
    assert r.returncode == 0, lang
EOF
echo "ok: units compile with the contract alone"

echo "OK"
