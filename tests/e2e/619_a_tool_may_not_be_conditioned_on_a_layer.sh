#!/usr/bin/env bash
# requires: gcc
# `[target.<selector>.xlings…]` accepts a target predicate. It must REFUSE one
# that names a target-side layer.
#
# The reason is schedule, not style. The five RESOLVED layer keys (`c-abi`,
# `compiler`, ...) are answered by dependency RESOLUTION, so a
# predicate naming one is held back to the second merge pass -- which runs after
# tools are provisioned and after every build.mcpp. An entry admitted there is
# declared and never installed, and the build that results is the worst-shaped
# failure there is: it succeeds, and the tool is simply absent.
#
# THE CRITERION IS TWO-SIDED, WHICH IS WHY THE TOOL NAMES NOTHING REAL. Without
# the refusal the entry is dropped in silence and this project BUILDS; with it
# the build fails naming both halves. A tool that exists would make both legs
# look the same on a machine that already has it.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'TOML'
[package]
name    = "layer-gated-tool"
version = "0.1.0"

[target.'cfg(c-abi = "musl")'.xlings.workspace]
"xim:mcpp-e2e-no-such-tool" = "1.0"
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP

if out=$("$MCPP" build 2>&1); then status=0; else status=$?; fi
echo "$out"

if [ "$status" -eq 0 ]; then
  echo "FAIL: the build succeeded; a tool conditioned on a layer was accepted and then never installed"
  exit 1
fi

# The message must name the TOOL -- the author has to know which line to move --
# and the PREDICATE, which is the half they are not looking at.
case "$out" in
  *"xim:mcpp-e2e-no-such-tool"*) ;;
  *) echo "FAIL: the refusal does not name the tool"; exit 1 ;;
esac
case "$out" in
  *'c-abi = "musl"'*) ;;
  *) echo "FAIL: the refusal does not name the predicate"; exit 1 ;;
esac
# And it must offer both ways out, or it is a refusal without a repair.
case "$out" in
  *"feature-xlings"*) ;;
  *) echo "FAIL: the refusal does not name the gate form as the way out"; exit 1 ;;
esac

echo "PASS: a tool conditioned on a target-side layer is refused, naming the tool and the predicate"
