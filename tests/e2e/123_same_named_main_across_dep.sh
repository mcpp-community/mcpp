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

# THE invariant: every object the link edge names must be PRODUCED by an edge in
# the same graph. #240's bug was a link input that no edge produced.
#
# This deliberately does not assert *which* path the consumer's main lands on.
# The original assertion ("must not be the flat obj/main.o") encoded the shape of
# the first fix rather than the property: back then the consumer's main WAS
# renamed, because disambiguation was decided by a census over the whole build
# dir, so a dependency's same-named file dragged the consumer along with it.
# Since mcpp#344 a dependency's objects live under obj/<pkg>/, cross-package
# collisions cannot happen, and the consumer's own main correctly stays flat.
# Both layouts satisfy #240; only "produced by some edge" distinguishes a fixed
# tree from a broken one.
link_line="$(grep -E '^build bin/consumer *:' "$ninja_file")"
[[ -n "$link_line" ]] || { echo "FAIL: no link edge for bin/consumer"; cat "$ninja_file"; exit 1; }

for obj in $(echo "$link_line" | sed -E 's/^build bin\/consumer *: *cxx_link //' | tr ' ' '\n' | grep -E '\.o$'); do
    grep -qE "^build ${obj//\//\\/} *:" "$ninja_file" || {
        echo "FAIL: link input '$obj' is not produced by any edge"
        echo "$link_line"
        cat "$ninja_file"
        exit 1
    }
done

# And the dependency's same-named source must have gotten its own object rather
# than silently overwriting the consumer's.
grep -qE '^build obj/mydep/.*main\.o *:' "$ninja_file" || {
    echo "FAIL: the dependency's src/main.cpp has no object of its own"
    grep -n 'main\.o' "$ninja_file"
    exit 1
}

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "val=41" ]] || { echo "unexpected output: $out"; exit 1; }

echo "OK"
