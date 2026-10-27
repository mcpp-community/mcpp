#!/usr/bin/env bash
# upgrade_lane.sh <variant> <new-mcpp> <new-xlings> -- a home the previous
# release made keeps working under this commit's mcpp (design 2026-10-17 D9).
#
# Every other lane starts from an empty home, so the installed packaging
# revision always equals the index's and an upgrade is never executed. A user
# upgraded to 2026.10.16.3 and every `mcpp run` failed from then on; nothing
# in CI could see it. This lane builds the home with the PREVIOUS RELEASE --
# the newest published release that is not this commit's version, read from
# GitHub, so it moves forward with every release -- and then runs this
# commit's mcpp in it, laid out as a release (bin/mcpp, registry/bin/xlings).
#
# variant:
#   plain     the previous release installs from today's index.
#   snapshot  the previous release installs from the xim index of 2026-10-07
#             (xim-pkgindex ebf1fbb): payloads genuinely of older packaging
#             revisions (llvm revision 0, gcc 16.1.0 before -r1).
#   orphan    every payload recorded at another revision, and `c++` active at
#             gcc's version whose registration is gone (a reported home).
#
# Asserted, every variant: the first `mcpp new && mcpp run` of the new mcpp
# succeeds and downloads nothing; a second run likewise; `mcpp self doctor`
# in the project installs nothing and names the toolchain the build uses.
# snapshot: `mcpp index update` names the newer packagings, the run after it
# still downloads nothing, and `mcpp toolchain list` offers the new one.
# orphan: `mcpp toolchain install` replaces the payload, and the dangling
# record is reported as repaired instead of refusing the replacement.
set -uo pipefail
VARIANT="${1:?variant}"
NEW_MCPP="${2:?new mcpp binary}"
NEW_XLINGS="${3:?new xlings binary}"
SNAPSHOT=ebf1fbb3417ec513b923279044433887b3af8850
TC=llvm@23.1.3

unset MCPP_VENDORED_XLINGS MCPP MCPP_FRESH MCPP_BOOT XLINGS_HOME XLINGS_PROJECT_DIR XLINGS_ACTIVE_SUBOS
W="${RUNNER_TEMP:-/tmp}/upgrade-lane"
rm -rf "$W"; mkdir -p "$W"
export MCPP_HOME="$W/home"
REG="$MCPP_HOME/registry"
PY=$(command -v python3 || command -v python)
FAILED=0
fail() { echo "::error::upgrade lane ($VARIANT): $*"; FAILED=1; }

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)  PLAT=linux-x86_64;   EXT=tar.gz; EXE= ;;
  Linux-aarch64) PLAT=linux-aarch64;  EXT=tar.gz; EXE= ;;
  Darwin-arm64)  PLAT=macosx-arm64;   EXT=tar.gz; EXE= ;;
  MINGW*|MSYS*)  PLAT=windows-x86_64; EXT=zip;    EXE=.exe ;;
  *) echo "::error::unknown platform $(uname -a)"; exit 1 ;;
esac
IS_LINUX=0; [[ $PLAT == linux-* ]] && IS_LINUX=1

# ── the previous release ───────────────────────────────────────────────
CURRENT=$(sed -n 's/^version *= *"\([^"]*\)".*/\1/p' mcpp.toml | head -1)
OLD_VER=$(curl -fsSL ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} \
            "https://api.github.com/repos/mcpp-community/mcpp/releases?per_page=20" \
          | "$PY" -c '
import json, sys
current = sys.argv[1]
for r in json.load(sys.stdin):
    v = r["tag_name"].lstrip("v")
    if not r["draft"] and not r["prerelease"] and v != current:
        print(v); break
' "$CURRENT")
case "$OLD_VER" in ''|*[!0-9.]*) echo "::error::no previous release resolved (got '$OLD_VER')"; exit 1 ;; esac
echo "previous release: $OLD_VER; this commit: $CURRENT ($PLAT, variant $VARIANT)"

