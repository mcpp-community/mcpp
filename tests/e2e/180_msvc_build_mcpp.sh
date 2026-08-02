#!/usr/bin/env bash
# requires: windows msvc
# 180_msvc_build_mcpp.sh — build.mcpp works under a native MSVC toolchain
#
# `MSVC x build.mcpp` was an empty cell in the CI matrix, and the feature was
# correspondingly at zero: `grep -i msvc` over build_program.cppm hit only
# comments. Three separate layers had to be fixed, and each only becomes
# visible once the previous one is gone — the argv[0] quoting, the GNU-only
# flag spellings, and the missing INCLUDE/LIB environment. A test that only
# checked "does it build" would have passed on any one of them being fixed,
# so this checks the produced effect instead.
set -e

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT

cd "$TMP"
"$MCPP" toolchain default msvc >/dev/null 2>&1 \
    || { echo "FAIL: msvc@system not selectable"; exit 1; }

"$MCPP" new msvc_bp >/dev/null 2>&1
cd msvc_bp

# 1) An #include-based build.mcpp — the supported shape under MSVC. It writes
#    a file AND emits a define, so a pass requires the helper to have compiled,
#    linked, and actually run.
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::FILE* f = std::fopen("helper-ran", "w");
    if (!f) return 2;
    std::fputs("ok\n", f);
    std::fclose(f);
    std::puts("mcpp:cfg=MSVC_BUILD_PROGRAM_RAN");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MSVC_BUILD_PROGRAM_RAN
    std::println("msvc-build-mcpp-ok");
    return 0;
#else
    std::println("define missing");
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: msvc build with build.mcpp: $out"; exit 1; }
[[ -f helper-ran ]] || { echo "FAIL: build.mcpp did not run under MSVC: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"msvc-build-mcpp-ok"* ]] \
    || { echo "FAIL: run output: $run_out"; exit 1; }

# 2) `mcpp:link-lib` must be spelled the MSVC way. Naming a library that is
#    always present in the SDK proves the translation reached the linker:
#    the GNU spelling `-ladvapi32` would be an unknown option, then LNK1181.
cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    std::puts("mcpp:link-lib=advapi32");
    std::puts("mcpp:cfg=MSVC_LINK_LIB_OK");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MSVC_LINK_LIB_OK
    std::println("msvc-link-lib-ok");
    return 0;
#else
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: msvc link-lib translation: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run (link-lib): $run_out"; exit 1; }
[[ "$run_out" == *"msvc-link-lib-ok"* ]] \
    || { echo "FAIL: run output (link-lib): $run_out"; exit 1; }

# 3) Named modules under cl.exe. build.mcpp is "one host C++ program", and the
#    main build has compiled those with modules under cl.exe for a while
#    (e2e 99 produces real .ifc artifacts) — so build.mcpp must too. It used to
#    refuse, because it hand-rolled its own compile path instead of reading the
#    shared BmiTraits/CommandDialect rows.
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    std::string tag = std::format("MSVC_MODULES_{}", 1 + 1);
    mcpp::define(tag.c_str());
    mcpp::rerun_if_changed("build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MSVC_MODULES_2
    std::println("msvc-modules-ok");
    return 0;
#else
    std::println("define missing");
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: msvc build.mcpp with modules: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run (modules): $run_out"; exit 1; }
[[ "$run_out" == *"msvc-modules-ok"* ]] \
    || { echo "FAIL: run output (modules): $run_out"; exit 1; }

# The .ifc really came from the msvc module pipeline, not a silent fallback.
find target/.build-mcpp -name "*.ifc" | grep -q . \
    || { echo "FAIL: no .ifc produced for the bundled mcpp module"; exit 1; }

echo "PASS: MSVC build.mcpp — include path, link-lib translation, named modules"
