#!/usr/bin/env bash
# 698 -- a source that a package's build program takes from under its
# dependency's root has an object address that does not grow with the distance
# between the two packages, and a tool built that way through `tools = [...]`
# builds and runs (#641, item 3).
#
# The reported failure was on windows-2022: a host tool's build program added
# `mcpp::source("<dependency root>/platform/windows/x.cpp")`, the object
# address mirrored the path from the tool package to the file with one `__up`
# per level, the `.ddi` path reached 271 characters inside the tool store's
# sub-build scratch, and `mcpp dyndep` could not open it. Three changes answer
# it: the address is `obj/<declaring>/__pkg/<owner>/<path inside the owner>`,
# the sub-build scratch is `<cache>/tool/.build/<hash>`, and the engine's
# ninja-invoked subcommands open extended-length paths on Windows.
#
#   A  the address names the owning package, has no `__up`, and is the same
#      when the declaring package sits two directories deeper (portable);
#   B  a consumer builds the package as a host tool and runs its output;
#   C  the same with the file nested deep inside the dependency, so that on a
#      Windows runner the longest sub-build path passes 260 characters; the
#      build must succeed wherever the runner enables long paths, and the
#      readings say which case this runner is.
#
# No `# requires:` line: the host's default toolchain builds every leg.
set -e

source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

is_windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) is_windows=1 ;; esac

# $1 = dependency directory, $2 = directory inside it that holds the helper
write_dep() {
    mkdir -p "$1/src" "$1/$2"
    cat > "$1/mcpp.toml" <<'EOF'
[package]
name    = "dep"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.dep]
kind = "lib"
EOF
    echo 'int dep_base() { return 40; }' > "$1/src/dep.cpp"
    echo 'int dep_base(); int helper_answer() { return dep_base() + 2; }' \
        > "$1/$2/helper.cpp"
}

# $1 = installer directory, $2 = path to the dependency from it,
# $3 = directory of the helper inside the dependency
write_installer() {
    local DEP_HOST
    DEP_HOST=$(host_path "$2")
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "installer"
version = "0.1.0"

[dependencies]
dep = { path = "$DEP_HOST" }

[targets.installer-app]
kind = "bin"
main = "src/main.cpp"
EOF
    cat > "$1/src/main.cpp" <<'EOF'
#include <cstdio>
int helper_answer();
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("HELPER=%d\n", helper_answer()); return 0; }
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", helper_answer());
    std::fclose(f);
    return 0;
}
EOF
    cat > "$1/build.mcpp" <<EOF
#include <string>
import mcpp;
int main() {
    const char* dep = mcpp::dep_dir("dep");
    if (!dep || !*dep) return 1;
    mcpp::source((std::string(dep) + "/$3/helper.cpp").c_str());
}
EOF
}

# $1 = consumer directory, $2 = path to the installer from it
write_app() {
    local INSTALLER_HOST
    INSTALLER_HOST=$(host_path "$2")
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
installer = { path = "$INSTALLER_HOST", tools = ["installer-app"] }
EOF
    cat > "$1/src/main.cpp" <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
    cat > "$1/build.mcpp" <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("installer", "installer-app");
    if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
#ifdef _WIN32
    // cmd.exe strips the first and the last quote of the string it is given,
    // which splits two quoted words; a further pair keeps both intact.
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    mcpp::generated(out.c_str());
}
EOF
}

# The object address of the helper, read from the plan ninja was given.
helper_address() {   # $1 = package directory
    local ninja
    ninja=$(ls "$1"/target/*/*/build.ninja 2>/dev/null | head -1)
    [ -n "$ninja" ] || return 1
    grep -o 'obj/[^ :|]*helper\.o\(bj\)\?' "$ninja" | head -1
}

# ── A: the address ──────────────────────────────────────────────────────────
write_dep fw/dep platform/impl
write_installer fw/tools/installer ../../dep platform/impl
(cd fw/tools/installer && "$MCPP" build > "$TMP/a1.log" 2>&1) \
    || fail "the installer did not build at its own root" a1.log
