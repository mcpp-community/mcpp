#!/usr/bin/env bash
# requires: gcc
# 1036 -- a `mcpp::context` states the engine's values to the interface's accessors in-process
# (docs/30 "Testing a program's logic").
#
# Every accessor of the build-program interface reads the engine's values through one function:
# within a context, the values it states, the innermost context first, and a name it does not
# state from the context around it or the environment; outside every context, the environment.
#
# Criteria: a build program that states contexts reads their values, nested and restored, and
# `has_feature` and `dep_dir` answer within them; the build succeeds and its program prints what the
# build program read.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new ctx > /dev/null
cd ctx
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const std::string os = mcpp::target_os();
    std::string seen;
    {
        mcpp::context outer({{"MCPP_TARGET_OS", "plan9"}, {"MCPP_FEATURE_FAST", "1"}});
        seen += mcpp::target_os();
        seen += mcpp::has_feature("fast") ? " fast" : " slow";
        {
            mcpp::context inner({{"MCPP_PROFILE", "inner"}, {"MCPP_DEP_ZLIB_DIR", "/z"}});
            seen += std::format(" {} {} {}", mcpp::target_os(), mcpp::profile(), mcpp::dep_dir("zlib"));
        }
        seen += std::format(" {}", std::string(mcpp::profile()) == "inner" ? "leaked" : "restored");
    }
    seen += std::string(mcpp::target_os()) == os ? " environment" : " leaked";
    std::ofstream("src/seen.cpp") << "extern \"C\" const char* seen() { return \"" << seen << "\"; }\n";
    mcpp::generated("src/seen.cpp");
    return 0;
}
EOF
cat > src/main.cpp <<'EOF'
import std;
extern "C" const char* seen();
int main() { std::println("{}", seen()); return 0; }
EOF
"$MCPP" run > run.log 2>&1 || fail "the build or the program failed" run.log
grep -q "plan9 fast plan9 inner /z restored environment" run.log || fail "the values in scope" run.log
echo "PASS: 1036 a context states the engine's values in-process"
