#!/usr/bin/env bash
# requires: gcc unix-shell
# `[xlings.workspace]` entries carry a tier, and a verb installs only its own.
#
# THE ASSERTION IS ON WHAT mcpp ASKS FOR, NOT ON WHAT GOT INSTALLED.
#
# Verifying the tier by installing would need a clean machine and a network,
# and neither is available here. It would also be the wrong object: what the
# tier changes is the SET mcpp requests, and that set is observable without a
# single download — `MCPP_NO_AUTO_INSTALL=1` refuses to provision and names
# exactly what it would have provisioned.
#
# AND THE CRITERION CARRIES ITS OWN DENOMINATOR. "The run-tier tool is
# absent from `mcpp build`" is satisfied by an mcpp that provisions nothing at
# all, or by a manifest whose entry never parsed. The pair of commands is the
# test: the same project, the same package, present under `run` and absent
# under `build`. Only a working tier produces both readings.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/app/src"
cat > "$work/app/mcpp.toml" <<'TOML'
[package]
name    = "tiers"
version = "0.1.0"

[build]
sources = ["src/main.cpp"]

[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[xlings.workspace]
"xim:tier-always" = "1.0.0"
"xim:tier-build"  = { version = "1.0.0", when = "build" }
"xim:tier-run"    = { version = "1.0.0", when = "run" }

[feature-xlings.hardware]
"xim:tier-hardware" = "1.0.0"
TOML
cat > "$work/app/src/main.cpp" <<'CPP'
int main() { return 0; }
CPP

cd "$work/app"

# The refusal names the list it would have installed. It is a hard error, so
# the exit status is expected non-zero and only the text is read.
probe() {
    MCPP_NO_AUTO_INSTALL=1 "$MCPP" "$@" 2>&1 || true
}

build_out="$(probe build)"
run_out="$(probe run)"
hw_out="$(probe build --features hardware)"

echo "--- build ---"; echo "$build_out" | head -20
echo "--- run ---";   echo "$run_out"   | head -20

# Both verbs must actually reach the provisioning refusal; otherwise every
# assertion below is vacuous.
case "$build_out" in
  *"not provisioned"*) ;;
  *) echo "FAIL: mcpp build never reached the provisioning gate"; exit 1 ;;
esac
case "$run_out" in
  *"not provisioned"*) ;;
  *) echo "FAIL: mcpp run never reached the provisioning gate"; exit 1 ;;
esac

want() {  # want <text> <needle> <what>
    case "$1" in *"$2"*) ;; *) echo "FAIL: $3"; exit 1 ;; esac
}
lack() {
    case "$1" in *"$2"*) echo "FAIL: $3"; exit 1 ;; *) ;; esac
}

# The untiered entry is the 80% case and must behave exactly as before.
want "$build_out" "tier-always" "an entry with no 'when' was not provisioned by mcpp build"
want "$run_out"   "tier-always" "an entry with no 'when' was not provisioned by mcpp run"

# `build` is named explicitly and means the same thing.
want "$build_out" "tier-build"  "when = 'build' was not provisioned by mcpp build"

# The tier that buys something: absent from a build, present in a run.
lack "$build_out" "tier-run"    "when = 'run' was provisioned by mcpp build"
want "$run_out"   "tier-run"    "when = 'run' was NOT provisioned by mcpp run"

# A feature-gated tool is downloaded by nobody who does not ask for it.
lack "$build_out" "tier-hardware" "[feature-xlings.hardware] applied without the feature"
lack "$run_out"   "tier-hardware" "[feature-xlings.hardware] applied without the feature"
want "$hw_out"    "tier-hardware" "[feature-xlings.hardware] did not apply with --features hardware"

echo "PASS: tool tiers request the set the verb needs, and no more"