near=$(helper_address fw/tools/installer) || fail "no build.ninja for the installer" a1.log
[ -n "$near" ] || fail "no object address for helper.cpp in the installer's plan" a1.log
echo "READING A address: $near"
case "$near" in
    *__up*) fail "the address climbs with __up: $near" ;;
esac
case "$near" in
    # The owner's slug is its qualified name (`mcpplibs.dep` for a path
    # package that states no namespace), spelled with `_`.
    obj/installer/__pkg/*dep/platform/impl/helper.o|obj/installer/__pkg/*dep/platform/impl/helper.obj) ;;
    *) fail "the address does not name the owning package and the path inside it: $near" ;;
esac

write_installer fw/tools/windows/nested/installer ../../../../dep platform/impl
(cd fw/tools/windows/nested/installer && "$MCPP" build > "$TMP/a2.log" 2>&1) \
    || fail "the deeper installer did not build" a2.log
deeper=$(helper_address fw/tools/windows/nested/installer)
echo "READING A deeper address: $deeper"
[ "$deeper" = "$near" ] || fail "moving the installer two levels deeper changed the address: $near -> $deeper"
out=$(cd fw/tools/windows/nested/installer && "$MCPP" run 2>&1 | grep '^HELPER=' | tail -1)
[ "$out" = "HELPER=42" ] || fail "the installer does not run its dependency's helper: $out"
echo "ok: the address names the owning package and does not grow with distance"

# ── B: through `tools = [...]` ─────────────────────────────────────────────
write_app app ../fw/tools/windows/nested/installer
(cd app && "$MCPP" build > "$TMP/b.log" 2>&1) || fail "the consumer did not build the tool" b.log
grep -q "host tool" b.log || fail "no host tool was built" b.log
out=$(cd app && "$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)
[ "$out" = "ANSWER=42" ] || fail "the tool's output did not reach the consumer: $out" b.log
echo "ok: the tool builds through tools = [...] and its output is linked"

# ── C: a deep file inside the dependency ───────────────────────────────────
deep="platform/windows/installer_interface_components/generated_resources_and_views/bootstrapper_application_integration"
write_dep deep/dep "$deep"
write_installer deep/tools/installer ../../dep "$deep"
write_app deep/app ../tools/installer

cache_dir=$("$MCPP" self env 2>/dev/null | sed -n 's/^build cache *= *//p' | head -1)
(cd deep/tools/installer && "$MCPP" build > "$TMP/c0.log" 2>&1) \
    || fail "the deep installer did not build at its own root" c0.log
deep_addr=$(helper_address deep/tools/installer)
deep_build=$(ls -d deep/tools/installer/target/*/*/ | head -1)
triple_fp=${deep_build#deep/tools/installer/}
# `<cache>/tool/.build/<16 hex>/` + `target/<triple>/<fingerprint>/` + the
# address and the scan output beside it (`<source>.ddi`).
scratch_tail="/tool/.build/0123456789abcdef/"
longest=$(( ${#cache_dir} + ${#scratch_tail} + ${#triple_fp} + ${#deep_addr} + 8 ))
echo "READING C cache directory: $cache_dir"
echo "READING C longest sub-build path (estimated, characters): $longest"

long_paths=unknown
if [ "$is_windows" = 1 ]; then
    long_paths=$(powershell -NoProfile -Command \
        "(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem').LongPathsEnabled" \
        2>/dev/null | tr -d '\r' || true)
    [ -n "$long_paths" ] || long_paths=unreadable
fi
echo "READING C LongPathsEnabled: $long_paths"

set +e
(cd deep/app && "$MCPP" build > "$TMP/c.log" 2>&1)
rc=$?
set -e
echo "READING C deep tool build exit: $rc"
if [ "$rc" = 0 ]; then
    out=$(cd deep/app && "$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)
    [ "$out" = "ANSWER=42" ] || fail "the deep tool's output did not reach the consumer: $out" c.log
    echo "ok: a tool whose sub-build paths are $longest characters long builds"
elif [ "$is_windows" = 1 ] && [ "$long_paths" != 1 ]; then
    echo "READING C the runner does not enable long paths; the failure is recorded, not asserted"
    grep -m5 -E "error|failed" c.log | sed 's/^/  /'
else
    fail "the deep tool did not build" c.log
fi

echo "PASS: 698"
