#!/usr/bin/env bash
# Ecosystem verification for round 7: one package, one version -- and a rule
# package that brings its own environment. Against a PUBLISHED mcpp and a
# PUBLISHED mcpp:plugins.
#
#   # The sandbox has an EMPTY $HOME and a fresh /tmp, so this file is not
#   # visible from inside it. Pass the script itself in:
#   B64=$(base64 -w0 <this file>)
#   xlings subos use verify-966 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.6.6 bash /tmp/v.sh"
#
# mcpp is addressed by its STORE path, which is the one thing the sandbox does
# share: the xlings data directory. A bare `mcpp` is not on PATH in there.
#
# WHY A SANDBOX IS THE RIGHT PLACE FOR THIS ONE. Every criterion below is about
# what gets INSTALLED. On a machine that has built any of this before, the
# payload is in the registry already and "one version" reads the same whether
# the engine unified anything or not. A sandbox has an empty registry, so the
# question can be asked at all.
#
# EVERY CRITERION NAMES THE OBJECT IT SELECTED, and every section that did not
# run is listed again in the summary. "0 assertions failed" printed by a script
# that skipped four sections is the failure mode this shape exists to prevent.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
PLUGINS="${MCPP_VERIFY_PLUGINS:-0.2.4}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The store this run installs into, so "how many versions of X are here" is a
# question about THIS run and not about the machine.
# `MCPP_HOME` first: the sandbox does not set it, so `$HOME/.mcpp` is right
# there -- but a rehearsal on the host does set it, and reading the real home
# instead answers "one version installed" from a registry this run never
# touched. Measured: section D passed on the host for exactly that reason.
XPKGS="${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs"
count_versions() { ls -1 "$XPKGS/xim-x-$1" 2>/dev/null | wc -l | tr -d ' '; }
list_versions()  { ls -1 "$XPKGS/xim-x-$1" 2>/dev/null | sort | tr '\n' ' '; }

# ── A. identity ─────────────────────────────────────────────────────────────
section "A. identity"
if [ ! -x "$STORE" ]; then
    fail "no released binary at $STORE"
    printf 'FAIL: nothing further can run\n'; exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
if [ "$got" = "mcpp $VER" ]; then ok "$got from $STORE"
else fail "version is '$got' at $STORE"; fi

"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true

# ── B. a range is installed AND answered ────────────────────────────────────
#
# Engine-only: no rule package, no index entry beyond the tool itself. This is
# the gap that made a floor unusable -- the payload arrived and `xpkg_dir`
# answered "".
section "B. a range is installed and answered (engine only)"
mkdir -p "$work/b/src"
printf 'int main(){return 0;}\n' > "$work/b/src/main.cpp"
cat > "$work/b/build.mcpp" <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "shaderc");
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
EOF
cat > "$work/b/mcpp.toml" <<'EOF'
[package]
name    = "rangecheck"
version = "0.1.0"
[xlings.workspace]
"xim:shaderc" = ">=2026.1"
[build]
sources = ["src/main.cpp"]
[targets.rangecheck]
kind = "bin"
main = "src/main.cpp"
EOF
if ( cd "$work/b" && "$STORE" build >build.log 2>&1 ); then
    if [ -s "$work/b/answered.txt" ] && grep -q 'xim-x-shaderc' "$work/b/answered.txt"; then
        ok "a range was answered: $(cat "$work/b/answered.txt")"
    else
        fail "the range installed but xpkg_dir answered '$(cat "$work/b/answered.txt" 2>/dev/null)'"
    fi
else
    fail "a project declaring a range did not build"
    tail -5 "$work/b/build.log"
fi

# …and the other direction, so a build that treats the range as a literal is
# not read as a pass.
mkdir -p "$work/b2/src"
printf 'int main(){return 0;}\n' > "$work/b2/src/main.cpp"
sed 's/>=2026.1/>=2099.1/' "$work/b/mcpp.toml" > "$work/b2/mcpp.toml"
if ( cd "$work/b2" && "$STORE" build >build.log 2>&1 ); then
    fail "an unsatisfiable range was accepted"
