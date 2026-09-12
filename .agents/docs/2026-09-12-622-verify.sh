#!/usr/bin/env bash
# Ecosystem verification for #622 against a PUBLISHED mcpp: the engine items
# A1 to A7, A10 and A11 of the design record, each read from the published
# binary rather than from a development tree, and (when the plugin release
# exists) `mcpp:plugins`' dist-web through the index.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-12-622-verify.sh)
#   xlings subos use verify-622 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=<ver> bash /tmp/v.sh"
#
# The sandbox has an empty $HOME on first use and a fresh /tmp; it shares the
# xlings data directory, so the published mcpp is addressed by its store path
# and the payloads the machine holds (emsdk, android-ndk) are visible. Every
# section clears its own probe directory first, so a second run reads the
# release and not the first run's residue. A section that cannot run says so
# and is listed again in the summary.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
PLUGINS="${MCPP_VERIFY_PLUGINS:-}"        # e.g. 0.7.0; empty skips section J

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }
has_line() { grep -qxF -- "$2" "$1"; }
has_text() { grep -qF -- "$2" "$1"; }

root=/tmp/verify-622
rm -rf "$root"; mkdir -p "$root"

section "A. the published mcpp answers for itself, with the CN mirror configured"
if [ ! -x "$STORE" ]; then
    fail "no mcpp at $STORE"; printf '\nfails=%d (nothing else can run)\n' "$fails"; exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
case "$got" in *"$VER"*) ok "mcpp --version says $got" ;; *) fail "mcpp --version says '$got', expected $VER" ;; esac
"$STORE" self config --mirror CN >/dev/null 2>&1 && ok "mcpp self config --mirror CN" || fail "mcpp self config --mirror CN"

# A small program every section reuses.
mkprog() {   # $1 dir, $2 name, $3 extra manifest text
    rm -rf "$1"; mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n%s\n' "$2" "$2" "$3" > "$1/mcpp.toml"
    printf '#include <cstdio>\nint main() { std::puts("1-2-3"); return 0; }\n' > "$1/src/main.cpp"
}

section "B. the wasm row: bin/<name>.js, the .wasm sibling, the staged family, shared refused"
d=$root/b; mkprog "$d" w ""
if (cd "$d" && "$STORE" build --target wasm32-emscripten > build.log 2>&1); then
    js=$(find "$d/target/wasm32-emscripten" -path '*/bin/w.js' | head -1)
    wasm=$(find "$d/target/wasm32-emscripten" -path '*/bin/w.wasm' | head -1)
    bare=$(find "$d/target/wasm32-emscripten" -path '*/bin/w' | head -1)
    [ -n "$js" ] && ok "launcher is bin/w.js" || fail "no bin/w.js"
    [ -n "$wasm" ] && ok "bin/w.wasm beside it" || fail "no bin/w.wasm"
    [ -z "$bare" ] && ok "no bare bin/w" || fail "a bare bin/w exists"
    (cd "$d" && "$STORE" run --target wasm32-emscripten > run.log 2>&1); has_line "$d/run.log" "1-2-3" && ok "mcpp run prints 1-2-3 through the payload's runner" || { fail "mcpp run did not print 1-2-3"; tail -3 "$d/run.log"; }
    (cd "$d" && "$STORE" pack --target wasm32-emscripten --format dir > pack.log 2>&1)
    staged=$(find "$d/target/dist" -name 'w.js' | head -1)
    if [ -n "$staged" ] && [ -f "${staged%.js}.wasm" ]; then ok "pack --format dir stages w.js and w.wasm"; else fail "pack did not stage the family"; tail -3 "$d/pack.log"; fi
    printf '\n[targets.wshared]\nkind = "shared"\n' >> "$d/mcpp.toml"; printf 'int f() { return 1; }\n' > "$d/src/lib.cpp"
    (cd "$d" && "$STORE" build --target wasm32-emscripten > shared.log 2>&1) && fail "kind = shared on wasm was accepted" || { has_text "$d/shared.log" 'a side module needs -sSIDE_MODULE' && ok "shared refused naming -sSIDE_MODULE" || { fail "shared refused for another reason"; tail -3 "$d/shared.log"; }; }
else
    fail "wasm build failed"; tail -5 "$d/build.log"
fi

