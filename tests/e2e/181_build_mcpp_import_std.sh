#!/usr/bin/env bash
# requires: unix-shell
# 181_build_mcpp_import_std.sh — build.mcpp can `import std;`
#
# mcpp asks projects to `import std;` everywhere, then made their build script
# fall back to `#include` — there was no std BMI channel in the build.mcpp
# compile at all. This locks the gap shut. Cross-platform on purpose:
# `import std;` is not a Windows-specific concern, and running it on Linux
# gives the fastest feedback.
set -e

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
"$MCPP" new imp_std >/dev/null 2>&1
cd imp_std

# 1) `import std;` alone — the container/algorithm/format surface a real build
#    script reaches for, none of which is available without the std module.
cat > build.mcpp <<'EOF'
import std;
int main() {
    std::vector<std::string> defines{"MCPP_FROM_IMPORT_STD", "MCPP_STD_COUNT_2"};
    std::ranges::sort(defines);
    for (auto const& d : defines) std::println("mcpp:cfg={}", d);
    std::println("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#if defined(MCPP_FROM_IMPORT_STD) && defined(MCPP_STD_COUNT_2)
    std::println("import-std-ok");
    return 0;
#else
    std::println("defines missing");
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: build with import std: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"import-std-ok"* ]] \
    || { echo "FAIL: run output: $run_out"; exit 1; }

# 2) `import std;` together with `import mcpp;` — both module channels active
#    at once. These share the staged-BMI cwd, and an implementation that
#    handles them as two independent conditions gets the cwd wrong for one.
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    std::string tag = std::format("MCPP_BOTH_{}", 1 + 1);
    mcpp::define(tag.c_str());
    mcpp::rerun_if_changed("build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MCPP_BOTH_2
    std::println("both-modules-ok");
    return 0;
#else
    std::println("define missing");
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: build with import std + mcpp: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run (both): $run_out"; exit 1; }
[[ "$run_out" == *"both-modules-ok"* ]] \
    || { echo "FAIL: run output (both): $run_out"; exit 1; }

# 3) An `#include`-only build.mcpp must still take the plain path — no std BMI
#    staged, no -fmodules, cwd = project root. Regression guard: the naive
#    detector ("does the text contain 'import std'") would fire on a comment.
cat > build.mcpp <<'EOF'
#include <cstdio>
// This program deliberately mentions import std; in a comment.
int main() {
    std::puts("mcpp:cfg=MCPP_PLAIN_PATH");
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MCPP_PLAIN_PATH
    std::println("plain-path-ok");
    return 0;
#else
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: build plain build.mcpp: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run (plain): $run_out"; exit 1; }
[[ "$run_out" == *"plain-path-ok"* ]] \
    || { echo "FAIL: run output (plain): $run_out"; exit 1; }

echo "PASS: build.mcpp import std (alone, with import mcpp, and the plain path)"
