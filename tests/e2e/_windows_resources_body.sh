# Shared body for the Windows-resource e2e tests (mcpp#365).
#
# Sourced by 197 (native Windows) and 198 (Linux → Windows via mingw-cross).
# The two differ only in how the target is selected, and that difference is the
# point: the msvc dialect compiles the script to a `.res` that lld-link/link.exe
# consume directly, while the GNU dialect must go through `windres -O coff`
# because GNU ld cannot read a `.res` at all. Asserting the same behaviour
# through both keeps that fork honest.
#
# Callers must set, before sourcing:
#   TMP           scratch dir (already created, trap-cleaned)
#   MCPP          the binary under test
#   BUILD_ARGS    extra `mcpp build` arguments ("" natively, --target when cross)
#   EXE_SUFFIX    ".exe"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# The ordinal assertions below read the LLVM payload, so the body needs to know
# where it is even when the caller was invoked directly rather than through
# run_all.sh (which exports this).
export MCPP_HOME="${MCPP_HOME:-$HOME/.mcpp}"

# Hex dump of a file as one unbroken lowercase string — enough to search for a
# byte pattern without needing `strings`, python, or a PE parser on the runner.
hexof() { od -An -v -tx1 "$1" | tr -d ' \n'; }

# A literal ASCII string as it appears in a Windows resource: UTF-16LE hex.
utf16hex() {
    printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n' | sed 's/../&00/g'
}

# ── A project whose only interesting feature is [resources] ───────────────
mkdir -p "$TMP/proj/src" "$TMP/proj/assets"
cd "$TMP/proj"

# A minimal but structurally valid 4x1 32bpp icon: ICONDIR + ICONDIRENTRY +
# BITMAPINFOHEADER + four BGRA pixels + AND mask.
#
# Four pixels, not one, purely so the payload is findable AGAIN inside the
# linked image without false positives: an .ico's bitmap data is embedded
# verbatim, and searching a megabyte-scale binary for a 4-byte pattern hits by
# chance often enough to make the assertion meaningless. 16 bytes does not.
write_icon() {   # $1 = 4 BGRA pixels (16 bytes) as printf escapes
    printf '\x00\x00\x01\x00\x01\x00\x04\x01\x00\x00\x01\x00\x20\x00\x3c\x00\x00\x00\x16\x00\x00\x00\x28\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x01\x00\x20\x00\x00\x00\x00\x00\x14\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'"$1"'\x00\x00\x00\x00' > assets/app.ico
}
ICON_A='\xd3\x1c\x7a\x45\x92\xe6\x0b\xa8\x41\xf7\x2d\x63\xbe\x50\x84\x19'
ICON_B='\x6c\xa2\x38\xd7\xe1\x4b\x95\x0f\x77\xc4\x1a\x8e\x2b\xf3\x60\xd5'
ICON_A_HEX='d31c7a4592e60ba841f72d63be508419'
ICON_B_HEX='6ca238d7e14b950f77c41a8e2bf360d5'
write_icon "$ICON_A"

printf 'int main() { return 0; }\n' > src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name        = "resapp"
version     = "1.2.3"
description = "Resource fixture"
license     = "MIT"
authors     = ["Acme Corp"]

[resources]
icon = "assets/app.ico"

[targets.resapp]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build $BUILD_ARGS > b1.log 2>&1 || fail "build with [resources] failed" b1.log

BUILD_DIR=$(dirname "$(find target -name 'build.ninja' -print | head -1)")
[ -n "$BUILD_DIR" ] || fail "no build dir" b1.log

