#!/usr/bin/env bash
# requires: unix-shell
# 868 -- what `mcpp pack` over several members refuses, and that it refuses before
# anything is compiled, naming what it refused.
#
# mcpp#749 (K1, member selection design 2026-09-30). A pack over several members
# pays for one build, so an input it cannot serve is worth refusing before that
# build rather than after it. Each refusal below is asserted to arrive with no
# object compiled anywhere in the workspace.
#
# Criteria:
#   E. A selected member without the requested `--format` is refused by name.
#      `plain` provides nothing; `cli` provides `zap`.
#   F. A target name is refused with several members: it names a target of one
#      package. `--target` given twice is refused too.
#   G. `--output` with several members must be a directory, and a path that exists
#      as a file is refused.
#   H. A member with no program target is refused by name when `-p` names it, and
#      skipped by `--workspace`.
#   I. Two members that would write one archive are refused, naming both.
#   J. A package that several packed members reach cannot provide a format for
#      them: a provider is given one staged tree. The members it serves are
#      refused by name. `mcpp pack -p <member>` over the same provider works.
#   K. A member that declared the format and submitted nothing fails alone: the
#      others are packed, the failure names the member, and the status is not zero.
#      So does a configuration that cannot be planned, for its members.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# No object was compiled in $1, and no build was driven.
compiled_nothing() {
    [ -z "$(find "$1" -name .ninja_log 2>/dev/null)" ]
}

# A program member named $1, optionally using the packages in $2.
program_member() {
    local m="$1" deps="$2"
    mkdir -p "$m/src"
    {
        printf '[package]\nname    = "%s"\nversion = "0.1.0"\n' "$m"
        [ -z "$deps" ] || printf '\n[dependencies]\n%s\n' "$deps"
        printf '\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' "$m"
    } > "$m/mcpp.toml"
    printf '#include <cstdio>\nint main() { std::puts("%s"); return 0; }\n' "$m" > "$m/src/main.cpp"
}

# The distribution step: what the staged tree held.
write_dist() {
    cat > "$1/dist.sh" <<'EOF'
#!/usr/bin/env bash
set -e
stage="$1"; out="$2"
{ ( cd "$stage" && find . -type f | sort ); } > "$out"
EOF
    chmod +x "$1/dist.sh"
}

# A build program that provides `zap` and submits it, or only declares it.
provider() {   # provider <member-dir> submit|silent
    local gate="if (std::string_view(mcpp::pack_format()) != \"zap\") return 0;"
    if [ "$2" = silent ]; then gate="return 0;"; fi
    cat > "$1/build.mcpp" <<EOF
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    $gate
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = std::string(mcpp::out_dir()) + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("\${mcpp.stage_dir}").arg(dist.c_str())
     .input((std::string("\${mcpp.target_file:") + mcpp::package_name() + "}").c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}
EOF
}

mkdir -p "$TMP/ws"
cd "$TMP/ws"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["cli", "plain", "lib"]
EOF
write_dist .
program_member cli ""
provider cli submit
program_member plain ""
mkdir -p lib/src
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "lib"
version = "0.1.0"

[targets.lib]
kind = "lib"
EOF
printf 'export module zlib;\nexport int f() { return 1; }\n' > lib/src/lib.cppm

# ── E ────────────────────────────────────────────────────────────────────────
set +e
"$MCPP" pack --mode system --workspace --format zap > e.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "E: a member without the format was packed" e.log
grep -q "plain" e.log || fail "E: the refusal does not name the member" e.log
grep -q "zap" e.log || fail "E: the refusal does not name the format" e.log
grep -q "member 'cli'" e.log && fail "E: the refusal names a member that provides the format" e.log
compiled_nothing . || fail "E: the refusal arrived after a compile" e.log
echo "ok: E, a member without the format is refused by name, before the build"

