#!/usr/bin/env bash
# requires:
# mcpp#233: object paths must mirror the source's relative directory, not
# fold onto `obj/<pkg>_<immediate-parent-dir-name>/<file>`. Two sources with
# the SAME basename under DIFFERENT subtrees that happen to share the same
# immediate parent directory NAME — a/src/util.cpp and b/src/util.cpp, both
# under a literal `src/` — used to fold onto the same object output
# (`obj/<pkg>_src/util.o`), and ninja rejected the generated plan with
# "multiple rules generate obj/...". This must now build successfully, with
# each source compiling to its own, distinct object.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new objcollide > /dev/null
cd objcollide

mkdir -p a/src b/src

cat > a/src/util.cpp <<'EOF'
int fa() { return 1; }
EOF

cat > b/src/util.cpp <<'EOF'
int fb() { return 2; }
EOF

cat > src/main.cpp <<'EOF'
import std;
extern int fa();
extern int fb();
int main() {
    int sum = fa() + fb();
    std::println("sum={}", sum);
    return sum == 3 ? 0 : 1;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "objcollide"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp", "a/src/**/*.cpp", "b/src/**/*.cpp"]
[targets.objcollide]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: build failed (expected: colliding basenames must not collapse to the same object path)"
    exit 1
}

ninja_file="$(find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

# Both util.cpp compile edges must be present, each with a DISTINCT object
# output (mirroring a/src vs b/src rather than folding both onto the same
# "..._src/util.o").
util_objects="$(grep -oE 'build [^ ]*util[^ ]*\.o : cxx_object' "$ninja_file" | awk '{print $2}' | sort -u)"
util_count="$(echo "$util_objects" | grep -c . || true)"
[[ "$util_count" == "2" ]] || {
    echo "FAIL: expected 2 distinct util.cpp object outputs, got $util_count:"
    echo "$util_objects"
    cat "$ninja_file"
    exit 1
}

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "sum=3" ]] || { echo "unexpected output: $out"; exit 1; }

echo "OK"