# ── A1. The generated script names the version resource by ORDINAL ────────
#
# This is the whole mcpp#365 bug. `VS_VERSION_INFO` is a <windows.h> macro; an
# undefined identifier in the name position files the resource under a STRING
# name, and GetFileVersionInfo — which looks up ordinal 1 — then reports every
# field as empty while every tool that prints the resource TYPE says it is fine.
GEN_RC="$BUILD_DIR/res/resapp.mcpp.rc"
[ -f "$GEN_RC" ] || fail "no generated resource script at $GEN_RC" b1.log
grep -q '^1 VERSIONINFO' "$GEN_RC" || fail "generated script must use ordinal 1" "$GEN_RC"
grep -q 'VS_VERSION_INFO' "$GEN_RC" && fail "generated script must never name the macro" "$GEN_RC"
grep -q 'FILEVERSION    1,2,3,0' "$GEN_RC" || fail "FILEVERSION must come from [package].version" "$GEN_RC"
grep -q '"CompanyName", "Acme Corp"' "$GEN_RC" || fail "metadata must default from [package]" "$GEN_RC"

# The compiled resource artifact exists and is a link input.
RES_ART=$(ls "$BUILD_DIR"/res/resapp.mcpp.res "$BUILD_DIR"/res/resapp.mcpp.o 2>/dev/null | head -1)
[ -n "$RES_ART" ] || fail "no compiled resource artifact under $BUILD_DIR/res" b1.log
grep -q 'rc_object' "$BUILD_DIR/build.ninja" || fail "no rc_object edge" "$BUILD_DIR/build.ninja"

case "$RES_ART" in
  *.res)
    # `.res` is a documented container: a 32-byte null header, then per
    # resource dataSize+headerSize followed by type and name. `ffff` introduces
    # an ordinal, so RT_VERSION(16) named 1 is exactly ffff1000ffff0100.
    hexof "$RES_ART" | cut -c81-96 | grep -qi '^ffff1000ffff0100$' \
        || fail "version resource is not at ordinal 1 (this is the #365 bug)" b1.log
    ;;
esac

# The same assertion in a form that covers the GNU fork too, since the byte
# check above reads the `.res` container and windres emits a COFF object.
#
# `windres -J coff -O rc` round-trips a compiled resource back to rc SOURCE, so
# the name is readable without a PE parser — and it is guaranteed present on
# this fork, because it is the tool that produced the file. Measured on both a
# `.o` and the linked `.exe`:
#
#     1 VERSIONINFO                   ← Windows finds it
#     "VS_VERSION_INFO" VERSIONINFO   ← Windows does not
#
# NOT a whole-file search for the string `VS_VERSION_INFO`: that is the `szKey`
# of the VS_VERSIONINFO struct itself, so a CORRECT resource contains it too.
# Written that way first, and it fired on a good windres build — which is the
# only reason this comment exists.
#
# The rc tool comes out of build.ninja rather than off PATH: mcpp resolved it
# payload-relative on purpose (a bare `windres` on PATH is an xlings shim), so
# asking PATH here would inspect the artifact with a different tool than the one
# that built it.
RC_TOOL=$(sed -n 's/^rc *= *//p' "$BUILD_DIR/build.ninja" | head -1)
[ -n "$RC_TOOL" ] || fail "no 'rc =' binding in build.ninja" "$BUILD_DIR/build.ninja"

