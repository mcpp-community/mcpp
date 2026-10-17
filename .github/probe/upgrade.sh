#!/usr/bin/env bash
# TEMPORARY probe (do not merge). Prints, never asserts.
#
# Measures what a user sees when they upgrade from the previous release
# (2026.10.11.1, bundled xlings 2026.10.10.2) to the current one
# (2026.10.16.3, bundled xlings 2026.10.16.2) on a home the previous release
# made, and then run `mcpp new && mcpp run`.
#
# variant:
#   plain     the previous release installs from today's index; nothing altered
#   snapshot  the previous release installs from the xim index as of
#             2026-10-07 (xim-pkgindex ebf1fbb, before llvm revision 1 and
#             gcc 16.1.0-r1): payloads genuinely of the old revision
#   stamp     like plain, then every payload stamp records a revision other
#             than its recipe's (the mechanism, on every platform)
#   orphan    like stamp, then the version DB loses c++@<gcc version> while
#             the workspace keeps it active (the shape of the reported home)
set -u
VARIANT="${1:-plain}"
OLD_VER=2026.10.11.1
NEW_VER=2026.10.16.3
SNAPSHOT=ebf1fbb3417ec513b923279044433887b3af8850

W="${RUNNER_TEMP:-/tmp}/upg"
rm -rf "$W"; mkdir -p "$W"
export MCPP_HOME="$W/home"
REG="$MCPP_HOME/registry"
PY=$(command -v python3 || command -v python)

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)  PLAT=linux-x86_64;  EXT=tar.gz; EXE= ;;
  Linux-aarch64) PLAT=linux-aarch64; EXT=tar.gz; EXE= ;;
  Darwin-arm64)  PLAT=macosx-arm64;  EXT=tar.gz; EXE= ;;
  MINGW*|MSYS*)  PLAT=windows-x86_64; EXT=zip;   EXE=.exe ;;
  *) echo "unknown platform $(uname -a)"; exit 0 ;;
esac
IS_LINUX=0; [[ $PLAT == linux-* ]] && IS_LINUX=1

section() { echo; echo "================ [$(date -u +%H:%M:%S)] $* ================"; }

fetch_release() {  # ver dest
  local ver=$1 dest=$2 name="mcpp-$1-$PLAT.$EXT"
  curl -fsSL -o "$W/$name" "https://github.com/mcpp-community/mcpp/releases/download/v$ver/$name" || { echo "download $name failed"; return 1; }
  mkdir -p "$W/x-$ver"
  if [[ $EXT == zip ]]; then
    "$PY" -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$W/$name" "$W/x-$ver"
  else
    tar xzf "$W/$name" -C "$W/x-$ver"
  fi
  mv "$W/x-$ver"/mcpp-$ver-$PLAT "$dest"
  chmod +x "$dest/bin/mcpp$EXE" "$dest/registry/bin/xlings$EXE" 2>/dev/null || true
}

# run <label> <dir> <mcpp> args...: time, exit code, and the lines that matter
RUNS="$W/runs.tsv"; : > "$RUNS"
run() {
  local label=$1 dir=$2 bin=$3; shift 3
  section "$label: (cd $dir && mcpp $*)"
  local t0=$(date +%s) rc
  (cd "$dir" && "$bin" "$@") > "$W/$label.log" 2>&1; rc=$?
  local dt=$(( $(date +%s) - t0 ))
  sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "$W/$label.log" | tr '\r' '\n' | grep -v '^\s*Planning ' | tail -40
  local refresh=$(grep -c "Refreshing package index" "$W/$label.log")
  local reinstall=$(grep -o "reinstall[a-z]*: recipe revision [0-9]*, installed revision [0-9]*\|reinstalling [^:]*" "$W/$label.log" | sort -u | tr '\n' ';')
  local dl=$(grep -o "Downloading [^ ]*\( done, [0-9.]* [KMG]B\)\?" "$W/$label.log" | tr '\n' ';')
  echo "--> rc=$rc ${dt}s refresh=$refresh reinstall=[$reinstall] downloads=[$dl]"
  printf '%s\t%s\t%ss\t%s\t%s\t%s\n' "$label" "$rc" "$dt" "$refresh" "$reinstall" "$dl" >> "$RUNS"
}

