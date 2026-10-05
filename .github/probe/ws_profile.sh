#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): where [profile.*], [workspace.build] and a
# rooted workspace's own [build] reach, per member and per selection.
# Prints the markers each ninja edge carries; asserts nothing.
set -u
M="${MCPP_BUILT:?}"
W="$RUNNER_TEMP/ws-profile"; rm -rf "$W"; mkdir -p "$W"
section() { echo; echo "### $1"; }
markers() {   # every compile/link edge and the markers it (or the file-level default) carries
  local found=0
  for n in $(find target -name build.ninja | sort); do found=1
  echo "  [graph $(basename "$(dirname "$n")")]"
  python3 - "$n" <<'PY'
import re, sys
glob, edges, cur = {}, {}, None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    if line.startswith("build "):
        cur = line.split(" : ")[0][6:].strip()
        rule = line.split(" : ", 1)[1].split()[0] if " : " in line else "?"
        edges[cur] = {"_rule": rule}; continue
    if line.startswith("  ") and cur:
        k, _, v = line.strip().partition(" = "); edges[cur][k] = v; continue
    cur = None
    m = re.match(r"^(\w+)\s*=\s*(.*)$", line)
    if m: glob[m.group(1)] = m.group(2)
pat = re.compile(r"MARK_[A-Z_]+|-O[0-3s]\b")
for e, v in sorted(edges.items()):
    r = v["_rule"]
    if r in ("cxx_object", "cxx_module", "c_object"):
        text = " ".join([glob.get("cxxflags", ""), v.get("unit_cxxflags", ""), v.get("cxxflags", ""), v.get("package_cxxflags", "")] + [x for k, x in v.items() if "flags" in k])
    elif r in ("cxx_link", "cxx_shared", "c_link"):
        text = v.get("ldflags", glob.get("ldflags", "")) + " " + v.get("unit_ldflags", "")
    else:
        continue
    found = sorted(set(pat.findall(text)))
    print(f"    {r:11s} {e}: {' '.join(found) or '-'}")
PY
  done
  [ $found = 1 ] || echo "(no build.ninja)"
}
build() { echo "\$ mcpp $*"; rm -rf target; "$M" "$@" 2>&1 | grep -E "warning|error|note|Finished" | head -8; markers; }

prof() {  # a [profile.release] table whose markers name its file
  cat <<EOF
[profile.release]
opt = 3
cxxflags = ["-DMARK_PROFILE_$1"]
ldflags = ["-Wl,-rpath,/MARK_PROFILE_$1"]
EOF
}
member() {  # member <dir> <name> <kind> [extra toml]
  mkdir -p "$1/src"
  { printf '[package]\nname = "%s"\nversion = "0.1.0"\n\n' "$2"
    printf '[build]\ncxxflags = ["-DMARK_OWN_%s"]\nldflags = ["-Wl,-rpath,/MARK_OWN_%s"]\n\n' "${2^^}" "${2^^}"
    [ "$3" = shared ] && printf '[targets.%s]\nkind = "shared"\n' "$2"
    [ "$3" = bin ] && printf '[dependencies]\nlib = { path = "%s" }\n\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' "${DEP_PATH:-../lib}" "$2"
    printf '%s\n' "${4:-}"; } > "$1/mcpp.toml"
  if [ "$3" = shared ]; then printf 'export module ws_lib;\nexport int lv() { return 1; }\n' > "$1/src/lib.cppm"
  else printf 'import ws_lib;\nint main() { return lv() == 1 ? 0 : 1; }\n' > "$1/src/main.cpp"; fi
}

section "W1 virtual root: [profile.release] + [workspace.build] at the root; members declare no profile"
mkdir -p "$W/w1" && cd "$W/w1"
{ printf '[workspace]\nmembers = ["app", "lib"]\n\n[workspace.build]\ncxxflags = ["-DMARK_WS_BUILD"]\nldflags = ["-Wl,-rpath,/MARK_WS_BUILD"]\n\n'; prof WSROOT; } > mcpp.toml
member lib lib shared; member app app bin
build build --release
build build --release -p app

section "W2 virtual root with a profile, and member app declares its own [profile.release]"
mkdir -p "$W/w2" && cd "$W/w2"
{ printf '[workspace]\nmembers = ["app", "lib"]\n\n'; prof WSROOT; } > mcpp.toml
member lib lib shared; member app app bin "$(prof APP)"
build build --release
build build --release -p app
build build --release -p lib

section "W3 rooted workspace: the root file is [package] app + [workspace] + [profile.release] + its own [build]"
mkdir -p "$W/w3" && cd "$W/w3"
DEP_PATH=lib member . app bin "$(printf '[workspace]\nmembers = ["lib"]\n\n[workspace.build]\ncxxflags = ["-DMARK_WS_BUILD"]\n\n'; prof ROOTFILE)"
member lib lib shared
build build --release
build build --release --workspace
build build --release -p lib

section "W4 a plain project (no workspace), for comparison"
mkdir -p "$W/w4" && cd "$W/w4"
member lib lib shared
mkdir -p app && member app app bin "$(prof APP)"
cd app && build build --release