# Two routes, TRIED in order rather than dispatched on the tool's name: GNU
# binutils' windres round-trips, `llvm-windres` does not (it only wraps llvm-rc,
# one way) — and both answer to `*windres*`, so the name tells you nothing. Only
# when NEITHER route exists is this a hard failure; silently skipping the
# assertion is how #365 shipped in the first place.
#
# $1 = file to inspect, $2 = log suffix, $3 = "ordinal" | "string"
version_name_is() {
    case "$3" in
      ordinal) _want='^[[:space:]]*1[[:space:]]+VERSIONINFO' ; _n='Name: (ID 1)' ;;
      string)  _want='"VS_VERSION_INFO"[[:space:]]+VERSIONINFO' ; _n='Name: VS_VERSION_INFO' ;;
    esac

    # Route 1 — windres back to rc SOURCE. Readable, and no PE parser needed.
    if [ -n "$RC_TOOL" ] \
       && "$RC_TOOL" -J coff -O rc -i "$1" -o "rt.$2.rc" 2>"rt.$2.err" \
       && grep -qiE 'VERSIONINFO' "rt.$2.rc"; then
        grep -qE "$_want" "rt.$2.rc" \
            || fail "expected the version resource to be named by $3 in $1" "rt.$2.rc"
        return 0
    fi

    # Route 2 — the resource DIRECTORY, via llvm-readobj.
    READOBJ=$(ls "$MCPP_HOME"/registry/data/xpkgs/xim-x-llvm/*/bin/llvm-readobj \
                 "$MCPP_HOME"/registry/data/xpkgs/xim-x-llvm/*/bin/llvm-readobj.exe \
                 2>/dev/null | head -1)
    [ -n "$READOBJ" ] || fail "no way to read back $1: '$RC_TOOL' cannot round-trip and no llvm-readobj was found under $MCPP_HOME" "rt.$2.err"
    "$READOBJ" --coff-resources "$1" > "readobj.$2.log" 2>&1 \
        || fail "llvm-readobj could not read $1" "readobj.$2.log"
    grep -q 'Type: VERSIONINFO' "readobj.$2.log" \
        || fail "$1 carries no version resource at all" "readobj.$2.log"
    # The window is the VERSIONINFO type table only — an icon is also `(ID 1)`,
    # so matching anywhere in the file would pass for the wrong reason.
    grep -A5 'Type: VERSIONINFO' "readobj.$2.log" | grep -qF "$_n" \
        || fail "expected the version resource to be named by $3 in $1" "readobj.$2.log"
}
version_name_is "$RES_ART" art ordinal

# ── The resource actually reached the linked image ────────────────────────
EXE="$BUILD_DIR/bin/resapp$EXE_SUFFIX"
[ -f "$EXE" ] || fail "no executable at $EXE" b1.log
# The ordinal has to survive the LINK, not just the resource compile — that is
# what GetFileVersionInfo actually reads.
version_name_is "$EXE" exe ordinal
EXE_HEX=$(hexof "$EXE")
echo "$EXE_HEX" | grep -q "$(utf16hex 'Acme Corp')" \
    || fail "the version metadata did not reach the executable" b1.log
echo "$EXE_HEX" | grep -q "$ICON_A_HEX" \
    || fail "the icon payload did not reach the executable" b1.log

# ── A2. Resources are tracked build inputs ────────────────────────────────
#
# The workaround this feature replaces (a pre-built .res named in ldflags) is
# invisible to ninja: the reporter's symptom was "ninja: no work to do" after
# changing the icon.
"$MCPP" build $BUILD_ARGS > b2.log 2>&1 || fail "no-op rebuild failed" b2.log
grep -qE 'no work to do|Finished' b2.log || fail "unexpected rebuild output" b2.log

sleep 1
write_icon "$ICON_B"                  # same size, entirely different bytes
"$MCPP" build $BUILD_ARGS > b3.log 2>&1 || fail "rebuild after icon change failed" b3.log
NEW_HEX=$(hexof "$BUILD_DIR/bin/resapp$EXE_SUFFIX")
echo "$NEW_HEX" | grep -q "$ICON_B_HEX" \
    || fail "editing the icon did not reach the executable (the #365 symptom)" b3.log
echo "$NEW_HEX" | grep -q "$ICON_A_HEX" \
    && fail "the old icon is still embedded — the resource was not rebuilt" b3.log

# Metadata is an input too: the .rc is regenerated and everything downstream
# re-runs.
sleep 1
sed -i.bak 's/^description = .*/description = "Changed description"/' mcpp.toml && rm -f mcpp.toml.bak
"$MCPP" build $BUILD_ARGS > b4.log 2>&1 || fail "rebuild after metadata change failed" b4.log
grep -q '"FileDescription", "Changed description"' "$GEN_RC" \
    || fail "the generated script did not follow [package].description" "$GEN_RC"
hexof "$BUILD_DIR/bin/resapp$EXE_SUFFIX" | grep -q "$(utf16hex 'Changed description')" \
    || fail "the changed description did not reach the executable" b4.log

