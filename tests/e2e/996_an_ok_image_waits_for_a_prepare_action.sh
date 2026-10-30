#!/usr/bin/env bash
# requires: llvm
# 996 -- an ok image whose graph declares a `prepare` action builds: its link
# edge waits for the action, and the edge that seals the image is not taken
# for a link (SPEC-013 §4.6; mcpp#702). The ordering guard read the edge that
# publishes `bin/<name>.oke` as the link and refused the build as an emitter
# defect (mcppls-clangd's clangd.okb, 2026-10-31).
#
# Criteria:
#   A. `mcpp run --target <host-arch>-openkal` builds and runs a program whose
#      build.mcpp declares a `prepare` action writing a directory.
#   B. In the manifest, the link edge waits for the package's action phony.
#   C. The image's debug information is compressed by its `ld.lld` link and
#      its unit's is beside the object (`split_debuginfo`, design 2026-10-31
#      §4.1): okld seals such an image and okrun runs it (A).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
T="$HOST_ARCH-openkal"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
cat > build.mcpp <<'CPP'
#include <string>
import mcpp;
int main() {
    const std::string dir = std::string(mcpp::out_dir()) + "/gen";
    mcpp::action a;
    a.id = "gen:dir";
    a.role = mcpp::roles::prepare;
    a.arg("sh").arg("-c").arg(("mkdir -p " + dir + " && : > " + dir + "/made && : > " + dir + ".stamp").c_str())
     .output((dir + ".stamp").c_str())
     .output_dir(dir.c_str())
     .submit();
    return 0;
}
CPP

# A
"$MCPP" run --target "$T" > run.log 2>&1 || fail "A: the image did not build and run" run.log
grep -q "internal:" run.log && fail "A: the ordering guard refused the build" run.log

# B
manifest=$(find target/"$T" -name build.ninja -not -path "*/.build-mcpp/*" | head -1)
grep -E "^build obj/platform/bin/app\.oke\.unsealed[ :].*\|\|.*mcpp-actions-app" "$manifest" > /dev/null \
    || fail "B: the link edge does not wait for the action" "$manifest"

# C
image=$(find target/"$T" -name app.oke | head -1)
if command -v readelf > /dev/null; then
    readelf -S -W "$image" | grep -E '\.debug_info' | grep -q ' C ' \
        || fail "C: the image's debug information is not compressed" <(readelf -S -W "$image")
fi
find target/"$T" -name 'main*.dwo' | grep -q . || fail "C: the image's unit has no .dwo" <(find target/"$T" -name '*.dwo')

echo "OK"
