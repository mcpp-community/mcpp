#!/usr/bin/env bash
# Ecosystem verification for #655 against the PUBLISHED mcpp and index, run
# inside a SubOS sandbox with CN mirrors for xlings and mcpp.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-17-655-verify.sh)
#   E2E=$(base64 -w0 tests/e2e/736_compile_flag_words_reach_the_compiler_and_the_databases.sh)
#   xlings subos new v655
#   xlings subos use v655 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && echo $E2E | base64 -d > /tmp/e2e736.sh && MCPP_VERIFY_VERSION=2026.9.17.1 bash /tmp/v.sh"
#
# Run it against the previous release first (MCPP_VERIFY_VERSION=2026.9.16.2):
# the sections marked CHANGE must fail there and pass on the new release, and
# the sections marked GUARD must pass on both. A CHANGE section that passes on
# both releases measured nothing.
#
# The sandbox shares the xlings data directory, so the published mcpp is
# addressed by its store path and exact version. Its $HOME content persists
# between runs of one SubOS, so every section clears its own probe directory.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }
reading() { printf 'READING %s\n' "$*"; }
unset XLINGS_ACTIVE_SUBOS

root="$HOME/verify-655"
rm -rf "$root"; mkdir -p "$root"

# The first entry of compile_commands.json whose file matches $2 and whose
# arguments contain $3, executed with `-fsyntax-only` in place of `-c <src> -o
# <obj>`. Prints the exit status and the first error line.
exec_unit() {   # <compile_commands.json> <file substring> <argument substring>
    python3 - "$@" <<'PY'
import json, subprocess, sys
db, fsub, asub = sys.argv[1], sys.argv[2], sys.argv[3]
for e in json.load(open(db)):
    args = e["arguments"]
    if fsub in e["file"] and any(asub in a for a in args):
        i = args.index("-c")
        argv = args[:i] + ["-fsyntax-only", e["file"]]
        r = subprocess.run(argv, cwd=e["directory"], capture_output=True, text=True)
        err = next((l for l in r.stderr.splitlines() if "error" in l), "")
        print(f"rc={r.returncode} file={e['file'].split('/')[-1]} arg={[a for a in args if asub in a][0]!r} {err}")
        sys.exit(0)
print("rc=none no unit matched")
PY
}

section "A. GUARD: the published mcpp answers for itself, with CN mirrors for xlings and mcpp"
if [ ! -x "$STORE" ]; then
    fail "no mcpp at $STORE"; printf '\nfails=%d (nothing else can run)\n' "$fails"; exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
case "$got" in *"$VER"*) ok "mcpp --version says $got" ;; *) fail "mcpp --version says '$got', expected $VER" ;; esac
if command -v xlings >/dev/null 2>&1; then
    xlings config --mirror CN >/dev/null 2>&1 && ok "xlings config --mirror CN" || fail "xlings config --mirror CN"
fi
"$STORE" self config --mirror CN >/dev/null 2>&1 && ok "mcpp self config --mirror CN" || fail "mcpp self config --mirror CN"
grep -q '"mirror": *"CN"' "$HOME/.mcpp/registry/.xlings.json" 2>/dev/null \
    && ok "mcpp's xlings reads mirror CN" || fail "mcpp's xlings does not read mirror CN"

section "B. CHANGE: the issue's five macros read the same in the build and in the database"
d=$root/b; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name = "i655"
version = "0.1.0"

[build]
sources = ["src/*.c"]
cflags  = ["-DESC=\\\"esc.h\\\"", "-DMID=\"mid\"", "-DSQ='sq'"]
defines = ["DEF=\"def\"", "SPACE=\"a b\""]
EOF
printf 'int main(void){return 0;}\n' > "$d/src/main.c"
( cd "$d" && timeout 1800 "$STORE" build > build.log 2>&1 ) || fail "B: the build failed"
build_cmd=$(cd "$d"/target/*/*/ 2>/dev/null && ninja -t commands obj/main.o 2>/dev/null | tail -1)
if [ -z "$build_cmd" ]; then
    skip "B: ninja is not on PATH in the sandbox; the database leg runs alone"
