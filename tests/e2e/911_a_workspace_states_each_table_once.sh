#!/usr/bin/env bash
# requires: unix-shell
# 911 -- what a workspace root states, and to whom (docs/07, mcpp#797).
#
#   R4  `[workspace.xlings.overrides]` on a root with [package] that depends on
#       a member by path: the overrides belong to the root of the build, and
#       the member reached as a dependency does not receive them;
#   R5  the root's own `[xlings.overrides]` serves its own build without a
#       position warning; a member built alone still receives it by position,
#       and is told the reading is deprecated;
#   V   `[workspace.xlings.overrides]` on a root without [package] serves its
#       members: the declared payload is not provisioned;
#   W1  a build.mcpp beside a root without [package] is reported;
#   W2  `features` of a `[workspace.dependencies]` entry reach the member that
#       inherits it, and a request key there is reported.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export NO_COLOR=1
MCPP="${MCPP:-mcpp}"

mkdir -p "$TMP/opt/bin"
printf '#!/bin/sh\necho tool\n' > "$TMP/opt/bin/tool"; chmod +x "$TMP/opt/bin/tool"

rooted() {  # $1 = dir, $2 = tables of the root
    mkdir -p "$1/src" "$1/sub/src"
    printf '[package]\nname = "root"\nversion = "0.1.0"\n[dependencies]\nsub = { path = "sub" }\n[targets.root]\nkind = "bin"\nmain = "src/main.cpp"\n[workspace]\nmembers = ["sub"]\n%s\n' "$2" > "$1/mcpp.toml"
    printf 'import sub;\nint main() { return f() - 1; }\n' > "$1/src/main.cpp"
    printf '[package]\nname = "sub"\nversion = "0.1.0"\n[targets.sub]\nkind = "lib"\n' > "$1/sub/mcpp.toml"
    printf 'export module sub;\nexport int f() { return 1; }\n' > "$1/sub/src/sub.cppm"
}

# R4
rooted "$TMP/r4" "$(printf '[workspace.xlings.overrides]\n"xim:mcpp-e2e-absent-tool" = "%s"' "$TMP/opt/bin/tool")"
( cd "$TMP/r4" && "$MCPP" build > r4.log 2>&1 ) || fail "R4: the workspace's overrides failed its own build" "$TMP/r4/r4.log"
grep -q "is a dependency of this build" "$TMP/r4/r4.log" && fail "R4: the member received the overrides" "$TMP/r4/r4.log"

# R5
rooted "$TMP/r5" "$(printf '[xlings.overrides]\n"xim:mcpp-e2e-absent-tool" = "%s"' "$TMP/opt/bin/tool")"
( cd "$TMP/r5" && "$MCPP" build > r5.log 2>&1 ) || fail "R5: the root's own overrides failed its own build" "$TMP/r5/r5.log"
grep -q "workspace-position\|deprecated since" "$TMP/r5/r5.log" \
    && fail "R5: the root's own build was told about a position reading" "$TMP/r5/r5.log"
( cd "$TMP/r5/sub" && "$MCPP" build > r5b.log 2>&1 ) || fail "R5: the member did not build alone" "$TMP/r5/sub/r5b.log"
grep -q "deprecated since mcpp 2026.10.10.1 and removed from mcpp 2027.1" "$TMP/r5/sub/r5b.log" \
    || fail "R5: the member built alone was not told the position reading is deprecated" "$TMP/r5/sub/r5b.log"

# V
mkdir -p "$TMP/v/m/src"
printf '[workspace]\nmembers = ["m"]\n[workspace.xlings.overrides]\n"xim:mcpp-e2e-absent-tool" = { program = "%s", version = "2.1" }\n' "$TMP/opt/bin/tool" > "$TMP/v/mcpp.toml"
printf '[package]\nname = "m"\nversion = "0.1.0"\n[xlings.workspace]\n"xim:mcpp-e2e-absent-tool" = ">=2.0"\n[targets.m]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/v/m/mcpp.toml"
printf 'int main() { return 0; }\n' > "$TMP/v/m/src/main.cpp"
( cd "$TMP/v" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build > v.log 2>&1 ) \
    || fail "V: [workspace.xlings.overrides] did not serve a member of a root without [package]" "$TMP/v/v.log"
grep -q "custom: xim:mcpp-e2e-absent-tool" "$TMP/v/v.log" || fail "V: the build does not state the source" "$TMP/v/v.log"
( cd "$TMP/v/m" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build > v2.log 2>&1 ) \
    || fail "V: the member built from its own directory was not served" "$TMP/v/m/v2.log"

# W1
mkdir -p "$TMP/w1/m/src"
printf '[workspace]\nmembers = ["m"]\n' > "$TMP/w1/mcpp.toml"
printf 'int main() { return 1; }\n' > "$TMP/w1/build.mcpp"
printf '[package]\nname = "m"\nversion = "0.1.0"\n[targets.m]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/w1/m/mcpp.toml"
printf 'int main() { return 0; }\n' > "$TMP/w1/m/src/main.cpp"
( cd "$TMP/w1" && "$MCPP" build > w1.log 2>&1 ) || fail "W1: the workspace did not build" "$TMP/w1/w1.log"
grep -q "build.mcpp is written beside a workspace root without \[package\]" "$TMP/w1/w1.log" \
    || fail "W1: the build.mcpp beside the root was not reported" "$TMP/w1/w1.log"

# W2
mkdir -p "$TMP/w2/hl/src" "$TMP/w2/m/src"
printf '[package]\nname = "hl"\nversion = "0.1.0"\n[features]\nx = []\n[targets.hl]\nkind = "lib"\n' > "$TMP/w2/hl/mcpp.toml"
cat > "$TMP/w2/hl/src/hl.cppm" <<'EOF'
export module hl;
export int feature_x() {
#ifdef MCPP_FEATURE_X
    return 1;
#else
    return 0;
#endif
}
EOF
printf '[workspace]\nmembers = ["m"]\n[workspace.dependencies]\nhl = { path = "hl", features = ["x"], linkage = "static" }\n' > "$TMP/w2/mcpp.toml"
printf '[package]\nname = "m"\nversion = "0.1.0"\n[dependencies]\nhl.workspace = true\n[targets.m]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/w2/m/mcpp.toml"
printf '#include <cstdio>\nimport hl;\nint main() { std::printf("X=%%d\\n", feature_x()); }\n' > "$TMP/w2/m/src/main.cpp"
( cd "$TMP/w2" && "$MCPP" build > w2.log 2>&1 ) || fail "W2: the workspace did not build" "$TMP/w2/w2.log"
[ "$(cd "$TMP/w2" && "$MCPP" run -p m 2>/dev/null | grep '^X=' | tail -1)" = "X=1" ] \
    || fail "W2: the feature stated in [workspace.dependencies] did not reach the member" "$TMP/w2/w2.log"
grep -q "'linkage' is a request of a member's own edge" "$TMP/w2/w2.log" \
    || fail "W2: the request key in [workspace.dependencies] was not reported" "$TMP/w2/w2.log"
echo "PASS: 911_a_workspace_states_each_table_once"
