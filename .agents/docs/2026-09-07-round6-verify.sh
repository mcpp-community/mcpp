#!/usr/bin/env bash
# Ecosystem verification for round 6: the tool plane's TARGET axis
# (`[target.<selector>.xlings…]`, SPEC-004 section 4), against a PUBLISHED mcpp.
#
#   # The sandbox has an EMPTY $HOME and a fresh /tmp, so this file is not
#   # visible from inside it. Pass the script itself in:
#   B64=$(base64 -w0 <this file>)
#   xlings subos use verify-964 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.6.4 bash /tmp/v.sh"
#
# mcpp is addressed by its STORE path, which is the one thing the sandbox does
# share: the xlings data directory. A bare `mcpp` is not on PATH in there.
#
# WHY A SANDBOX IS THE RIGHT PLACE FOR THIS ONE. The criterion below is that a
# tool declared on the target axis is INSTALLED. On any machine that has built
# the package before, the payload is already in the registry and the answer is
# the same whether the axis works or not -- the shape that hid the ordering
# defect this round's engine change fixed. A sandbox has an empty registry, so
# the question can actually be asked.
#
# EVERY CRITERION NAMES THE OBJECT IT SELECTED, and every leg has a side that
# fails. A check whose "no" is silence is a check that measures nothing.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
XL="${XLINGS_BIN:-$(command -v xlings || true)}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# THE TOOL MUST BE ONE mcpp DOES NOT INSTALL FOR ITS OWN REASONS. A minimal
# isolated registry holds binutils, gcc, gcc-runtime, gcc-specs-config, glibc,
# linux-headers, ninja and patchelf; `shaderc` is in none of that. Using one of
# those would answer whether or not the axis had been read.
TOOL=shaderc
TOOL_VERSION="2026.3"
# And the negative leg names something that CANNOT resolve, so selecting it by
# mistake is loud rather than merely wasteful.
ABSENT="mcpp-verify-no-such-tool"

# -- A. identity and mirror --------------------------------------------------
section "A. identity"
if [ ! -x "$STORE" ]; then
    fail "no released binary at $STORE"
    printf 'FAIL: nothing further can run\n'; exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
if [ "$got" = "mcpp $VER" ]; then ok "$got from $STORE"
else fail "version is '$got' at $STORE"; fi

"$STORE" self config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
[ -n "$XL" ] && "$XL" config --mirror "${MCPP_VERIFY_MIRROR:-CN}" >/dev/null 2>&1 || true
xm=$(python3 -c "import json,os;print(json.load(open(os.path.expanduser('~/.xlings/.xlings.json'))).get('mirror',''))" 2>/dev/null || echo "")
if [ "$xm" = "${MCPP_VERIFY_MIRROR:-CN}" ]; then ok "xlings mirror is $xm"
else fail "xlings mirror is '$xm'"; fi

# The registry this run starts from. A non-empty one does not invalidate B, but
# it changes what B can prove, so it is printed rather than assumed.
#
# ASKED, NOT GUESSED. A released mcpp carries a `registry/` beside itself and
# resolves MCPP_HOME from its own location, so `$HOME/.mcpp` is a guess about a
# layout this binary may not use.
MH="$("$STORE" index status 2>/dev/null | grep -oE '/[^ ]*/registry' | head -1)"
[ -n "$MH" ] || MH="${MCPP_HOME:-$HOME/.mcpp}/registry"
reg="$MH/data/xpkgs"
ok "registry at $reg"
if [ -d "$reg/xim-x-$TOOL" ]; then
    ok "NOTE: xim:$TOOL is ALREADY in $reg -- section B proves less here"
else
    ok "xim:$TOOL is absent from $reg, so B measures an install"
fi

# -- B. the target axis provisions, and only for the matching target --------
section "B. [target.<selector>.xlings.workspace]"
mkdir -p "$work/axis/src"
cat > "$work/axis/mcpp.toml" <<TOML
[package]
name    = "axisprobe"
version = "0.1.0"

[targets.axisprobe]
kind = "bin"
main = "src/main.cpp"

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:$TOOL" = "$TOOL_VERSION"

[target.'cfg(os = "windows")'.xlings.workspace]
"xim:$ABSENT" = "1.0"
TOML
printf 'int main() { return 0; }\n' > "$work/axis/src/main.cpp"
cat > "$work/axis/build.mcpp" <<'CPP'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    // TO A FILE, NOT stdout: mcpp prints a build program's output only when it
    // fails, so a criterion read from stdout can only ever see the failing run.
    std::string out = std::string(mcpp::manifest_dir()) + "/seen.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 1;
    const char* d = mcpp::xpkg_dir("shaderc");
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
CPP
if (cd "$work/axis" && "$STORE" build >"$work/axis.log" 2>&1); then
    seen=$(head -1 "$work/axis/seen.txt" 2>/dev/null || echo "")
    if [ -n "$seen" ] && [ -d "$seen" ]; then
        ok "the build program found its target-axis tool at $seen"
    else
        fail "xpkg_dir answered '$seen' for a tool declared on the target axis"
        tail -20 "$work/axis.log"
    fi
    # The negative leg. `xim:$ABSENT` resolves to nothing, so a build that
    # selected the windows entry on a linux host would have FAILED above --
    # this line states what the success just proved.
    if grep -q "$ABSENT" "$work/axis.log"; then
        fail "the linux build touched the windows-only entry ($ABSENT)"
    else
        ok "the windows-only entry was not selected on a linux target"
    fi
