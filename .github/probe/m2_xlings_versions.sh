#!/usr/bin/env bash
# M2: an environment declares one version of a compiler; a payload installed into
# it depends on another. What xlings does, and what mcpp does with the same two
# statements. Prints, never asserts.
. "$(dirname "$0")/common.sh"
export XLINGS_AGENT_MODE=1
fetch_xlings
export XLINGS_HOME="$PROBE_TMP/xlings-home"
mkdir -p "$XLINGS_HOME"
say "a home for the released xlings"
show "$XLINGS_BIN" self install --help
tailed 15 "$XLINGS_BIN" --version
X="$XLINGS_BIN"

cat > "$PROBE_TMP/probe-sdk.lua" <<'LUA'
package = {
    spec = "1",
    name = "probe-sdk",
    description = "probe: a library payload that depends on one version of a compiler",
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "dev",
    xpm = {
        linux = {
            deps = { "xim:gcc@16.1.0" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}
function installed() return false end
function install() return true end
function uninstall() return true end
LUA

P="$PROBE_TMP/m2-project"; mkdir -p "$P"; cd "$P"
printf '{ "workspace": { "gcc": { "linux": "15.1.0" } } }\n' > .xlings.json
say "M2 step 1: the project declares gcc 15.1.0"
tailed 25 "$X" install
show cat .xlings.json
show sh -c 'ls -la .xlings 2>/dev/null; ls .xlings/subos 2>/dev/null; ls .xlings/subos/_/bin 2>/dev/null | head -20'
GCC_SHIM="$(ls "$P"/.xlings/subos/_/bin/gcc "$XLINGS_HOME"/subos/*/bin/gcc 2>/dev/null | head -1)"
echo "GCC_SHIM=$GCC_SHIM"
[ -n "$GCC_SHIM" ] && show "$GCC_SHIM" --version
show sh -c "ls $XLINGS_HOME/data/xpkgs 2>/dev/null; ls $XLINGS_HOME/data/xpkgs/xim-x-gcc 2>/dev/null"

say "M2 step 2: a payload that depends on xim:gcc@16.1.0 is installed into the project"
tailed 20 "$X" config --add-xpkg "$PROBE_TMP/probe-sdk.lua"
tailed 10 "$X" config --list-xpkg
tailed 40 "$X" install probe-sdk@1.0.0
show cat .xlings.json
show sh -c "ls $XLINGS_HOME/data/xpkgs/xim-x-gcc 2>/dev/null"
[ -n "$GCC_SHIM" ] && show "$GCC_SHIM" --version
show sh -c 'cat .xlings/subos/_/.xlings.json 2>/dev/null | head -60'
tailed 30 "$X" list
tailed 30 "$X" self doctor

say "M2 step 3: the same two statements through mcpp ([toolchain] and [xlings.workspace])"
export MCPP_HOME="$PROBE_TMP/mcpp-home"
fetch_mcpp
M="$PROBE_TMP/m2-mcpp"; hello_project "$M"
printf '\n[toolchain]\nlinux = "gcc@15.1.0"\n\n[xlings.workspace]\n"xim:gcc" = "16.1.0"\n' >> "$M/mcpp.toml"
( cd "$M" && tailed 40 "$MCPP" build )
show sh -c "ls $MCPP_HOME/registry/data/xpkgs/xim-x-gcc 2>/dev/null"
say "M2 step 4: a floor instead of a version"
sed -i 's/"xim:gcc" = "16.1.0"/"xim:gcc" = ">=16.1.0"/' "$M/mcpp.toml"
( cd "$M" && tailed 40 "$MCPP" build )
say "M2 step 5: how a library payload of the index states its runtime (for comparison)"
show sh -c "grep -n 'deps' $XLINGS_HOME/data/xim-pkgindex/pkgs/l/libxcb.lua $XLINGS_HOME/data/xim-pkgindex/pkgs/h/harfbuzz.lua 2>/dev/null | head"
