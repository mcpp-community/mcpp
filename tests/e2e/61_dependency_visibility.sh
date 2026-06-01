#!/usr/bin/env bash
# requires: gcc
# Dependency visibility: private deps must be available while compiling the
# dependent package, but their include dirs must not leak to consumers.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

cd "$TMP"
mkdir -p hidden/include/hidden hidden/src facade/src app/src

cat > hidden/include/hidden/value.hpp <<'EOF'
#pragma once
inline int hidden_value() { return 7; }
EOF

cat > hidden/src/hidden.cppm <<'EOF'
module;
#include <hidden/value.hpp>
export module hidden;
export int hidden_answer() { return hidden_value(); }
EOF

cat > hidden/mcpp.toml <<'EOF'
[package]
name    = "hidden"
version = "0.1.0"

[build]
include_dirs = ["include"]

[modules]
sources = ["src/*.cppm"]

[targets.hidden]
kind = "lib"
EOF

cat > facade/src/facade.cppm <<'EOF'
export module facade;
import hidden;
export int facade_answer() { return hidden_answer(); }
EOF

cat > facade/mcpp.toml <<'EOF'
[package]
name    = "facade"
version = "0.1.0"

[modules]
sources = ["src/*.cppm"]

[targets.facade]
kind = "lib"

[dependencies]
hidden = { path = "../hidden", visibility = "private" }
EOF

cat > app/src/main.cpp <<'EOF'
import facade;
int main() {
    return facade_answer() == 7 ? 0 : 1;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
facade = { path = "../facade" }
EOF

cd app
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "private dep should compile its direct consumer"
    exit 1
}
"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "app run failed"
    exit 1
}

cat > src/main.cpp <<'EOF'
#include <hidden/value.hpp>
import facade;
int main() {
    return hidden_value() == facade_answer() ? 0 : 1;
}
EOF

if "$MCPP" build > leak.log 2>&1; then
    cat leak.log
    echo "private dependency include dir leaked to consumer"
    exit 1
fi
grep -q "hidden/value.hpp" leak.log || {
    cat leak.log
    echo "consumer failed for an unexpected reason"
    exit 1
}

echo "OK"
