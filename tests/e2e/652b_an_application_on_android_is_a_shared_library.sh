#!/usr/bin/env bash
# requires: elf gcc android-ndk
# 652b_an_application_on_android_is_a_shared_library.sh -- the row half of
# `kind = "app"` (#622 A3, design record §2.3): on `*-linux-android` an
# application's link form is `SharedObject`, not `Executable`. Split from 652
# because it needs the NDK payload (`xim:android-ndk`) rather than the host
# ELF/GCC toolchain, and a script that needed it would skip on every runner
# that lacks the ~700 MB payload -- this repository has paid for that mistake
# once already (`# requires: llvm` ran on no CI shard while reporting green).
#
# `run_all.sh` gates this on the `android-ndk` capability, detected the same
# way `qemu-arm`/`nasm`/`scan-deps` are: a payload path probe, not a PATH
# probe, because a distro-supplied Android tool would answer the wrong
# question. `MCPP_NO_AUTO_INSTALL` is left UNSET on purpose: the point of this
# script is that the row's own toolchain resolution reaches the payload
# already on this machine, exactly as an end user's first `--target
# x86_64-linux-android` build would.
#
# Section 4/5 below also does one HOST build (`elf gcc`, added to the header)
# to hold the negative direction of #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION
# is empty there, and numeric on the Android row.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

TARGET=x86_64-linux-android

cd "$TMP"
mkdir -p src
cat > mcpp.toml <<'TOML'
[package]
name    = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "app"
main = "src/main.cpp"

[targets.tool]
kind = "bin"
main = "src/tool.cpp"
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP
cat > src/tool.cpp <<'CPP'
int main() { return 0; }
CPP

"$MCPP" build --target "$TARGET" > build.log 2>&1 \
    || fail "the Android build failed" build.log
outdir=$(dirname "$(ls -t target/$TARGET/*/build.ninja | head -1)")

# ── 1. `app` on Android links as `shared` does: lib<name>.so, not <name> ───
[ -f "$outdir/bin/libmyapp.so" ] \
    || fail "libmyapp.so was not linked" build.log
[ ! -e "$outdir/bin/myapp" ] \
    || fail "bin/myapp exists -- an Android app must have NO executable form" build.log
file "$outdir/bin/libmyapp.so" | grep -qi "shared object" \
    || fail "bin/libmyapp.so is not a shared object" build.log
echo "app links as lib<name>.so on Android OK"

# ── 2. `bin` keeps meaning binary: `tool` links as an ordinary executable ──
[ -f "$outdir/bin/tool" ] || fail "bin/tool was not linked" build.log
file "$outdir/bin/tool" | grep -qi "pie executable" \
    || fail "bin/tool is not an executable (bin must be unaffected by A3)" build.log
echo "bin keeps meaning binary on Android OK"

# ── 3. `mcpp run` of the app, with no --format, is refused naming it ───────
out=$("$MCPP" run myapp --target "$TARGET" 2>&1) \
    && fail "mcpp run of an app whose form is a library must be refused" <(echo "$out")
expected="error: 'myapp' is an application, and on $TARGET an application is a shared library that a package installs. Run it through a distributable: mcpp run --format <name>, where <name> is one of: none declared"
grep -qF "$expected" <<<"$out" \
    || fail "the refusal sentence does not match the design record's exact wording" <(echo "$out")
echo "mcpp run of the app without --format is refused, naming --format OK"

# Negative-direction control for (3): `tool` is a real Binary on this same
# row, so it is not caught by the app-only refusal above.
out=$("$MCPP" run tool --target "$TARGET" --no-runner 2>&1) || true
if grep -q "is an application" <<<"$out"; then
    fail "the app refusal fired for a plain bin target" <(echo "$out")
fi
echo "the refusal does not fire for a bin target OK"

