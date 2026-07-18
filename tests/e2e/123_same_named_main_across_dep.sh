#!/usr/bin/env bash
# requires: elf gcc
# mcpp#240 (follow-up to #233): when a dependency package and the consumer
# ship a SAME-NAMED source (the near-universal case: both have src/main.cpp —
# e.g. OpenCV's sample main.cpp's vs the consumer's own), #233's object-path
# disambiguation renames the scanned consumer main to obj/<pkg>/src/main.o,
# but the LINK step kept referencing the pre-disambiguation flat obj/main.o:
#   ninja: error: 'obj/main.o', needed by 'bin/<x>', missing and no known rule
# The entry-main link input must follow the same disambiguation as its
# compile edge. Member-scoped tests dodge this (distinct filenames), so only
# a real dep+consumer collision exercises it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Dependency (kind=lib) that ships its OWN src/main.cpp (an impl file here,
# same basename as the consumer's entry).
mkdir -p mydep/src
cat > mydep/src/main.cpp <<'EOF'
int dep_helper() { return 41; }
EOF
cat > mydep/mcpp.toml <<'EOF'
[package]
name    = "mydep"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[targets.mydep]
kind = "lib"
EOF

# Consumer with its OWN src/main.cpp (globbed into sources, so it is scanned
# and subject to disambiguation).
mkdir -p consumer/src
cat > consumer/src/main.cpp <<'EOF'
import std;
extern int dep_helper();
int main() {
    std::println("val={}", dep_helper());
    return dep_helper() == 41 ? 0 : 1;
}
EOF
cat > consumer/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
mydep = { path = "../mydep" }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

cd consumer
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: build failed (expected: same-named main across dep must link the disambiguated object)"
    exit 1
}

ninja_file="$(find target -name build.ninja | head -1)"
[[ -n "$ninja_file" ]] || { echo "no build.ninja generated"; exit 1; }

# The link edge must NOT reference a bare obj/main.o (the stale, unproduced
# flat path). It must reference the consumer main's real, disambiguated object.
link_line="$(grep -E 'bin/consumer *:' "$ninja_file")"
if echo "$link_line" | grep -qE '(^| )obj/main\.o( |$)'; then
    echo "FAIL: link still references stale flat obj/main.o"
    echo "$link_line"
    cat "$ninja_file"
    exit 1
fi

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "val=41" ]] || { echo "unexpected output: $out"; exit 1; }

echo "OK"
