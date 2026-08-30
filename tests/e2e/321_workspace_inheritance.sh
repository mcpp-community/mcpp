#!/usr/bin/env bash
# requires: gcc
# 321_workspace_inheritance.sh — [workspace.package] / [workspace.build] (#527).
#
# THE ASSERTION IS ON THE COMPILE COMMAND, NOT ON BUILD SUCCESS. A member that
# inherits nothing still builds; only the flags say whether inheritance
# happened, and only they say WHICH value won when both sides declared one.
#
# Four members, chosen so that each one falsifies a different wrong
# implementation:
#
#   silent    declares nothing                  → inherits standard and cxxflags
#   pinned    declares standard = "c++23"       → KEEPS it under a c++26 workspace.
#                                                 Fails if declaredness was faked
#                                                 by comparing against the default,
#                                                 since the pin IS the default.
#   adds      declares its own cxxflags         → gets BOTH, workspace first
#   inside    built from inside its own dir     → the second inheritance site;
#                                                 an assertion on the first proves
#                                                 nothing about it
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p silent/src pinned/src adds/src shared/inc
printf '#define SHARED_HEADER_FOUND 1\n' > shared/inc/shared.h

cat > mcpp.toml <<'EOF'
[workspace]
members = ["silent", "pinned", "adds"]

[workspace.package]
standard = 26
version  = "0.4.2"
license  = "Apache-2.0"

[workspace.build]
cxxflags     = ["-DFROM_WORKSPACE=1"]
include_dirs = ["shared/inc"]
EOF

# `version` is deliberately absent from every member: it is a required field,
# and the workspace supplying it is the half of this feature that the parser
# cannot enforce on its own.
printf '[package]\nname = "silent"\n' > silent/mcpp.toml
printf '[package]\nname = "pinned"\nstandard = "c++23"\n' > pinned/mcpp.toml
printf '[package]\nname = "adds"\n\n[build]\ncxxflags = ["-DFROM_MEMBER=1"]\n' > adds/mcpp.toml
for m in silent pinned adds; do
    # The include also proves the workspace-relative path was anchored to the
    # WORKSPACE ROOT and not to each member: `shared/inc` lives at
    # `<workspace>/shared/inc`, and resolving it per-member would look for
    # `<member>/shared/inc` and fail with a missing header three members deep,
    # naming neither the manifest that declared it nor the root it meant.
    printf '#include <shared.h>\n#if !defined(FROM_WORKSPACE) || !defined(SHARED_HEADER_FOUND)\n#error "workspace [build] did not reach the member"\n#endif\nint main(){return 0;}\n' > "$m/src/main.cpp"
done

flags_of() {  # $1 = member dir
    grep -oE '\-std=c\+\+[0-9a-z]+|\-DFROM_[A-Z]+=1' "$1/compile_commands.json" \
        | sort -u | tr '\n' ' '
}

for m in silent pinned adds; do
    "$MCPP" build -p "$m" > "build_$m.log" 2>&1 || { cat "build_$m.log"; exit 1; }
done

# ── silent: inherits both ───────────────────────────────────────────────────
got=$(flags_of silent)
case "$got" in
    *"-DFROM_WORKSPACE=1"*) ;;
    *) echo "FAIL: member 'silent' did not inherit [workspace.build] cxxflags"
       echo "      got: $got"; exit 1 ;;
esac
case "$got" in
    *"-std=c++26"*) ;;
    *) echo "FAIL: member 'silent' did not inherit [workspace.package] standard"
       echo "      got: $got"; exit 1 ;;
esac
grep -q '"version": *"0.4.2"' silent/target/*/*/resolution.json 2>/dev/null \
    || grep -q "0.4.2" build_silent.log \
    || { echo "FAIL: member 'silent' did not inherit [workspace.package] version"
         cat build_silent.log; exit 1; }

# ── pinned: its own declaration wins ────────────────────────────────────────
#
# THE ONE THAT CATCHES A FAKED DECLAREDNESS BIT. c++23 is also the default, so
# an implementation that decided "did the member declare it?" by comparing
# against the default would overwrite this pin with c++26 and every other
# assertion in this file would still pass.
got=$(flags_of pinned)
case "$got" in
    *"-std=c++23"*) ;;
    *) echo "FAIL: member 'pinned' declared standard = c++23 and did not get it"
       echo "      got: $got"; exit 1 ;;
esac
case "$got" in
    *"-DFROM_WORKSPACE=1"*) ;;
    *) echo "FAIL: member 'pinned' should still inherit [workspace.build]"
       echo "      got: $got"; exit 1 ;;
esac

