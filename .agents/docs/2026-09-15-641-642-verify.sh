#!/usr/bin/env bash
# Ecosystem verification for #641 and #642 against the PUBLISHED mcpp and index:
# each engine item read from the released binary, and the two package revisions
# (`llvm.libcxx` 22.1.8.3, `openkal-llvm-runtime` 0.9.7) read from the index.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-15-641-642-verify.sh)
#   xlings subos new v641
#   xlings subos use v641 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.15.2 bash /tmp/v.sh"
#
# The sandbox shares the xlings data directory, so the published mcpp is
# addressed by its store path and exact version. Its $HOME content persists
# between runs of one SubOS, so every section clears its own probe directory
# first. A section that cannot run says so and is listed again in the summary.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
LIBCXX="${MCPP_VERIFY_LIBCXX:-22.1.8.3}"
RUNTIME="${MCPP_VERIFY_OKLR:-0.9.7}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }
has_line() { grep -qxF -- "$2" "$1"; }
has_text() { grep -qF -- "$2" "$1"; }
unset XLINGS_ACTIVE_SUBOS

root=/tmp/verify-641
rm -rf "$root"; mkdir -p "$root"

section "A. the published mcpp answers for itself, with CN mirrors for xlings and mcpp"
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

section "B. the index names the two package revisions"
"$STORE" search libcxx > "$root/search-libcxx.log" 2>&1
has_text "$root/search-libcxx.log" "$LIBCXX" && ok "llvm.libcxx $LIBCXX is in the index" \
    || { fail "llvm.libcxx $LIBCXX is not in the index"; cat "$root/search-libcxx.log"; }
"$STORE" search openkal-llvm > "$root/search-oklr.log" 2>&1
has_text "$root/search-oklr.log" "$RUNTIME" && ok "openkal-llvm-runtime $RUNTIME is in the index" \
    || { fail "openkal-llvm-runtime $RUNTIME is not in the index"; cat "$root/search-oklr.log"; }

section "C. #641 item 1: a required llvm on a home with no llvm resolves the llvm pin"
d=$root/c; rm -rf "$d"; mkdir -p "$d/proj/src" "$d/home"
printf '[package]\nname = "c"\nversion = "0.1.0"\nrequires = ["mcpp:compiler=llvm"]\n' > "$d/proj/mcpp.toml"
printf 'int main() { return 0; }\n' > "$d/proj/src/main.cpp"
vendored=$(dirname "$(dirname "$STORE")")/registry/bin/xlings
( cd "$d/proj" && MCPP_HOME="$d/home" MCPP_VENDORED_XLINGS="$vendored" XLINGS_NON_INTERACTIVE=1 \
    timeout 240 "$STORE" build > build.log 2>&1 ) || true
if has_text "$d/proj/build.log" "llvm@30.0.16248370"; then
    fail "the requirement still names the NDK's version"; grep -n "llvm@" "$d/proj/build.log" | head -3
elif grep -q "llvm@22\.1\.8" "$d/proj/build.log"; then
    ok "the requirement resolves llvm@22.1.8 on a fresh home"
else
    fail "no llvm spec was named"; tail -5 "$d/proj/build.log"
fi

section "D. #641 item 2: a c++20 program over llvm.libcxx $LIBCXX from the index"
d=$root/d; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<EOF
[package]
name     = "d"
version  = "0.1.0"
standard = "c++20"

[toolchain]
default = "llvm@22.1.8"

[dependencies]
llvm.libcxx = "$LIBCXX"
EOF
cat > "$d/src/main.cpp" <<'EOF'
import std;
int main() {
    std::unordered_map<std::string, int> m; m["one"] = 1;
    try { throw std::runtime_error("x"); } catch (const std::runtime_error&) { m["two"] = 2; }
    std::cout << std::format("{}-{}", m["one"], m["two"]) << std::endl;
}
EOF
if (cd "$d" && "$STORE" build > build.log 2>&1); then
    has_text "$d/build.log" "libcxx@$LIBCXX, graph" && ok "the report names llvm.libcxx $LIBCXX as the C++ layer" \
        || fail "the report does not name llvm.libcxx $LIBCXX"
    (cd "$d" && "$STORE" run > run.log 2>&1)
    has_line "$d/run.log" "1-2" && ok "the c++20 program runs over llvm.libcxx" || { fail "the c++20 program did not print 1-2"; tail -3 "$d/run.log"; }
else
    fail "the c++20 program over llvm.libcxx did not build"; grep -m3 "error" "$d/build.log"
fi