stamps() {
  section "payload stamps ($1)"
  for d in "$REG"/data/xpkgs/xim-x-llvm/* "$REG"/data/xpkgs/xim-x-gcc/* "$REG"/data/xpkgs/xim-x-glibc/* "$REG"/data/xpkgs/xim-x-gcc-runtime/*; do
    [ -f "$d/.xpkg-install.json" ] && echo "$d: $(tr -d '\n ' < "$d/.xpkg-install.json")"
  done
  section "xlings ($1)"
  "$REG/bin/xlings$EXE" --version 2>&1 | head -1
  local targets='["xim:llvm@23.1.3"'; [ $IS_LINUX = 1 ] && targets+=',"xim:gcc@16.1.0","xim:glibc@2.44.3"'; targets+=']'
  (cd "$REG" && XLINGS_HOME="$REG" "$REG/bin/xlings$EXE" interface payload_status --args "{\"targets\":$targets}" 2>&1 | grep -o '{"targets".*' | head -1) || echo "(payload_status not available)"
  section "index layout ($1)"
  ls "$REG/data" 2>&1 | tr '\n' ' '; echo
  ls -d "$MCPP_HOME"/indices/* 2>/dev/null
  for f in "$REG/data/xim-pkgindex/pkgs/l/llvm.lua" "$REG/data/xim-pkgindex/pkgs/g/gcc.lua"; do
    if [ -f "$f" ]; then echo "$f revisions: $(grep -o 'revision = [0-9]*' "$f" | tr '\n' ' ')"; else echo "$f: MISSING"; fi
  done
  "$PY" - "$REG/.xlings.json" <<'PY' 2>&1
import json, sys
d = json.load(open(sys.argv[1]))
print("home config keys:", sorted(d))
for r in d.get("index_repos", []): print("  index_repo:", {k: r.get(k) for k in ("name", "dir", "store", "url")})
PY
}

llvm_facts() {
  section "llvm payload facts ($1)"
  local root="$REG/data/xpkgs/xim-x-llvm/23.1.3"
  for cfg in "$root"/bin/*.cfg; do [ -f "$cfg" ] && { echo "--- $cfg"; cat "$cfg"; }; done
  [ -x "$root/bin/clang++$EXE" ] && echo | "$root/bin/clang++$EXE" -v -E -x c++ - 2>&1 | sed -n '/search starts here/,/End of search list/p'
}

section "probe variant=$VARIANT platform=$PLAT"
fetch_release "$OLD_VER" "$W/old" || exit 0
fetch_release "$NEW_VER" "$W/new" || exit 0
OLD="$W/old/bin/mcpp$EXE"; NEW="$W/new/bin/mcpp$EXE"
"$OLD" --version; "$NEW" --version

new_project() {  # dir toolchain
  (cd "$W" && "$OLD" new "$1" > /dev/null 2>&1) || mkdir -p "$W/$1"
  printf '\n[toolchain]\ndefault = "%s"\n' "$2" >> "$W/$1/mcpp.toml"
}

# ---------------------------------------------------------------- old release
run old-env "$W" "$OLD" self env
if [ "$VARIANT" = snapshot ]; then
  section "swap the xim index for xim-pkgindex@$SNAPSHOT"
  git clone -q --filter=blob:none https://github.com/openxlings/xim-pkgindex "$W/xpi" && git -C "$W/xpi" checkout -q "$SNAPSHOT"
  idx="$REG/data/xim-pkgindex"
  find "$idx" -mindepth 1 -maxdepth 1 ! -name .xlings-index-version -exec rm -rf {} +
  (cd "$W/xpi" && git archive HEAD) | tar x -C "$idx"
  echo "llvm.lua now: $(grep -o 'revision = [0-9]*' "$idx/pkgs/l/llvm.lua" | tr '\n' ' ')"
fi

run old-tc-llvm "$W" "$OLD" toolchain install llvm@23.1.3
[ $IS_LINUX = 1 ] && run old-tc-gcc "$W" "$OLD" toolchain install gcc@16.1.0
new_project p-llvm llvm@23.1.3
run old-run-llvm "$W/p-llvm" "$OLD" run
if [ $IS_LINUX = 1 ]; then new_project p-gcc gcc@16.1.0; run old-run-gcc "$W/p-gcc" "$OLD" run; fi
stamps after-old
[ $IS_LINUX = 1 ] && llvm_facts after-old

if [ "$VARIANT" = stamp ] || [ "$VARIANT" = orphan ]; then
  section "rewrite stamps to a revision other than the recipe's"
  "$PY" - "$REG/data/xpkgs" <<'PY'
import json, os, sys, glob
for f in glob.glob(os.path.join(sys.argv[1], "xim-x-*", "*", ".xpkg-install.json")):
    d = json.load(open(f)); r = d.get("revision", 0)
    d["revision"] = 1 if r == 0 else r - 1
    json.dump(d, open(f, "w"), indent=2); print(f, r, "->", d["revision"])
PY
fi
if [ "$VARIANT" = orphan ]; then
  section "drop c++@<gcc version> from the version DB, keep the workspace"
  "$PY" - "$REG" <<'PY'
import json, os, sys
reg = sys.argv[1]
ws = json.load(open(os.path.join(reg, "subos/default/.xlings.json")))["workspace"]
print("workspace c++:", ws.get("c++"))
active = (ws.get("c++") or {}).get("active")
for path, key in ((os.path.join(reg, "data/versions.json"), "versions"), (os.path.join(reg, ".xlings.json"), "versions")):
    if not os.path.exists(path): continue
    doc = json.load(open(path)); db = doc.get(key, {})
    cxx = db.get("c++", {})
    gone = cxx.get("versions", {}).pop(active, None) is not None
    for edges in cxx.get("bindings", {}).values(): edges.pop(active, None)
    json.dump(doc, open(path, "w")); print(path, "dropped c++@%s:" % active, gone)
PY
fi

# ---------------------------------------------------------------- new release
if [ "$VARIANT" = snapshot ]; then
  # the old index is still in place: does the new release use the old-revision
  # payload, and does it build with it?
  run new-run-llvm-on-snapshot "$W/p-llvm" "$NEW" run
  stamps after-new-on-snapshot
  llvm_facts after-new-on-snapshot
  run new-index-update "$W" "$NEW" index update
fi
new_project p-new llvm@23.1.3
run new-run1-llvm "$W/p-new" "$NEW" run
run new-run2-llvm "$W/p-new" "$NEW" run
run new-run3-llvm-old-project "$W/p-llvm" "$NEW" run
if [ $IS_LINUX = 1 ]; then
  run new-run1-gcc "$W/p-gcc" "$NEW" run
  run new-run2-gcc "$W/p-gcc" "$NEW" run
fi
stamps after-new
[ $IS_LINUX = 1 ] && llvm_facts after-new
run new-doctor "$W" "$NEW" self doctor

section "SUMMARY variant=$VARIANT platform=$PLAT"
{
  echo "### upgrade probe: $PLAT / $VARIANT"
  echo
  echo "| run | rc | time | index refreshes | reinstall | downloads |"
  echo "|---|---|---|---|---|---|"
  while IFS=$'\t' read -r a b c d e f; do echo "| $a | $b | $c | $d | ${e:-} | ${f:-} |"; done < "$RUNS"
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
mkdir -p "${GITHUB_WORKSPACE:-.}/probe-out"; cp -r "$W"/*.log "$W/runs.tsv" "${GITHUB_WORKSPACE:-.}/probe-out/" 2>/dev/null || true
exit 0
