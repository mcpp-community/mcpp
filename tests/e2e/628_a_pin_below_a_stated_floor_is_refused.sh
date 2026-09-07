#!/usr/bin/env bash
# requires: gcc
# A PIN THAT DOES NOT SATISFY A STATED FLOOR IS A REFUSAL, NOT A SECOND INSTALL.
#
# Once one package means one version, the two declarations have to be
# compatible. A rule package writes `>=0.9.9` because that is the version its
# rule needs; a project that pins 0.9.7 has stated something that cannot hold at
# the same time. Installing both -- what the engine used to do -- postponed the
# contradiction to whatever the rule failed to find, or to nothing at all.
#
# THIS IS A COMPARISON, NOT A SEARCH. The version is chosen by adjudication (the
# declaration nearer the artifact wins) and then checked against the
# requirements that lost. Nothing here asks the index which versions exist, so
# there is no constraint solver in the engine and none is needed for this.
#
# THE MESSAGE MUST NAME BOTH SIDES. "cannot be satisfied" without saying who
# asked for what leaves the author to find the second declaration by grep, and
# it is usually in a package they do not own.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TOOL=zoxide

mkdir -p dep/src
cat > dep/src/lib.cpp <<'EOF'
int dep_touch() { return 1; }
EOF
cat > dep/mcpp.toml <<EOF
[package]
name    = "toolowner"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[xlings.workspace]
"xim:$TOOL" = ">=0.9.9"
[targets.toolowner]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
toolowner = { path = "../dep" }
[xlings.workspace]
"xim:$TOOL" = "0.9.7"
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"

cd app
if "$MCPP" build >build.log 2>&1; then
    echo "FAIL: a pin below the dependency's floor was accepted"
    grep -iE "$TOOL" build.log | head -5
    exit 1
fi
out=$(cat build.log)

for needle in "xim:$TOOL" "0.9.7" ">=0.9.9" "toolowner" "this project"; do
    echo "$out" | grep -q -- "$needle" || {
        echo "FAIL: the refusal does not mention '$needle'"
        echo "$out" | tail -10
        exit 1
    }
done
echo "$out" | grep -q "drop the pin" || {
    echo "FAIL: the refusal does not state the way out"
    echo "$out" | tail -10
    exit 1
}

# AND THE REFUSAL HAS A TOKEN. A machine consumer classifying this outcome must
# not be reduced to matching the prose -- which is what docs/11 exists to
# promise, and what a reworded sentence silently breaks.
if command -v jq >/dev/null 2>&1; then
    "$MCPP" why toolchain --format json >why.json 2>/dev/null || true
    reason=$(jq -r '.data.reason // empty' why.json 2>/dev/null || true)
    [ "$reason" = "tool-version-conflict" ] || {
        echo "FAIL: reason was '$reason', expected tool-version-conflict"
        head -c 400 why.json
        exit 1
    }
    echo "machine-readable reason: $reason"
fi

# …AND RAISING THE PIN CLEARS IT. Without this leg the test would also pass on
# an engine that refuses every project declaring a tool its dependency also
# declares.
cd "$TMP"
sed -i.bak "s/\"0.9.7\"/\"0.9.9\"/" app/mcpp.toml
rm -f app/mcpp.toml.bak
cd app
"$MCPP" build >build2.log 2>&1 || {
    echo "FAIL: a pin that DOES satisfy the floor was still refused"
    tail -20 build2.log
    exit 1
}

echo "PASS: a pin below a stated floor is refused, and one that satisfies it is not"
