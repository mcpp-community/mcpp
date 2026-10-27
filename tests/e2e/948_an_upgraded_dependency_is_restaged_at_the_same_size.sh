#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 948_an_upgraded_dependency_is_restaged_at_the_same_size.sh — a dependency
# upgraded to a version the build cache already holds is staged over the
# previous version's files, whatever their size.
#
# A package's BMI and objects land at paths named by the module and the source,
# not by the cache entry, so after an upgrade the previous version's files are
# where the new ones go. Staging under `--verify size` took an equal size for an
# equal file. A change of a constant keeps both sizes, so the previous BMI and
# object stayed: the program printed the old value, and a unit compiled against
# the new graph met the old BMI ("CRC mismatch").
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/c"
cat > "$INDEX_DIR/pkgs/c/constant.lua" <<'EOF'
package = {
    spec = "1",
    name = "constant",
    description = "One constant",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/constant-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["1.0.1"] = {
                url = "https://example.invalid/constant-1.0.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["constant"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# `make_project <dir> <version>`: a program printing the constant of `version`,
# whose payloads hold 41 (1.0.0) and 42 (1.0.1) --- one BMI size, one object size.
make_project() {
    local dir="$1" version="$2"
    mkdir -p "$TMP/$dir/src"
    for v in 1.0.0:41 1.0.1:42; do
        local d="$TMP/$dir/.mcpp/.xlings/data/xpkgs/local-dev.constant/${v%%:*}/src"
        mkdir -p "$d"
        cat > "$d/constant.cppm" <<EOF
export module constant;
export constexpr int value = ${v##*:};
export int stated() { return value; }
EOF
    done
    cat > "$TMP/$dir/src/main.cpp" <<'EOF'
import std;
import constant;
int main() { std::println("{} {}", value, stated()); return 0; }
EOF
    cat > "$TMP/$dir/mcpp.toml" <<EOF
[package]
name = "$dir"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[dependencies]
"local-dev.constant" = "$version"

[targets.$dir]
kind = "bin"
main = "src/main.cpp"
EOF
}

run_prints() {   # $1 = project, $2 = expected line
    local out
    out="$("$TMP/$1"/target/*/*/bin/"$1")"
    [[ "$out" == "$2" ]] || { echo "FAIL: $1 printed '$out', not '$2'"; exit 1; }
}

# The cache holds 1.0.1, built by another project. The two projects' names have
# one length, so the paths a BMI records have one length and the sizes agree.
make_project omega 1.0.1
(cd "$TMP/omega" && "$MCPP" build > build.log 2>&1) || { cat "$TMP/omega/build.log"; exit 1; }
run_prints omega "42 42"

# This project builds 1.0.0, then moves to 1.0.1: the staged files replace the
# ones 1.0.0 left at the same paths.
make_project alpha 1.0.0
cd "$TMP/alpha"
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }
run_prints alpha "41 41"
bmi="$(find target -path '*gcm.cache*' -name 'constant.gcm' | head -1)"
[[ -n "$bmi" ]] || { echo "FAIL: no BMI of the dependency in the build directory"; exit 1; }
before="$(wc -c < "$bmi")"

sed -i.bak 's/"local-dev.constant" = "1.0.0"/"local-dev.constant" = "1.0.1"/' mcpp.toml
rm -f mcpp.toml.bak
"$MCPP" build > upgrade.log 2>&1 || { cat upgrade.log; exit 1; }
grep -qE 'Cached local-dev\.constant v1\.0\.1' upgrade.log || {
    echo "FAIL: 1.0.1 was not served from the cache, so nothing here is staged"
    cat upgrade.log; exit 1
}
[[ "$(wc -c < "$bmi")" == "$before" ]] || { echo "FAIL: the BMI's size changed; the case is not the same-size one"; exit 1; }
run_prints alpha "42 42"
cmp -s "$bmi" "$(find "$MCPP_HOME/build-cache" -path '*constant@1.0.1*' -name 'constant.gcm' | head -1)" || {
    echo "FAIL: the staged BMI is not 1.0.1's"; exit 1
}

echo "OK"