# ── F ────────────────────────────────────────────────────────────────────────
set +e
"$MCPP" pack --mode system -p cli -p plain cli > f1.log 2>&1
rc1=$?
"$MCPP" pack --mode system -p cli -p plain --target x86_64-linux-gnu --target aarch64-linux-gnu > f2.log 2>&1
rc2=$?
set -e
[ "$rc1" -ne 0 ] && grep -q "target name" f1.log && grep -q "'cli'" f1.log \
    || fail "F: a target name with several members was not refused, naming it" f1.log
[ "$rc2" -ne 0 ] && grep -q -- "--target may be given once" f2.log \
    || fail "F: two --target with several members were not refused" f2.log
compiled_nothing . || fail "F: a refusal arrived after a compile" f1.log f2.log
echo "ok: F, a target name and two --target are refused with several members"

# ── G ────────────────────────────────────────────────────────────────────────
: > a-file
set +e
"$MCPP" pack --mode system -p cli -p plain -o a-file > g.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] && grep -q -- "--output names a file" g.log \
    || fail "G: --output naming a file was not refused with several members" g.log
compiled_nothing . || fail "G: the refusal arrived after a compile" g.log
echo "ok: G, --output must be a directory with several members"

# ── H ────────────────────────────────────────────────────────────────────────
set +e
"$MCPP" pack --mode system -p cli -p lib > h.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] && grep -q "member 'lib' has no program target" h.log \
    || fail "H: a member with no program was not refused by name" h.log
compiled_nothing . || fail "H: the refusal arrived after a compile" h.log
"$MCPP" pack --mode system --workspace > h2.log 2>&1 || fail "H: --workspace failed over a member with no program" h2.log
grep -q "Skipping lib" h2.log || fail "H: --workspace did not say it skipped lib" h2.log
[ -n "$(find cli plain -name 'cli-0.1.0-*.tar.gz' -o -name 'plain-0.1.0-*.tar.gz' | head -1)" ] \
    || fail "H: the programs were not packed" h2.log
[ -z "$(find lib -name '*.tar.gz' | head -1)" ] || fail "H: the library was packed as a program" h2.log
echo "ok: H, a member with no program is refused by -p and skipped by --workspace"

# ── I ────────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/ns" && cd "$TMP/ns"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["a/tool", "b/tool"]
EOF
for ns in a b; do
    mkdir -p $ns/tool/src
    cat > $ns/tool/mcpp.toml <<EOF
[package]
name      = "tool"
namespace = "$ns"
version   = "0.1.0"

[targets.tool]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'int main() { return 0; }\n' > $ns/tool/src/main.cpp
done
set +e
"$MCPP" pack --mode system --workspace -o out > i.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] && grep -q "would both write" i.log && grep -q "a.tool" i.log && grep -q "b.tool" i.log \
    || fail "I: two members writing one archive were not refused, naming both" i.log
compiled_nothing . || fail "I: the refusal arrived after a compile" i.log
# Without --output each is written below its own target/dist, and both are packed.
"$MCPP" pack --mode system --workspace > i2.log 2>&1 || fail "I: two members of one name were not packed below their own directories" i2.log
[ -n "$(find a/tool/target/dist -name 'tool-0.1.0-*.tar.gz' | head -1)" ] \
    && [ -n "$(find b/tool/target/dist -name 'tool-0.1.0-*.tar.gz' | head -1)" ] \
    || fail "I: a member of the two was not packed" i2.log
echo "ok: I, two members that write one archive are refused naming both"

# ── J ────────────────────────────────────────────────────────────────────────
# `prov` provides the format, and both `cli2` and `gui2` reach it: a provider is
# given one staged tree, and two members would need two.
mkdir -p "$TMP/shared" && cd "$TMP/shared"
PROV_HOST=$(host_path "$TMP/shared/prov")
cat > mcpp.toml <<'EOF'
[workspace]
members = ["cli2", "gui2"]
EOF
write_dist .
mkdir -p prov/src
cat > prov/mcpp.toml <<'EOF'
[package]
name    = "prov"
version = "0.1.0"

