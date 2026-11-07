#!/usr/bin/env bash
# A LATER EDGE WALKS THE FEATURE-DEPS IT ACTIVATES (mcpp#897).
# `v` asks `a` for `f1`, and its feature `x` asks `a` for `f2` through
# `[feature-deps.x]`; `s` asks `v` for `x`. With `v` selected beside `s`, `v`
# was walked as a member first, without `x`, and `s`'s edge later added `x` to
# it: `x`'s sources were compiled, `x`'s feature-deps never reached the graph,
# and `a` was built without `f2` (`a.f2: failed to read compiled module`). The
# feature-deps and forwards of a feature a later edge activates are walked now.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

cd "$TMP"
mkdir -p a/src/f1 a/src/f2 v/src/base v/src/x s/src
printf '[workspace]\nmembers = ["a", "v", "s"]\n' > mcpp.toml
cat > a/mcpp.toml <<'EOF'
[package]
name = "a"
version = "0.1.0"
[targets.a]
kind = "lib"
[build]
sources = []
[features]
f1 = { sources = ["src/f1/*.cppm"] }
f2 = { sources = ["src/f2/*.cppm"] }
EOF
printf 'export module a.f1;\nexport int f1() { return 1; }\n' > a/src/f1/f1.cppm
printf 'export module a.f2;\nexport int f2() { return 2; }\n' > a/src/f2/f2.cppm
cat > v/mcpp.toml <<'EOF'
[package]
name = "v"
version = "0.1.0"
[targets.v]
kind = "lib"
[build]
sources = ["src/base/*.cppm"]
[dependencies]
a = { path = "../a", features = ["f1"] }
[features]
x = { sources = ["src/x/*.cppm"] }
[feature-deps.x]
a = { path = "../a", features = ["f2"] }
EOF
printf 'export module v;\nimport a.f1;\nexport int vv() { return f1(); }\n' > v/src/base/v.cppm
printf 'export module v.x;\nimport a.f2;\nexport int vx() { return f2(); }\n' > v/src/x/x.cppm
cat > s/mcpp.toml <<'EOF'
[package]
name = "s"
version = "0.1.0"
[targets.s]
kind = "bin"
main = "src/main.cpp"
[dependencies]
v = { path = "../v", features = ["x"] }
EOF
printf 'import std;\nimport v;\nimport v.x;\nint main() { std::println("sum {}", vv() + vx()); }\n' > s/src/main.cpp

for order in "-p s -p v" "-p v -p s"; do
    rm -rf target
    "$MCPP" build $order > build.log 2>&1 || fail "mcpp build $order" build.log
done
"$MCPP" run -p s > run.log 2>&1 || fail "the program" run.log
grep -q "sum 3" run.log || fail "the program's output" run.log
echo "PASS: 1019 a later edge walks the feature-deps it activates"
