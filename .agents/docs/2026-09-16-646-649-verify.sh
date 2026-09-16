#!/usr/bin/env bash
# Ecosystem verification for #646 to #649 against the PUBLISHED mcpp, plugins and
# index, run inside a SubOS sandbox with CN mirrors for xlings and mcpp.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-16-646-649-verify.sh)
#   xlings subos new v646
#   xlings subos use v646 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.16.1 bash /tmp/v.sh"
#
# The script is run once against the previous release first
# (MCPP_VERIFY_VERSION=2026.9.15.2): the sections marked CHANGE must fail there
# and pass on the new release; the sections marked GUARD must pass on both. A
# script that passes on both releases in a CHANGE section measured nothing.
#
# The sandbox shares the xlings data directory, so a published mcpp is addressed
# by its store path and exact version. Its $HOME content persists between runs
# of one SubOS, so every section clears its own probe directory first. A section
# that cannot run says so and is listed again in the summary.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
PLUGINS="${MCPP_VERIFY_PLUGINS:-0.12.0}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }
has_text() { grep -qF -- "$2" "$1"; }
reading() { printf 'READING %s\n' "$*"; }
unset XLINGS_ACTIVE_SUBOS

# A BUILD PROGRAM MUST BE NEW EVERY RUN. The sandbox's $HOME persists between
# runs, and a build program is cached by its source; a cache hit does not re-run
# it, and mcpp replays neither its output nor its warnings. A probe that reads a
# build program would then measure the previous run. Every build program this
# script writes carries this token, so each run compiles and runs its own.
RUNID="$(date +%s)-$$"

root="$HOME/verify-646"
rm -rf "$root"; mkdir -p "$root"

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

section "B. CHANGE (#648 A6): the mcpplibs index artifact has a CN base, and an index update succeeds"
if grep -q 'gitcode.com/xlings-res/mcpp-index' "$HOME/.mcpp/registry/.xlings.json" 2>/dev/null; then
    ok "the mcpplibs artifact names the GitCode mirror"
else
    fail "the mcpplibs artifact has no CN base"; grep -n artifact "$HOME/.mcpp/registry/.xlings.json" | head -3
fi
if timeout 600 "$STORE" index update > "$root/index-update.log" 2>&1; then
    ok "mcpp index update"
else
    fail "mcpp index update failed"; tail -5 "$root/index-update.log"
fi
reading "A6 mcpplibs index version: $(cat "$HOME/.mcpp/registry/data/mcpplibs/.xlings-index-version" 2>/dev/null || echo none)"

section "C. CHANGE (#646 F3a): an llvm program over a C++ shared library runs"
d=$root/c; rm -rf "$d"; mkdir -p "$d/app/src" "$d/lib/src"
printf '[package]\nname = "lib"\nversion = "0.1.0"\n[targets.lib]\nkind = "shared"\n' > "$d/lib/mcpp.toml"
printf 'export module lib;\nimport std;\nexport [[gnu::visibility("default")]] std::string lib_greet(int n);\n' > "$d/lib/src/lib.cppm"
printf 'module lib;\nimport std;\nstd::string lib_greet(int n) { return std::format("lib-{}", n); }\n' > "$d/lib/src/lib.cpp"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[toolchain]\ndefault = "llvm@22.1.8"\n[dependencies.lib]\npath = "../lib"\n' > "$d/app/mcpp.toml"
printf 'import std;\nimport lib;\nint main() { std::println("{}", lib_greet(3)); }\n' > "$d/app/src/main.cpp"
( cd "$d/app" && timeout 1800 "$STORE" run > run.log 2>&1 ); rc=$?
reading "F3a run exit=$rc: $(grep -m1 -E 'lib-3|bad_cast' "$d/app/run.log")"
if [ "$rc" -eq 0 ] && has_text "$d/app/run.log" "lib-3"; then
    ok "the program runs on one C++ runtime"
else
    fail "the llvm program over a C++ shared library did not run"; tail -5 "$d/app/run.log"
fi

