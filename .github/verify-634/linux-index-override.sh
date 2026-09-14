#!/usr/bin/env bash
# M7: can an `xim:` address resolve from a local checkout of an unmerged
# xim-pkgindex branch, through mcpp's `config.toml [index.repos.xim]`?
#   A  fresh home, the override written before the first command;
#   B  fresh home, no override (the negative);
#   C  an existing home, the override written after its first build.
set +e
set -u
. "$(dirname "$0")/common.sh"

mkdir -p "$RUNNER_TEMP/m7"
cd "$RUNNER_TEMP/m7" || exit 1
R="$RUNNER_TEMP/xim-pr"
rm -rf "$R"
git clone --depth 1 https://github.com/openxlings/xim-pkgindex "$R" > clone.txt 2>&1
reading m7.clone "exit=$? rev=$(git -C "$R" rev-parse --short HEAD)"
mkdir -p "$R/pkgs/m"
cat > "$R/pkgs/m/m634-probe.lua" <<'L'
package = {
    spec = "1",
    name = "m634-probe",
    description = "mcpp#634 measurement: a recipe that exists only on this branch",
    authors = {"mcpp-community"},
    licenses = {"Apache-2.0"},
    repo = "https://github.com/mcpp-community/mcpp",
    type = "script",
    status = "stable",
    categories = {"test"},
    keywords = {"test"},
    xpm = {
        linux = { ["0.0.1"] = { } },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.mkdir(bindir)
    local program = path.join(bindir, "m634-probe")
    local f = io.open(program, "w")
    f:write("#!/bin/sh\necho m634-probe\n")
    f:close()
    os.iorun('chmod +x "' .. program .. '"')
    return true
end

function config()
    xvm.add("m634-probe", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
L
git -C "$R" -c user.name=m634 -c user.email=m634@example.invalid add pkgs/m/m634-probe.lua
git -C "$R" -c user.name=m634 -c user.email=m634@example.invalid commit -q -m "m634 probe recipe"
reading m7.branch "rev=$(git -C "$R" rev-parse --short HEAD)"

make_consumer() {  # make_consumer <dir> <with-payload 0|1>
    rm -rf "$1"; mkdir -p "$1/src"
    printf '[package]\nname = "consumer"\nversion = "0.1.0"\n' > "$1/mcpp.toml"
    if [ "$2" = 1 ]; then
        printf '\n[xlings.workspace]\n"xim:m634-probe" = "0.0.1"\n' >> "$1/mcpp.toml"
    fi
    printf 'int main() { return 0; }\n' > "$1/src/main.cpp"
}
state() {  # state <label>
    local reg="$MCPP_HOME/registry"
    reading "m7.$1.payload" "$(ls -d "$reg/data/xpkgs/xim-x-m634-probe"/* 2>/dev/null | tr '\n' ' ')"
    reading "m7.$1.index-dir" "$(ls -ld "$reg/data/xim-pkgindex" 2>/dev/null | awk '{print $1, $NF}')"
    reading "m7.$1.xlings-json" "$(python3 -c "
import json,sys
try:
    d=json.load(open('$reg/.xlings.json'))
    print([ (r.get('name'), r.get('url')) for r in d.get('index_repos', []) ])
except Exception as e:
    print('unreadable', e)")"
}

# ── A: fresh home, override first ──────────────────────────────────────────
export MCPP_HOME="$RUNNER_TEMP/home-m7a"
rm -rf "$MCPP_HOME"; mkdir -p "$MCPP_HOME"
printf '[index.repos.xim]\nurl = "%s"\n' "$R" > "$MCPP_HOME/config.toml"
"$MCPP" self config --mirror GLOBAL > selfcfg.txt 2>&1
reading m7.A.config "$(one_line "$MCPP_HOME/config.toml")"
make_consumer "$RUNNER_TEMP/m7/a" 1
(cd "$RUNNER_TEMP/m7/a" && "$MCPP" build > build1.log 2>&1); rc=$?
reading m7.A.build "exit=$rc $(grep -m2 -iE 'error|m634' "$RUNNER_TEMP/m7/a/build1.log" | tr '\n' '|')"
state A
"$MCPP" index update > "$RUNNER_TEMP/m7/a/update.log" 2>&1
reading m7.A.index-update "exit=$? $(tail -2 "$RUNNER_TEMP/m7/a/update.log" | tr '\n' '|')"
rm -rf "$MCPP_HOME/registry/data/xpkgs/xim-x-m634-probe"
(cd "$RUNNER_TEMP/m7/a" && touch src/main.cpp && "$MCPP" build > build2.log 2>&1); rc=$?
reading m7.A.build-after-update "exit=$rc $(grep -m2 -iE 'error|m634' "$RUNNER_TEMP/m7/a/build2.log" | tr '\n' '|')"
state A-after

# ── B: fresh home, no override ─────────────────────────────────────────────
export MCPP_HOME="$RUNNER_TEMP/home-m7b"
rm -rf "$MCPP_HOME"; mkdir -p "$MCPP_HOME"
"$MCPP" self config --mirror GLOBAL > /dev/null 2>&1
make_consumer "$RUNNER_TEMP/m7/b" 1
(cd "$RUNNER_TEMP/m7/b" && "$MCPP" build > build.log 2>&1); rc=$?
reading m7.B.build "exit=$rc $(grep -m3 -iE 'error|m634' "$RUNNER_TEMP/m7/b/build.log" | tr '\n' '|')"

# ── C: existing home, override added afterwards ────────────────────────────
export MCPP_HOME="$RUNNER_TEMP/home-m7c"
rm -rf "$MCPP_HOME"; mkdir -p "$MCPP_HOME"
"$MCPP" self config --mirror GLOBAL > /dev/null 2>&1
make_consumer "$RUNNER_TEMP/m7/c0" 0
(cd "$RUNNER_TEMP/m7/c0" && "$MCPP" build > build.log 2>&1)
reading m7.C.bootstrap "exit=$?"
printf '\n[index.repos.xim]\nurl = "%s"\n' "$R" >> "$MCPP_HOME/config.toml"
make_consumer "$RUNNER_TEMP/m7/c" 1
(cd "$RUNNER_TEMP/m7/c" && "$MCPP" build > build.log 2>&1); rc=$?
reading m7.C.build "exit=$rc $(grep -m3 -iE 'error|m634' "$RUNNER_TEMP/m7/c/build.log" | tr '\n' '|')"
state C
exit 0