else
    if grep -q 'not found in the synced index' "$work/b2/build.log"; then
        ok "an unsatisfiable range is refused, naming it"
    else
        fail "refused, but not for the stated reason: $(grep -m1 error "$work/b2/build.log")"
    fi
fi

# ── C. one edge, and the rule brings its environment ────────────────────────
section "C. one edge, no [xlings.workspace] anywhere in the project"
mk_spirv_project() {   # $1 = dir, $2 = extra manifest text
    mkdir -p "$1/src" "$1/shaders"
    cat > "$1/shaders/scale.comp" <<'EOF'
#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Data { float v[]; };
layout(push_constant) uniform Push { float a; uint n; } push;
void main() {
    const uint i = gl_GlobalInvocationID.x;
    if (i >= push.n) return;
    v[2u * push.n + i] = push.a * v[i] + v[push.n + i];
}
EOF
    cat > "$1/src/main.cpp" <<'EOF'
#include <cstdint>
#include <cstdio>
#include "scale_comp.h"
int main() {
    const std::uint32_t magic = scale_comp_spv[0];
    std::printf("magic=%08x\n", magic);
    return magic == 0x07230203u ? 0 : 1;
}
EOF
    cat > "$1/build.mcpp" <<'EOF'
import std;
import mcpp;
import mcpp.rules.spirv;
int main() {
    mcpp::rerun_if_changed_glob("shaders/**/*.comp");
    mcpp::rules::spirv::options opt;
    opt.includes = { "shaders" };
    return mcpp::rules::spirv::compile(opt) ? 0 : 1;
}
EOF
    cat > "$1/mcpp.toml" <<EOF
[package]
name         = "onedge"
version      = "0.1.0"
accelerators = ["vulkan"]

[language]
standard   = "c++23"
modules    = true
import_std = true

[build-dependencies.mcpp]
plugins = { version = "$PLUGINS", features = ["rules-spirv"], host-module = true }

[build]
accel = "vulkan1.2"
sources = [
  "src/*.cpp",
  { glob = "shaders/*.comp", accel = "vulkan1.2" },
]
$2
[targets.onedge]
kind = "bin"
main = "src/main.cpp"
EOF
}

mk_spirv_project "$work/c" ""
if ( cd "$work/c" && "$STORE" build >build.log 2>&1 ); then
    if grep -q 'entries declared by dependencies' "$work/c/build.log"; then
        ok "the payload came from the rule: $(grep -m1 'entries declared by dependencies' "$work/c/build.log")"
    else
        fail "it built, but nothing was provisioned from the graph -- the rule declared no payload"
    fi
    out=$( cd "$work/c" && "$STORE" run 2>&1 | tail -1 )
    if [ "$out" = "magic=07230203" ]; then ok "and it runs: $out"
    else fail "ran and printed '$out'"; fi
else
    # THE SKIP IS RECOGNISED BY THE RESOLUTION ERROR, NOT BY "it failed and the
    # log mentions plugins". `E_NOT_FOUND` with the wire address is a sentence
    # only the resolver writes; a compile or link failure cannot produce it, so
    # a genuine defect is never filed as "not published yet".
    if grep -q "E_NOT_FOUND: package 'mcpp:plugins@$PLUGINS' not found" "$work/c/build.log"; then
        skip "C: mcpp:plugins@$PLUGINS is not in the index yet"
        skip "D: needs C"
        skip "E: needs C"
    else
        fail "a project writing only the plugins edge did not build"
        tail -15 "$work/c/build.log"
    fi
fi

# ── D. the override: one version, and the nearer declaration wins ───────────
if [ -d "$work/c/target" ]; then
    section "D. the project's own pin overrides the rule's, and only one is installed"
    mk_spirv_project "$work/d" '
