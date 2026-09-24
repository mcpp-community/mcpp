#!/usr/bin/env bash
# requires: python3
# 765_a_consumer_include_directory_stays_in_the_consumer.sh
#
# #690, design record F7. A package's `[build] include_dirs` (and
# `private_include_dirs`, a subset of them) are its PRIVATE build requirement:
# they reach the package's own units and nothing else. Until 2026.9.25.1 the
# root's directories were written into the file-level compile flags of
# build.ninja, which every unit in the graph reads, so a root header named like
# a system header shadowed it inside a dependency, and a dependency object in
# the global cache could have been compiled against another project's root
# headers (the key never contained them).
#
# Four assertions, each on the artefact rather than on build success alone:
#   (a) a root PRIVATE `limits.h` that is an `#error` does not reach the
#       dependency's C or C++ unit: the build succeeds;
#   (b) compile_commands.json: no dependency entry carries a root directory;
#       the root's own entry carries each root directory exactly once; the
#       dependency's PUBLIC include directory still reaches the root;
#   (c) the dependency's include/define/feature words are identical under two
#       roots that differ only in their include settings;
#   (d) a dependency that compiled only because the root provided its header
#       now fails, and the output names the root directory that has it, on the
#       full path and again on the fast path.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

cd "$TMP"
mkdir -p dep/include/dep dep/src app/src app/appinc app/appprivinc app2/src app2/other

cat > dep/include/dep/api.h <<'EOF'
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int dep_c_value(void);
#ifdef __cplusplus
}
#endif
int dep_cxx_value();
EOF
cat > dep/src/dep.cpp <<'EOF'
#include <limits.h>
#include <dep/api.h>
int dep_cxx_value() { return INT_MAX > 0 ? 3 : 0; }
EOF
cat > dep/src/dep_c.c <<'EOF'
#include <limits.h>
#include <dep/api.h>
int dep_c_value(void) { return CHAR_BIT == 8 ? 4 : 0; }
EOF
cat > dep/mcpp.toml <<'EOF'
[package]
name    = "dep"
version = "0.1.0"

[build]
include_dirs = ["include"]
sources      = ["src/dep.cpp", "src/dep_c.c"]

[targets.dep]
kind = "lib"
EOF

# The root: two include directories, one of them private, and a private header
# that must never be seen by anyone but the root itself.
echo '#define APP_CFG 7' > app/appinc/appcfg.h
echo '#define APP_PRIV 1' > app/appprivinc/apppriv.h
printf '#error ROOT PRIVATE HEADER REACHED A DEPENDENCY\n' > app/appprivinc/limits.h
cat > app/src/main.cpp <<'EOF'
#include <dep/api.h>
#include "appcfg.h"
#include "apppriv.h"
int main() { return dep_cxx_value() + dep_c_value() == APP_CFG + APP_PRIV - 1 ? 0 : 1; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }

[build]
include_dirs         = ["appinc", "appprivinc"]
private_include_dirs = ["appprivinc"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

# A second root that differs only in its include settings.
echo '#define OTHER 1' > app2/other/other.h
cat > app2/src/main.cpp <<'EOF'
#include <dep/api.h>
#include "other.h"
int main() { return dep_cxx_value() + dep_c_value() + OTHER == 8 ? 0 : 1; }
EOF
cat > app2/mcpp.toml <<'EOF'
[package]
name    = "app2"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }

[build]
include_dirs = ["other"]

[targets.app2]
kind = "bin"
main = "src/main.cpp"
EOF

# ── (a) ──────────────────────────────────────────────────────────────────────
(cd app && "$MCPP" build > build.log 2>&1) || {
    cat app/build.log
    echo "FAIL (a): the root's private include directory reached a dependency unit"
    exit 1
}
(cd app2 && "$MCPP" build > build.log 2>&1) || { cat app2/build.log; echo "FAIL: app2 build"; exit 1; }