section "D. CHANGE (#646 F1): a static package under a shared package is linked into it"
d=$root/d; rm -rf "$d"; mkdir -p "$d/x/src" "$d/fw/src" "$d/app/src"
printf '[package]\nname = "x"\nversion = "0.1.0"\n[targets.x]\nkind = "lib"\n' > "$d/x/mcpp.toml"
printf 'int x_answer(void) { return 42; }\n' > "$d/x/src/x.c"
printf '[package]\nname = "fw"\nversion = "0.1.0"\n[targets.fw]\nkind = "shared"\n[dependencies.x]\npath = "../x"\n' > "$d/fw/mcpp.toml"
printf 'int x_answer(void);\n__attribute__((visibility("default"))) int fw_answer(void) { return x_answer(); }\n' > "$d/fw/src/fw.c"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.fw]\npath = "../fw"\n' > "$d/app/mcpp.toml"
printf 'extern "C" int fw_answer(void);\nint main() { return fw_answer() == 42 ? 0 : 1; }\n' > "$d/app/src/main.cpp"
( cd "$d/app" && timeout 1200 "$STORE" build > build.log 2>&1 ) || true
lib=$(ls "$d"/app/target/*/*/bin/libfw.so 2>/dev/null | head -1)
if [ -z "$lib" ]; then
    fail "libfw.so was not built"; tail -5 "$d/app/build.log"
elif command -v python3 >/dev/null 2>&1; then
    if python3 -c 'import ctypes,sys; ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_GLOBAL|2)' "$lib" 2> "$d/dlopen.err"; then
        ok "libfw.so loads on its own (RTLD_NOW): x is inside it"
    else
        fail "libfw.so does not load on its own: $(cat "$d/dlopen.err" | tail -1)"
    fi
else
    skip "D: no python3 in the sandbox to dlopen libfw.so"
fi

section "E. CHANGE (#649 E6, #647 E4.3): a feature tool that depends on its declaring package"
d=$root/e; rm -rf "$d"; mkdir -p "$d/fw/src" "$d/fw/tool/src" "$d/app/src"
cat > "$d/fw/mcpp.toml" <<'EOF'
[package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
installer = []

[target.'cfg(os = "linux")'.feature-deps.installer]
spike.fw-installer = { path = "tool", tools = ["fw-installer"], reexport = true }
EOF
cat > "$d/fw/tool/mcpp.toml" <<'EOF'
[package]
name      = "fw-installer"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.fw-installer]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = ".." }
EOF
cat > "$d/app/mcpp.toml" <<'EOF'
[package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
spike.fw = { path = "../fw" }

[features]
windows-installer = ["spike.fw/installer"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$d/app/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("fw-installer", "fw-installer");
    const char* b = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    // `mcpp::warning`, not `std::println`: the fixture states c++20, where
    // `std::println` does not exist, and mcpp discards a build program's
    // standard output when it exits 0, so a reading printed there is never
    // seen. Every e2e script that reads a build program takes this route.
    mcpp::warning((std::string("SPIKE short=[") + (a ? a : "") + "] qualified=["
                   + (b ? b : "") + "]").c_str());
    return 0;
}
EOF
printf '// run %s\n' "$RUNID" >> "$d/app/build.mcpp"
printf 'int fw_answer() { return 42; }\n' > "$d/fw/src/fw.cpp"
printf 'int fw_answer();\n#include <cstdio>\nint main() { std::printf("fw-installer ran %%d\\n", fw_answer()); return 0; }\n' > "$d/fw/tool/src/main.cpp"
printf 'int fw_answer();\nint main() { return fw_answer() == 42 ? 0 : 1; }\n' > "$d/app/src/main.cpp"
( cd "$d/app" && timeout 1200 "$STORE" build --features windows-installer > build.log 2>&1 ); rc=$?
reading "E6 build exit=$rc: $(grep -m1 -E 'SPIKE|cycle' "$d/app/build.log")"
if [ "$rc" -eq 0 ] && grep -q 'qualified=\[/' "$d/app/build.log" && grep -q 'short=\[/' "$d/app/build.log"; then
    ok "the tool builds, and dep_bin answers under both spellings"
else
    fail "the feature tool over its declaring package did not build with both dep_bin spellings"; tail -5 "$d/app/build.log"
fi

section "F. CHANGE (#649 E8): --features <dependency>/<feature> opens a dependency's feature"
sed -i 's/^windows-installer = \["spike.fw\/installer"\]$/windows-installer = []/' "$d/app/mcpp.toml"
rm -rf "$d/app/target"
( cd "$d/app" && timeout 1200 "$STORE" build --strict --features spike.fw/installer > f.log 2>&1 ); rc=$?
reading "E8 exit=$rc: $(grep -m1 -E 'SPIKE|features' "$d/app/f.log")"
if [ "$rc" -eq 0 ] && grep -q 'short=\[/' "$d/app/f.log"; then
    ok "--features spike.fw/installer built the dependency's tool"
else
    fail "--features spike.fw/installer did not open the feature"; tail -5 "$d/app/f.log"
fi

section "G. CHANGE (#647 E1): the root build program reads the graph and a dependency's metadata"
d=$root/g; rm -rf "$d"; mkdir -p "$d/app/src" "$d/a/src" "$d/b/src"
printf '[package]\nname = "b"\nnamespace = "spike"\nversion = "0.2.0"\n\n[package.metadata.demo]\nresources = "res"\n\n[targets.b]\nkind = "lib"\n' > "$d/b/mcpp.toml"
printf 'int b_answer() { return 40; }\n' > "$d/b/src/b.cpp"
printf '[package]\nname = "a"\nnamespace = "spike"\nversion = "0.1.0"\n\n[targets.a]\nkind = "lib"\n\n[dependencies]\nspike.b = { path = "../b" }\n' > "$d/a/mcpp.toml"
printf 'int b_answer();\nint a_answer() { return b_answer() + 2; }\n' > "$d/a/src/a.cpp"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\nspike.a = { path = "../a" }\n' > "$d/app/mcpp.toml"
printf 'int a_answer();\nint main() { return a_answer() == 42 ? 0 : 1; }\n' > "$d/app/src/main.cpp"
cat > "$d/app/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    std::ifstream in(mcpp::graph_file());
    std::stringstream text;
    text << in.rdbuf();
    const auto doc = text.str();
    const auto posA = doc.find("\"spike.a@");
    const auto posB = doc.find("\"spike.b@");
    // See section E on why this is a warning rather than standard output.
    mcpp::warning((std::string("GRAPH b-before-a=")
                   + (posB < posA ? "true" : "false") + " metadata="
                   + (doc.find("\"resources\"") != std::string::npos ? "true" : "false")).c_str());
    return 0;
}
EOF
printf '// run %s\n' "$RUNID" >> "$d/app/build.mcpp"
( cd "$d/app" && timeout 1200 "$STORE" build > build.log 2>&1 ); rc=$?
reading "E1 exit=$rc: $(grep -m1 -E 'GRAPH|graph_file' "$d/app/build.log")"
if grep -q 'GRAPH b-before-a=true metadata=true' "$d/app/build.log"; then
    ok "the graph lists b before a, with b's [package.metadata]"
