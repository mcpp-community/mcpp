#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# #253: per-OS `features` in xpkg descriptors — the `mcpp.<os>` additive
# overlay applies to `features` like every other mcpp-segment key: a same-named
# feature merges per-subkey by APPEND (neutral body first, host-OS body after),
# and the non-host OS section stays invisible. This is the common/delta shape
# compat.opencv's `dnn` uses (neutral common payload + per-OS kernels/flags).
# Locked end-to-end here; per-leg merge semantics are unit-tested via
# osOverride (test_manifest.cpp PerOsFeaturesAdditiveMerge).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# Host OS key in xpkg vocabulary; the "other" OS carries a poison section that
# must never leak into this build.
case "$(uname -s)" in
    Darwin) OSKEY=macosx; OTHER=linux ;;
    MINGW*|MSYS*|CYGWIN*) OSKEY=windows; OTHER=linux ;;
    *) OSKEY=linux; OTHER=macosx ;;
esac

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/o"
cat > "$INDEX_DIR/pkgs/o/osfeat.lua" <<EOF
package = {
    spec = "1",
    name = "osfeat",
    description = "per-OS features probe",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        $OSKEY = {
            ["1.0.0"] = {
                url = "https://example.invalid/osfeat-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/base.c" },
        features = {
            ["accel"] = {
                defines = { "OSFEAT_ACCEL" },
                sources = { "src/accel_common.c" },
            },
        },
        $OSKEY = {
            features = {
                ["accel"] = {
                    sources = { "src/accel_os.c" },
                    flags   = { { glob = "src/accel_os.c", defines = { "OSFEAT_OS_KERNEL=5" } } },
                },
            },
        },
        $OTHER = {
            features = {
                ["accel"] = { sources = { "src/other_os_only.c" } },
            },
        },
        targets = { ["osfeat"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

PKGSRC="$TMP/project/app/.mcpp/.xlings/data/xpkgs/local-dev.osfeat/1.0.0/src"
mkdir -p "$TMP/project/app/src" "$PKGSRC"

cat > "$PKGSRC/base.c" <<'EOF'
int osfeat_base(void) { return 1; }
EOF
cat > "$PKGSRC/accel_common.c" <<'EOF'
#ifndef OSFEAT_ACCEL
#error "neutral feature define missing"
#endif
int osfeat_common(void) { return 2; }
EOF
cat > "$PKGSRC/accel_os.c" <<'EOF'
/* Compiled only via the host-OS features overlay; its per-OS feature flag
 * must land here (and only here). */
#ifndef OSFEAT_OS_KERNEL
#error "per-OS feature flags did not reach the per-OS feature source"
#endif
int osfeat_os_kernel(void) { return OSFEAT_OS_KERNEL; }
EOF
# Poison: if the OTHER-OS section leaks, this file compiles and breaks the build.
cat > "$PKGSRC/other_os_only.c" <<'EOF'
#error "non-host OS features section must be invisible"
EOF

cd "$TMP/project/app"
cat > src/main.cpp <<'EOF'
extern "C" int osfeat_base(void);
extern "C" int osfeat_common(void);
extern "C" int osfeat_os_kernel(void);
int main() {
    // 1 + 2 + 5 == 8: base + neutral feature payload + per-OS feature delta.
    return (osfeat_base() + osfeat_common() + osfeat_os_kernel()) == 8 ? 0 : 1;
}
EOF

cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.osfeat" = { version = "1.0.0", features = ["accel"] }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
grep -q "matched no source file" build.log && {
    cat build.log; echo "per-OS feature flags glob must match its per-OS source"; exit 1; } || true
"$MCPP" run > /dev/null 2>&1 || { echo "run failed (payload sum wrong or link failed)"; exit 1; }

# Feature OFF: every accel source (neutral + per-OS) must stay out.
cat > src/main.cpp <<'EOF'
extern "C" int osfeat_base(void);
int main() { return osfeat_base() == 1 ? 0 : 1; }
EOF
cat > mcpp.toml <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.osfeat" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
rm -rf target   # drop the feature-on build's output dir before sweeping for .o
"$MCPP" build > build_off.log 2>&1 || { cat build_off.log; echo "build (off) failed"; exit 1; }
find . -name 'accel_*.o' | grep -q . && { echo "accel sources compiled with feature off"; exit 1; } || true
"$MCPP" run > /dev/null 2>&1 || { echo "run (off) failed"; exit 1; }

echo "OK"
