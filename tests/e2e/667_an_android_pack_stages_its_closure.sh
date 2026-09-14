#!/usr/bin/env bash
# requires: elf gcc android-ndk
# 667 -- #634 A3 and A4 on the Android rows. `mcpp pack` reads the application
# object's closure from the files and stages it beside the object under `lib/`:
# a graph-built dependency and the NDK's `libc++_shared.so` travel, and a name
# the API level's stub directory holds (`libc.so`, `libm.so`, `libdl.so`) is the
# device's. The stage manifest says so in `needs` lines. Before this, the tree
# held `lib/libapp.so` alone and the manifest said `closure = walked`.
#
# Every ELF shared library carries a SONAME equal to its file name (A4), which
# bionic enforces from API level 23.
#
# The negative directions: a library the link used and that is gone by the
# time of the pack makes `dir` and `tar` refuse naming it, and a dispatched
# format receives `closure = not-walked` with the name as `unresolved`.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
TAB=$(printf '\t')

cd "$TMP"
mkdir -p fw/src app/src
cat > fw/mcpp.toml <<'EOF'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
EOF
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > fw/src/fw.cppm

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind = "app"
main = "src/main.cpp"
[dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
EOF
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > app/src/main.cpp
cd app

# ── 1. one triple: the closure is staged under lib/ ───────────────────────
"$MCPP" pack --target x86_64-linux-android --format dir > dir.log 2>&1 \
    || fail "mcpp pack --format dir failed" dir.log
tree=target/dist/app-0.1.0-x86_64-linux-android
[ -d "$tree" ] || fail "no staged tree at $tree" dir.log
got=$(cd "$tree/lib" && find . -type f | sort | tr '\n' ' ')
[ "$got" = "./libapp.so ./libc++_shared.so ./libfw.so " ] \
    || fail "lib/ holds '$got', not the object and its two libraries" dir.log

manifest="$tree.stage-manifest"
[ "$(sed -n '1p' "$manifest")" = "closure = walked" ] \
    || fail "the manifest's first line is not 'closure = walked'" "$manifest"
for line in "needs${TAB}libfw.so${TAB}lib/libfw.so" \
            "needs${TAB}libc++_shared.so${TAB}lib/libc++_shared.so" \
            "needs${TAB}libc.so${TAB}platform" \
            "needs${TAB}libm.so${TAB}platform" \
            "needs${TAB}libdl.so${TAB}platform"; do
    grep -qxF "$line" "$manifest" || fail "the manifest lacks the line '$line'" "$manifest"
done
echo "  ok: lib/ carries libfw.so and libc++_shared.so; the stubs are the platform's"

for so in libapp.so libfw.so; do
    soname=$(readelf -d "$tree/lib/$so" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
    [ "$soname" = "$so" ] || fail "lib/$so carries SONAME '$soname'" dir.log
done
readelf -d "$tree/lib/libapp.so" | grep -q 'NEEDED.*\[libfw.so\]' \
    || fail "libapp.so no longer needs libfw.so by its file name" dir.log
echo "  ok: libapp.so and libfw.so carry their file names as SONAME"

# ── 2. a prebuilt library the link named travels, and its absence refuses ──
NDKCC=$(ls "$MCPP_HOME"/registry/data/xpkgs/xim-x-android-ndk/*/toolchains/llvm/prebuilt/*/bin/clang 2>/dev/null | head -1)
[ -x "$NDKCC" ] || fail "no NDK clang under $MCPP_HOME/registry"
mkdir -p ext/lib
printf 'int ext_value(void) { return 1; }\n' > ext/ext.c
"$NDKCC" --target=x86_64-linux-android24 -shared -fPIC -Wl,-soname,libext.so \
    -o ext/lib/libext.so ext/ext.c
cat >> mcpp.toml <<'EOF'
[target.'cfg(env = "android")'.runtime]
link_library_dirs = ["ext/lib"]
libraries = ["ext"]
EOF
printf 'import fw;\nextern "C" int ext_value();\nint main() { return fw_anchor() == 41 && ext_value() == 1 ? 0 : 1; }\n' > src/main.cpp

"$MCPP" pack --target x86_64-linux-android --format dir > ext.log 2>&1 \
    || fail "the pack with a prebuilt library failed" ext.log
[ -f "$tree/lib/libext.so" ] || fail "the prebuilt libext.so is not staged" ext.log
grep -qxF "needs${TAB}libext.so${TAB}lib/libext.so" "$manifest" \
    || fail "the manifest does not state libext.so as staged" "$manifest"
echo "  ok: a library from [runtime] link_library_dirs is staged"

# The build is up to date, so the pack does not relink; the file is simply gone.
rm ext/lib/libext.so
for fmt in dir tar; do
    if "$MCPP" pack --target x86_64-linux-android --format "$fmt" > "gone-$fmt.log" 2>&1; then
        fail "--format $fmt succeeded with libext.so gone" "gone-$fmt.log"
    fi
    grep -q "libext.so: found in none of" "gone-$fmt.log" \
        || fail "--format $fmt did not name libext.so" "gone-$fmt.log"
    grep -q "incomplete" "gone-$fmt.log" \
        || fail "--format $fmt did not say the closure is incomplete" "gone-$fmt.log"
done
echo "  ok: dir and tar refuse, naming the library that is gone"

# ── 3. a dispatched format receives the incomplete closure ────────────────
cat > dist.sh <<'EOF'
set -e
manifest="$1"; out="$2"
cp "$manifest" "$out"
EOF
chmod +x dist.sh
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    mcpp::provides_pack_format("copy");
    if (std::string_view(mcpp::pack_format()) != "copy") return 0;
    const std::string root  = mcpp::manifest_dir();
    const std::string stage = std::string("${mcpp.stage_dir}");
    const std::string out   = std::string(mcpp::out_dir()) + "/stage.txt";
    mcpp::action a;
    a.id          = "copy";
    a.role        = "artifact";
    a.description = "copy";
    a.arg("/bin/sh")
     .arg((root + "/dist.sh").c_str())
     .arg((stage + ".stage-manifest").c_str())
     .arg(out.c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF
"$MCPP" pack --target x86_64-linux-android --format copy > copy.log 2>&1 \
    || fail "the dispatched format failed" copy.log
seen=$(find target -name stage.txt | head -1)
[ -f "$seen" ] || fail "the provider wrote nothing" copy.log
[ "$(sed -n '1p' "$seen")" = "closure = not-walked" ] \
    || fail "the provider did not receive 'closure = not-walked'" "$seen"
grep -qxF "needs${TAB}libext.so${TAB}unresolved" "$seen" \
    || fail "the provider did not receive libext.so as unresolved" "$seen"
grep -qxF "needs${TAB}libfw.so${TAB}lib/libfw.so" "$seen" \
    || fail "the provider lost the members that did resolve" "$seen"
echo "  ok: a dispatched format receives not-walked, the unresolved name and the members"
rm -f build.mcpp dist.sh

# ── 4. two triples: each lib/<abi>/ carries its own closure ───────────────
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind = "app"
main = "src/main.cpp"
[dependencies]
demo.fw = { path = "../fw", linkage = "shared" }
EOF
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > src/main.cpp
rm -rf target/dist
"$MCPP" pack --target x86_64-linux-android --target aarch64-linux-android --format dir \
    > two.log 2>&1 || fail "the two-triple pack failed" two.log
tree2=$(find target/dist -mindepth 1 -maxdepth 1 -type d | head -1)
[ -n "$tree2" ] || fail "no staged tree for two triples" two.log
for abi in x86_64 arm64-v8a; do
    got=$(cd "$tree2/lib/$abi" && find . -type f | sort | tr '\n' ' ')
    [ "$got" = "./libapp.so ./libc++_shared.so ./libfw.so " ] \
        || fail "lib/$abi holds '$got'" two.log
    grep -qxF "needs${TAB}libfw.so${TAB}lib/$abi/libfw.so" "$tree2.stage-manifest" \
        || fail "the manifest lacks lib/$abi/libfw.so" "$tree2.stage-manifest"
done
file "$tree2/lib/arm64-v8a/libc++_shared.so" | grep -qi "aarch64" \
    || fail "lib/arm64-v8a/libc++_shared.so is not the aarch64 runtime" two.log
echo "  ok: lib/x86_64/ and lib/arm64-v8a/ each carry their own closure"

echo "667: an Android pack stages its closure OK"