section "E. #641 item 5: a shared dependency over the graph runtime"
d=$root/e; rm -rf "$d"; mkdir -p "$d/fw/src" "$d/app/src"
printf '[package]\nname = "fw"\nversion = "0.1.0"\n\n[targets.fw]\nkind = "shared"\n' > "$d/fw/mcpp.toml"
printf 'export module fw;\nimport std;\nexport std::string fw_greet(int n);\n' > "$d/fw/src/fw.cppm"
printf 'module fw;\nimport std;\nstd::string fw_greet(int n) { return std::format("fw-{}", n); }\n' > "$d/fw/src/fw.cpp"
printf 'import std;\nimport fw;\nint main() { std::cout << fw_greet(3) << std::endl; }\n' > "$d/app/src/main.cpp"
write_e() {
    cat > "$d/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[build]
$1

[dependencies]
llvm.libcxx = "$LIBCXX"
fw = { path = "../fw" }
EOF
}
write_e ''
if (cd "$d/app" && "$STORE" build > refused.log 2>&1); then
    fail "the shared dependency was not refused"
else
    has_text "$d/app/refused.log" "'fw' is linked as a shared library" \
        && has_text "$d/app/refused.log" 'cxx_runtime = { shared = "self-contained" }' \
        && ok "refused before compiling, naming the private copy" \
        || { fail "refused for another reason"; tail -5 "$d/app/refused.log"; }
fi
write_e 'cxx_runtime = { shared = "self-contained" }'
rm -rf "$d/app/target"
if (cd "$d/app" && "$STORE" build > private.log 2>&1); then
    (cd "$d/app" && "$STORE" run > run.log 2>&1)
    has_line "$d/app/run.log" "fw-3" && ok "the stated private copy builds and runs" || { fail "the private copy did not print fw-3"; tail -3 "$d/app/run.log"; }
else
    fail "the stated private copy did not build"; grep -m3 "error" "$d/app/private.log"
fi

section "F. #642 E1 and E2: a package's default form, and the root build program reads it"
d=$root/f; rm -rf "$d"; mkdir -p "$d/fw/src" "$d/app/src"
cat > "$d/fw/mcpp.toml" <<'EOF'
[package]
name    = "fw"
version = "0.1.0"

[targets.fw]
kind    = "lib"
linkage = "shared"
EOF
printf 'int fw_answer() { return 42; }\n' > "$d/fw/src/fw.cpp"
printf 'int fw_answer();\nconst char* dep_form();\n#include <cstdio>\nint main() { std::printf("form=%%s answer=%%d\\n", dep_form(), fw_answer()); }\n' > "$d/app/src/main.cpp"
# A build program's output is shown only when it fails, so the value it reads is
# written into a generated source and printed by the program.
cat > "$d/app/build.mcpp" <<'EOF'
import mcpp;
#include <cstdio>
#include <string>
int main() {
    std::string out = std::string(mcpp::out_dir()) + "/form.cpp";
    FILE* f = std::fopen(out.c_str(), "w");
    if (!f) return 1;
    std::fprintf(f, "const char* dep_form() { return \"%s\"; }\n", mcpp::dep_linkage("fw"));
    std::fclose(f);
    mcpp::generated(out.c_str());
}
EOF
write_f() {
    cat > "$d/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@22.1.8"

[features]
extra = {}

[dependencies]
fw = $1
EOF
}
write_f '{ path = "../fw" }'
if (cd "$d/app" && "$STORE" build > silent.log 2>&1); then
    ls "$d"/app/target/*/*/bin/libfw.so >/dev/null 2>&1 && ok "a silent consumer gets the package's default shared form" \
        || fail "a silent consumer did not get libfw.so"
    (cd "$d/app" && "$STORE" run > run.log 2>&1)
    has_line "$d/app/run.log" "form=shared answer=42" && ok "the root build program reads dep_linkage=shared" \
        || { fail "the program did not print form=shared"; tail -3 "$d/app/run.log"; }
else
    fail "the silent consumer did not build"; tail -5 "$d/app/silent.log"
fi
write_f '{ path = "../fw", linkage = "static" }'
rm -rf "$d/app/target"
if (cd "$d/app" && "$STORE" build --strict > static.log 2>&1); then
    ls "$d"/app/target/*/*/bin/libfw.so >/dev/null 2>&1 && fail "an explicit static consumer still got libfw.so" \
        || ok "an explicit linkage = \"static\" overrides the default under --strict"
    has_text "$d/app/static.log" "overrides the package's default" && ok "the information line names both statements" \
        || fail "no information line for the override"
    (cd "$d/app" && "$STORE" run > run.log 2>&1)
    has_line "$d/app/run.log" "form=static answer=42" && ok "the root build program reads dep_linkage=static" \
        || { fail "the program did not print form=static"; tail -3 "$d/app/run.log"; }
else
    fail "the explicit static consumer did not build under --strict"; tail -5 "$d/app/static.log"
fi

section "G. #641 item 4: mcpp pack --features"
if (cd "$d/app" && "$STORE" pack --format dir --features extra > pack.log 2>&1); then
    ok "mcpp pack --format dir --features extra succeeds"
else
    fail "mcpp pack --features failed"; tail -5 "$d/app/pack.log"
fi

printf '\n== summary ==\nfails=%d\n' "$fails"
[ -z "$skipped" ] || printf 'not run:%s\n' "$skipped"
[ "$fails" -eq 0 ]
