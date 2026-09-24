#!/usr/bin/env bash
# Sandbox verification of mcpp 2026.9.25.1 (#690), run against the PUBLISHED
# release inside an xlings sandbox:
#
#   B64=$(base64 -w0 .agents/docs/2026-09-25-issue-690-verify.sh)
#   xlings subos new v690 2>/dev/null || true
#   xlings subos use v690 --sandbox --cmd "echo $B64 | base64 -d > /tmp/v.sh && VER=2026.9.25.1 bash /tmp/v.sh"
#
# VER selects the release under test. Running it with VER=2026.9.24.1 is the
# control: every section marked CHANGE must fail there, and every other section
# must pass on both.
#
# Every probe directory is removed at the start of its section, because the
# sandbox's $HOME persists between runs of the same subos. A section that does
# not run is reported as SKIP and counted separately from a pass.
set -u
VER="${VER:-2026.9.25.1}"
W="$HOME/v690"
fails=0; passes=0; skips=0
pass() { echo "PASS  $1"; passes=$((passes+1)); }
fail() { echo "FAIL  $1"; [ -n "${2:-}" ] && [ -f "$2" ] && tail -15 "$2"; fails=$((fails+1)); }
skip() { echo "SKIP  $1"; skips=$((skips+1)); }

# ── 0. the release under test, from the published channel ──────────────────
# MCPP_OVERRIDE runs the script against a local binary, for debugging the
# script itself; the verification proper always installs from the index.
if [ -n "${MCPP_OVERRIDE:-}" ]; then
    MCPP="$MCPP_OVERRIDE"
else
    xlings config --mirror CN >/dev/null 2>&1 || true
    xlings update >/dev/null 2>&1 || true
    xlings install "mcpp@$VER" -y > /tmp/v690-install.log 2>&1 || true
    MCPP="$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp"
fi
if [ ! -x "$MCPP" ]; then
    echo "FATAL: mcpp $VER is not installable from the index"; tail -20 /tmp/v690-install.log; exit 2
fi
got=$("$MCPP" --version 2>&1 | head -1)
case "$got" in *"$VER"*) pass "0 installed: $got";; *) fail "0 version: $got";; esac
"$MCPP" self config --mirror CN >/dev/null 2>&1 || true

count() {  # count <cdb> <file-suffix> <word>
    python3 - "$1" "$2" "$3" <<'EOF'
import json, sys
cdb, suffix, word = sys.argv[1:4]
for e in json.load(open(cdb)):
    if e["file"].replace("\\", "/").endswith(suffix):
        print(e["arguments"].count(word)); sys.exit(0)
print("missing")
EOF
}

# ── 1. CHANGE: #690 as reported: workspace defines reach a sibling ─────────
rm -rf "$W/s1"; mkdir -p "$W/s1/lib" "$W/s1/app/src"; cd "$W/s1"
printf '[workspace]\nmembers = ["lib", "app"]\n\n[workspace.package]\nversion = "0.1.0"\n\n[workspace.build]\ndefines = ["WORKSPACE_DEFINE=1"]\n' > mcpp.toml
printf '[package]\nnamespace = "probe"\nname = "lib"\n\n[targets.probe_lib]\nkind = "lib"\n\n[build]\nsources = ["lib.cpp"]\n' > lib/mcpp.toml
printf '#ifndef WORKSPACE_DEFINE\n#error WORKSPACE_DEFINE missing in library dependency\n#endif\nint probe_lib() { return WORKSPACE_DEFINE; }\n' > lib/lib.cpp
printf '[package]\nnamespace = "probe"\nname = "app"\n\n[dependencies]\n"probe.lib" = { path = "../lib" }\n\n[targets.probe_app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
printf 'int probe_lib();\nint main() { return probe_lib() != 1; }\n' > app/src/main.cpp
if "$MCPP" build -p app > s1.log 2>&1 && [ "$(count app/compile_commands.json lib/lib.cpp -DWORKSPACE_DEFINE=1)" = 1 ]; then
    pass "1 CHANGE: the sibling receives the workspace define exactly once"