[targets.prov]
kind = "lib"
EOF
printf 'export module zprov;\nexport int p() { return 1; }\n' > prov/src/prov.cppm
cat > prov/build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string dist = std::string(mcpp::out_dir()) + "/prov.zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((std::string(mcpp::manifest_dir()) + "/../dist.sh").c_str()).arg("${mcpp.stage_dir}").arg(dist.c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}
EOF
for m in cli2 gui2; do
    program_member $m "prov = { path = \"$PROV_HOST\" }"
done
set +e
"$MCPP" pack --mode system --workspace --format zap > j.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] && grep -q "member 'cli2'" j.log && grep -q "member 'gui2'" j.log && grep -q "prov" j.log \
    || fail "J: members served only by a shared provider were not refused by name" j.log
compiled_nothing . || fail "J: the refusal arrived after a compile" j.log
"$MCPP" pack --mode system -p cli2 --format zap > j2.log 2>&1 || fail "J: one member over the same provider was not packed" j2.log
provided=$(find . -name prov.zap | head -1)
[ -n "$provided" ] && grep -qx "./bin/cli2" "$provided" \
    || fail "J: the provider was not given the member's tree" j2.log
echo "ok: J, a provider several packed members reach is refused for them, and serves one"

# A provider that only one packed member reaches acts for that member, beside a
# member that provides the format from its own build program.
mkdir -p "$TMP/mixed" && cd "$TMP/mixed"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["cli3", "gui3"]
EOF
write_dist .
program_member cli3 "prov = { path = \"$PROV_HOST\" }"
program_member gui3 ""
provider gui3 submit
"$MCPP" pack --mode system --workspace --format zap > j3.log 2>&1 || fail "J: a provider one member reaches did not serve it beside another member's own" j3.log
provided=$(find . -name prov.zap | head -1)
[ -n "$provided" ] && grep -qx "./bin/cli3" "$provided" && ! grep -q "gui3" "$provided" \
    || fail "J: the provider was not given cli3's tree alone" j3.log
own=$(find . -name gui3.zap | head -1)
[ -n "$own" ] && grep -qx "./bin/gui3" "$own" || fail "J: gui3's own provider was not given gui3's tree" j3.log
grep -q "Packed .*prov.zap" j3.log && grep -q "Packed .*gui3.zap" j3.log \
    || fail "J: a distributable was not reported for each member" j3.log
echo "ok: J, a provider one packed member reaches acts for that member"

# ── K ────────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/partial" && cd "$TMP/partial"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["good", "mute"]
EOF
write_dist .
program_member good ""
provider good submit
program_member mute ""
provider mute silent
set +e
"$MCPP" pack --mode system --workspace --format zap > k.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "K: a member that claimed nothing was reported as packed" k.log
grep -q "member 'mute': no action claimed --format 'zap'" k.log \
    || fail "K: the failure does not name the member" k.log
[ -n "$(find target/.build-mcpp/out -path '*/good/good.zap' | head -1)" ] || fail "K: the member that claimed the format was not packed" k.log
grep -q "Packed .*good.zap" k.log || fail "K: the member that was packed was not reported" k.log
echo "ok: K, a member that claimed nothing fails alone, by name, and the others are packed"

# A configuration that cannot be planned fails for its members alone, too: `broken`
# asks for another standard, so it is planned in a group of its own, and its build
# program does not compile.
mkdir -p "$TMP/planfail" && cd "$TMP/planfail"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["fine", "broken"]
EOF
program_member fine ""
mkdir -p broken/src
cat > broken/mcpp.toml <<'EOF'
[package]
name     = "broken"
version  = "0.1.0"
standard = "c++26"

[targets.broken]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > broken/src/main.cpp
printf 'this is not C++\n' > broken/build.mcpp
set +e
"$MCPP" pack --mode system --workspace > k2.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "K: a configuration that could not be planned was reported as packed" k2.log
grep -q "broken" k2.log || fail "K: the failure does not name the member" k2.log
[ -n "$(find fine -name 'fine-0.1.0-*.tar.gz' | head -1)" ] || fail "K: the member of the other configuration was not packed" k2.log
echo "ok: K, a configuration that cannot be planned fails for its members alone"

echo "PASS: 868_a_pack_over_several_members_is_refused_before_anything_is_compiled"