# ── (b) and (c) ──────────────────────────────────────────────────────────────
python3 - "$TMP" <<'PY'
import json, os, sys
tmp = sys.argv[1]
def entries(root):
    return json.load(open(os.path.join(tmp, root, "compile_commands.json")))
def args(e):
    return e.get("arguments") or e["command"].split()
def norm(s):
    return s.replace("\\", "/")
def count(a, needle):
    return sum(1 for x in a if needle in norm(x))
fails = []
app = entries("app")
dep_units = [e for e in app if norm(e["file"]).split("/")[-1] in ("dep.cpp", "dep_c.c")]
main_units = [e for e in app if norm(e["file"]).endswith("app/src/main.cpp")]
if len(dep_units) != 2: fails.append(f"(b) expected 2 dependency entries, found {len(dep_units)}")
if len(main_units) != 1: fails.append(f"(b) expected 1 root entry, found {len(main_units)}")
for e in dep_units:
    for d in ("app/appinc", "app/appprivinc"):
        if count(args(e), d):
            fails.append(f"(b) {norm(e['file'])} carries the root directory {d}")
for e in main_units:
    a = args(e)
    for d in ("app/appinc", "app/appprivinc"):
        if count(a, d) != 1:
            fails.append(f"(b) root entry carries {d} {count(a, d)} times, expected once")
    if count(a, "dep/include") < 1:
        fails.append("(b) the dependency's public include directory does not reach the root")
def words(root):
    out = {}
    for e in entries(root):
        f = norm(e["file"]).split("/")[-1]
        if f not in ("dep.cpp", "dep_c.c"): continue
        # A word naming the root's own build directory (clang's
        # `-fprebuilt-module-path=`, MSVC's `/ifcSearchDir`) is where that
        # project keeps its BMIs, not an input the dependency is compiled
        # against, and it differs between two projects by construction.
        own = f"/{root}/target/"
        out[f] = sorted(norm(x) for x in args(e)
                        if x.startswith(("-I", "-D", "-f", "-idirafter", "/I", "/D"))
                        and own not in norm(x))
    return out
w1, w2 = words("app"), words("app2")
if not w1 or w1 != w2:
    fails.append(f"(c) the dependency's words differ between two roots:\n  app : {w1}\n  app2: {w2}")
for f in fails: print("FAIL", f)
sys.exit(1 if fails else 0)
PY

# ── (d) ──────────────────────────────────────────────────────────────────────
mkdir -p leaner/src app3/src app3/appinc
echo '#define FROM_CONSUMER 1' > app3/appinc/needs_consumer.h
cat > leaner/src/leaner.cpp <<'EOF'
#include <needs_consumer.h>
int leaner_value() { return FROM_CONSUMER; }
EOF
cat > leaner/mcpp.toml <<'EOF'
[package]
name    = "leaner"
version = "0.1.0"

[build]
sources = ["src/leaner.cpp"]

[targets.leaner]
kind = "lib"
EOF
cat > app3/src/main.cpp <<'EOF'
int leaner_value();
int main() { return leaner_value() == 1 ? 0 : 1; }
EOF
cat > app3/mcpp.toml <<'EOF'
[package]
name    = "app3"
version = "0.1.0"

[dependencies]
leaner = { path = "../leaner" }

[build]
include_dirs = ["appinc"]

[targets.app3]
kind = "bin"
main = "src/main.cpp"
EOF
cd app3
for pass in full fast; do
    if "$MCPP" build > "d-$pass.log" 2>&1; then
        cat "d-$pass.log"
        echo "FAIL (d, $pass): a dependency compiled against its consumer's header"
        exit 1
    fi
    grep -q "needs_consumer.h" "d-$pass.log" || {
        cat "d-$pass.log"; echo "FAIL (d, $pass): failed for an unexpected reason"; exit 1; }
    grep -q "no longer" "d-$pass.log" && grep -q "appinc" "d-$pass.log" || {
        cat "d-$pass.log"
        echo "FAIL (d, $pass): the advice naming the consumer's directory is missing"
        exit 1
    }
done

echo "OK: a consumer's include directories stay in the consumer"