section "C. a build program deploys what it generated (mcpp::deploy), and the copy survives a cache hit"
d=$root/c; mkprog "$d" dep ""
mkdir -p "$d/gen"; printf 'generated-1\n' > "$d/gen/in.txt"
cat > "$d/build.mcpp" <<'CPP'
import mcpp;
#include <string>
int main() {
    // An action runs with the build directory as its cwd, so the source is
    // named by its absolute path; the manifest directory is the anchor.
    const std::string in  = std::string(mcpp::manifest_dir()) + "/gen/in.txt";
    const std::string out = std::string(mcpp::out_dir()) + "/res.bin";
    mcpp::action a; a.id = "gen:res"; a.role = "source";
    a.arg("/bin/cp").arg(in.c_str()).arg(out.c_str()).input(in.c_str()).output(out.c_str()).submit();
    mcpp::deploy(out.c_str(), "dep.resources");
    return 0;
}
CPP
if (cd "$d" && "$STORE" build > build.log 2>&1); then
    f=$(find "$d/target" -path '*/bin/dep.resources/res.bin' | head -1)
    [ -n "$f" ] && has_line "$f" "generated-1" && ok "bin/dep.resources/res.bin carries the generated content" || { fail "deploy'd file absent or wrong"; tail -3 "$d/build.log"; }
    bindir=$(dirname "$(dirname "$f")"); rm -rf "$bindir"
    (cd "$d" && "$STORE" build > build2.log 2>&1); [ -f "$f" ] && ok "restored after bin/ was deleted (replay on a cache hit)" || fail "not restored on the rebuild"
    (cd "$d" && "$STORE" pack --format dir > pack.log 2>&1); [ -n "$(find "$d/target/dist" -path '*dep.resources/res.bin' | head -1)" ] && ok "pack --format dir stages it" || fail "pack did not stage the deploy'd file"
else
    fail "build with mcpp::deploy failed"; tail -5 "$d/build.log"
fi

section "D. kind = app: a program on the host, a shared library on Android, run refused without --format"
d=$root/d; rm -rf "$d"; mkdir -p "$d/src"
printf '[package]\nname = "myapp"\nversion = "0.1.0"\n\n[targets.myapp]\nkind = "app"\nmain = "src/main.cpp"\n\n[targets.tool]\nkind = "bin"\nmain = "src/tool.cpp"\n\n[target.x86_64-linux-android]\nmin_api_level = 24\n' > "$d/mcpp.toml"
printf '#include <cstdio>\nint main() { std::puts("app-1-2-3"); return 0; }\n' > "$d/src/main.cpp"
printf '#include <cstdio>\nint main() { std::puts("tool-1-2-3"); return 0; }\n' > "$d/src/tool.cpp"
if (cd "$d" && "$STORE" run myapp > run.log 2>&1) && has_line "$d/run.log" "app-1-2-3"; then ok "an app is a program on the host"; else fail "host run of the app"; tail -3 "$d/run.log"; fi
if (cd "$d" && "$STORE" build --target x86_64-linux-android > android.log 2>&1); then
    [ -n "$(find "$d/target/x86_64-linux-android" -name 'libmyapp.so' | head -1)" ] && ok "libmyapp.so on Android" || fail "no libmyapp.so"
    [ -n "$(find "$d/target/x86_64-linux-android" -path '*/bin/tool' | head -1)" ] && ok "bin keeps meaning binary on Android" || fail "no bin/tool on Android"
    (cd "$d" && "$STORE" run myapp --target x86_64-linux-android > arun.log 2>&1) && fail "run of a library-form app was accepted" || { has_text "$d/arun.log" 'an application is a shared library that a package installs' && ok "run refused naming --format" || { fail "refused for another reason"; tail -3 "$d/arun.log"; }; }
else
    skip "D (Android leg): build for x86_64-linux-android failed; is the NDK payload visible? $(tail -1 "$d/android.log")"
fi