name="mcpp-$OLD_VER-$PLAT.$EXT"
bash .github/tools/fetch_release.sh \
  "https://github.com/mcpp-community/mcpp/releases/download/v$OLD_VER/$name" "$W/$name" || exit 1
mkdir -p "$W/x"
if [[ $EXT == zip ]]; then
  "$PY" -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$W/$name" "$W/x"
else
  tar xzf "$W/$name" -C "$W/x"
fi
mv "$W/x/mcpp-$OLD_VER-$PLAT" "$W/old"
OLD="$W/old/bin/mcpp$EXE"
mkdir -p "$W/new/bin" "$W/new/registry/bin"
cp "$NEW_MCPP" "$W/new/bin/mcpp$EXE"
cp "$NEW_XLINGS" "$W/new/registry/bin/xlings$EXE"
chmod +x "$OLD" "$W/old/registry/bin/xlings$EXE" "$W/new/bin/mcpp$EXE" "$W/new/registry/bin/xlings$EXE" 2>/dev/null
NEW="$W/new/bin/mcpp$EXE"
"$OLD" --version; "$NEW" --version

# run <label> <dir> <bin> args...: the log is kept; returns the exit code.
run() {
  local label=$1 dir=$2 bin=$3; shift 3
  echo "::group::$label: (cd $dir && mcpp $*)"
  (cd "$dir" && "$bin" "$@") > "$W/$label.log" 2>&1
  local rc=$?
  sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "$W/$label.log" | tr '\r' '\n' | grep -v '^\s*Planning ' | tail -60
  echo "--> rc=$rc"
  echo "::endgroup::"
  return $rc
}
downloads() { grep -o "Downloading [^ ]*" "$W/$1.log" | sort -u | tr '\n' ' '; }
new_project() {  # <dir> <bin>
  (cd "$W" && "$2" new "$1" > /dev/null 2>&1) || mkdir -p "$W/$1"
  printf '\n[toolchain]\ndefault = "%s"\n' "$TC" >> "$W/$1/mcpp.toml"
}

# ── the previous release builds the home ───────────────────────────────
run old-env "$W" "$OLD" self env || fail "the previous release does not start"
if [ "$VARIANT" = snapshot ]; then
  # self env is read-only since 2026.10.19.1. Establish the old home
  # explicitly before replacing its index; otherwise tar's empty -C argument
  # leaves the live index in use and this lane never installs old packaging.
  run old-init "$W" "$OLD" self init || { fail "the previous release cannot initialize its home"; exit 1; }
  git clone -q --filter=blob:none https://github.com/openxlings/xim-pkgindex "$W/xpi" \
    && git -C "$W/xpi" checkout -q "$SNAPSHOT" || { echo "::error::cannot fetch the snapshot index"; exit 1; }
  idx="$REG/data/xim-pkgindex"
  [ -d "$idx" ] || idx=$(find "$MCPP_HOME" -type d -name xim-pkgindex | head -1)
  [ -n "$idx" ] && [ -d "$idx/pkgs" ] \
    || { fail "the previous release created no index directory"; exit 1; }
  find "$idx" -mindepth 1 -maxdepth 1 ! -name .xlings-index-version -exec rm -rf {} +
  (cd "$W/xpi" && git archive HEAD) | tar x -C "$idx" \
    || { fail "the snapshot index was not materialized"; exit 1; }
  [ -f "$idx/pkgs/l/llvm.lua" ] \
    || { fail "the snapshot has no LLVM descriptor"; exit 1; }
  echo "llvm.lua revisions now: $(grep -o 'revision = [0-9]*' "$idx/pkgs/l/llvm.lua" | sort -u | tr '\n' ' ')"
fi
run old-tc "$W" "$OLD" toolchain install "$TC" || fail "the previous release cannot install $TC"
if [ "$VARIANT" = snapshot ]; then
  "$PY" - "$REG/data/xpkgs/xim-x-llvm/23.1.3/.xpkg-install.json" <<'PY' \
    || { fail "the snapshot did not install LLVM revision zero"; exit 1; }