else fail "1 CHANGE: sibling position" s1.log; fi
if "$MCPP" build -p lib > s1r.log 2>&1 && [ "$(count lib/compile_commands.json lib/lib.cpp -DWORKSPACE_DEFINE=1)" = 1 ]; then
    pass "1 the root position receives it exactly once"
else fail "1 root position" s1r.log; fi

# ── 2. CHANGE: keyed defines and a sibling's workspace dependency ──────────
rm -rf "$W/s2"; mkdir -p "$W/s2/lib" "$W/s2/app/src" "$W/s2/util"; cd "$W/s2"
printf '[workspace]\nmembers = ["lib", "app", "util"]\n\n[workspace.package]\nversion = "0.1.0"\n\n[workspace.dependencies]\nutil = { path = "util" }\n\n[workspace.build]\ndefines = ["LEVEL=1", "TRACE"]\n' > mcpp.toml
printf '[package]\nname = "util"\n\n[targets.util]\nkind = "lib"\n\n[build]\nsources = ["u.cpp"]\n' > util/mcpp.toml
echo 'int util_v() { return 5; }' > util/u.cpp
printf '[package]\nname = "lib"\n\n[dependencies]\nutil.workspace = true\n\n[targets.lib]\nkind = "lib"\n\n[build]\nsources = ["l.cpp"]\ndefines = ["LEVEL=2", "!TRACE"]\n' > lib/mcpp.toml
printf '#if LEVEL != 2\n#error LEVEL\n#endif\n#ifdef TRACE\n#error TRACE\n#endif\nint util_v();\nint lib_v() { return util_v() + LEVEL; }\n' > lib/l.cpp
printf '[package]\nname = "app"\n\n[dependencies]\nlib = { path = "../lib" }\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
printf 'int lib_v();\nint main() { return lib_v() == 7 ? 0 : 1; }\n' > app/src/main.cpp
if "$MCPP" run -p app > s2.log 2>&1 && [ "$(count app/compile_commands.json lib/l.cpp -DLEVEL=1)" = 0 ]; then
    pass "2 CHANGE: LEVEL replaced, TRACE removed, util.workspace resolved for a sibling"
else fail "2 CHANGE: keyed defines / sibling workspace dependency" s2.log; fi

# ── 3. CHANGE: a git-hosted member receives its repository's [workspace.build]
rm -rf "$W/s3"; mkdir -p "$W/s3/repo/glib" "$W/s3/consumer/src"; cd "$W/s3/repo"
printf '[workspace]\nmembers = ["glib"]\n\n[workspace.package]\nversion = "0.2.0"\n\n[workspace.build]\ncxxflags = ["-DREPO_FLAG=1"]\n' > mcpp.toml
printf '[package]\nnamespace = "v690"\nname = "glib"\n\n[targets.glib]\nkind = "lib"\n\n[build]\nsources = ["g.cpp"]\n' > glib/mcpp.toml
printf '#ifndef REPO_FLAG\n#error REPO_FLAG\n#endif\nint g_v() { return REPO_FLAG; }\n' > glib/g.cpp
if command -v git >/dev/null 2>&1; then
    git init -q -b main . && git add -A && git -c user.email=v@v -c user.name=v commit -qm init
    cd "$W/s3/consumer"
    printf '[package]\nname = "consumer"\nversion = "0.1.0"\n\n[dependencies]\n"v690.glib" = { git = "file://%s/s3/repo", branch = "main" }\n\n[targets.consumer]\nkind = "bin"\nmain = "src/main.cpp"\n' "$W" > mcpp.toml
    printf 'int g_v();\nint main() { return g_v() == 1 ? 0 : 1; }\n' > src/main.cpp
    if "$MCPP" run > s3.log 2>&1; then pass "3 CHANGE: git-consumed member compiled with REPO_FLAG"
    else fail "3 CHANGE: git-hosted member" s3.log; fi