section "E. requires_abi on the target axis, and the two-member abi table"
d=$root/e; rm -rf "$d"; mkdir -p "$d/lib/src" "$d/src"
printf '[package]\nname = "needsthreads"\nversion = "0.1.0"\n\n[lib]\npath = "src/lib.cppm"\n\n[target.'"'"'cfg(linux)'"'"']\nrequires_abi = { threads = true }\n' > "$d/lib/mcpp.toml"
printf 'export module needsthreads;\nexport int nt() { return 3; }\n' > "$d/lib/src/lib.cppm"
printf '[package]\nname = "e"\nversion = "0.1.0"\n\n[dependencies]\nneedsthreads = { path = "lib" }\n\n[targets.e]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/mcpp.toml"
printf 'import needsthreads;\n#include <cstdio>\nint main() { std::printf("1-2-%%d\\n", nt()); return 0; }\n' > "$d/src/main.cpp"
(cd "$d" && "$STORE" build > b1.log 2>&1) && fail "a selector-scoped requirement was not enforced" || { has_text "$d/b1.log" "requires the artefact's ABI to have threads" && has_text "$d/b1.log" "cfg(linux)" && ok "refused naming the member and the selector" || { fail "refused for another reason"; tail -4 "$d/b1.log"; }; }
printf '\n[target.'"'"'cfg(linux)'"'"'.abi]\nthreads = true\n' >> "$d/mcpp.toml"
(cd "$d" && "$STORE" run > r.log 2>&1) && has_line "$d/r.log" "1-2-3" && ok "satisfied by the root's table" || { fail "build with the table"; tail -3 "$d/r.log"; }
printf '\n[target.'"'"'cfg(linux)'"'"'.abi]\nfrobnicate = true\n' >> "$d/mcpp.toml"
(cd "$d" && "$STORE" build > b3.log 2>&1) && fail "an unknown abi member was accepted" || { has_text "$d/b3.log" "threads, exceptions" && ok "unknown member refused naming both members" || { fail "refused without naming the members"; tail -2 "$d/b3.log"; }; }

section "F. frameworks per target: a Linux build is byte-identical with and without the tables"
d=$root/f; mkprog "$d" fw ""
(cd "$d" && "$STORE" build > b1.log 2>&1) && n1=$(find "$d/target" -name build.ninja | head -1) && cp "$n1" "$d/n1"
printf '\n[runtime]\nframeworks = ["Foundation"]\n\n[target.macos.runtime]\nframeworks = ["AppKit"]\n\n[target.'"'"'cfg(os = "ios")'"'"'.runtime]\nframeworks = ["UIKit"]\n' >> "$d/mcpp.toml"
rm -rf "$d/target"; (cd "$d" && "$STORE" build > b2.log 2>&1) && n2=$(find "$d/target" -name build.ninja | head -1)
if [ -n "${n2:-}" ] && cmp -s "$d/n1" "$n2"; then ok "build.ninja identical on Linux"; else fail "build.ninja differs or build failed"; fi
grep -q "unsupported key 'frameworks'" "$d/b2.log" && fail "frameworks reported as unsupported under a target table" || ok "frameworks accepted under [target.<sel>.runtime]"

section "G. platforms names the rows that exist"
d=$root/g; mkprog "$d" pl 'platforms = ["linux", "ios", "android", "emscripten"]'
sed -i 's/^\[targets.pl\]/[targets.pl]/' "$d/mcpp.toml"; python3 - "$d/mcpp.toml" <<'PY'
import sys,re; p=sys.argv[1]; s=open(p).read()
s=s.replace('version = "0.1.0"\n','version = "0.1.0"\nplatforms = ["linux", "ios", "android", "emscripten"]\n',1).replace('\nplatforms = ["linux", "ios", "android", "emscripten"]\n\n','\n\n')
open(p,'w').write(s)
PY
(cd "$d" && "$STORE" build --strict > b1.log 2>&1) && ok "the six-word vocabulary passes --strict" || { fail "a known platform name was refused"; grep -i platform "$d/b1.log" | head -2; }
sed -i 's/platforms = \[.*\]/platforms = ["web"]/' "$d/mcpp.toml"; rm -rf "$d/target"
(cd "$d" && "$STORE" build --strict > b2.log 2>&1) && fail "'web' was accepted" || { has_text "$d/b2.log" "unknown platform 'web'" && has_text "$d/b2.log" "linux | macos | windows | ios | android | emscripten" && ok "'web' refused naming the six" || { fail "refused without the vocabulary"; tail -2 "$d/b2.log"; }; }