import json, sys
with open(sys.argv[1]) as f:
    installed = json.load(f)
assert installed.get("revision", 0) == 0, installed
PY
fi
[ "$VARIANT" = orphan ] && { run old-tc-gcc "$W" "$OLD" toolchain install gcc@16.1.0 || fail "the previous release cannot install gcc"; }
# modules_project <dir>: `openkal.timeout` imports `openkal.datagram`, an
# INDIRECT import of the program's unit, found at the path its BMI records.
modules_project() {
  mkdir -p "$W/$1/src"
  cat > "$W/$1/mcpp.toml" <<'TOML'
[package]
name = "kp"
version = "0.1.0"

[dependencies]
openkal = "0.16.0"

[target.linux.dependencies]
openkal-linux = "0.17.0"

[toolchain]
default = "gcc@16.1.0"
TOML
  printf 'import openkal.timeout;\nint main() { return 0; }\n' > "$W/$1/src/main.cpp"
}
if [ "$VARIANT" = modules ]; then
  run old-tc-gcc "$W" "$OLD" toolchain install gcc@16.1.0 || fail "the previous release cannot install gcc"
  modules_project m-old
  run old-modules "$W/m-old" "$OLD" build || fail "the previous release cannot build the module project"
fi
new_project p-old "$OLD"
run old-run "$W/p-old" "$OLD" run || fail "the previous release cannot run a new project"

if [ "$VARIANT" = orphan ]; then
  "$PY" - "$REG" <<'PY' || fail "could not shape the orphan home"
import glob, json, os, sys
reg = sys.argv[1]
for f in glob.glob(os.path.join(reg, "data", "xpkgs", "xim-x-*", "*", ".xpkg-install.json")):
    d = json.load(open(f)); r = d.get("revision", 0)
    d["revision"] = 1 if r == 0 else r - 1
    json.dump(d, open(f, "w"), indent=2)
wsf = os.path.join(reg, "subos", "default", ".xlings.json")
wsdoc = json.load(open(wsf)); ws = wsdoc["workspace"]
dbf = os.path.join(reg, "data", "versions.json")
src = json.load(open(dbf))["versions"] if os.path.exists(dbf) else json.load(open(os.path.join(reg, ".xlings.json")))["versions"]
owners = {k: (v.get("bindingGroup") or {}).get("provider") for k, v in src.get("c++", {}).get("versions", {}).items()}
keys = [k for k, p in owners.items() if p and "gcc" in p]
if not keys: sys.exit("no gcc-owned c++ registration: %r" % owners)
key = keys[0]
entry = ws.get("c++")
if isinstance(entry, dict): entry["active"] = key
else: ws["c++"] = key
json.dump(wsdoc, open(wsf, "w"))
for path in (dbf, os.path.join(reg, ".xlings.json")):
    if not os.path.exists(path): continue
    doc = json.load(open(path)); c = doc.get("versions", {}).get("c++", {})
    c.get("versions", {}).pop(key, None)
    for edges in c.get("bindings", {}).values(): edges.pop(key, None)
    json.dump(doc, open(path, "w"))
print("c++ active at %s, its registration removed" % key)
PY
fi

# ── this commit's mcpp in that home ────────────────────────────────────
if [ "$VARIANT" = snapshot ]; then
  run new-run-on-old-index "$W/p-old" "$NEW" run || fail "the old project does not run on the old index"
  [ -z "$(downloads new-run-on-old-index)" ] || fail "the run on the old index downloaded: $(downloads new-run-on-old-index)"
  run new-index-update "$W" "$NEW" index update || fail "index update failed"
  grep -q "newer packagings" "$W/new-index-update.log" \
    || fail "index update does not name the newer packagings of installed payloads"