# ── 4. the pack pipeline treats this app as the program target too ────────
# (#622 A10, "the second half"): an `Application` is a program target on
# EVERY row, whatever file it links to; on Android that file is
# `lib/libmyapp.so`, and the closure stages a shared object there. Added
# AFTER the checks above, and only now, because the build program below
# declares `provides_pack_format("blob")` UNCONDITIONALLY (§1 rule 3) --
# adding it earlier would change what check 3's refusal lists (it asserts
# "one of: none declared", which is only true while this package provides
# no format at all).
printf "resource-1\n" > res.txt
cat > copy.sh <<'EOF'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
EOF
chmod +x copy.sh

cat > build.mcpp <<'EOF'
import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    // #622 A11: written on EVERY build, host and Android alike, so this one
    // build program's environment answers both halves of the criterion --
    // numeric on Android, empty on the host -- without a second fixture.
    if (FILE* f = std::fopen(
            (std::string(mcpp::out_dir()) + "/minplat.txt").c_str(), "w")) {
        std::fputs(mcpp::min_platform_version(), f);
        std::fclose(f);
    }

    // A deploy'd file (#622 A4) travels with the library: staged under
    // `bin/<to>/` on this row as on every other, where a provider that maps
    // it into its own layout (dist-apk: assets/) reads it.
    mcpp::deploy((std::string(mcpp::manifest_dir()) + "/res.txt").c_str(), "myres");

    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;

    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/myapp.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    // The FILE, whatever it is on this row -- `${mcpp.target_file:myapp}`
    // resolves through the same link-unit table on every row (mcpp.build.plan),
    // so nothing here asks the row's form at all.
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:myapp}")
     .arg(out.c_str())
     .input("${mcpp.target_file:myapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF

"$MCPP" pack --format blob --target "$TARGET" > pack.log 2>&1 \
    || fail "mcpp pack --format blob on Android failed" pack.log
staged=$(ls -d target/dist/myapp-0.1.0-*/ 2>/dev/null | head -1)
[ -n "$staged" ] || fail "no staged tree under target/dist for the Android pack" pack.log
[ -f "${staged}lib/libmyapp.so" ] \
    || fail "the staged tree has no lib/libmyapp.so" pack.log
[ -f "${staged}bin/myres/res.txt" ] \
    || fail "the deploy'd file was not staged under bin/myres/ on the Android row" pack.log
[ -n "$(find target -name 'myapp.blob' 2>/dev/null)" ] \
    || fail "the reported artifact myapp.blob does not exist" pack.log
echo "mcpp pack --format blob on Android stages lib/libmyapp.so OK"

# ── 5. MCPP_TARGET_MIN_PLATFORM_VERSION: numeric on Android, empty on host ─
# `mcpp::out_dir()` for a build PROGRAM (as opposed to `${mcpp.out_dir}` inside
# an action, which is per-triple) is one path per package,
# `target/.build-mcpp/out/` -- read the value RIGHT AFTER the Android pack
# above, before the host build below writes the SAME file with its own answer.
minplatFile="target/.build-mcpp/out/minplat.txt"
[ -f "$minplatFile" ] || fail "build.mcpp did not write $minplatFile" pack.log
androidMinplat=$(cat "$minplatFile")
case "$androidMinplat" in
    ''|*[!0-9]*) fail "MCPP_TARGET_MIN_PLATFORM_VERSION '$androidMinplat' is not numeric on $TARGET" pack.log ;;
esac
echo "MCPP_TARGET_MIN_PLATFORM_VERSION is numeric on $TARGET ($androidMinplat) OK"

"$MCPP" build > buildhost.log 2>&1 || fail "the host build failed" buildhost.log
hostMinplat=$(cat "$minplatFile" 2>/dev/null || true)
[ -z "$hostMinplat" ] \
    || fail "MCPP_TARGET_MIN_PLATFORM_VERSION was '$hostMinplat', not empty, on the host" buildhost.log
echo "MCPP_TARGET_MIN_PLATFORM_VERSION is empty on the host OK"

echo "652b: kind = \"app\" on Android is a shared library OK"
