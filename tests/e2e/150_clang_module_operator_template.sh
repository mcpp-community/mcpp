#!/usr/bin/env bash
# #256: known-issue canary for a Clang regression that mcpp cannot fix but
# that silently limits what a module package may export.
#
# A module exporting replacement operator templates whose parameters are NOT
# all pinned by the first argument poisons name lookup for that operator in
# every importer: the frontend SIGSEGVs on any use of the name, on any type.
# Clang 18 and GCC 16 are fine; Clang 20 and 22 crash. Since mcpp bundles
# LLVM, the hazard ships with the toolchain.
#
# This is a STATE canary, not a pass/fail contract: today the expected result
# for the bundled toolchains is "crashes". The test fails when reality stops
# matching the recorded expectation — i.e. when a Clang bump fixes it (time
# to update the docs and the table) or when a version previously known-good
# regresses. Silence here would mean a future toolchain bump quietly changed
# what packages can express.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/_llvm_env.sh"

if [[ ! -d "$LLVM_ROOT" ]]; then
    echo "SKIP: no llvm payload installed"
    exit 0
fi

CLANGXX="$LLVM_ROOT/bin/clang++"
[[ -x "$CLANGXX" ]] || { echo "SKIP: no clang++ in $LLVM_ROOT"; exit 0; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

cat > matx.h <<'EOF'
#pragma once
namespace cv {
template<typename T, int m, int n> struct Matx {
    T val[m*n];
    typedef Matx<T, m, n> mat_type;
    enum { rows = m, cols = n };
};
template<typename T> struct Point_ { T x, y; };
using Point = Point_<int>;
template<typename T, int m, int n, int l> static inline
Matx<T, m, n> operator*(const Matx<T, m, l>& a, const Matx<T, l, n>& b) { return Matx<T,m,n>{}; }
template<typename T> static inline
Point_<T> operator*(const Point_<T>& a, int b) { return Point_<T>{a.x*b, a.y*b}; }
}
EOF

# The module layer: upstream shape mirrored with a trivially-true constraint.
# The 4-parameter Matx*Matx overload is the poisonous one — `n` and `l` are
# not determined by the first argument.
cat > m.cppm <<'EOF'
module;
#include "matx.h"
export module m;
export namespace cv {
using cv::Matx;
using cv::Point_;
using cv::Point;
inline namespace repl {
template<typename T> inline constexpr bool pick = true;
template<typename T, int m, int n, int l> requires pick<T> inline
Matx<T, m, n> operator*(const Matx<T, m, l>& a, const Matx<T, l, n>& b) { return Matx<T,m,n>{}; }
template<typename T> requires pick<T> inline
Point_<T> operator*(const Point_<T>& a, int b) { return Point_<T>{a.x*b, a.y*b}; }
}
}
EOF

# Note: only a Point is multiplied. The crash is name-keyed, so an unrelated
# type is enough — that is what makes the hazard so hard to attribute.
cat > use.cpp <<'EOF'
import m;
int main() { cv::Point p{1,2}; auto q = p * 2; return q.x; }
EOF

"$CLANGXX" -std=c++23 --precompile m.cppm -o m.pcm > precompile.log 2>&1 || {
    cat precompile.log
    echo "FAIL: producing the BMI should always succeed — the defect is on the import side"
    exit 1
}

set +e
"$CLANGXX" -std=c++23 -fprebuilt-module-path=. -c use.cpp -o use.o > use.log 2>&1
IMPORT_RC=$?
set -e

# Control: the same module WITHOUT the unpinned overload must always compile.
# If this ever fails, the canary itself is broken rather than the toolchain.
grep -v 'Matx<T, m, n> operator\*(const Matx<T, m, l>& a' m.cppm \
    | grep -v 'template<typename T, int m, int n, int l> requires pick<T> inline' > ctl.cppm
"$CLANGXX" -std=c++23 --precompile ctl.cppm -o ctl.pcm > ctl_pre.log 2>&1 || {
    cat ctl_pre.log; echo "FAIL: control module failed to precompile"; exit 1; }

# Recorded expectations, by LLVM major version.
case "${LLVM_VERSION%%.*}" in
    18|19) EXPECTED=ok ;;
    20|21|22) EXPECTED=crash ;;
    *) EXPECTED=unknown ;;
esac

if [[ $IMPORT_RC -eq 0 ]]; then ACTUAL=ok; else ACTUAL=crash; fi

echo "  llvm@${LLVM_VERSION}: expected=${EXPECTED} actual=${ACTUAL}"

if [[ "$EXPECTED" == "unknown" ]]; then
    echo "  no recorded expectation for LLVM ${LLVM_VERSION%%.*} — record one in this test"
    echo "OK"
    exit 0
fi

if [[ "$ACTUAL" != "$EXPECTED" ]]; then
    echo
    echo "FAIL: #256 canary state changed for llvm@${LLVM_VERSION}."
    if [[ "$ACTUAL" == "ok" ]]; then
        echo "  The importer now compiles — the Clang regression appears FIXED."
        echo "  Update the expectation table here and the hazard section in"
        echo "  docs/03-toolchains.md, and tell mcpp-community/mcpp#256."
    else
        echo "  The importer now crashes on a version previously known good."
        echo "  A toolchain bump has re-broken module operator templates."
        sed -n '1,25p' use.log
    fi
    exit 1
fi

echo "OK"
