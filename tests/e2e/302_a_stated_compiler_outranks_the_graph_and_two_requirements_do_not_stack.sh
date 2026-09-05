#!/usr/bin/env bash
# requires: unix-shell jq
# The graph's compiler requirement is applied where mcpp's own answer was
# revisable — and only there.
#
# THE RANK IS NOT NEW. `TcOrigin` already sorted these and
# `tc_origin_is_user_explicit` already answered "may mcpp revise this"; the
# defect the previous test covers was that nobody asked. This file is the other
# side: the one case that must still refuse, and the one that cannot be resolved
# by choosing.
#
#   half one   `[toolchain] default = <other>`   the project stated it → refuse
#   half two   two packages, two families        no compiler satisfies both
#
# AND THE ADVICE IS PART OF THE CLAIM. Until 2026.8.26.2 the first remedy
# offered was `mcpp toolchain default <family>` — global, and in the only case
# that still reaches here it does not even work, because a project-level
# statement is what decided. A remedy that cannot fix the failure it is printed
# under is worse than none.
set -e

MCPP="${MCPP:-mcpp}"

installed="$("$MCPP" toolchain list --format json 2>/dev/null \
             | jq -r '[.data.toolchains[] | select(.family=="gcc" or .family=="llvm")
                       | .family + "@" + .version] | unique | .[]' | tr -d '\r')"
gccspec="$(printf '%s\n' "$installed" | grep '^gcc@'  | head -1)"
llvmspec="$(printf '%s\n' "$installed" | grep '^llvm@' | head -1)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/needs-gcc/src" "$work/needs-llvm/src" "$work/app/src"
printf '[package]\nname     = "needs-gcc"\nversion  = "0.1.0"\nrequires = ["mcpp:compiler=gcc"]\n' \
    > "$work/needs-gcc/mcpp.toml"
printf '[package]\nname     = "needs-llvm"\nversion  = "0.1.0"\nrequires = ["mcpp:compiler=llvm"]\n' \
    > "$work/needs-llvm/mcpp.toml"
printf 'export module needs_gcc;\nexport int ng() { return 1; }\n'  > "$work/needs-gcc/src/a.cppm"
printf 'export module needs_llvm;\nexport int nl() { return 2; }\n' > "$work/needs-llvm/src/b.cppm"
printf 'extern "C" int main(int, char**, char**) { return 0; }\n'   > "$work/app/src/main.cpp"
cd "$work/app"

# ── Half one: the project states its own compiler ────────────────────────
if [ -n "$gccspec" ] && [ -n "$llvmspec" ]; then
    printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "%s"\n\n[dependencies]\nneeds-llvm = { path = "../needs-llvm" }\n' \
        "$gccspec" > mcpp.toml
    j="$("$MCPP" why toolchain --format json 2>/dev/null)"
    reason="$(printf '%s' "$j" | jq -r '.data.reason // "-"' | tr -d '\r')"
    msg="$(printf '%s' "$j" | jq -r '.diagnostics[].message' | tr -d '\r')"

    if [ "$reason" != layer-requirement ]; then
        echo "FAIL: a stated compiler was overridden by the graph (reason '$reason')"
        printf '%s\n' "$msg" | sed 's/^/        /'
        exit 1
    fi
    echo "  ok  a compiler the project stated is not revised"

    # AND THE REMEDY POINTS AT THE STATEMENT THAT DECIDED.
    ok=1
    printf '%s\n' "$msg" | grep -q '\[toolchain\]'        || ok=0
    printf '%s\n' "$msg" | grep -q 'mcpp.toml'            || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok  and the remedy names it"
    else
        echo "FAIL: the refusal does not name where the compiler was stated"
        printf '%s\n' "$msg" | sed 's/^/        /'
        exit 1
    fi

    # AND IT DOES NOT SEND THE READER TO A GLOBAL SETTING. Changing
    # `mcpp toolchain default` here fixes nothing: the project's own statement
    # is what is being used.
    if printf '%s\n' "$msg" | grep -q 'mcpp toolchain default'; then
        echo "FAIL: the remedy is a global change that would not fix this failure"
        printf '%s\n' "$msg" | sed 's/^/        /'
        exit 1
    fi
    echo "  ok  and it does not offer a global change that would not work"
