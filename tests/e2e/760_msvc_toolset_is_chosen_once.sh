#!/usr/bin/env bash
# requires: msvc python3
# 760_msvc_toolset_is_chosen_once.sh — on the MSVC ABI one toolset is chosen,
# once, and every consumer reads it.
#
# The clang row used to reach its toolset twice: the driver searched the
# machine for headers and libraries (VCToolsInstallDir, PATH, the newest
# instance's default) while mcpp searched it again for `std.ixx` by a different
# order, and the result reached neither the cache key nor any record. The
# toolset is the row's sysroot now (`[target.<triple>].sysroot`, default
# `msvc@system`), resolved by prepare and handed to the driver as
# `-Xmicrosoft-*` words.
#
# Everything here uses the runner's own Visual Studio and downloads nothing;
# the package half of the same rule (`xim:msvc@<toolset>`) is exercised by
# 239 in ci-windows-msvc-xlings.yml.
#
# The EXPECTED toolset is read independently of the code under test: vswhere
# names the newest instance, and that instance's
# `Microsoft.VCToolsVersion.default.txt` names its default toolset.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[[ -x "$VSWHERE" ]] || { echo "SKIP: no vswhere on this runner"; exit 0; }
VSROOT="$("$VSWHERE" -latest -prerelease -products '*' \
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath | tr -d '\r')"
[[ -n "$VSROOT" ]] || { echo "FAIL: vswhere reports no instance with C++ tools"; exit 1; }
VSROOT_U="$(cygpath -u "$VSROOT")"
EXPECTED="$(tr -d '\r\n ' < "$VSROOT_U/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")"
[[ -n "$EXPECTED" ]] || { echo "FAIL: no default toolset in $VSROOT"; exit 1; }
echo "runner: $VSROOT, default toolset $EXPECTED"

# No developer prompt: the default selection must not depend on one.
unset VCToolsInstallDir VSINSTALLDIR VCINSTALLDIR

resolution() { find target -name resolution.json | head -1; }

# ── 1. The clang row, no sysroot declared: the machine's default ──────────
"$MCPP" new chosen > /dev/null
cd chosen
"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }
grep -q "sysroot msvc@system → MSVC $EXPECTED" build.log \
    || { echo "FAIL: no line naming the chosen toolset:"; cat build.log; exit 1; }

python3 - "$(resolution)" "$EXPECTED" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
t = d.get("msvc_toolset")
assert t, "resolution.json records no msvc_toolset for the clang row"
assert t["version"] == sys.argv[2], f"toolset {t['version']} != {sys.argv[2]}"
assert t["origin"] == "system", t
assert t["root"].endswith("/" + sys.argv[2]), t
sdk = d.get("windows_sdk", {})
assert sdk.get("version"), "no Windows SDK recorded"
rid = d.get("runtime", {}).get("binding", {}).get("runtime_id", "")
# The SDK identity now reaches the clang row's contract too.
assert rid == "ucrt@" + sdk["version"], (rid, sdk)
print("OK:", t["version"], t["origin"], sdk["version"])
PY

# The driver is TOLD the toolset, with the same directory the record names.
NINJA="$(find target -name build.ninja | head -1)"
grep -q -- "-Xmicrosoft-visualc-tools-root" "$NINJA" \
    || { echo "FAIL: build.ninja does not pass the toolset to clang"; exit 1; }
grep -q -- "-Xmicrosoft-windows-sdk-version" "$NINJA" \
    || { echo "FAIL: build.ninja does not pass the SDK to clang"; exit 1; }
grep -q "MSVC\\\\$EXPECTED\|MSVC/$EXPECTED" "$NINJA" \
    || { echo "FAIL: build.ninja names a toolset other than $EXPECTED"; exit 1; }

out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $out"; exit 1; }
[[ "$out" == *"Hello"* || "$out" == *"hello"* ]] || { echo "FAIL: run output: $out"; exit 1; }

# ── 2. Pinned to an installed toolset; the environment is not consulted ───
#
# A developer prompt opened with `-vcvars_ver=` exports VCToolsInstallDir.
# The pin must win, and the ignored declaration must be reported.
mkdir -p "$TMP/fake/VC/Tools/MSVC/14.99.0/include"
cat >> mcpp.toml <<EOF

[target.x86_64-windows-msvc]
sysroot = "msvc@$EXPECTED"
EOF
VCToolsInstallDir="$(cygpath -w "$TMP/fake/VC/Tools/MSVC/14.99.0")\\" \
    "$MCPP" build > pinned.log 2>&1 || { cat pinned.log; exit 1; }
grep -q "VCToolsInstallDir (14.99.0) is ignored" pinned.log \
    || { echo "FAIL: the ignored VCToolsInstallDir was not reported:"; cat pinned.log; exit 1; }
grep -q "sysroot msvc@$EXPECTED → MSVC $EXPECTED (system" pinned.log \
    || { echo "FAIL: pinned toolset not taken from the machine:"; cat pinned.log; exit 1; }

# ── 3. What is not an MSVC toolset is refused where the manifest is read ──
sed -i "s|sysroot = \"msvc@$EXPECTED\"|sysroot = \"xim:glibc@2.39\"|" mcpp.toml
if "$MCPP" build > refused.log 2>&1; then
    echo "FAIL: a C library package was accepted as an MSVC sysroot"; exit 1
fi
grep -q "MSVC toolset" refused.log || { cat refused.log; exit 1; }

# ── 4. The cl.exe row: a pinned version this machine has is used in place ─
cd "$TMP"
"$MCPP" new clrow > /dev/null
cd clrow
cat >> mcpp.toml <<EOF

[toolchain]
windows = "msvc@$EXPECTED"
EOF
"$MCPP" build --verbose > cl.log 2>&1 || { cat cl.log; exit 1; }
line="$(grep -E "Resolved msvc@$EXPECTED" cl.log | head -1)"
[[ "$line" == *"(installed:"* ]] || { echo "FAIL: cl.exe row did not use the installed toolset: $line"; cat cl.log; exit 1; }
[[ "$line" != *"xim-x-msvc"* ]] || { echo "FAIL: resolved to a package: $line"; exit 1; }

# ── 5. On the cl.exe row the compiler is its own sysroot ──────────────────
cat >> mcpp.toml <<'EOF'

[target.x86_64-windows-msvc]
sysroot = "msvc@14.1.0"
EOF
if "$MCPP" build > clmismatch.log 2>&1; then
    echo "FAIL: a sysroot naming another toolset was accepted on the cl.exe row"; exit 1
fi
grep -q "names a different toolset than the compiler" clmismatch.log \
    || { cat clmismatch.log; exit 1; }

echo "PASS: one toolset per build on the MSVC ABI, pinned or not, recorded and passed to the driver"