fi
python3 - "$d/compile_commands.json" > "$d/db-macros.txt" <<'PY'
import json, subprocess, sys
e = json.load(open(sys.argv[1]))[0]
a = e["arguments"]; i = a.index("-c")
r = subprocess.run(a[:i] + ["-E", "-dM", "-x", "c", "/dev/null"], cwd=e["directory"], capture_output=True, text=True)
print(f"rc {r.returncode}")
for l in r.stdout.splitlines():
    w = l.split()
    if len(w) > 1 and w[1] in ("ESC", "MID", "SQ", "DEF", "SPACE"):
        print(l)
PY
reading "B database macros: $(tr '\n' ';' < "$d/db-macros.txt")"
grep -qx 'rc 0' "$d/db-macros.txt" || fail "B: the database's arguments do not execute"
for want in '#define ESC "esc.h"' '#define MID mid' '#define SQ sq' '#define DEF "def"' '#define SPACE "a b"'; do
    grep -qxF "$want" "$d/db-macros.txt" || fail "B: the database gives no '$want'"
done
if [ -n "$build_cmd" ]; then
    (cd "$d"/target/*/*/ && /bin/sh -c "$(printf '%s' "$build_cmd" | sed 's/ -MMD .*$/ -E -dM -x c \/dev\/null/')") \
        | grep -E 'define (ESC|MID|SQ|DEF|SPACE) ' | sort > "$d/build-macros.txt"
    grep -v '^rc' "$d/db-macros.txt" | sort | diff -q - "$d/build-macros.txt" >/dev/null \
        && ok "B: the build and the database define the same five macros" \
        || fail "B: the build and the database differ: $(tr '\n' ';' < "$d/build-macros.txt")"
fi

section "C. CHANGE: e2e 736 against the published binary"
if [ -f /tmp/e2e736.sh ]; then
    if MCPP="$STORE" timeout 1800 bash /tmp/e2e736.sh > "$root/e2e736.log" 2>&1; then
        ok "C: e2e 736 passes"
    else
        fail "C: e2e 736: $(grep -m1 FAIL "$root/e2e736.log")"
    fi
else
    skip "C: /tmp/e2e736.sh was not passed in"
fi

section "D. CHANGE and GUARD: an index consumer of compat.libarchive"
d=$root/d; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name = "arc"
version = "0.1.0"

[dependencies.compat]
libarchive = "3.8.7"
EOF
cat > "$d/src/main.cpp" <<'EOF'
#include <archive.h>
#include <cstdio>
int main() { std::printf("%s\n", archive_version_string()); return 0; }
EOF
( cd "$d" && timeout 3600 "$STORE" run > run.log 2>&1 ); rc=$?
reading "D run exit=$rc: $(grep -m1 -E 'libarchive [0-9]' "$d/run.log")"
[ "$rc" -eq 0 ] && grep -q 'libarchive 3.8.7' "$d/run.log" && ok "D: the consumer builds and runs" \
    || fail "D: the consumer did not run: $(tail -3 "$d/run.log" | tr '\n' ' ')"
grep -q 'reaches the compiler as' "$d/run.log" \
    && fail "D GUARD: a published descriptor produced a build/flag-words note" \
    || ok "D GUARD: no build/flag-words note for the published descriptors"
if [ -f "$d/compile_commands.json" ]; then
    grep -qF -- '"-DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\""' "$d/compile_commands.json" \
        && ok "D: compile_commands.json lists -DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\"" \
        || fail "D: compile_commands.json lists $(grep -o '"-DPLATFORM_CONFIG_H[^,]*' "$d/compile_commands.json" | head -1)"
    unit=$(exec_unit "$d/compile_commands.json" "libarchive/archive_entry.c" "PLATFORM_CONFIG_H")
    reading "D libarchive unit: $unit"
    case "$unit" in rc=0*) ok "D: a libarchive unit's arguments execute" ;; *) fail "D: a libarchive unit's arguments do not execute" ;; esac
