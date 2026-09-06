#!/usr/bin/env bash
# requires: elf gcc
# A RULE PACKAGE'S `[feature-xlings.<f>]` must be findable from the CONSUMER's
# build program, because that is where the rule's code runs.
#
# The sibling case (618) is a dependency reading its OWN declaration from its
# OWN build.mcpp, and that works. This one is different in the only way that
# matters: a rule is compiled INTO its consumer's build program, so
# `mcpp::xpkg_dir` is asked in the consumer's environment while the payload was
# declared in the rule's manifest. `fillXpkgDirs` read one manifest, so the
# address was fetched, unpacked, and then unreachable from the only code that
# wanted it -- an answer of "" that reads as "the toolkit is not installed"
# while it sits on disk.
#
# THE CRITERION IS THE PATH THE RULE ANSWERS WITH, not that the build
# succeeded: a build whose rule silently found nothing succeeds too.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Same reasoning as 618: the payload has to be one mcpp does not install for
# its own reasons, or the criterion selects an object that is present anyway.
TOOL=shaderc
TOOL_VERSION="2026.3"

mkdir -p rule/src
cat > rule/src/rule.cppm <<EOF
export module rule;
import std;
import mcpp;

export namespace testrule {
// The rule's own code, running inside the CONSUMER's build program.
inline std::string tool_dir() {
    const char* d = mcpp::xpkg_dir("xim", "$TOOL");
    return d == nullptr ? std::string() : std::string(d);
}
}
EOF
cat > rule/mcpp.toml <<EOF
[package]
name    = "rule"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
[build]
sources = ["src/rule.cppm"]
[features]
default  = []
usestool = []
# The payload the RULE needs, declared where the knowledge is. Which packages a
# device compiler wants is the rule's knowledge, not something every project
# should have to rediscover.
[feature-xlings.usestool]
"xim:$TOOL" = "$TOOL_VERSION"
[targets.rule]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
import rule;
int main() {
    // TO A FILE, NOT stdout: mcpp prints a build program's output only when it
    // FAILS, so an assertion grepping the build log would be unreachable on
    // exactly the run that is supposed to produce it.
    std::string out = std::string(mcpp::manifest_dir()) + "/rule-saw.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", testrule::tool_dir().c_str());
    std::fclose(f);
    return 0;
}
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[language]
standard   = "c++23"
modules    = true
import_std = true
# `[build-dependencies]`, because the rule must never reach the target, and
# `host-module = true`, because its module must be compiled for the build
# program. Two axes, and this package answers them separately.
[build-dependencies]
rule = { path = "../rule", features = ["usestool"], host-module = true }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

# An isolated home, for 618's reason: the ambient registry very likely holds
# the payload already, and then this passes on a broken engine.
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"

cd app
if ! "$MCPP" build >build.log 2>&1; then
    echo "FAIL: the consumer did not build"
    grep -iE 'error|not provisioned' build.log | head -5
    exit 1
fi
seen="$TMP/app/rule-saw.txt"
[ -s "$seen" ] || {
    echo "FAIL: the rule left no record"
    tail -10 build.log
    exit 1
}
answer=$(cat "$seen")
echo "the rule, inside the consumer's build program, answered: '${answer}'"
case "$answer" in
    *xim-x-$TOOL*) ;;
    "") echo "FAIL: the rule got an empty answer -- its payload was installed and unreachable"; exit 1 ;;
    *)  echo "FAIL: the answer does not name $TOOL"; exit 1 ;;
esac
echo "PASS: a rule package's [feature-xlings] payload is reachable from the consumer's build program"