fi
new_project p-new "$NEW"
run new-run1 "$W/p-new" "$NEW" run || fail "the first run of the new mcpp failed"
[ -z "$(downloads new-run1)" ] || fail "the first run downloaded: $(downloads new-run1)"
# A payload replaced names its package (`Updating llvm@23.1.3  packaging
# revision 0 → 1`); the vendored xlings update is not one.
grep -Eq "^\s*(Updating|Reinstalling) [^ ]+@" "$W/new-run1.log" && fail "the first run replaced a payload"
run new-run2 "$W/p-new" "$NEW" run || fail "the second run failed"
[ -z "$(downloads new-run2)" ] || fail "the second run downloaded: $(downloads new-run2)"
run new-run-old-project "$W/p-old" "$NEW" run || fail "the project the previous release made does not run"
[ -z "$(downloads new-run-old-project)" ] || fail "the old project's run downloaded: $(downloads new-run-old-project)"

run new-doctor "$W/p-new" "$NEW" self doctor
[ -z "$(downloads new-doctor)" ] || fail "doctor downloaded: $(downloads new-doctor)"
grep -q "what a build here uses" "$W/new-doctor.log" || fail "doctor does not name the toolchain the build uses"

if [ "$VARIANT" = snapshot ]; then
  run new-toolchain-list "$W" "$NEW" toolchain list || fail "toolchain list failed"
  grep -q "packaging revision .* available" "$W/new-toolchain-list.log" \
    || fail "toolchain list does not offer the newer packaging"
fi
if [ "$VARIANT" = modules ]; then
  # A BMI of another spelling is not read: the dependency is rebuilt once (its
  # cache key carries the spelling), then served from the cache. The spelling
  # moved in 2026.10.26.2 (the module map's `$root`); a previous release from
  # then on spells as this one, and its entry is served at once.
  old_version=$("$OLD" --version | grep -Eo '[0-9]{4}\.[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  same_spelling=$(printf '%s\n2026.10.26.2\n' "$old_version" | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | head -1)
  modules_project m-new1
  run new-modules1 "$W/m-new1" "$NEW" build -v || fail "the module project does not build over the previous release's cache"
  if [ "$same_spelling" = 2026.10.26.2 ]; then
    grep -q "STAGE gcm.cache/mcpplibs.openkal/" "$W/new-modules1.log" \
      || fail "a dependency BMI of the same spelling ($old_version) was not served from the cache"
  else
    grep -q "STAGE gcm.cache/mcpplibs.openkal/" "$W/new-modules1.log" \
      && fail "a dependency BMI of the previous spelling ($old_version) was taken from the cache"
  fi
  modules_project m-new2
  run new-modules2 "$W/m-new2" "$NEW" build -v || fail "the second module project failed"
  grep -q "STAGE gcm.cache/mcpplibs.openkal/" "$W/new-modules2.log" \
    || fail "the rebuilt dependency was not served from the cache"
fi
if [ "$VARIANT" = orphan ]; then
  run new-provision "$W" "$NEW" toolchain install "$TC" || fail "toolchain install could not replace the payload"
  grep -q "installed asset registration missing" "$W/new-provision.log" \
    && fail "the replacement was refused by the dangling record"
  grep -qi "deactivated\|dangling" "$W/new-provision.log" \
    || fail "the repaired record is not reported"
  run new-run-after-provision "$W/p-new" "$NEW" run || fail "the run after the replacement failed"
fi

{
  echo "### upgrade lane: $PLAT / $VARIANT ($OLD_VER → $CURRENT)"
  echo
  for l in "$W"/*.log; do
    b=$(basename "$l" .log)
    echo "- \`$b\`: downloads [$(downloads "$b")]"
  done
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
mkdir -p "${GITHUB_WORKSPACE:-.}/upgrade-lane-logs"
cp "$W"/*.log "${GITHUB_WORKSPACE:-.}/upgrade-lane-logs/" 2>/dev/null || true
[ "$FAILED" = 0 ] && echo "PASS: upgrade lane ($VARIANT)"
exit "$FAILED"
