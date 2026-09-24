#!/usr/bin/env bash
# requires: gcc
# 772_a_published_member_is_self_contained.sh — the published form of a
# workspace member (#690, design 2026-09-25 sections 3.6 and 5.5).
#
# A member's own mcpp.toml is valid only inside its workspace: it omits the
# `version` and `license` that `[workspace.package]` supplies, its flags come
# partly from `[workspace.build]`, and it reaches a sibling by `path`. The
# archive `mcpp publish` produces contains the member directory alone, and a
# consumer reads the manifest in it as written. Before #690 that manifest was
# the raw file, so a consumer received a package that did not build.
#
# THE CRITERIA READ THE ARCHIVE AND BUILD FROM IT. `publish` exiting 0 says
# nothing: before the fix it also exited 0 while producing the broken archive.
#
#   A. the archived mcpp.toml carries the inherited version, license and
#      cxxflags; mcpp.toml.orig is the file as written
#   B. a consumer of the unpacked archive builds past an #error guard that
#      needs the workspace flag, with this mcpp and with a released one
#   C. two runs produce byte-identical archives
#   D. a sibling edge with `version` is published as a version edge, and the
#      descriptor lists it
#   E. a sibling edge without `version` is refused, naming the version to add
#   F. `emit xpkg` works in a member that omits `version`
#   G. a package outside any workspace is archived exactly as `git archive`
#      archives it
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

fail() { echo "FAIL: $*"; exit 1; }

REPO="$TMP/repo"
mkdir -p "$REPO/util/src" "$REPO/lib/src" "$REPO/edgeless/src"
cd "$REPO"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["util", "lib", "edgeless"]

[workspace.package]
version = "0.3.0"
license = "MIT"
repo    = "https://github.com/example/probe"

[workspace.build]
cxxflags = ["-DWS_FLAG=1"]
EOF
cat > util/mcpp.toml <<'EOF'
[package]
namespace = "probe"
name = "util"

[targets.probe_util]
kind = "lib"
EOF
cat > util/src/util.cppm <<'EOF'
module;
#ifndef WS_FLAG
#error "WS_FLAG missing: the published manifest lost [workspace.build]"
#endif
export module probe.util;
export int util_value() { return WS_FLAG + 41; }
EOF
cat > lib/mcpp.toml <<'EOF'
[package]
namespace = "probe"
name = "lib"

[dependencies]
"probe.util" = { path = "../util", version = "0.3.0" }

[targets.probe_lib]
kind = "lib"
EOF
cat > lib/src/lib.cppm <<'EOF'
export module probe.lib;
import probe.util;
export int lib_value() { return util_value(); }
EOF
cat > edgeless/mcpp.toml <<'EOF'
[package]
namespace = "probe"
name = "edgeless"

[dependencies]
"probe.util" = { path = "../util" }

[targets.probe_edgeless]
kind = "lib"
EOF
cat > edgeless/src/edgeless.cppm <<'EOF'
export module probe.edgeless;
export int edgeless_value() { return 1; }
EOF
git init -q -b main . && git add -A \
    && git -c user.email=e2e@example.invalid -c user.name=e2e commit -qm init

