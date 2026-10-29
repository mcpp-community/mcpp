#!/usr/bin/env bash
# requires: elf gcc
# #852: two dependencies whose manifests both say `rules` (namespaces openmui
# and gongfu). The bare name is no package's: it is not published, the build
# does not warn on every run, MCPP_DEP_AMBIGUOUS_RULES names the qualified
# names, and each qualified name still reaches its own directory.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

for ns in openmui gongfu; do
    mkdir -p "$ns-rules/src"
    echo "int ${ns}_rules_touch() { return 1; }" > "$ns-rules/src/r.cpp"
    cat > "$ns-rules/mcpp.toml" <<EOF
[package]
namespace = "$ns"
name      = "rules"
version   = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[targets.rules]
kind = "lib"
EOF
done

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
import std;
extern const char* seen();
int main() { std::println("{}", seen()); }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    auto env = [](const char* n) { const char* v = std::getenv(n); return std::string(v ? v : "<unset>"); };
    std::string line = "bare=" + env("MCPP_DEP_RULES_DIR")
        + " ambiguous=" + env("MCPP_DEP_AMBIGUOUS_RULES")
        + " openmui=" + env("MCPP_DEP_OPENMUI_RULES_DIR")
        + " gongfu=" + env("MCPP_DEP_GONGFU_RULES_DIR");
    std::string out = "src/seen.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "const char* seen() { return R\"(%s)\"; }\n", line.c_str());
    std::fclose(f);
    mcpp::generated("src/seen.cpp");
    return 0;
}
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
openmui.rules = { path = "../openmui-rules" }
gongfu.rules  = { path = "../gongfu-rules" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cd app
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: build failed"; exit 1; }
if grep -q "collides on" build.log; then
    cat build.log; echo "FAIL: an ambiguous name still warns"; exit 1
fi
seen="$("$MCPP" run 2>/dev/null | tail -1)"
echo "$seen"
[[ "$seen" == *"bare=<unset>"* ]] || { echo "FAIL: the bare name is bound"; exit 1; }
[[ "$seen" == *"ambiguous=gongfu.rules,openmui.rules"* || "$seen" == *"ambiguous=openmui.rules,gongfu.rules"* ]] \
    || { echo "FAIL: MCPP_DEP_AMBIGUOUS_RULES does not name both"; exit 1; }
[[ "$seen" == *"openmui="*"openmui-rules"* ]] || { echo "FAIL: openmui.rules lost its directory"; exit 1; }
[[ "$seen" == *"gongfu="*"gongfu-rules"* ]] || { echo "FAIL: gongfu.rules lost its directory"; exit 1; }

# A build program that asks for the ambiguous name ends, naming both.
cat > build.mcpp <<'EOF'
import mcpp;
int main() { return mcpp::dep_dir("rules")[0] ? 0 : 1; }
EOF
rm -f src/seen.cpp
if "$MCPP" build > ask.log 2>&1; then cat ask.log; echo "FAIL: dep_dir(\"rules\") answered"; exit 1; fi
grep -q 'dep_dir("rules") names more than one dependency' ask.log \
    || { cat ask.log; echo "FAIL: the program did not say the name is ambiguous"; exit 1; }
grep -q 'openmui.rules' ask.log && grep -q 'gongfu.rules' ask.log \
    || { cat ask.log; echo "FAIL: the refusal does not name both packages"; exit 1; }
echo "OK"
