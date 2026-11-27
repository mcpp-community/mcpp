#!/usr/bin/env bash
# M5 (Linux): a home reached through a symbolic link, installed from a bundle
# without the network's help; which spelling ends up in what the home records.
. "$(dirname "$0")/common.sh"
export XLINGS_AGENT_MODE=1
fetch_xlings
X="$XLINGS_BIN"

say "M5 step 1: a bundle written with a plain home"
export XLINGS_HOME="$PROBE_TMP/plain-home"; mkdir -p "$XLINGS_HOME"
B="$PROBE_TMP/bundle"
tailed 25 "$X" download ninja --to "$B" --with-index
show sh -c "ls $B; ls $B/archives | head; head -40 $B/bundle.toml"

say "M5 step 2: a home that is a symbolic link, installing from the bundle"
REAL="$PROBE_TMP/real-home"; LINK="$PROBE_TMP/link-home"
mkdir -p "$REAL"; ln -s "$REAL" "$LINK"
export XLINGS_HOME="$LINK"
tailed 40 "$X" install --from "$B" ninja
tailed 30 "$X" self doctor
echo "-- which spelling the home's own files carry"
echo "files naming the physical directory: $(grep -rIl -- "$REAL" "$REAL" 2>/dev/null | wc -l)"
grep -rIl -- "$REAL" "$REAL" 2>/dev/null | head -12
echo "files naming the link: $(grep -rIl -- "$LINK" "$REAL" 2>/dev/null | wc -l)"
grep -rIl -- "$LINK" "$REAL" 2>/dev/null | head -12
echo "-- links under the home: targets by spelling"
find "$REAL" -maxdepth 5 -type l 2>/dev/null | head -40 | while read -r l; do printf '%s -> %s\n' "${l#$REAL/}" "$(readlink "$l")"; done | cut -c1-220
NIN="$(ls "$LINK"/subos/*/bin/ninja 2>/dev/null | head -1)"; echo "NIN=$NIN"
[ -n "$NIN" ] && show "$NIN" --version

say "M5 step 3: mcpp with its home through a symbolic link"
MREAL="$PROBE_TMP/mcpp-real"; MLINK="$PROBE_TMP/mcpp-link"; mkdir -p "$MREAL"; ln -s "$MREAL" "$MLINK"
export MCPP_HOME="$MLINK"
unset XLINGS_HOME
fetch_mcpp
H="$PROBE_TMP/m5-hello"; hello_project "$H"
( cd "$H" && "$MCPP" build -v > "$PROBE_TMP/m5-build.log" 2>&1; echo "[exit $?]" >> "$PROBE_TMP/m5-build.log" )
grep -nE 'warning|error|degraded|root|link' "$PROBE_TMP/m5-build.log" | head -30 | cut -c1-400
tail -n 8 "$PROBE_TMP/m5-build.log" | cut -c1-400
echo "-- the build's command lines: which spelling of the home"
echo "lines naming the link: $(grep -c -- "$MLINK" "$PROBE_TMP/m5-build.log")"
echo "lines naming the physical directory: $(grep -c -- "$MREAL" "$PROBE_TMP/m5-build.log")"
grep -n -- "$MREAL" "$PROBE_TMP/m5-build.log" | head -6 | cut -c1-500
echo "-- the store's roots"
show sh -c "ls -la $MLINK/registry/data/roots 2>/dev/null | head; find $MLINK/registry/data/roots -type f 2>/dev/null | head -3 | xargs -r head -20"
BIN="$(find "$H/target" -path '*/bin/hello' -type f | head -1)"
[ -n "$BIN" ] && { readelf -l "$BIN" | grep -i interpreter; readelf -d "$BIN" | grep -E 'RPATH|RUNPATH' | cut -c1-300; "$BIN"; echo "[run exit $?]"; }
say "M5 step 4: the bundled xlings of that home"
XB="$(find "$MLINK/" -type f -name xlings -perm -u+x 2>/dev/null | head -1)"; echo "bundled xlings: $XB"
[ -n "$XB" ] && XLINGS_HOME="$MLINK/registry" tailed 30 "$XB" self doctor
