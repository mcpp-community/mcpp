#!/usr/bin/env bash
# requires: elf gcc
# mcpp#242 (transitive edge, found in the 0.0.99 architecture review): a
# consumer's `default-features = false` must be honored even when the consumer
# is itself a DEPENDENCY (transitive edge), not only for the root's direct deps.
#
# Before the edge-graph convergence, feature *resolution* honored the per-edge
# opt-out (mergeActiveFeatureDeps) but feature *activation* re-derived the flag
# by scanning only the ROOT manifest's direct deps — so a transitive dep's
# opt-out was silently dropped: activation still seeded the dep's default
# feature (defining its macro / keeping its default-gated sources) that
# resolution had already skipped. Now both consume the authoritative
# consumer→dep edge graph, so they agree.
#
# Layout: root(bin) -> A(lib) -> B(lib, `default = ["heavy"]`, heavy defines
# B_HEAVY). A depends on B with `default-features = false`. B's heavy macro must
# therefore be OFF in the transitive build.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# B: a library with a default feature `heavy` that defines B_HEAVY.
mkdir -p B/src
cat > B/mcpp.toml <<'EOF'
[package]
name    = "B"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[features]
default = ["heavy"]
heavy   = { defines = ["B_HEAVY=1"] }
[targets.B]
kind = "lib"
EOF
cat > B/src/b.cpp <<'EOF'
int b_heavy() {
#ifdef B_HEAVY
    return 1;
#else
    return 0;
#endif
}
EOF

# A: depends on B, opting OUT of B's default features.
mkdir -p A/src
cat > A/mcpp.toml <<'EOF'
[package]
name    = "A"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
B = { path = "../B", default-features = false }
[targets.A]
kind = "lib"
EOF
cat > A/src/a.cpp <<'EOF'
extern int b_heavy();
int a_val() { return b_heavy(); }
EOF

# root consumer -> A (which transitively pulls B).
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
A = { path = "../A" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > app/src/main.cpp <<'EOF'
import std;
extern int a_val();
int main() { std::println("heavy={}", a_val()); return 0; }
EOF

cd app
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "FAIL: build failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^heavy=' | tail -1)"
[[ "$out" == "heavy=0" ]] || {
    echo "FAIL: transitive default-features opt-out not honored (got '$out', want heavy=0)"
    exit 1
}

# Control: flip A to KEEP B's defaults -> heavy must come back on. Clean first:
# changing a transitive dep's active feature set across an in-place rebuild is a
# separate fingerprint concern; this test isolates the activation logic with a
# fresh build.
cat > ../A/mcpp.toml <<'EOF'
[package]
name    = "A"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[dependencies]
B = { path = "../B" }
[targets.A]
kind = "lib"
EOF
"$MCPP" clean > /dev/null 2>&1 || true
"$MCPP" build > build2.log 2>&1 || { cat build2.log; echo "FAIL: control build failed"; exit 1; }
out2="$("$MCPP" run 2>&1 | grep '^heavy=' | tail -1)"
[[ "$out2" == "heavy=1" ]] || {
    echo "FAIL: control (defaults kept) expected heavy=1, got '$out2'"
    exit 1
}

echo "OK"