else skip "3 git is not available in the sandbox"; fi

# ── 4. CHANGE: a consumer's private header does not reach a dependency ─────
rm -rf "$W/s4"; mkdir -p "$W/s4/app/src" "$W/s4/app/priv" "$W/s4/dep/inc"; cd "$W/s4"
printf '#error ROOT PRIVATE HEADER REACHED A DEPENDENCY\n' > app/priv/limits.h
echo '#define DEP_HDR 3' > dep/inc/dep.h
printf '[package]\nname = "dep"\nversion = "0.1.0"\n\n[targets.dep]\nkind = "lib"\n\n[build]\ninclude_dirs = ["inc"]\nsources = ["d.c"]\n' > dep/mcpp.toml
printf '#include <limits.h>\n#include <dep.h>\nint dep_v(void) { return DEP_HDR + (INT_MAX > 0); }\n' > dep/d.c
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\ndep = { path = "../dep" }\n\n[build]\ninclude_dirs = ["priv"]\nprivate_include_dirs = ["priv"]\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
printf 'extern "C" int dep_v(void);\nint main() { return dep_v() == 4 ? 0 : 1; }\n' > app/src/main.cpp
cd app
if "$MCPP" run > s4.log 2>&1; then pass "4 CHANGE: the dependency compiled against its own headers"
else fail "4 CHANGE: root private header reached the dependency" s4.log; fi

# ── 5. CHANGE: the index dependency cache is not poisoned across projects ──
rm -rf "$W/s5"; mkdir -p "$W/s5/a/src" "$W/s5/a/priv" "$W/s5/b/src"; cd "$W/s5"
printf '#include_next <float.h>\n#undef DBL_EPSILON\n#define DBL_EPSILON 0.5\n' > a/priv/float.h
main_src='#include <cJSON.h>\n#include <cstdio>\nint main() {\n  cJSON* x = cJSON_CreateNumber(1.0); cJSON* y = cJSON_CreateNumber(1.2);\n  std::printf("compare=%%d\\n", (int)cJSON_Compare(x, y, 1));\n  cJSON_Delete(x); cJSON_Delete(y);\n}\n'
printf "$main_src" > a/src/main.cpp; printf "$main_src" > b/src/main.cpp
printf '[package]\nname = "a"\nversion = "0.1.0"\n\n[dependencies.compat]\ncjson = "1.7.19"\n\n[build]\ninclude_dirs = ["priv"]\nprivate_include_dirs = ["priv"]\n\n[targets.a]\nkind = "bin"\nmain = "src/main.cpp"\n' > a/mcpp.toml
printf '[package]\nname = "b"\nversion = "0.1.0"\n\n[dependencies.compat]\ncjson = "1.7.19"\n\n[targets.b]\nkind = "bin"\nmain = "src/main.cpp"\n' > b/mcpp.toml
# 5a compiles cJSON unconditionally (--cache off), so the reading does not
# depend on what the cache already holds. 5b then lets A populate the cache and
# reads B: in a fresh sandbox home the cache is empty, so A compiles cJSON and
# B is served A's object.
( cd a && "$MCPP" build --cache off > ../s5a0.log 2>&1 && "$MCPP" run --cache off > ../s5a.log 2>&1 )
if grep -q "compare=0" s5a.log; then
    pass "5a CHANGE: cJSON compiled against its own float.h, not the consumer's"
else fail "5a CHANGE: consumer header reached cJSON ($(grep -o 'compare=[0-9]' s5a.log))" s5a0.log; fi
( cd a && "$MCPP" run > ../s5ac.log 2>&1 ); ( cd b && "$MCPP" run > ../s5b.log 2>&1 )
if grep -q "compare=0" s5b.log; then
    pass "5b CHANGE: an unrelated project is not served a poisoned object"
