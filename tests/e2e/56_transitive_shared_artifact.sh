#!/usr/bin/env bash
# requires: elf
# A shared dependency that depends on another shared package must link against
# that package itself, so the intermediate .so records the correct NEEDED edge.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p depBase/src depMid/src app/src

cat > depBase/src/base.c <<'EOF'
int dep_base_answer(void) {
    return 41;
}
EOF

cat > depBase/mcpp.toml <<'EOF'
[package]
name    = "depBase"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.depBase]
kind = "shared"
EOF

cat > depMid/src/mid.c <<'EOF'
extern int dep_base_answer(void);

int dep_mid_answer(void) {
    return dep_base_answer() + 1;
}
EOF

cat > depMid/mcpp.toml <<'EOF'
[package]
name    = "depMid"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.depMid]
kind = "shared"

[dependencies.depBase]
path = "../depBase"
EOF

cat > app/src/main.cpp <<'EOF'
extern "C" int dep_mid_answer(void);

int main() {
    return dep_mid_answer() == 42 ? 0 : 1;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies.depMid]
path = "../depMid"
EOF

cd app
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "build failed"
    exit 1
}

base_so="$(find target -name 'libdepBase.so' | head -1)"
mid_so="$(find target -name 'libdepMid.so' | head -1)"
app_bin="$(find target -path '*/bin/app' -type f | head -1)"
[[ -n "$base_so" && -n "$mid_so" && -n "$app_bin" ]] || {
    cat build.log
    echo "expected shared artifacts were not produced"
    exit 1
}

readelf -d "$mid_so" | grep -q 'Shared library: \[libdepBase.so\]' || {
    readelf -d "$mid_so" || true
    echo "libdepMid.so does not link against libdepBase.so"
    exit 1
}

readelf -d "$app_bin" | grep -q 'Shared library: \[libdepMid.so\]' || {
    readelf -d "$app_bin" || true
    echo "app binary does not link against libdepMid.so"
    exit 1
}

"$MCPP" run > run.log 2>&1 || {
    cat run.log
    echo "run failed"
    exit 1
}

echo "OK"
