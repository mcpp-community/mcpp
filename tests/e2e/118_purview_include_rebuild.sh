#!/usr/bin/env bash
# mcpp#235 / mcpp#257: compile edges must track header/purview/GMF
# `#include`s via a depfile. Before #235, `cxx_module`/`cxx_object` had NO
# depfile on non-MSVC (only the msvcDeps branch added `deps=msvc`) — the
# P1689 scan's `$out.dep` was generated then discarded. So editing a file
# `#include`d inside a module's purview (or a plain header included by a
# .cpp) did NOT invalidate the compile edge: `mcpp run` kept printing stale
# output.
#
# #257: the 0.0.97 fix was scoped to GCC because GCC's `-fmodules -MMD`
# depfile needs an awk filter and Clang's shape was unverified. It turned
# out Clang emits a single plain rule with nothing to filter, so this test
# no longer declares `# requires: gcc` — the contract holds on every POSIX
# toolchain, and running it under Clang is the point.
set -e

# Resolved before any cd: the script changes directory below.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Portable in-place edit. This test lost its `# requires: gcc` gate in #257,
# so it now runs on macOS too, where BSD sed reads `-i`'s next argument as a
# backup suffix and swallows the script.
subst() {  # subst <sed-expr> <file>
    sed "$1" "$2" > "$2.tmp" && mv "$2.tmp" "$2"
}

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new purviewinc > /dev/null
cd purviewinc

cat > src/vals.inc <<'EOF'
export inline int answer() { return 41; }
EOF

cat > src/m.cppm <<'EOF'
export module m;
#include "vals.inc"
EOF

cat > src/main.cpp <<'EOF'
import std;
import m;
int main() {
    std::println("{}", answer());
    return 0;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "purviewinc"
version = "0.1.0"
EOF

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "41" ]] || { echo "unexpected initial output: $out"; exit 1; }

subst 's/41/42/' src/vals.inc

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "42" ]] || {
    echo "FAIL: editing a purview #include'd file did not trigger a rebuild"
    echo "got: $out (expected 42)"
    exit 1
}

# --- Second assertion: a plain .h included by a .cpp must also rebuild ----
cat > src/helper.h <<'EOF'
inline int helper_val() { return 100; }
EOF

cat > src/helper_user.cpp <<'EOF'
#include "helper.h"
int use_helper() { return helper_val(); }
EOF

cat > src/main.cpp <<'EOF'
import std;
import m;
extern int use_helper();
int main() {
    std::println("{} {}", answer(), use_helper());
    return 0;
}
EOF

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "42 100" ]] || { echo "unexpected output before header edit: $out"; exit 1; }

subst 's/100/200/' src/helper.h

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "42 200" ]] || {
    echo "FAIL: editing a plain .h included by a .cpp did not trigger a rebuild"
    echo "got: $out (expected 42 200)"
    exit 1
}


# --- #257: the same contract under Clang -----------------------------------
# 0.0.97 shipped the depfile for GCC only, so this exact scenario silently
# served a stale BMI on every Clang platform (macOS default, Windows hosted).
# Re-run both assertions with an LLVM toolchain when one is installed.
source "$SCRIPT_DIR/_llvm_env.sh"
if [[ -d "$LLVM_ROOT" ]]; then
    cd "$TMP"
    "$MCPP" new purviewinc_clang > /dev/null
    cd purviewinc_clang

    cat > src/vals.inc <<'EOF'
export inline int answer() { return 41; }
EOF
    cat > src/m.cppm <<'EOF'
export module m;
#include "vals.inc"
EOF
    cat > src/main.cpp <<'EOF'
import std;
import m;
int main() {
    std::println("{}", answer());
    return 0;
}
EOF
    cat > mcpp.toml <<EOF
[package]
name    = "purviewinc_clang"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION}"
EOF

    out="$("$MCPP" run 2>&1 | tail -1)"
    [[ "$out" == "41" ]] || { echo "clang: unexpected initial output: $out"; exit 1; }

    subst 's/41/42/' src/vals.inc

    out="$("$MCPP" run 2>&1 | tail -1)"
    [[ "$out" == "42" ]] || {
        echo "FAIL (#257, clang): editing a purview #include'd file did not trigger a rebuild"
        echo "got: $out (expected 42)"
        exit 1
    }

    cat > src/helper.h <<'EOF'
inline int helper_val() { return 100; }
EOF
    cat > src/helper_user.cpp <<'EOF'
#include "helper.h"
int use_helper() { return helper_val(); }
EOF
    cat > src/main.cpp <<'EOF'
import std;
import m;
extern int use_helper();
int main() {
    std::println("{} {}", answer(), use_helper());
    return 0;
}
EOF
    out="$("$MCPP" run 2>&1 | tail -1)"
    [[ "$out" == "42 100" ]] || { echo "clang: unexpected output before header edit: $out"; exit 1; }

    subst 's/100/200/' src/helper.h

    out="$("$MCPP" run 2>&1 | tail -1)"
    [[ "$out" == "42 200" ]] || {
        echo "FAIL (#257, clang): editing a plain .h included by a .cpp did not trigger a rebuild"
        echo "got: $out (expected 42 200)"
        exit 1
    }
    echo "  clang leg (llvm@${LLVM_VERSION}) ok"
else
    echo "  clang leg skipped (no llvm payload installed)"
fi

echo "OK"