else fail "5b CHANGE: B=$(grep -o 'compare=[0-9]' s5b.log)" s5b.log; fi

# ── 6. CHANGE: the published form of a workspace member is self-contained ──
rm -rf "$W/s6"; mkdir -p "$W/s6/lib" "$W/s6/util"; cd "$W/s6"
if command -v git >/dev/null 2>&1; then
    printf '[workspace]\nmembers = ["lib", "util"]\n\n[workspace.package]\nversion = "0.3.0"\nlicense = "MIT"\nrepo = "https://example.invalid/v690"\n\n[workspace.build]\ncxxflags = ["-DWS_FLAG=1"]\n' > mcpp.toml
    printf '[package]\nnamespace = "v690"\nname = "util"\n\n[targets.util]\nkind = "lib"\n\n[build]\nsources = ["u.cpp"]\n' > util/mcpp.toml
    echo 'int util_v() { return 2; }' > util/u.cpp
    printf '[package]\nnamespace = "v690"\nname = "lib"\n\n[dependencies]\n"v690.util" = { path = "../util", version = "0.3.0" }\n\n[targets.lib]\nkind = "lib"\n\n[build]\nsources = ["lib.cpp"]\n' > lib/mcpp.toml
    printf '#ifndef WS_FLAG\n#error WS_FLAG\n#endif\nint lib_v() { return WS_FLAG; }\n' > lib/lib.cpp
    git init -q -b main . && git add -A && git -c user.email=v@v -c user.name=v commit -qm init
    ( cd lib && "$MCPP" publish --dry-run --allow-dirty > ../s6.log 2>&1 )
    tarball=$(ls lib/target/dist/*.tar.gz 2>/dev/null | head -1)
    if [ -n "$tarball" ]; then
        toml=$(tar xzf "$tarball" -O "lib-0.3.0/mcpp.toml" 2>/dev/null)
        if echo "$toml" | grep -q 'WS_FLAG' && echo "$toml" | grep -q '0.3.0' \
            && ! echo "$toml" | grep -q 'path' \
            && tar tzf "$tarball" | grep -q 'mcpp.toml.orig'; then
            pass "6 CHANGE: the archive carries the inherited values and a version edge"
        else fail "6 CHANGE: archived manifest is not normalised" s6.log; echo "$toml"; fi
    else fail "6 CHANGE: publish --dry-run produced no archive" s6.log; fi
else skip "6 git is not available in the sandbox"; fi

# ── 7. index packages still build (unchanged behaviour) ────────────────────
for pkg in zlib fmt; do
    rm -rf "$W/s7-$pkg"; mkdir -p "$W/s7-$pkg/src"; cd "$W/s7-$pkg"
    case $pkg in
        zlib) dep='zlib = "1.3.2"'; src='#include <zlib.h>\nint main() { return zlibVersion()[0] == 49 ? 0 : 1; }\n';;
        fmt)  dep='fmt = "12.2.0"'; src='import std;\nimport fmt;\nint main() { return fmt::format("{}", 42) == std::string("42") ? 0 : 1; }\n';;
    esac
    table=compat; [ "$pkg" = fmt ] && table=fmtlib
    printf '[package]\nname = "s7%s"\nversion = "0.1.0"\n\n[dependencies.%s]\n%s\n\n[targets.s7%s]\nkind = "bin"\nmain = "src/main.cpp"\n' "$pkg" "$table" "$dep" "$pkg" > mcpp.toml
    printf "$src" > src/main.cpp
    if "$MCPP" run > s7.log 2>&1; then pass "7 $table.$pkg builds and runs"
    else fail "7 $table.$pkg" s7.log; fi
done

echo
echo "summary: mcpp $VER  passes=$passes fails=$fails skips=$skips"
[ "$fails" -eq 0 ]
