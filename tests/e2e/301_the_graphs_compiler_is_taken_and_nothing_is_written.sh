#!/usr/bin/env bash
# requires: unix-shell jq
# A compiler the dependency graph requires is USED, not merely checked — and
# selecting it writes no configuration.
#
# ⭐⭐ `provides` AND `requires` ARE TWO HALVES OF ONE VOCABULARY AND THEY WERE
# READ AT OPPOSITE ENDS OF `prepare_build`. The block that decides the toolchain
# once the graph exists consulted the first; the second was collected a thousand
# lines later and used only to reject the outcome.
#
# ⚠️ MEASURED ON 2026.8.26.1, three-line manifest, llvm already installed:
#
#     $ mcpp build                       # global default gcc@16.1.0
#       error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
#              Select that compiler …  mcpp toolchain default llvm
#     $ MCPP_TOOLCHAIN=llvm@22.1.8 mcpp build
#       Finished dev [unoptimized + debuginfo] in 1.02s
#
# Nothing was missing. The remedy printed was a GLOBAL change — the default for
# every project on the machine — made because ONE project's dependency asked.
#
# ⭐ AND THE SECOND HALF IS THE POINT OF THE FIRST. `resolve_target_toolchain`
# has two call sites and both are downstream of the graph, so every
# `write_default_toolchain` lives on a branch this selection no longer enters.
# "Touch no configuration" is not a rule someone has to remember; it is where
# the decision sits. The sha256 is what enforces it.
#
# ⚠️ THE CRITERION IS A HASH, NOT "THE BUILD SUCCEEDED". A build that succeeds
# and rewrites the user's default is exactly the behaviour being removed, and
# the two are indistinguishable from the exit code.
set -e

MCPP="${MCPP:-mcpp}"

# ⭐ THE REQUIRED FAMILY IS CHOSEN AGAINST THIS MACHINE, NOT HARDCODED. The
# claim is "a requirement that differs from mcpp's own answer is applied", so
# the test needs a family that (a) is installed here and (b) is not the one
# already resolving. Hardcoding `llvm` would silently assert nothing on a box
# whose default is already llvm.
installed="$("$MCPP" toolchain list --format json 2>/dev/null \
             | jq -r '[.data.toolchains[].family] | unique | .[]' | tr -d '\r')"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/needs/src" "$work/app/src"
printf 'extern "C" int main(int, char**, char**) { return 0; }\n' > "$work/app/src/main.cpp"
printf 'export module needs_it;\nexport int needs_it() { return 0; }\n' \
    > "$work/needs/src/needs_it.cppm"

# ⚠️⚠️ THE BASELINE IS MEASURED IN THE PROJECT UNDER TEST, AND MEASURING IT
# ANYWHERE ELSE MAKES THIS FILE ASSERT NOTHING.
#
# The first draft asked `why toolchain` from whatever directory the runner
# happened to start in — mcpp's own repo, whose `mcpp.toml` states gcc. It read
# `gcc`, chose `llvm` as the family to require, and then ran in a scratch
# project where the GLOBAL default was already llvm. Every assertion passed
# while the requirement had changed nothing at all.
#
# The baseline has to come from the same manifest, minus the dependency.
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > "$work/app/mcpp.toml"
cd "$work/app"
current="$("$MCPP" why toolchain --format json 2>/dev/null \
           | jq -r '.data.compiler.family // "-"' | tr -d '\r')"
# `compiler.family` reports the driver identity (`clang`); packages and users
# write the family (`llvm`). One name for the axis, as compiler_family() says.
case "$current" in clang) current=llvm ;; esac

want=""
for f in $installed; do
    [ "$f" = "$current" ] && continue
    case "$f" in gcc|llvm) want="$f"; break ;; esac
done
if [ -z "$want" ]; then
    echo "SKIP: this project already resolves '$current' and no other family is"
    echo "      installed here, so no requirement could differ from it"
    exit 0
fi

printf '[package]\nname     = "needs-%s"\nversion  = "0.1.0"\nrequires = ["mcpp:compiler=%s"]\n' \
    "$want" "$want" > "$work/needs/mcpp.toml"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\nneeds-%s = { path = "../needs" }\n' \
    "$want" > "$work/app/mcpp.toml"

cfg="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
before=""
[ -f "$cfg" ] && before="$(sha256sum "$cfg" | cut -d' ' -f1)"

j="$("$MCPP" why toolchain --format json 2>/dev/null)"
reason="$(printf '%s' "$j" | jq -r '.data.reason // "-"' | tr -d '\r')"
got="$(printf '%s' "$j" | jq -r '.data.compiler.family // "-"' | tr -d '\r')"
case "$got" in clang) got=llvm ;; esac

if [ "$reason" != none ]; then
    echo "FAIL: the graph required '$want' and mcpp refused with '$reason'"
    printf '%s' "$j" | jq -r '.diagnostics[].message' | sed 's/^/        /'
    exit 1
fi
if [ "$got" != "$want" ]; then
    echo "FAIL: the graph required '$want', mcpp resolved '$got'"
    exit 1
fi
echo "  ok  the graph required '$want' and mcpp took it (was '$current')"

# ── And it changed no configuration ──────────────────────────────────────
after=""
[ -f "$cfg" ] && after="$(sha256sum "$cfg" | cut -d' ' -f1)"
if [ "$before" = "$after" ]; then
    echo "  ok  and $cfg is byte-identical"
else
    echo "FAIL: selecting the graph's compiler rewrote the global configuration"
    echo "        before $before"
    echo "        after  $after"
    exit 1
fi

# ── And the status line says who asked ───────────────────────────────────
#
# ⚠️ A COMPILER THE USER DID NOT NAME, REPORTED WITHOUT ITS REASON, IS A RULE
# THAT CAN ONLY BE LEARNED BY EXPERIMENT. The same argument that put a reason on
# the target row's substitution applies here, and more so: this one overrides a
# value the user set with `mcpp toolchain default`.
out="$("$MCPP" build 2>&1 || true)"
ok=1
printf '%s\n' "$out" | grep -q "needs-$want"          || ok=0
printf '%s\n' "$out" | grep -q "mcpp:compiler=$want"  || ok=0
if [ "$ok" = 1 ]; then
    echo "  ok  and the status line names the package that asked"
else
    echo "FAIL: the resolution does not say who required this compiler"
    printf '%s\n' "$out" | head -6 | sed 's/^/        /'
    exit 1
fi

echo "OK: the graph's compiler is taken and nothing is written"
