#!/usr/bin/env bash
# requires: gcc
# 958 -- a build program's cached run is replayed only while what it wrote is
# there. The program writes a header below `MCPP_OUT_DIR` and names the
# directory with `mcpp::include_dir`; once that directory is deleted, a replay
# would hand the compile an include directory with nothing in it. The program
# runs again instead (measured 2026-10-28 on openkal-image's build program: a
# deleted output directory, then generated includes missing).
#
# Criteria:
#   A. A second build replays the program (cached) and compiles nothing.
#   B. With the program's output deleted, the build runs the program again and
#      the unit finds its header.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > build.mcpp <<'CPP'
#include <filesystem>
#include <fstream>
#include <string>
import mcpp;
int main() {
    const std::string inc = std::string(mcpp::out_dir()) + "/inc";
    std::filesystem::create_directories(inc);
    std::ofstream(inc + "/generated.h") << "#define GENERATED 7\n";
    mcpp::include_dir(inc.c_str());
    return 0;
}
CPP
printf '#include <generated.h>\n#include <cstdio>\nint main() { std::printf("generated %%d\\n", GENERATED); return 0; }\n' > src/main.cpp
printf '[package]\nname    = "regen"\nversion = "0.1.0"\n' > mcpp.toml

"$MCPP" run > a1.log 2>&1 || fail "A: the first build" a1.log
grep -q "generated 7" a1.log || fail "A: the header did not arrive" a1.log
"$MCPP" build -v > a2.log 2>&1 || fail "A: the second build" a2.log
grep -q "Compiling regen" a2.log && fail "A: the second build compiled" a2.log
echo "ok: a second build replays the program"

# B
out=$(find target -type f -name generated.h | head -1)
[ -n "$out" ] || fail "B: the generated header is not under target/"
rm -rf "$(dirname "$out")"
rm -rf target/*/*/bin target/*/*/obj
"$MCPP" run -v > b.log 2>&1 || fail "B: the build after the output was deleted" b.log
grep -q "generated 7" b.log || fail "B: the program did not run again" b.log
echo "ok: the program runs again when its output is gone"

echo "PASS: 958 a build program runs again when its output is gone"
