#!/usr/bin/env bash
# 646_runtime_deploy_places_files_in_a_directory.sh -- `runtime.deploy` (#615)
# places a runtime file in a directory relative to the executable.
#
# `deploy_files` flattens every entry into the executable's directory, and a
# loader that reads a fixed subdirectory (the Vulkan loader on macOS reads
# `<executable dir>/vulkan/icd.d`) never finds a flattened copy. Asserted:
#   1. the root's entries land at bin/<to>/<file>, and `to = "."` places the
#      file beside the executable; a test binary finds the same layout;
#   2. a dependency's entry resolves `from` against the dependency and lands in
#      the consumer's bin/<to>;
#   3. one file name in two directories is not a collision, and two sources for
#      one destination are refused naming the destination;
#   4. a destination that leaves the executable's directory is refused naming
#      the entry.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# ── A dependency that deploys into a subdirectory ─────────────────────────
mkdir -p "$TMP/icd/share/vulkan/icd.d" "$TMP/icd/src"
cd "$TMP/icd"
printf '{"ICD": {"library_path": "libvulkan_lvp.so"}}\n' > share/vulkan/icd.d/lvp_icd.json
printf 'export module icd;\nexport int icd_value() { return 3; }\n' > src/icd.cppm
cat > mcpp.toml <<'TOML'
[package]
name    = "icd"
version = "0.1.0"

[targets.icd]
kind = "lib"

[runtime]
deploy = [ { from = "share/vulkan/icd.d/lvp_icd.json", to = "vulkan/icd.d" } ]
TOML

# ── The consumer ───────────────────────────────────────────────────────────
mkdir -p "$TMP/app/src" "$TMP/app/assets/layers"
cd "$TMP/app"
printf 'root readme\n' > assets/readme.txt
printf 'layer manifest\n' > assets/layers/lvp_icd.json
cat > src/main.cpp <<'CPP'
import icd;
int main() { return icd_value() == 3 ? 0 : 1; }
CPP

write_manifest() {   # $1 = the entries of the root's `runtime.deploy`
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[dependencies]
icd = { path = "../icd" }

[runtime]
deploy = [ $1 ]
TOML
}

# ── 1, 2, and the first half of 3 ─────────────────────────────────────────
write_manifest '{ from = "assets/readme.txt", to = "." }, { from = "assets/layers/lvp_icd.json", to = "layers" }'
"$MCPP" build > build.log 2>&1 || fail "the build failed" build.log
exe=$(find target -type f \( -name app -o -name app.exe \) -path '*/bin/*' | head -1)
[ -n "$exe" ] || fail "no executable under target/" build.log
bin=$(dirname "$exe")
[ -f "$bin/readme.txt" ] || fail "to = \".\" did not place readme.txt beside the executable" build.log
grep -q 'layer manifest' "$bin/layers/lvp_icd.json" 2>/dev/null \
    || fail "bin/layers/lvp_icd.json is missing or is not the root's file" build.log
grep -q 'library_path' "$bin/vulkan/icd.d/lvp_icd.json" 2>/dev/null \
    || fail "bin/vulkan/icd.d/lvp_icd.json is missing or is not the dependency's file" build.log
[ ! -e "$bin/lvp_icd.json" ] || fail "an entry with a directory was also flattened beside the executable" build.log
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
echo "placement OK"

# The test binaries see the same layout beside themselves.
mkdir -p tests
cat > tests/layout.cpp <<'CPP'
#include <filesystem>
int main(int, char** argv) {
    const auto dir = std::filesystem::absolute(argv[0]).parent_path();
    return std::filesystem::exists(dir / "vulkan" / "icd.d" / "lvp_icd.json")
        && std::filesystem::exists(dir / "layers" / "lvp_icd.json") ? 0 : 1;
}
CPP
"$MCPP" test > test.log 2>&1 \
    || fail "a test binary did not find the deployed layout beside itself" test.log
rm -rf tests
echo "test layout OK"

# ── 3. Two sources for one destination ────────────────────────────────────
write_manifest '{ from = "assets/layers/lvp_icd.json", to = "vulkan/icd.d" }'
if "$MCPP" build > collision.log 2>&1; then
    fail "two sources for bin/vulkan/icd.d/lvp_icd.json were accepted" collision.log
fi
grep -Eq "runtime deploy collision: .* both target 'bin.vulkan.icd\.d.lvp_icd\.json'" collision.log \
    || fail "the collision is not refused naming the destination" collision.log
echo "collision OK"

# ── 4. A destination outside the executable's directory ───────────────────
write_manifest '{ from = "assets/readme.txt", to = "../outside" }'
if "$MCPP" build > escape.log 2>&1; then
    fail "a destination outside the executable's directory was accepted" escape.log
fi
grep -Fq 'runtime.deploy[1]: `to` has a `.` or `..` component' escape.log \
    || fail "the refusal does not name the entry and the component" escape.log
echo "escape refusal OK"