else
    echo "SKIP(half one): both gcc and llvm must be installed to state one of them"
fi

# ── Half two: two packages, two families ─────────────────────────────────
#
# ONE SUPPLIER PER LAYER, AND TWO IS AN ERROR RATHER THAN A PICK — the rule
# `provides` already follows. Resolving by graph-traversal order would make the
# answer depend on an order the author neither writes nor can predict, and would
# silently satisfy one package while failing the other inside a header.
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\nneeds-gcc  = { path = "../needs-gcc" }\nneeds-llvm = { path = "../needs-llvm" }\n' \
    > mcpp.toml
j2="$("$MCPP" why toolchain --format json 2>/dev/null)"
reason2="$(printf '%s' "$j2" | jq -r '.data.reason // "-"' | tr -d '\r')"
msg2="$(printf '%s' "$j2" | jq -r '.diagnostics[].message' | tr -d '\r')"

if [ "$reason2" != compiler-requirement-conflict ]; then
    echo "FAIL: two conflicting requirements gave reason '$reason2'"
    printf '%s\n' "$msg2" | sed 's/^/        /'
    exit 1
fi
echo "  ok  two packages requiring different compilers is an error"

ok=1
printf '%s\n' "$msg2" | grep -q 'needs-gcc'  || ok=0
printf '%s\n' "$msg2" | grep -q 'needs-llvm' || ok=0
if [ "$ok" = 1 ]; then
    echo "  ok  and both packages are named"
else
    echo "FAIL: the conflict does not name both packages"
    printf '%s\n' "$msg2" | sed 's/^/        /'
    exit 1
fi

# ── Half three: a capability row's remedy is not a convention row's ──────
#
# THE TWO ROWS REFUSE UNDER ONE RULE AND FOR TWO REASONS, AND ONE REMEDY
# DOES NOT SERVE BOTH.
#
# A convention pin is cancelled by a graph that supplies the target's system, so
# "depend on a package that supplies it" is the way out. A capability pin is
# not — it stays applied whatever the graph supplies, because no other family
# emits the target. Printed there, that remedy is an instruction the sentence
# directly above it has already ruled out.
#
# FOUND BY READING THE MESSAGE, NOT BY A FAILING BUILD. This half exists so
# the next rewording cannot put it back.
#
# The refusal is decided from the VOCABULARY (the row's pin) before any
# payload is resolved, so this half is host-independent and needs nothing
# installed.
printf '[package]\nname    = "app"\nversion = "0.1.0"\n\n[dependencies]\nneeds-gcc = { path = "../needs-gcc" }\n' \
    > mcpp.toml
j3="$("$MCPP" why toolchain --target riscv64-none-elf --format json 2>/dev/null)"
reason3="$(printf '%s' "$j3" | jq -r '.data.reason // "-"' | tr -d '\r')"
msg3="$(printf '%s' "$j3" | jq -r '.diagnostics[].message' | tr -d '\r')"

if [ "$reason3" != compiler-requirement-conflict ]; then
    echo "FAIL: a capability row against a gcc requirement gave reason '$reason3'"
    printf '%s\n' "$msg3" | sed 's/^/        /'
    exit 1
fi
echo "  ok  a capability row refuses a requirement it cannot satisfy"

if printf '%s\n' "$msg3" | grep -qi 'capability'; then
    echo "  ok  and it says the pin is a capability"
else
    echo "FAIL: the refusal does not say the row's pin is a capability"
    printf '%s\n' "$msg3" | sed 's/^/        /'
    exit 1
fi
if printf '%s\n' "$msg3" | grep -q "supplies this target's system"; then
    echo "FAIL: the remedy offers to supply the system, which a capability pin ignores"
    printf '%s\n' "$msg3" | sed 's/^/        /'
    exit 1
fi
echo "  ok  and it does not offer a remedy a capability pin ignores"

echo "OK: a stated compiler outranks the graph and two requirements do not stack"
