#!/usr/bin/env bash
# requires: gcc
# 793_an_unknown_action_role_is_refused.sh — SPEC-007 R3.6 (design §5.4 P,
# mcpp#702): an action role that names none of the five mcpp knows is
# refused, naming the value and the list. Before this, `decode_action` mapped
# any unrecognised string to `source` silently, so a misspelt role changed
# an action's meaning without a word (a "prepare" typo'd "prepar" quietly
# became a source-generating action instead of construction).
#
# `a.role` is spelled as a bare string here, on purpose: the typed API's
# `mcpp::roles` constants exist so this exact mistake fails to COMPILE
# instead, and this test is about the OTHER half of R3.6 -- the wire-level
# refusal that still has to hold for a hand-written frozen-surface program,
# which has no constants to misuse in the first place.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/app/src"
cd "$TMP/app"

cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF
printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); }\n' > src/main.cpp

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    mcpp::action a;
    a.id   = "typo";
    a.role = "prepar";   // typo of "prepare" -- not one of the five roles
    a.arg("true")
     .output((std::string(mcpp::out_dir()) + "/typo.stamp").c_str())
     .submit();
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"
if "$MCPP" build > b.log 2>&1; then
    cat b.log
    echo "FAIL: an action with an unrecognised role let the build succeed"
    exit 1
fi

grep -qF '"prepar"' b.log \
    || { cat b.log; echo "FAIL: the refusal does not name the misspelt role"; exit 1; }
for role in source check object artifact prepare; do
    grep -qF "$role" b.log \
        || { cat b.log; echo "FAIL: the refusal does not list role '$role'"; exit 1; }
done

# THE CONTROL: each of the five real roles is accepted (well-formed
# otherwise), so this is a property of the STRING, not of `action` as a
# whole rejecting everything. `object`'s command produces a REAL object file
# (a link input) rather than a plain stamp, so this control does not fail the
# LINK for an unrelated reason and misread that as the role being refused.
echo 'int ctrl_unused_symbol(void) { return 0; }' > extra.c

for role in source check artifact; do
    cat > build.mcpp <<EOF
import mcpp;
#include <string>
int main() {
    mcpp::action a;
    a.id   = "ctrl";
    a.role = "$role";
    a.arg("true")
     .output((std::string(mcpp::out_dir()) + "/ctrl.stamp").c_str())
     .submit();
    return 0;
}
EOF
    rm -rf target
    "$MCPP" build > c.log 2>&1 || { cat c.log; echo "FAIL: a well-known role '$role' was refused"; exit 1; }
done

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string obj  = std::string(mcpp::out_dir()) + "/extra.o";
    mcpp::action a;
    a.id   = "ctrl-object";
    a.role = "object";
    a.arg("gcc").arg("-c").arg((root + "/extra.c").c_str()).arg("-o").arg(obj.c_str())
     .input((root + "/extra.c").c_str())
     .output(obj.c_str())
     .submit();
    return 0;
}
EOF
rm -rf target
"$MCPP" build > c.log 2>&1 || { cat c.log; echo "FAIL: the well-known role 'object' was refused"; exit 1; }

echo "PASS: 793_an_unknown_action_role_is_refused"