[target.'"'"'cfg(accelerator = "vulkan")'"'"'.xlings.workspace]
"xim:glslang" = "15.1.0"
'
    if ( cd "$work/d" && "$STORE" build >build.log 2>&1 ); then
        n=$(count_versions glslang)
        if [ "$n" = "1" ]; then ok "one glslang installed: $(list_versions glslang)"
        else fail "two declarations of one package installed $n versions: $(list_versions glslang)"; fi
    else
        fail "the override did not build"
        tail -10 "$work/d/build.log"
    fi

fi

# ── E. a pin below a stated floor is refused, naming both sides ─────────────
#
# NOT THROUGH A RULE PACKAGE, and the reason is worth recording: no published
# rule states a floor that a published version can sit below. `rules-spirv`
# requires `>=15.1.0` and 15.1.0 is the only glslang in the index; the same
# holds for dpcpp and for the CANN toolkit. A fixture pinning `1.0.0` to get
# under the floor is refused one step earlier, by provisioning, with
# `not found in the synced index` -- the root's own pins are resolved before
# the graph's constraints are known, so that refusal always wins. Measured:
# the first draft of this section asserted the conflict message and read back
# the provisioning one.
#
# So the construction is a local dependency and `xim:zoxide`, which publishes
# three versions. What it verifies here rather than in the e2e suite is the
# RELEASED binary.
section "E. a pin below a stated floor is refused"
mkdir -p "$work/e/dep/src" "$work/e/app/src"
printf 'int dep_touch() { return 1; }\n' > "$work/e/dep/src/lib.cpp"
cat > "$work/e/dep/mcpp.toml" <<'EOF'
[package]
name    = "toolowner"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[xlings.workspace]
"xim:zoxide" = ">=0.9.9"
[targets.toolowner]
kind = "lib"
EOF
printf 'int main(){return 0;}\n' > "$work/e/app/src/main.cpp"
cat > "$work/e/app/mcpp.toml" <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"
[dependencies]
toolowner = { path = "../dep" }
[xlings.workspace]
"xim:zoxide" = "0.9.7"
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF
if ( cd "$work/e/app" && "$STORE" build >build.log 2>&1 ); then
    fail "a pin below a dependency's stated floor was accepted"
else
    miss=""
    for needle in 'xim:zoxide' '0.9.7' '>=0.9.9' 'toolowner' 'drop the pin'; do
        grep -q -- "$needle" "$work/e/app/build.log" || miss="$miss $needle"
    done
    if [ -z "$miss" ]; then ok "refused, naming both sides and the way out"
    else fail "refused, but the message omits:$miss"; fi
fi

# …and raising it clears the refusal. Without this leg the section also passes
# on an engine that refuses every project naming a tool its dependency names.
sed 's/"0.9.7"/"0.9.9"/' "$work/e/app/mcpp.toml" > "$work/e/app/mcpp.toml.new"
mv "$work/e/app/mcpp.toml.new" "$work/e/app/mcpp.toml"
rm -rf "$work/e/app/target"
#
# NO VERSION COUNT HERE. The refused leg above already installed 0.9.7: the
# root's own pins are provisioned before the graph's constraints are known, so
# a build that is about to be refused has already paid for what the project
# asked for. Counting across both legs therefore reads two, and it is not the
# defect this section is about -- section D owns "one package, one version",
# on a store that only one build has touched.
if ( cd "$work/e/app" && "$STORE" build >build2.log 2>&1 ); then
    ok "raising the pin to satisfy the floor builds"
else
    fail "a pin that satisfies the floor was still refused"
    tail -10 "$work/e/app/build2.log"
fi

# ── summary ────────────────────────────────────────────────────────────────
section "summary"
printf '%s assertion(s) failed\n' "$fails"
if [ -n "$skipped" ]; then
    printf 'sections that did NOT run:%s\n' "$skipped"
fi
[ "$fails" -eq 0 ] || exit 1