else
    fail "the root build program did not read the graph"; tail -5 "$d/app/build.log"
fi

section "H. CHANGE (#649 E5, E9): pack strips a graph-built library, and reports what it produced"
d=$root/h; rm -rf "$d"; mkdir -p "$d/dep/src" "$d/app/src"
printf '[package]\nname = "dep"\nnamespace = "spike"\nversion = "0.1.0"\nstandard = "c++20"\n\n[targets.dep]\nkind = "shared"\n\n[build]\nsources = ["src/*.cpp"]\n' > "$d/dep/mcpp.toml"
printf '#include <string>\n[[gnu::visibility("default")]] int dep_answer() { return static_cast<int>(std::to_string(42).size()) + 40; }\n' > "$d/dep/src/dep.cpp"
printf '[package]\nname = "hostapp"\nversion = "0.1.0"\nstandard = "c++20"\n\n[dependencies]\nspike.dep = { path = "../dep" }\n\n[targets.hostapp]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/app/mcpp.toml"
printf 'int dep_answer();\nint main() { return dep_answer() == 42 ? 0 : 1; }\n' > "$d/app/src/main.cpp"
( cd "$d/app" && timeout 1800 "$STORE" pack --release --format tar --message-format json > pack.json 2> pack.err ); rc=$?
reading "E9 exit=$rc stdout-bytes=$(wc -c < "$d/app/pack.json")"
art=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["data"]["artifacts"][0]["path"])' "$d/app/pack.json" 2>/dev/null)
if [ -n "$art" ] && [ -f "$art" ]; then
    ok "pack --release --message-format json names an artifact that exists"
    mkdir -p "$d/x" && tar -xzf "$art" -C "$d/x"
    so=$(find "$d/x" -name libdep.so | head -1)
    if [ -n "$so" ] && command -v readelf >/dev/null 2>&1; then
        symtab=$(readelf -S -W "$so" | grep -c ' .symtab')
        reading "E5 libdep.so symtab=$symtab"
        [ "$symtab" -eq 0 ] && ok "the graph-built libdep.so is stripped" || fail "the graph-built libdep.so keeps .symtab"
    else
        skip "H: no libdep.so in the archive or no readelf"
    fi
else
    fail "pack produced no machine-readable report"; tail -3 "$d/app/pack.err"
fi

