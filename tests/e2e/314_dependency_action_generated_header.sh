#!/usr/bin/env bash
# requires: gcc
# 314_dependency_action_generated_header.sh — a package that generates its own
# header compiles after the generator, not alongside it (mcpp#534).
#
# THE SHAPE THAT WAS BROKEN. `role = "source"` was documented as "outputs join
# the compile set; the compile edge consumes them", and that is true of a
# generated `.cpp` and false of a generated `.h`. A header is never an edge
# input — it is reached through `-I`, and the depfile that would record it does
# not exist until a compile has already succeeded. So the action's node sat in
# build.ninja with nothing able to reach it: not in `default` (Source outputs
# are deliberately excluded), not in the goal phony (objects and link outputs
# only), and consumed by no edge.
#
# ⚠️ THE ASSERTION IS ON CONTENT, NOT EXISTENCE. `prepare_actions` used to
# write a zero-byte placeholder for every Source output including headers, so
# the file was on disk whether or not the generator ran — which is exactly why
# mcpp#534 was filed as an intermittent race when it was a deterministic
# never-runs. A test asserting `[[ -f gen.h ]]` would have passed against the
# defect.
#
# The dependency is a PATH dependency because that is the reported shape: the
# generator, the header and the consumer of the header are all inside one
# dependency package, which is the case `tests/examples/protobuf-protoc` does
# not cover (there the generated header is used by the ROOT project).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# ── the dependency: generates a header, and compiles a source that uses it ──
mkdir -p "$TMP/proto/src"
cat > "$TMP/proto/mcpp.toml" <<'EOF'
[package]
name    = "proto"
version = "0.1.0"

[targets.proto]
kind = "lib"
EOF

cat > "$TMP/proto/gen.sh" <<'EOF'
#!/usr/bin/env bash
# Slow on purpose: if the ordering edge is missing, the compile wins the race
# every time rather than occasionally, so a red run means the defect and not
# the weather.
sleep 2
printf '#define PROTO_ANSWER 42\n' > "$1"
EOF
chmod +x "$TMP/proto/gen.sh"

# The lib root is a module interface, so this package really is a module
# library — keeping the lib-root convention in play rather than side-stepping
# it. It includes the generated header from its own implementation.
cat > "$TMP/proto/src/proto.cppm" <<'EOF'
module;
#include "proto_generated.h"
export module proto;
export int proto_answer() { return PROTO_ANSWER; }
EOF

cat > "$TMP/proto/build.mcpp" <<'EOF'
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action a;
    a.id   = "proto:header";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str())
     .arg((out + "/proto_generated.h").c_str())
     .output((out + "/proto_generated.h").c_str())   // HEADER, and the only output
     .submit();
    mcpp::include_dir(out.c_str());
}
EOF

# ── the consumer ────────────────────────────────────────────────────────────
mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
proto = { path = "../proto" }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include <cstdio>
import proto;
int main() { std::printf("ANSWER=%d\n", proto_answer()); }
EOF

cd "$TMP/app"
"$MCPP" build > b.log 2>&1 || {
    cat b.log
    echo "FAIL: a dependency whose action generates its only header did not build"
    exit 1; }

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == "ANSWER=42" ]] || {
    echo "FAIL: expected ANSWER=42 from the generated header, got '$out'"; exit 1; }

# The generator actually ran. Size, not existence — see the header comment.
hdr=$(find "$TMP" -name proto_generated.h | head -1)
[[ -n "$hdr" ]] || { echo "FAIL: no proto_generated.h anywhere"; exit 1; }
bytes=$(wc -c < "$hdr")
[[ "$bytes" -gt 0 ]] || {
    echo "FAIL: $hdr is $bytes bytes — the placeholder, not the generator's output"
    exit 1; }
grep -q 'PROTO_ANSWER' "$hdr" || {
    echo "FAIL: $hdr exists and is non-empty but is not what gen.sh writes:"
    cat "$hdr"; exit 1; }

# The ordering is declared, not merely observed. A green run on a fast machine
# proves nothing on its own; this is the edge that makes it a property.
nj=$(find "$TMP/app/target" -name build.ninja | head -1)
[[ -n "$nj" ]] || { echo "FAIL: no build.ninja"; exit 1; }

# The phony is named for the QUALIFIED package (`mcpp-actions-mcpplibs.proto`),
# because that is how `CompileUnit::packageName` spells it and the two are
# matched against each other. Read the name out rather than hardcoding it: a
# test that spelled it by hand would be asserting the naming policy, which is
# not what this test is about.
phony=$(grep -oE '^build (mcpp-actions-[^ ]*proto[^ ]*) : phony' "$nj" \
        | awk '{print $2}' | head -1)
[[ -n "$phony" ]] || {
    echo "FAIL: no action phony was emitted for the dependency"
    grep -nE '^build mcpp-actions' "$nj" || echo "  (none at all)"
    exit 1; }
count=$(grep -cE "^build $phony : phony" "$nj" || true)
[[ "$count" -eq 1 ]] || {
    echo "FAIL: expected exactly one '$phony' edge, found $count"; exit 1; }

# DENOMINATOR. "every edge that should carry it does" is vacuously true when no
# edge does, and that vacuum is precisely the pre-fix state — so the count of
# the dependency's compile edges is asserted separately from the count that
# carries the ordering.
# Matched on the RULE and the SOURCE, not on the object path. An object edge
# can carry implicit outputs before the colon (`build a.o | gcm.cache/x.gcm :`),
# so a pattern anchored on the output is a pattern that quietly matches nothing
# — which, for a denominator, is the one failure mode that matters.
rules='cxx_object|c_object|cxx_module|cxx_module_obj|cxx_module_bmi|cxx_scan|asm_object'
src='proto/src/proto\.cppm'
edges=$(grep -cE "^build .*: *($rules) .*$src" "$nj" || true)
ordered=$(grep -cE "^build .*: *($rules) .*$src.*\|\| *$phony" "$nj" || true)
[[ "$edges" -gt 0 ]] || {
    echo "FAIL: no compile edge for the dependency was found at all — the"
    echo "      assertion below would have passed by describing nothing"
    grep -nE '^build ' "$nj" | head -20
    exit 1; }
[[ "$ordered" -eq "$edges" ]] || {
    echo "FAIL: $ordered of $edges dependency compile edges wait for the action"
    grep -nE "^build .*: *($rules) .*$src" "$nj"
    exit 1; }

echo "PASS: 314 (generator ran before the compile; $ordered/$edges edges ordered)"