# ── A6. L0 → L1 has no cliff ──────────────────────────────────────────────
#
# Taking the generated script over must reproduce the same resource byte for
# byte, or "each layer is the next layer's default" is only a slogan.
cp "$RES_ART" "$TMP/generated.artifact"
mkdir -p res
cp "$GEN_RC" res/app.rc
cat > mcpp.toml <<'EOF'
[package]
name        = "resapp"
version     = "1.2.3"
description = "Changed description"
license     = "MIT"
authors     = ["Acme Corp"]

[resources]
files = ["res/app.rc"]

[targets.resapp]
kind = "bin"
main = "src/main.cpp"
EOF
"$MCPP" build $BUILD_ARGS > b5.log 2>&1 || fail "build with an author-written .rc failed" b5.log
AUTHORED=$(ls "$BUILD_DIR"/res/app.res "$BUILD_DIR"/res/app.o 2>/dev/null | head -1)
[ -n "$AUTHORED" ] || fail "the author-written script was not compiled" b5.log
cmp -s "$AUTHORED" "$TMP/generated.artifact" \
    || fail "taking over the generated script changed the resource bytes" b5.log
# ...and with files declared, mcpp stops synthesising: the author owns the ID
# space, and a second RT_VERSION at ordinal 1 is not a thing that can exist.
# Asserted on the GRAPH, not on the directory: the previous build's artifact is
# still lying around, and "the file is absent" would be testing rm, not mcpp.
grep -q 'resapp\.mcpp\.' "$BUILD_DIR/build.ninja" \
    && fail "mcpp must not add a second VERSIONINFO behind an author-written script" "$BUILD_DIR/build.ninja"

# ── A3 (lint). A script Windows cannot read is named, not shipped quietly ──
#
# The `.rc` is the ONLY thing that changes here, so the rebuild has to come from
# its mtime — which is exactly the project-level fast-path hole this feature had
# to close. A same-second write against a coarse-granularity filesystem would
# make the whole section silently not run.
sleep 1
cat > res/app.rc <<'EOF'
VS_VERSION_INFO VERSIONINFO
 FILEVERSION 1,2,3,0
 PRODUCTVERSION 1,2,3,0
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "ProductName", "resapp"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
EOF
"$MCPP" build $BUILD_ARGS > b6.log 2>&1 || true
grep -q 'instead of ordinal 1' b6.log \
    || fail "the VS_VERSION_INFO-without-windows.h shape must be diagnosed" b6.log
# It is a DEGRADATION, not a bare warning: the impact is a shipped binary whose
# version metadata Windows cannot read, so --strict has to see it.
grep -q 'GetFileVersionInfo' b6.log \
    || fail "the diagnostic must state the impact, not just the shape" b6.log

# And the counter-check that makes the ordinal assertion at the top meaningful:
# compiled from THIS script, the resource really is filed under a STRING name.
# Without it, "the name is (ID 1)" could be passing for any reason at all.
BROKEN=$(ls "$BUILD_DIR"/res/app.res "$BUILD_DIR"/res/app.o 2>/dev/null | head -1)
[ -n "$BROKEN" ] || fail "the broken script produced no artifact to compare against" b6.log
version_name_is "$BROKEN" broken string

# ── D-6. A declared resource that does not exist is an ERROR ──────────────
#
# Deliberately not the "skip it" the issue asked for: silently shipping a
# release binary with no icon, and saying nothing, is the failure this whole
# feature exists to remove.
rm -rf res
cat > mcpp.toml <<'EOF'
[package]
name    = "resapp"
version = "1.2.3"

[resources]
icon = "assets/missing.ico"

[targets.resapp]
kind = "bin"
main = "src/main.cpp"
EOF
"$MCPP" build $BUILD_ARGS > b7.log 2>&1 && fail "a missing declared resource must fail the build" b7.log
grep -q 'does not exist' b7.log || fail "expected a clear missing-file error" b7.log

echo "OK"