section "I. CHANGE (#648 A1, A2): offline code, and planning children do not hold the caller's pipe"
d=$root/i; rm -rf "$d"; mkdir -p "$d/index/pkgs/w" "$d/app/src"
cat > "$d/index/pkgs/w/widget.lua" <<'EOF'
package = {
    spec = "1", namespace = "acme", name = "widget", description = "not installed",
    licenses = {"MIT"}, type = "package",
    xpm = { linux = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } } },
    mcpp = { language = "c++23", sources = { "src/*.cpp" }, targets = { ["widget"] = { kind = "lib" } } },
}
EOF
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[indices]\nacme = { path = "%s" }\n\n[dependencies.acme]\nwidget = "1.0.0"\n' "$d/index" > "$d/app/mcpp.toml"
printf 'int main() { return 0; }\n' > "$d/app/src/main.cpp"
( cd "$d/app" && MCPP_OFFLINE=1 timeout 300 "$STORE" emit build-database --format json > a1.json 2> a1.err ) || true
code=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["diagnostics"][0]["code"])' "$d/app/a1.json" 2>/dev/null)
reading "A1 code=$code"
[ "$code" = "MCPP_OFFLINE_DOWNLOAD_REQUIRED" ] && ok "an offline plan that needs a download has its own code" \
    || fail "the offline code is '$code'"
d2=$root/i2; rm -rf "$d2"; mkdir -p "$d2/src"
printf '[package]\nname = "fds"\nversion = "0.1.0"\n\n[targets.fds]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d2/mcpp.toml"
printf 'int main() { return 0; }\n' > "$d2/src/main.cpp"
cat > "$d2/build.mcpp" <<EOF
import std;
import mcpp;
int main() {
    std::ofstream out("$d2/fds.txt");
    for (auto const& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        out << std::filesystem::read_symlink(e.path(), ec).string() << "\n";
    }
    return 0;
}
EOF
printf '// run %s\n' "$RUNID" >> "$d2/build.mcpp"
( cd "$d2" && MCPP_OFFLINE=1 timeout 600 "$STORE" emit build-database --format json 2>/dev/null \
    | { readlink /proc/self/fd/0 > "$d2/reader.txt"; cat > /dev/null; } )
pipe=$(cat "$d2/reader.txt" 2>/dev/null)
if [ ! -s "$d2/fds.txt" ]; then
    fail "A2: the build program did not run"
elif grep -qxF "$pipe" "$d2/fds.txt"; then
    fail "A2: a build program holds the caller's pipe $pipe"
else
    ok "A2: no planning child holds the caller's pipe"
fi

section "J. CHANGE (#648 T): a bare compat.* dependency is not a refresh miss"
idx="$HOME/.mcpp/registry/data/mcpplibs/pkgs/c"
cand=""; for c in cjson argparse gtest; do [ -f "$idx/compat.$c.lua" ] && { cand=$c; break; }; done
if [ -z "$cand" ]; then
    skip "J: no compat descriptor in the local index"
else
    v=$(grep -o '\["[0-9][0-9.]*"\]' "$idx/compat.$cand.lua" | head -1 | tr -d '[]"')
    d=$root/j; rm -rf "$d"; mkdir -p "$d/src"
    printf '[package]\nname = "j"\nversion = "0.1.0"\n\n[dependencies]\n%s = "%s"\n' "$cand" "$v" > "$d/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$d/src/main.cpp"
    ( cd "$d" && MCPP_OFFLINE=1 timeout 300 "$STORE" emit build-database --format json -v > /dev/null 2> j.err ) || true
    if grep -q "index: $cand@$v: offline mode" "$d/j.err"; then
        fail "T: the refresh decision still wants a network refresh for $cand@$v"
    elif grep -q "deprecated bare-name search" "$d/j.err"; then
        ok "T: $cand@$v resolves through the rung and asks for no refresh"
    else
        skip "T: the fixture did not reach the bare-name rung"
    fi
fi

section "K. GUARD then CHANGE: mcpp:plugins $PLUGINS resolves from the index and builds a consumer"
d=$root/k; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<EOF
[package]
name     = "k"
version  = "0.1.0"
standard = "c++23"

[build-dependencies.mcpp]
plugins = { version = "$PLUGINS", features = ["tools-embed"], host-module = true }
EOF
printf 'int main() { return 0; }\n' > "$d/src/main.cpp"
( cd "$d" && timeout 1800 "$STORE" build > build.log 2>&1 ); rc=$?
reading "plugins exit=$rc: $(grep -m1 -E 'plugins|error' "$d/build.log")"
[ "$rc" -eq 0 ] && ok "mcpp:plugins $PLUGINS resolves and a consumer builds" \
    || { fail "mcpp:plugins $PLUGINS did not build a consumer"; tail -5 "$d/build.log"; }

printf '\n== summary ==\nversion=%s fails=%d\n' "$VER" "$fails"
[ -n "$skipped" ] && printf 'not run:%s\n' "$skipped"
exit $(( fails > 0 ? 1 : 0 ))
