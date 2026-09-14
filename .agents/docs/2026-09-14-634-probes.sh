#!/usr/bin/env bash
# Probes behind the measured statements of 2026-09-14-634-cmake-parity-items-by-home.md.
#
# Each section builds a small fixture in a fresh directory and prints one line
# per claim: `holds: <claim>` when the reading matches what the record states
# about mcpp 2026.9.14.1, `differs: <claim> (<reading>)` otherwise. After a fix
# lands, the corresponding claim is expected to read `differs`.
#
# Usage:  MCPP=/path/to/mcpp bash 2026-09-14-634-probes.sh [workdir]
# Needs:  gcc@16.1.0 (the default toolchain), llvm@22.1.8 and
#         xim:android-ndk installed in the mcpp home the binary uses; readelf.
#
# Deliberately NOT `set -e`: several probes run a command that is expected to
# fail and read its output afterwards.
set -u

MCPP=${MCPP:-mcpp}
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
echo "mcpp: $("$MCPP" --version)"
echo "work: $WORK"

verdict() {  # verdict <0|1> <claim> <reading>
    if [ "$1" -eq 0 ]; then echo "holds: $2"; else echo "differs: $2 ($3)"; fi
}

# A library `demo.fw` exporting module `fw`, reused by A1, A3 and A4.
make_fw() {
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<'T'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
T
    printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$1/src/fw.cppm"
}

# ── A1: one identity, a different linkage per row (host row) ──────────────────
A1=$WORK/a1; rm -rf "$A1"; make_fw "$A1/fw"
for n in 1 2 3; do
    mkdir -p "$A1/app$n/src"
    printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$A1/app$n/src/main.cpp"
