#!/usr/bin/env bash
# check_modules_wiring.sh — every package under `modules/` is wired into all
# THREE places that have to know about it, and no comment is trusted to keep
# them in agreement.
#
# WHY THIS EXISTS
#
# `modules/` holds independent packages that mcpp links into itself. Adding one
# means editing four files, and three of those edits fail LATER and ELSEWHERE
# when they are forgotten:
#
#   1. modules/<x>/mcpp.toml          the package itself
#   2. mcpp.toml [dependencies.mcpp]  what makes it importable
#   3. mcpp.toml [workspace] members  what makes `-p` and `mcpp test` see it
#   4. the two xmake source lists     the bootstrap and the benchmark, which
#                                     have no package manager and compile the
#                                     sources directly
#
# Forgetting (2) fails at once and is harmless. Forgetting (3) is invisible
# until someone runs the member's tests. Forgetting (4) is invisible until the
# macOS bootstrap runs on a machine with no mcpp — which is the one place that
# cannot be fixed by rebuilding.
#
# This repository has paid for the same shape before: `check_version_pins.sh`
# exists because a comment saying "keep in lock-step" kept not being read.
#
# WHAT THIS CANNOT CATCH: whether a module SHOULD have been split out, and
# whether the split is layered correctly. Absence of an entry is checkable;
# absence of judgement is not.
set -uo pipefail
cd "$(dirname "$0")/../.."

fail=0
untested=()
bad() { echo "FAIL: $*" >&2; fail=1; }
note() { echo "  $*"; }

[[ -d modules ]] || { echo "no modules/ directory; nothing to check"; exit 0; }

mapfile -t dirs < <(find modules -mindepth 1 -maxdepth 1 -type d | sort)
(( ${#dirs[@]} > 0 )) || { echo "modules/ is empty; nothing to check"; exit 0; }
echo "checking ${#dirs[@]} module package(s)"

for d in "${dirs[@]}"; do
    name="${d#modules/}"

    [[ -f "$d/mcpp.toml" ]] || { bad "$d has no mcpp.toml"; continue; }

    # The package's declared name must match the directory, because the
    # dependency KEY has to match the package identity and the key is written
    # against this path. A mismatch is reported by mcpp as "resolved to package
    # X (mismatch with declared name Y)" from inside resolution, which is a
    # long way from the line that is wrong.
    declared=$(grep -oE '^name[[:space:]]*=[[:space:]]*"[^"]+"' "$d/mcpp.toml" \
               | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
    [[ "$declared" == "$name" ]] \
        || bad "$d/mcpp.toml declares name = \"$declared\" but the directory is \"$name\""

    ns=$(grep -oE '^namespace[[:space:]]*=[[:space:]]*"[^"]+"' "$d/mcpp.toml" \
         | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
    [[ "$ns" == "mcpp" ]] \
        || bad "$d/mcpp.toml has namespace = \"${ns:-<none>}\"; modules of this project use \"mcpp\" so they do not squat bare registry names"

    # (2) importable
    grep -qE "^[[:space:]]*$name[[:space:]]*=[[:space:]]*\{[[:space:]]*path[[:space:]]*=[[:space:]]*\"modules/$name\"" mcpp.toml \
        || bad "mcpp.toml [dependencies.mcpp] has no entry for '$name' — it will not be importable"

    # (3) addressable
    grep -qF "\"modules/$name\"" <(sed -n '/^\[workspace\]/,/^\[/p' mcpp.toml) \
        || bad "mcpp.toml [workspace] members does not list modules/$name — 'mcpp test -p $name' will not find it"

    # (4) the two package-manager-less builds
    [[ -d "$d/src" ]] \
        || bad "$d has no src/ — the xmake source lists glob modules/*/src/**.cppm and would silently miss it"

    # (5) tests, REPORTED rather than required.
    #
    # `mcpp test -p <member>` exits 0 for a member with no tests, so CI's
    # per-subsystem loop is green either way and "has no tests" is
    # indistinguishable from "tests pass" in its output. Naming them here is
    # what makes the difference visible; failing would be wrong, because a
    # package of vendored parsers legitimately has nothing of its own to state.
    n=0
    [[ -d "$d/tests" ]] && n=$(find "$d/tests" -name '*.cpp' | wc -l)
    if (( n == 0 )); then
        untested+=("$name")
    else
        note "$name: $n subsystem test file(s)"
        grep -q '^\[dev-dependencies' "$d/mcpp.toml" \
            || bad "$d has tests but declares no [dev-dependencies] — they cannot link a framework"
    fi
done

if (( ${#untested[@]} )); then
    echo
    echo "  no subsystem tests (allowed, and reported so it is not read as a pass):"
    printf '    %s\n' "${untested[@]}"
fi

# The two xmake source lists must actually carry the glob. Asserted by content
# rather than by trusting the loop above: a renamed pattern would leave every
# per-module check passing while compiling nothing.
for f in scripts/bootstrap-macos.sh .github/workflows/bootstrap-macos.yml \
         bench/projects/mcpp/xmake.lua; do
    [[ -f "$f" ]] || { bad "$f is missing"; continue; }
    grep -qF 'modules/*/src/**.cppm' "$f" \
        || bad "$f does not add modules/*/src/**.cppm — it would build a strict subset of mcpp and report success"
done

# The vendored json header is reached through a private include dir. Its path
# appears in three places and has already moved once.
for f in scripts/bootstrap-macos.sh .github/workflows/bootstrap-macos.yml \
         bench/projects/mcpp/xmake.lua; do
    grep -qF 'modules/libs/src/json' "$f" \
        || bad "$f does not add the json include dir (modules/libs/src/json)"
done
[[ -f modules/libs/src/json/json.hpp ]] \
    || bad "modules/libs/src/json/json.hpp is missing, but three files name that path"

# Anything that hashes mcpp's sources has to hash BOTH trees. A cache key that
# saw only `src/**` would restore a target/ built from different sources and
# report success -- the exact failure a cache key exists to prevent, arriving
# silently. Found by sweeping for stale paths after the split, not by a test.
#
# ⚠️ THE UNIT IS THE LINE, NOT THE FILE. The first version asked whether the
# FILE mentioned `modules/**`, and passed — satisfied by the comment sitting
# above the key explaining why `modules/**` belongs there. A check that a
# comment can satisfy is checking the prose.
while IFS= read -r hit; do
    f="${hit%%:*}"
    line="${hit#*:}"
    case "$line" in
        *"hashFiles('src/**'"*)
            case "$line" in
                *"modules/**"*) ;;
                *) bad "$f hashes src/** but not modules/** on that line — half of mcpp's sources would not invalidate the cache" ;;
            esac ;;
    esac
done < <(grep -rn "hashFiles(" .github/ 2>/dev/null | grep -v 'check_modules_wiring.sh')

if (( fail )); then
    echo
    echo "modules/ wiring is incomplete." >&2
    exit 1
fi
echo "OK: modules/ wiring is complete"