section "H. mcpp run --format hands the distributable to the runner"
d=$root/h; mkprog "$d" rf ""
host=$("$STORE" --version >/dev/null 2>&1; uname -m)-linux-gnu
cat > "$d/runner.sh" <<'SH'
#!/usr/bin/env bash
echo "RUNNER: $1"; chmod +x "$1"; exec "$1"
SH
chmod +x "$d/runner.sh"
printf '\n[target.%s]\nrunner = ["%s"]\n' "$host" "$d/runner.sh" >> "$d/mcpp.toml"
cat > "$d/build.mcpp" <<'CPP'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/rf.blob";
    mcpp::action a; a.id = "blob"; a.role = "artifact";
    a.arg("/bin/cp").arg("${mcpp.target_file:rf}").arg(out.c_str()).input("${mcpp.target_file:rf}").output(out.c_str()).submit();
    return 0;
}
CPP
(cd "$d" && "$STORE" run --format blob > r1.log 2>&1); has_text "$d/r1.log" "RUNNER: " && has_text "$d/r1.log" "rf.blob" && has_line "$d/r1.log" "1-2-3" && ok "run --format blob ran the blob through the runner" || { fail "run --format blob"; tail -4 "$d/r1.log"; }
(cd "$d" && "$STORE" run > r2.log 2>&1); has_text "$d/r2.log" "RUNNER: " && ! has_text "$d/r2.log" "rf.blob" && ok "plain run hands the link output" || fail "plain run"
(cd "$d" && "$STORE" run --format bogus > r3.log 2>&1) && fail "--format bogus accepted" || { has_text "$d/r3.log" "blob" && ok "unknown format refused naming blob" || fail "unknown format refusal"; }
(cd "$d" && "$STORE" run --format blob --no-runner > r4.log 2>&1) && fail "--format with --no-runner accepted" || { has_text "$d/r4.log" "cannot be combined" && ok "--format with --no-runner refused" || fail "wrong refusal"; }

section "I. abi.exceptions on the Web row"
d=$root/i; rm -rf "$d"; mkdir -p "$d/src"
printf '[package]\nname = "ex"\nversion = "0.1.0"\n\n[targets.ex]\nkind = "bin"\nmain = "src/main.cpp"\n\n[target.'"'"'cfg(os = "emscripten")'"'"'.abi]\nexceptions = true\n' > "$d/mcpp.toml"
printf '#include <cstdio>\n#include <stdexcept>\nint main() { try { throw std::runtime_error("x"); } catch (const std::exception&) { std::puts("1-2-3"); } return 0; }\n' > "$d/src/main.cpp"
if (cd "$d" && "$STORE" run --target wasm32-emscripten > r1.log 2>&1) && has_line "$d/r1.log" "1-2-3"; then
    ok "throw and catch under node with exceptions = true"
    n=$(find "$d/target/wasm32-emscripten" -name build.ninja | head -1); grep -q -- '-fexceptions' "$n" && ok "-fexceptions in build.ninja" || fail "-fexceptions absent from build.ninja"
    sed -i '/\.abi\]/,$d' "$d/mcpp.toml"; rm -rf "$d/target"
    (cd "$d" && "$STORE" run --target wasm32-emscripten > r2.log 2>&1) && has_line "$d/r2.log" "1-2-3" && fail "the marker printed without the table" || ok "without the table the run does not print the marker"
else
    fail "exceptions on the Web row"; tail -4 "$d/r1.log"
fi

section "J. mcpp:plugins dist-web through the index"
if [ -z "$PLUGINS" ]; then
    skip "J: MCPP_VERIFY_PLUGINS not set (the plugin release does not exist yet)"
else
    d=$root/j; mkprog "$d" web ""
    printf '\n[build-dependencies.mcpp]\nplugins = { version = "%s", features = ["dist-web"], host-module = true }\n' "$PLUGINS" >> "$d/mcpp.toml"
    printf 'import mcpp;\nimport mcpp.dist.web;\nint main() { return mcpp::dist::web::generate({ .target = "web" }) ? 0 : 1; }\n' > "$d/build.mcpp"
    if (cd "$d" && "$STORE" pack --target wasm32-emscripten --format web > p.log 2>&1); then
        idx=$(find "$d/target" -path '*/web/index.html' | head -1)
        [ -n "$idx" ] && [ -f "$(dirname "$idx")/web.js" ] && [ -f "$(dirname "$idx")/web.wasm" ] && ok "dist-web wrote index.html, web.js, web.wasm" || { fail "dist-web output incomplete"; ls "$(dirname "${idx:-$d}")"; }
    else
        fail "pack --format web through mcpp:plugins@$PLUGINS"; tail -5 "$d/p.log"
    fi
fi

printf '\nfails=%d\n' "$fails"
if [ -n "$skipped" ]; then printf 'sections NOT RUN:%s\nA pass with sections not run is not a pass for those sections.\n' "$skipped"; fi
[ "$fails" -eq 0 ]