done
cat > "$A1/app1/mcpp.toml" <<'T'
[package]
name = "app1"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw" }
[target.'cfg(os = "linux")'.dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
T
cat > "$A1/app2/mcpp.toml" <<'T'
[package]
name = "app2"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw" }
[target.'cfg(os = "linux")'.dependencies]
demo.fw = { linkage = "shared" }
T
# The control: two complementary predicates, which the record says work.
# Without it, "no libfw.so" above could mean the shared form is broken in
# general rather than that the declaration was dropped.
cat > "$A1/app3/mcpp.toml" <<'T'
[package]
name = "app3"
version = "0.1.0"
[target.'cfg(os = "linux")'.dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
[target.'cfg(not(os = "linux"))'.dependencies]
demo.fw = { path = "../fw" }
T
(cd "$A1/app3" && "$MCPP" build > build.log 2>&1); rc3=$?
so3=$(find "$A1/app3/target" -name 'libfw.so' 2>/dev/null | head -1)
exe3=$(find "$A1/app3/target" -type f -name app3 -perm -u+x 2>/dev/null | head -1)
needed3=$(readelf -d "$exe3" 2>/dev/null | grep NEEDED | tr '\n' ' ')
[ "$rc3" -eq 0 ] && [ -n "$so3" ] && [[ "$needed3" == *"libfw.so"* ]]
verdict $? "A1 control: two complementary predicates build libfw.so and link it" "exit=$rc3 so=${so3:-none}"
(cd "$A1/app1" && "$MCPP" build > build.log 2>&1); rc1=$?
so1=$(find "$A1/app1/target" -name 'libfw.so' 2>/dev/null | head -1)
[ "$rc1" -eq 0 ] && [ -z "$so1" ]
verdict $? "A1 spelling 1 exits 0 and builds no libfw.so" "exit=$rc1 so=${so1:-none}"
(cd "$A1/app2" && "$MCPP" build > build.log 2>&1); rc2=$?
log2=$(cat "$A1/app2/build.log")
[ "$rc2" -ne 0 ] && [[ "$log2" == *"[dependencies] demo.fw.linkage = 'shared'"* ]] \
    && [[ "$log2" == *"(demo.fw, linkage)"* ]]
verdict $? "A1 spelling 2 is read as the selector (demo.fw, linkage) under the label [dependencies]" "exit=$rc2"

# ── A2: a key normalising to another identity than its manifest ───────────────
A2=$WORK/a2; rm -rf "$A2"; mkdir -p "$A2/fw/src" "$A2/comp/src" "$A2/app/src" "$A2/single/src"
cat > "$A2/fw/mcpp.toml" <<'T'
[package]
namespace = "huxdemo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
T
printf 'export module fw;\nexport int fw_anchor() { return 7; }\n' > "$A2/fw/src/fw.cppm"
cat > "$A2/comp/mcpp.toml" <<'T'
[package]
namespace = "huxdemo"
name      = "comp"
version   = "0.1.0"
[targets.comp]
kind = "lib"
[dependencies]
fw = { path = "../fw" }
T
printf 'export module comp;\nimport fw;\nexport int comp_anchor() { return fw_anchor(); }\n' > "$A2/comp/src/comp.cppm"
cat > "$A2/app/mcpp.toml" <<'T'
[package]
name = "app"
version = "0.1.0"
[dependencies]
huxdemo.fw   = { path = "../fw" }
huxdemo.comp = { path = "../comp" }
T
printf 'import fw;\nimport comp;\nint main() { return fw_anchor() + comp_anchor() == 14 ? 0 : 1; }\n' > "$A2/app/src/main.cpp"
cat > "$A2/single/mcpp.toml" <<'T'
[package]
name = "single"
version = "0.1.0"
[dependencies]
fw = { path = "../fw" }
T
printf 'import fw;\nint main() { return fw_anchor() == 7 ? 0 : 1; }\n' > "$A2/single/src/main.cpp"
(cd "$A2/app" && "$MCPP" build > build.log 2>&1); rc=$?
log=$(cat "$A2/app/build.log")
[ "$rc" -ne 0 ] && [[ "$log" == *"module 'fw' already provided by"* ]]
verdict $? "A2 two edges to one directory fail in the scanner" "exit=$rc"
(cd "$A2/single" && "$MCPP" build > build.log 2>&1); rc=$?
log=$(cat "$A2/single/build.log")
[ "$rc" -eq 0 ] && [[ "$log" != *"identity"* ]]
verdict $? "A2 one mismatched edge builds with no identity diagnostic" "exit=$rc"

# ── A3 and A4: the Android shared-object row ──────────────────────────────────
A3=$WORK/a3; rm -rf "$A3"; make_fw "$A3/fw"; mkdir -p "$A3/app/src"
cat > "$A3/app/mcpp.toml" <<'T'
[package]
name = "app"
version = "0.1.0"
[targets.app]
kind = "app"
main = "src/main.cpp"
[target.'cfg(env = "android")'.dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
[target.'cfg(not(env = "android"))'.dependencies]
demo.fw = { path = "../fw" }
T
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$A3/app/src/main.cpp"
(cd "$A3/app" && "$MCPP" pack --target x86_64-linux-android --format dir > pack.log 2>&1); rc=$?
tree=$(find "$A3/app/target/dist" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)
files=$(cd "$tree" 2>/dev/null && find . -type f | sort | tr '\n' ' ')
manifest=$(cat "$tree.stage-manifest" 2>/dev/null)
needed=$(readelf -d "$tree/lib/libapp.so" 2>/dev/null | grep NEEDED | tr -s ' ' | tr '\n' ' ')
[ "$rc" -eq 0 ] && [ "$files" = "./lib/libapp.so " ] && [[ "$manifest" == "closure = walked"* ]] \
    && [[ "$needed" == *"libfw.so"* ]] && [[ "$needed" == *"libc++_shared.so"* ]]
verdict $? "A3 --format dir stages only lib/libapp.so, needs libfw.so and libc++_shared.so, says walked" \
    "exit=$rc files=$files"
built=$(find "$A3/app/target/x86_64-linux-android" -path '*/bin/*.so' 2>/dev/null)
nosoname=1
for so in $built; do
    readelf -d "$so" | grep -q SONAME && nosoname=0
done
[ -n "$built" ] && [ "$nosoname" -eq 1 ]
verdict $? "A4 no DT_SONAME on the Android libfw.so or libapp.so" "libs=$(echo $built | wc -w)"

# ── A7: the SubOS pkg-config view (reads the registry; builds nothing) ────────
# `mcpp self env` prints `xlings home = <registry>`; the SubOS view is under it.
registry=$("$MCPP" self env 2>/dev/null | sed -n 's/^xlings home *= *//p' | head -1)
view="${registry:-$HOME/.mcpp/registry}/subos/default/usr/lib/pkgconfig"
if [ -f "$view/gtk4.pc" ]; then
    out=$(env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$view" pkg-config --cflags --libs gtk4 2>&1); rc=$?
    [ "$rc" -ne 0 ] && [[ "$out" == *"Package 'zlib'"* ]]
    # A reading of THIS registry's history, not of provisioning: on a fresh
    # home the same command exits 0 (CI, revision 2 of the record, section 5.7).
    verdict $? "A7 (this registry only) pkg-config gtk4 against the SubOS view fails on zlib" "exit=$rc"
else
    echo "skipped: A7 (no gtk4.pc in $view)"
fi

# ── A8: a transitive dependency's deploy reaches the application ─────────────
A8=$WORK/a8; rm -rf "$A8"; mkdir -p "$A8/libb/src" "$A8/liba/src" "$A8/app/src"
cat > "$A8/libb/mcpp.toml" <<'T'
[package]
namespace = "demo"
name = "libb"
version = "0.1.0"
[targets.libb]
kind = "lib"
T
printf 'export module libb;\nexport int b() { return 2; }\n' > "$A8/libb/src/libb.cppm"
echo "libb strings" > "$A8/libb/strings.txt"
cat > "$A8/libb/build.mcpp" <<'T'
import std;
import mcpp;
int main() {
    mcpp::deploy((std::string(mcpp::manifest_dir()) + "/strings.txt").c_str(), "resources/libb");
    return 0;
}
T
cat > "$A8/liba/mcpp.toml" <<'T'
[package]
namespace = "demo"
name = "liba"
version = "0.1.0"
[targets.liba]
kind = "lib"
[dependencies]
demo.libb = { path = "../libb" }
T
printf 'export module liba;\nimport libb;\nexport int a() { return b() + 1; }\n' > "$A8/liba/src/liba.cppm"
cat > "$A8/app/mcpp.toml" <<'T'
[package]
name = "app"
version = "0.1.0"
[dependencies]
demo.liba = { path = "../liba" }
T
printf 'import liba;\nint main() { return a() == 3 ? 0 : 1; }\n' > "$A8/app/src/main.cpp"
(cd "$A8/app" && "$MCPP" build > build.log 2>&1 && "$MCPP" pack --format dir > pack.log 2>&1); rc=$?
inbin=$(find "$A8/app/target" -path '*/bin/resources/libb/strings.txt' 2>/dev/null | head -1)
instage=$(find "$A8/app/target/dist" -path '*/resources/libb/strings.txt' 2>/dev/null | head -1)
[ "$rc" -eq 0 ] && [ -n "$inbin" ] && [ -n "$instage" ]
verdict $? "A8 a dependency two levels down deploys into the app's bin/ and staged tree" "exit=$rc"

# ── A10: MCPP_TOOLCHAIN reaches test and pack; the flag does not ─────────────
A10=$WORK/a10; rm -rf "$A10"; mkdir -p "$A10/src" "$A10/tests"
printf '[package]\nname = "a10"\nversion = "0.1.0"\n' > "$A10/mcpp.toml"
printf 'int main() { return 0; }\n' > "$A10/src/main.cpp"
printf '#include <cstdio>\nint main() { std::printf("compiler=%%s\\n", __VERSION__); return 0; }\n' > "$A10/tests/t.cpp"
out=$(cd "$A10" && MCPP_TOOLCHAIN=llvm@22.1.8 "$MCPP" test 2>&1); rc=$?
[ "$rc" -eq 0 ] && [[ "$out" == *"compiler=Clang 22.1.8"* ]]
verdict $? "A10 MCPP_TOOLCHAIN=llvm@22.1.8 mcpp test compiles the test with clang" "exit=$rc"
out=$(cd "$A10" && MCPP_TOOLCHAIN=llvm@22.1.8 "$MCPP" pack --format dir 2>&1); rc=$?
[ "$rc" -eq 0 ] && [[ "$out" == *"Resolved llvm@22.1.8"* ]]
verdict $? "A10 MCPP_TOOLCHAIN=llvm@22.1.8 mcpp pack resolves llvm" "exit=$rc"
for c in run test pack; do
    out=$(cd "$A10" && "$MCPP" "$c" --toolchain llvm@22.1.8 2>&1)
    [[ "$out" == *"unknown option: --toolchain"* ]]
    verdict $? "A10 mcpp $c refuses --toolchain" "$(printf '%s' "$out" | head -1)"
done

# ── Revision 2 ────────────────────────────────────────────────────────────────

# A1: a dependency that declares `kind = "shared"` is linked shared with no
# request from the root, and the degradation for an explicit `static` request
# does not name the package's statement.
R1=$WORK/r2-kind; rm -rf "$R1"; mkdir -p "$R1/fw/src" "$R1/app/src" "$R1/appstatic/src"
cat > "$R1/fw/mcpp.toml" <<'T'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "shared"
T
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$R1/fw/src/fw.cppm"
for a in app appstatic; do
    printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$R1/$a/src/main.cpp"
done
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fw" }\n' > "$R1/app/mcpp.toml"
printf '[package]\nname = "appstatic"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fw", linkage = "static" }\n' > "$R1/appstatic/mcpp.toml"
(cd "$R1/app" && "$MCPP" build > build.log 2>&1); rc=$?
so=$(find "$R1/app/target" -name libfw.so 2>/dev/null | head -1)
[ "$rc" -eq 0 ] && [ -n "$so" ]
verdict $? "A1 a kind = \"shared\" dependency is linked shared under a default request" "exit=$rc so=${so:-none}"
out=$(cd "$R1/appstatic" && "$MCPP" build 2>&1)
[[ "$out" == *"is linked as a shared library: the requested form is not available here"* ]]
verdict $? "A1 the explicit-static degradation does not name the package's kind" "$(printf '%s' "$out" | grep -m1 warning)"

# A6: test programs on the Android row need libc++_shared.so; -static-libstdc++
# removes it; the driver names its static archives for the effective target.
R6=$WORK/r2-a6; rm -rf "$R6"
for v in default static; do
    d="$R6/$v"; mkdir -p "$d/src" "$d/tests"
    printf '[package]\nname = "t6"\nversion = "0.1.0"\n[target.x86_64-linux-android]\nmin_api_level = 24\nrunner = ["/bin/false"]\n' > "$d/mcpp.toml"
    [ "$v" = static ] && printf '[target.x86_64-linux-android.build]\nldflags = ["-static-libstdc++"]\n' >> "$d/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$d/src/main.cpp"
    printf '#include <string>\nint main() { std::string s = "x"; return s.size() == 1 ? 0 : 1; }\n' > "$d/tests/t.cpp"
    (cd "$d" && "$MCPP" test --target x86_64-linux-android > test.log 2>&1)
done
nd=$(readelf -d "$(find "$R6/default/target" -type f -name t | head -1)" 2>/dev/null | grep -c 'libc++_shared.so')
ns=$(readelf -d "$(find "$R6/static/target" -type f -name t | head -1)" 2>/dev/null | grep -c 'libc++_shared.so')
[ "$nd" -eq 1 ] && [ "$ns" -eq 0 ]
verdict $? "A6 Android test programs need libc++_shared.so unless -static-libstdc++" "default=$nd static=$ns"
NDKCXX=$(find "$HOME/.mcpp/registry/data/xpkgs/xim-x-android-ndk" -path '*/bin/clang++' 2>/dev/null | head -1)
if [ -n "$NDKCXX" ]; then
    a=$("$NDKCXX" --target=x86_64-linux-android24 -print-file-name=libc++.a)
    [[ "$a" == */24/libc++.a ]] && grep -q 'c++_static' "$a"
    verdict $? "A6 the NDK driver names its API-level libc++.a linker script" "$a"
fi

# A9: a dependency's build program states a fact and a floor; the engine
# refuses a lower root floor before compiling.
R9=$WORK/r2-a9; rm -rf "$R9"; make_fw "$R9/fw"; mkdir -p "$R9/app/src"
cat > "$R9/fw/build.mcpp" <<'T'
import std;
import mcpp;
int main() {
    if (std::string_view(mcpp::target_env()).starts_with("android")) {
        mcpp::fact("demo.android-api", mcpp::min_platform_version());
        mcpp::floor("demo.android-api >= 23");
    }
    return 0;
}
T
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$R9/app/src/main.cpp"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\ndemo.fw = { path = "../fw" }\n[target.x86_64-linux-android]\nmin_api_level = 21\n' > "$R9/app/mcpp.toml"
out=$(cd "$R9/app" && "$MCPP" build --target x86_64-linux-android 2>&1); rc=$?
[ "$rc" -ne 0 ] && [[ "$out" == *"requires demo.android-api >= 23, and this machine has 21"* ]]
verdict $? "A9 a dependency's fact and floor refuse a root floor of 21" "exit=$rc"

# B1: `mcpp::link_flag` from a host module reaches the root program's link.
RB=$WORK/r2-b1; rm -rf "$RB"; mkdir -p "$RB/rules/src" "$RB/app/src"
printf '[package]\nnamespace = "demo"\nname = "rules"\nversion = "0.1.0"\n[targets.rules]\nkind = "lib"\n' > "$RB/rules/mcpp.toml"
printf 'export module demo.rules;\nimport std;\nimport mcpp;\nexport namespace demo::rules {\ninline void add() { mcpp::link_flag("-Wl,-rpath,/m634/marker/Frameworks"); }\n}\n' > "$RB/rules/src/rules.cppm"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies]\ndemo.rules = { path = "../rules", host-module = true }\n' > "$RB/app/mcpp.toml"
printf 'import std;\nimport mcpp;\nimport demo.rules;\nint main() { demo::rules::add(); return 0; }\n' > "$RB/app/build.mcpp"
printf 'int main() { return 0; }\n' > "$RB/app/src/main.cpp"
(cd "$RB/app" && "$MCPP" build > build.log 2>&1); rc=$?
rp=$(readelf -d "$(find "$RB/app/target" -type f -name app -perm -u+x | head -1)" 2>/dev/null | grep -E 'RPATH|RUNPATH')
[ "$rc" -eq 0 ] && [[ "$rp" == */m634/marker/Frameworks* ]]
verdict $? "B1 a host module's link_flag reaches the root program's link" "exit=$rc"

# X: `mcpp why deps` shows no path dependency, and resolution.json holds no graph.
out=$(cd "$R1/app" && "$MCPP" why deps 2>&1)
rj=$(find "$R1/app/target" -name resolution.json | head -1)
keys=$(python3 -c "import json,sys; print(sorted(json.load(open(sys.argv[1])).keys()))" "$rj" 2>/dev/null)
[[ "$out" == *"no mcpp.lock"* ]] && [[ "$keys" != *graph* ]]
verdict $? "X why deps reads only mcpp.lock and resolution.json has no graph" "keys=$keys"

# B5: dist-apk 0.9.3, when a mcpp-plugins checkout at 0.9.3 is given:
# a second pack drops the dependency's library; two ABIs are refused.
if [ -n "${PLUGINS_DIR:-}" ] && [ -d "$PLUGINS_DIR/tests/apk-consumer-shared" ]; then
    RP=$WORK/r2-b5; rm -rf "$RP"; mkdir -p "$RP"; cp -r "$PLUGINS_DIR" "$RP/plugins"
    F="$RP/plugins/tests/apk-consumer-shared"
    list() { unzip -l "$(find "$F/target" -name withlibs.apk | head -1)" 2>/dev/null | grep -c 'libapk-consumer-dep.so'; }
    (cd "$F" && "$MCPP" pack --target x86_64-linux-android --format apk > p1.log 2>&1); r1=$?; l1=$(list)
    (cd "$F" && "$MCPP" pack --target x86_64-linux-android --format apk > p2.log 2>&1); r2=$?; l2=$(list)
    [ "$r1" -eq 0 ] && [ "$r2" -eq 0 ] && [ "$l1" -eq 1 ] && [ "$l2" -eq 0 ]
    verdict $? "B5 a second dist-apk pack drops libapk-consumer-dep.so" "exits=$r1,$r2 libs=$l1,$l2"
    out=$(cd "$F" && "$MCPP" pack --target x86_64-linux-android --target aarch64-linux-android --format apk 2>&1); rc=$?
    [ "$rc" -ne 0 ] && [[ "$out" == *"no action claimed --format 'apk'"* ]]
    verdict $? "B5 two ABIs are refused with the engine's generic sentence" "exit=$rc"
else
    echo "skipped: B5 (set PLUGINS_DIR to a mcpp-plugins 0.9.3 checkout)"
fi

# B1 / A3: a link_flag rpath that begins with a Mach-O loader token is anchored
# to the package root, while `$ORIGIN` is not (the same code runs on every host).
RR=$WORK/r2-rpath; rm -rf "$RR"; cp -r "$RB" "$RR"; rm -rf "$RR/app/target"
sed -i 's|/m634/marker/Frameworks|@executable_path/../Frameworks|' "$RR/rules/src/rules.cppm"
(cd "$RR/app" && "$MCPP" build > build.log 2>&1); rc=$?
rp=$(readelf -d "$(find "$RR/app/target" -type f -name app -perm -u+x | head -1)" 2>/dev/null | grep -E 'RPATH|RUNPATH')
[ "$rc" -eq 0 ] && [[ "$rp" == *"$RR/app/@executable_path/../Frameworks"* ]]
verdict $? "B1 an @executable_path rpath from link_flag is anchored to the package root" "$(printf '%s' "$rp" | tr -s ' ' | cut -c1-200)"