# ── A. the archived manifest carries what the member inherited ───────────
( cd util && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub1.log" 2>&1 ) \
    || { cat "$TMP/pub1.log"; fail "publish of a member that omits version failed"; }
ARCHIVE="$REPO/util/target/dist/util-0.3.0.tar.gz"
[[ -f "$ARCHIVE" ]] || { cat "$TMP/pub1.log"; fail "no archive at $ARCHIVE"; }
tar -tzf "$ARCHIVE" | grep -qx 'util-0.3.0/mcpp.toml.orig' \
    || { tar -tzf "$ARCHIVE"; fail "the archive does not keep the original manifest"; }
tar -xzf "$ARCHIVE" -O util-0.3.0/mcpp.toml > "$TMP/util.toml"
for want in 'version = "0.3.0"' 'license = "MIT"' 'cxxflags = \["-DWS_FLAG=1"\]'; do
    grep -q "$want" "$TMP/util.toml" \
        || { cat "$TMP/util.toml"; fail "the archived mcpp.toml lacks: $want"; }
done
tar -xzf "$ARCHIVE" -O util-0.3.0/mcpp.toml.orig | cmp -s - util/mcpp.toml \
    || fail "mcpp.toml.orig is not the file as written"
echo "ok A: the archived manifest carries the inherited version, license and cxxflags"

# ── C. the archive is a function of the commit ───────────────────────────
sha1=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
rm -rf util/target
( cd util && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub2.log" 2>&1 ) \
    || { cat "$TMP/pub2.log"; fail "second publish failed"; }
sha2=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
[[ "$sha1" == "$sha2" ]] || fail "two runs produced different archives ($sha1 vs $sha2)"
echo "ok C: two runs produce the same archive ($sha1)"

# ── B. a consumer builds from the archive ────────────────────────────────
mkdir -p "$TMP/unpacked" "$TMP/app/src"
tar -xzf "$ARCHIVE" -C "$TMP/unpacked"
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
"probe.util" = { path = "$TMP/unpacked/util-0.3.0" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
import std;
import probe.util;
int main() { std::println("value={}", util_value()); return util_value() == 42 ? 0 : 1; }
EOF
( cd "$TMP/app" && "$MCPP" run > "$TMP/new.log" 2>&1 ) \
    || { cat "$TMP/new.log"; fail "a consumer of the published archive does not build"; }
grep -q 'value=42' "$TMP/new.log" || { cat "$TMP/new.log"; fail "wrong value"; }
echo "ok B: a consumer of the published archive builds and runs"

# The released client. Same guard as 252: a shim that answers --version with
# nothing is not an old client, and saying so is a note, not a verdict.
boot_ver=""
new_ver="$("$MCPP" --version 2>/dev/null || true)"
if [[ -n "${MCPP_BOOT:-}" && -x "${MCPP_BOOT}" ]]; then
    boot_ver="$("$MCPP_BOOT" --version 2>/dev/null || true)"
fi
case "$boot_ver" in
    mcpp\ [0-9]*)
        if [[ "$boot_ver" != "$new_ver" ]]; then
            rm -rf "$TMP/app/target"
            ( cd "$TMP/app" && "$MCPP_BOOT" run > "$TMP/old.log" 2>&1 ) \
                || { cat "$TMP/old.log"; fail "$boot_ver cannot build the normalised archive"; }
            grep -q 'value=42' "$TMP/old.log" || { cat "$TMP/old.log"; fail "old client: wrong value"; }
            echo "ok B: the released client ($boot_ver) builds the normalised archive"
        else
            echo "SKIP B (released client): \$MCPP_BOOT is this binary"
        fi ;;
    *) echo "SKIP B (released client): \$MCPP_BOOT is unset or not a released mcpp" ;;
esac

# ── D. a sibling edge with a version ─────────────────────────────────────
( cd lib && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub3.log" 2>&1 ) \
    || { cat "$TMP/pub3.log"; fail "publish of a member with a versioned sibling edge failed"; }
tar -xzf "$REPO/lib/target/dist/lib-0.3.0.tar.gz" -O lib-0.3.0/mcpp.toml > "$TMP/lib.toml"
grep -q '"probe.util" = { version = "0.3.0" }' "$TMP/lib.toml" \
    || { cat "$TMP/lib.toml"; fail "the sibling edge is not a version edge"; }
if grep -q 'path' "$TMP/lib.toml"; then cat "$TMP/lib.toml"; fail "a path survived"; fi
( cd lib && "$MCPP" emit xpkg > "$TMP/lib.lua" 2>&1 ) || { cat "$TMP/lib.lua"; fail "emit xpkg"; }
grep -q '\["probe.util"\] = "0.3.0"' "$TMP/lib.lua" \
    || { cat "$TMP/lib.lua"; fail "the descriptor does not list the sibling"; }
echo "ok D: the sibling edge is published as a version edge and listed in the descriptor"

# ── E. a sibling edge without a version is refused ───────────────────────
if ( cd edgeless && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub4.log" 2>&1 ); then
    cat "$TMP/pub4.log"; fail "a path-only sibling edge was published"
fi
grep -q 'version = "0.3.0"' "$TMP/pub4.log" \
    || { cat "$TMP/pub4.log"; fail "the refusal does not name the version to add"; }
echo "ok E: a path-only sibling edge is refused with the line to write"

# ── F. emit xpkg in a member that omits version ──────────────────────────
( cd util && "$MCPP" emit xpkg > "$TMP/util.lua" 2>&1 ) \
    || { cat "$TMP/util.lua"; fail "emit xpkg in a member that omits version"; }
grep -q "\['0.3.0'\]" "$TMP/util.lua" || { cat "$TMP/util.lua"; fail "descriptor version"; }
echo "ok F: emit xpkg reads the inherited version"

# ── G. a package outside any workspace is archived as git archives it ────
SOLO="$TMP/solo"
mkdir -p "$SOLO/src"
cat > "$SOLO/mcpp.toml" <<'EOF'
[package]
name    = "solo"
version = "1.0.0"
repo    = "https://github.com/example/solo"

[targets.solo]
kind = "lib"
EOF
echo 'export module solo; export int solo_v() { return 1; }' > "$SOLO/src/solo.cppm"
( cd "$SOLO" && git init -q -b main . && git add -A \
    && git -c user.email=e2e@example.invalid -c user.name=e2e commit -qm init )
( cd "$SOLO" && "$MCPP" publish --dry-run --allow-dirty > "$TMP/pub5.log" 2>&1 ) \
    || { cat "$TMP/pub5.log"; fail "publish outside a workspace"; }
git -C "$SOLO" archive --format=tar.gz --prefix=solo-1.0.0/ -o "$TMP/direct.tar.gz" HEAD
cmp -s "$TMP/direct.tar.gz" "$SOLO/target/dist/solo-1.0.0.tar.gz" \
    || fail "a package that needs no normalisation is not archived as git archive HEAD"
if grep -q 'Manifest' "$TMP/pub5.log"; then cat "$TMP/pub5.log"; fail "an unchanged manifest was reported as normalised"; fi
echo "ok G: an unchanged package is archived byte for byte as before"

echo "PASS: 772_a_published_member_is_self_contained"