else
    fail "a project declaring a tool on the target axis did not build"
    tail -25 "$work/axis.log"
fi

# -- C. the condition may not be written twice -------------------------------
section "C. selector plus platform keys is refused"
mkdir -p "$work/twice/src"
printf 'int main() { return 0; }\n' > "$work/twice/src/main.cpp"
cat > "$work/twice/mcpp.toml" <<TOML
[package]
name    = "twice"
version = "0.1.0"

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:$TOOL" = { linux = "$TOOL_VERSION", macosx = "$TOOL_VERSION" }
TOML
if out=$( (cd "$work/twice" && "$STORE" build 2>&1) ); then
    fail "a manifest that writes the condition twice built"
else
    if printf '%s' "$out" | grep -q 'platform keys' \
       && printf '%s' "$out" | grep -q 'cfg(os = "linux")'; then
        ok "refused, naming the platform keys and the outer selector"
    else
        fail "refused, but not for the stated reason:"
        printf '%s\n' "$out" | tail -6
    fi
fi

# -- D. a tool may not be conditioned on a target-side layer -----------------
section "D. a layer predicate is refused"
mkdir -p "$work/layer/src"
printf 'int main() { return 0; }\n' > "$work/layer/src/main.cpp"
cat > "$work/layer/mcpp.toml" <<TOML
[package]
name    = "layered"
version = "0.1.0"

[target.'cfg(c-abi = "musl")'.xlings.workspace]
"xim:$ABSENT" = "1.0"
TOML
if out=$( (cd "$work/layer" && "$STORE" build 2>&1) ); then
    fail "a tool conditioned on a layer built; it would be declared and never installed"
else
    if printf '%s' "$out" | grep -q "$ABSENT" \
       && printf '%s' "$out" | grep -q 'c-abi = "musl"' \
       && printf '%s' "$out" | grep -q 'feature-xlings'; then
        ok "refused, naming the tool, the predicate and the way out"
    else
        fail "refused, but not for the stated reason:"
        printf '%s\n' "$out" | tail -6
    fi
fi

# -- E. subos is not per target ----------------------------------------------
section "E. subos under a selector is refused"
mkdir -p "$work/subos/src"
printf 'int main() { return 0; }\n' > "$work/subos/src/main.cpp"
cat > "$work/subos/mcpp.toml" <<'TOML'
[package]
name    = "subosprobe"
version = "0.1.0"

[target.'cfg(os = "linux")'.xlings]
subos = "dev"
TOML
if out=$( (cd "$work/subos" && "$STORE" build 2>&1) ); then
    fail "a per-target subos built; the environment would be silently dropped"
else
    if printf '%s' "$out" | grep -q 'subos'; then
        ok "refused, naming subos"
    else
        fail "refused, but not for the stated reason:"
        printf '%s\n' "$out" | tail -6
    fi
fi

# -- F. the host axis is unchanged -------------------------------------------
section "F. the top-level table keeps its meaning"
# V4 in the sandbox: the platform-keyed value is not legacy, it is how the HOST
# axis is written, and it must still resolve and install.
mkdir -p "$work/host/src"
printf 'int main() { return 0; }\n' > "$work/host/src/main.cpp"
cat > "$work/host/mcpp.toml" <<TOML
[package]
name    = "hostaxis"
version = "0.1.0"

[targets.hostaxis]
kind = "bin"
main = "src/main.cpp"

[xlings.workspace]
"xim:$TOOL" = { linux = "$TOOL_VERSION", macosx = "$TOOL_VERSION", default = "$TOOL_VERSION" }
TOML
cat > "$work/host/build.mcpp" <<'CPP'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string out = std::string(mcpp::manifest_dir()) + "/seen.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 1;
    const char* d = mcpp::xpkg_dir("shaderc");
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
CPP
if (cd "$work/host" && "$STORE" build >"$work/host.log" 2>&1); then
    seen=$(head -1 "$work/host/seen.txt" 2>/dev/null || echo "")
    if [ -n "$seen" ] && [ -d "$seen" ]; then
        ok "the platform-keyed top-level entry still resolves, at $seen"
    else
        fail "the host axis answered '$seen'"
        tail -20 "$work/host.log"
    fi
else
    fail "a project using the pre-existing top-level spelling did not build"
    tail -25 "$work/host.log"
fi

printf '\n== summary ==\n'
if [ -n "$skipped" ]; then
    printf 'NOT RUN, and therefore not verified:%s\n' "$skipped"
fi
if [ "$fails" -eq 0 ]; then
    printf 'PASS: 0 assertions failed\n'
else
    printf 'FAIL: %s assertion(s) failed\n' "$fails"
fi
exit "$fails"