# ── adds: both, workspace first ─────────────────────────────────────────────
got=$(flags_of adds)
for want in "-DFROM_WORKSPACE=1" "-DFROM_MEMBER=1"; do
    case "$got" in
        *"$want"*) ;;
        *) echo "FAIL: member 'adds' is missing $want"; echo "      got: $got"; exit 1 ;;
    esac
done
# Order is load-bearing: later wins on a compiler command line, so a member's
# own flag has to come after the workspace's. The CDB is pretty-printed with one
# argument per line, so the question is which line number comes first — matching
# both on a single line would silently never fire.
ws_at=$(grep -n 'FROM_WORKSPACE=1' adds/compile_commands.json | head -1 | cut -d: -f1)
mem_at=$(grep -n 'FROM_MEMBER=1' adds/compile_commands.json | head -1 | cut -d: -f1)
[ -n "$ws_at" ] && [ -n "$mem_at" ] || {
    echo "FAIL: could not locate both flags in the compile command"; exit 1; }
[ "$ws_at" -lt "$mem_at" ] || {
    echo "FAIL: workspace cxxflags must precede the member's own"
    echo "      workspace at line $ws_at, member at line $mem_at"; exit 1; }

# ── inside: the second inheritance site ─────────────────────────────────────
( cd silent && "$MCPP" build > ../build_inside.log 2>&1 ) \
    || { cat build_inside.log; exit 1; }
got=$(flags_of silent)
case "$got" in
    *"-DFROM_WORKSPACE=1"*"-std=c++26"* | *"-std=c++26"*"-DFROM_WORKSPACE=1"*) ;;
    *) echo "FAIL: inheritance did not happen when the command was issued"
       echo "      inside the member directory (the second site)"
       echo "      got: $got"; exit 1 ;;
esac

# ── a member with no version and no workspace value is still refused ────────
#
# THE DENOMINATOR. Deferring the required-field check must not delete it: with
# the workspace value removed, the same member must fail, or this feature has
# quietly made `version` optional for everyone.
sed -i 's/^version  = "0.4.2"$//' mcpp.toml
rm -rf silent/target
if ( cd silent && "$MCPP" build > ../novers.log 2>&1 ); then
    echo "FAIL: a member with no version and no [workspace.package] version built"
    cat novers.log
    exit 1
fi
grep -q "package.version" novers.log || {
    echo "FAIL: the refusal does not name the missing field"
    cat novers.log
    exit 1
}

# ── a SIBLING member reached as a `path` dependency inherits too ────────────
#
# THE ORDINARY WORKSPACE SHAPE, AND THE ONE THAT WAS MISSED. Inheritance runs
# where the command's own manifest is loaded, so `mcpp build -p consumer` gave
# the consumer the workspace flags and gave the sibling none — while compiling
# both in the same command. `[workspace.package] standard` hid the gap, because
# the standard is imposed graph-wide from the root for BMI compatibility and
# reached the sibling anyway.
#
# The negative is in the same fixture on purpose: a `path` dependency that is
# NOT a member (a vendored copy, an example) must not acquire the flags, and a
# fix that inherited to every path dependency would pass the positive alone.
cat > mcpp.toml <<'EOF'
[workspace]
members = ["silent", "pinned", "adds", "consumer"]

[workspace.package]
standard = 26
version  = "0.4.2"

[workspace.build]
cxxflags = ["-DFROM_WORKSPACE=1"]
EOF
mkdir -p consumer/src vendored/src
printf '[package]\nname = "consumer"\n\n[dependencies]\nadds = { path = "../adds" }\nvend = { path = "../vendored" }\n' > consumer/mcpp.toml
printf 'import addslib;\nimport vend;\nint main(){ return addslib()+vend_v()==3 ? 0 : 1; }\n' > consumer/src/main.cpp
# `adds` becomes a library so it can be imported; its own source asserts it saw
# the workspace flag while being built as somebody else's dependency.
printf '[package]\nname = "adds"\n\n[targets.adds]\nkind = "lib"\n\n[build]\ncxxflags = ["-DFROM_MEMBER=1"]\n' > adds/mcpp.toml
rm -f adds/src/main.cpp
printf '#ifndef FROM_WORKSPACE\n#error "a SIBLING member built as a path dependency did not inherit"\n#endif\nexport module addslib;\nexport int addslib(){ return 1; }\n' > adds/src/addslib.cppm
printf '[package]\nname = "vend"\nversion = "0.1.0"\n\n[targets.vend]\nkind = "lib"\n' > vendored/mcpp.toml
printf '#ifdef FROM_WORKSPACE\n#error "a NON-member path dependency must not acquire workspace flags"\n#endif\nexport module vend;\nexport int vend_v(){ return 2; }\n' > vendored/src/vend.cppm

"$MCPP" build -p consumer > sibling.log 2>&1 || { cat sibling.log; exit 1; }

echo "PASS: 321_workspace_inheritance"
