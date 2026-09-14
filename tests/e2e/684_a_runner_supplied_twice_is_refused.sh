#!/usr/bin/env bash
# requires: unix-shell
# 684 -- a runner that a dependency's build program supplies and the project's
# own build program also emits is refused naming both, and the manifest is the
# way to choose (#634, triage record §9 item 8).
#
# The tokens a build program emits are appended to the runner's argv, in
# emission order, which is the directive contract. Two dependencies that supply
# one name were already refused. A dependency and the root were not: measured
# on 2026.9.14.1, the two argvs became one, `run-A.sh run-B.sh <artifact>`, and
# `mcpp run --list-runners` showed only the first.
#
# Criteria:
#   1. the build is refused, naming the dependency and the runner name;
#   2. with the name declared in `[target.<triple>.runners]`, the build succeeds
#      and `mcpp run --runner app` uses the manifest's runner;
#   3. negative direction: a runner only the dependency supplies is not refused.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

for r in A B C; do
    printf '#!/bin/sh\nprintf "RUNNER-%s\\n"\nexec "$@"\n' "$r" > "$TMP/run-$r.sh"
    chmod +x "$TMP/run-$r.sh"
done

mkdir -p dep/src app/src
cat > dep/mcpp.toml <<'TOML'
[package]
namespace = "demo"
name      = "dep"
version   = "0.1.0"
[targets.dep]
kind = "lib"
TOML
printf 'export module dep;\nexport int dep_anchor() { return 0; }\n' > dep/src/dep.cppm
cat > dep/build.mcpp <<CPP
import std;
import mcpp;
int main() {
    mcpp::runner("app", "$TMP/run-A.sh");
    return 0;
}
CPP

cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"
[dependencies]
demo.dep = { path = "../dep" }
TOML
printf 'import dep;\nint main() { return dep_anchor(); }\n' > app/src/main.cpp
cd app

# ── 3 (the negative direction first): only the dependency supplies it ────
"$MCPP" build > dep-only.log 2>&1 || fail "3: a runner only the dependency supplies was refused" dep-only.log

# ── 1 ─────────────────────────────────────────────────────────────────────
cat > build.mcpp <<CPP
import std;
import mcpp;
int main() {
    mcpp::runner("app", "$TMP/run-B.sh");
    return 0;
}
CPP
if "$MCPP" build > both.log 2>&1; then fail "1: a runner supplied twice was accepted" both.log; fi
grep -q "the dependency 'dep' and this project's build program both supply a runner named 'app'" both.log \
    || fail "1: the refusal does not name the dependency and the runner" both.log

# ── 2 ─────────────────────────────────────────────────────────────────────
# The host row's canonical name, which is what a native build resolves to.
arch=$(uname -m)
case "$arch" in arm64) arch=aarch64 ;; esac
case "$(uname -s)" in
    Linux)  row="$arch-linux-gnu" ;;
    Darwin) row="$arch-macos" ;;
    *)      echo "SKIP: no canonical host row for $(uname -s)"; exit 0 ;;
esac
printf '\n[target.%s.runners]\napp = ["%s/run-C.sh"]\n' "$row" "$TMP" >> mcpp.toml
"$MCPP" build > manifest.log 2>&1 || fail "2: the manifest's runner did not settle it" manifest.log
"$MCPP" run --runner app > run.log 2>&1 || true
grep -q "RUNNER-C" run.log || fail "2: the manifest's runner was not used" run.log

echo "PASS: 684_a_runner_supplied_twice_is_refused"