else
    fail "D: no compile_commands.json"
fi

section "E. GUARD: an index consumer of compat.lua (a packed -include element)"
d=$root/e; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name = "luac"
version = "0.1.0"

[dependencies.compat]
lua = "5.4.7"
EOF
cat > "$d/src/main.cpp" <<'EOF'
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <cstdio>
int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_dostring(L, "return 6 * 7");
    std::printf("lua=%d\n", static_cast<int>(lua_tointeger(L, -1)));
    lua_close(L);
    return 0;
}
EOF
( cd "$d" && timeout 3600 "$STORE" run > run.log 2>&1 ); rc=$?
reading "E run exit=$rc: $(grep -m1 'lua=' "$d/run.log")"
grep -q 'lua=42' "$d/run.log" && ok "E: the lua consumer builds and runs" \
    || fail "E: the lua consumer did not run: $(tail -3 "$d/run.log" | tr '\n' ' ')"
grep -q 'reaches the compiler as' "$d/run.log" \
    && fail "E GUARD: a published descriptor produced a build/flag-words note" \
    || ok "E GUARD: no build/flag-words note"

section "F. CHANGE: the reporter's project, openxlings/xlings"
d=$root/f; rm -rf "$d"
if timeout 900 git clone -q --depth 1 https://gitcode.com/openxlings/xlings.git "$d" 2>/dev/null \
   || timeout 900 git clone -q --depth 1 https://github.com/openxlings/xlings.git "$d" 2>/dev/null; then
    reading "F xlings commit: $(git -C "$d" log -1 --format=%h)"
    ( cd "$d" && timeout 3600 "$STORE" emit build-database --format json > db.json 2> db.err ); rc=$?
    reading "F emit exit=$rc"
    python3 - "$d/db.json" > "$d/db-reading.txt" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
hits = set()
def walk(o):
    if isinstance(o, dict):
        for v in o.values(): walk(v)
    elif isinstance(o, list):
        for v in o: walk(v)
    elif isinstance(o, str) and "PLATFORM_CONFIG_H" in o:
        hits.add(o)
walk(doc)
for h in sorted(hits): print(repr(h))
PY
    reading "F build database PLATFORM_CONFIG_H: $(tr '\n' ' ' < "$d/db-reading.txt")"
    grep -qxF "'-DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\"'" "$d/db-reading.txt" \
        && ok "F: the build database lists the define without shell escapes" \
        || fail "F: the build database lists $(cat "$d/db-reading.txt")"
    # The unit is taken from the build database itself: its `arguments` and
    # `work-directory`, with `-fsyntax-only` in place of `-c <src> -o <obj>`.
    unit=$(python3 - "$d/db.json" <<'PY2'
import json, subprocess, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
for s in db["sets"]:
    for u in s.get("translation-units", []):
        args = u.get("arguments", [])
        if u.get("source", "").endswith("libarchive/archive_read.c") and any("PLATFORM_CONFIG_H" in a for a in args):
            i = args.index("-c")
            r = subprocess.run(args[:i] + ["-fsyntax-only", u["source"]], cwd=u.get("work-directory", "."),
                               capture_output=True, text=True)
            err = next((l for l in r.stderr.splitlines() if "error" in l), "")
            print(f"rc={r.returncode} set={s.get('name')} {err}")
            sys.exit(0)
print("rc=none no unit matched")
PY2
)
    reading "F libarchive unit from the build database: $unit"
    case "$unit" in rc=0*) ok "F: a libarchive unit of xlings executes from the build database" ;; *) fail "F: a libarchive unit of xlings does not execute from the build database" ;; esac
else
    skip "F: could not clone openxlings/xlings"
fi

printf '\n== summary ==\nversion=%s fails=%d\n' "$VER" "$fails"
[ -n "$skipped" ] && printf 'not run:%s\n' "$skipped"
exit 0
