#!/usr/bin/env bash
# A fresh mcpp in Termux: `mcpp new` and `mcpp run` with the default toolchain
# (design 2026-10-10 §13, D27). Two places, because neither has both halves
# of a phone:
#
#   docker <tarball>          termux/termux-docker (aarch64): Termux's bionic
#                             userland and paths, on a native arm64 runner. It
#                             has no `/bin`; Android 10 and later do
#                             (`/bin -> /system/bin`), which is mcpp's baseline,
#                             so the link is made first. No SELinux.
#   emulator-download <tarball>, then emulator-run
#                             An Android 14 emulator (x86_64, KVM) with the
#                             debuggable Termux APK; commands run through
#                             `run-as com.termux`, in the app's SELinux domain,
#                             where link(2) is refused -- the half that made
#                             every install of the managed glibc fail.
#
# <tarball> is a release archive (`mcpp-<v>-linux-<arch>.tar.gz`) or a URL.
# Exits non-zero unless the program built and printed its line.
set -euo pipefail
MODE="${1:?mode}"
WORK="${RUNNER_TEMP:-/tmp}/termux-fresh"
mkdir -p "$WORK/logs"
S="${GITHUB_STEP_SUMMARY:-/dev/null}"
WANT="hello from termux"

fetch() {  # $1 = tarball path or URL, $2 = destination
    case "$1" in
        http*) curl -fsSL --retry 3 -o "$2" "$1" ;;
        *) cp "$1" "$2" ;;
    esac
}

# The script that runs inside Termux. POSIX sh: under run-as there is only the
# system shell, and in termux-docker it is the same file.
cat > "$WORK/inside.sh" <<'SH'
#!/bin/sh
set -u
TARBALL="$1"; HOME="$2"; export HOME
export TMPDIR="$HOME/tmp"; mkdir -p "$TMPDIR" "$HOME/dist"
tar -xzf "$TARBALL" -C "$HOME/dist"
MCPP=$(find "$HOME/dist" -path '*/bin/mcpp' -type f | head -1)
"$MCPP" --version
"$MCPP" self env --format json | grep -o '"android":[a-z]*'
cd "$HOME" && rm -rf hello && "$MCPP" new hello >/dev/null
cd hello
printf 'import std;\nint main() { std::println("hello from termux"); }\n' > src/main.cpp
timeout 5400 "$MCPP" run
SH
chmod 755 "$WORK/inside.sh"

case "$MODE" in
docker)
    fetch "${2:?tarball}" "$WORK/mcpp.tar.gz"
    chmod -R a+rX "$WORK"; chmod 777 "$WORK/logs"
    docker pull -q termux/termux-docker:aarch64
    set +e
    docker run --rm --privileged --entrypoint /system/bin/sh \
        -v "$WORK:/mnt/w" termux/termux-docker:aarch64 \
        -c 'ln -s /system/bin /bin && exec /entrypoint.sh sh /mnt/w/inside.sh /mnt/w/mcpp.tar.gz /data/data/com.termux/files/home' \
        2>&1 | tee "$WORK/logs/docker.log"
    set -e
    ;;
emulator-download)
    fetch "${2:?tarball}" "$WORK/mcpp.tar.gz"
    curl -fsSL --retry 3 -o "$WORK/termux.apk" \
        "https://github.com/termux/termux-app/releases/download/v0.118.3/termux-app_v0.118.3+github-debug_x86_64.apk"
    exit 0
    ;;
emulator-run)
    A=/data/data/com.termux/files
    adb wait-for-device
    adb install -r "$WORK/termux.apk" >/dev/null
    adb shell am start -n com.termux/com.termux.app.TermuxActivity >/dev/null
    for _ in $(seq 1 60); do
        adb shell run-as com.termux ls "$A/usr/bin/sh" >/dev/null 2>&1 && break
        sleep 5
    done
    for f in mcpp.tar.gz inside.sh; do
        adb push "$WORK/$f" /data/local/tmp/ >/dev/null
        adb shell chmod 644 "/data/local/tmp/$f"
        adb shell run-as com.termux cp "/data/local/tmp/$f" "$A/$f"
    done
    echo "SELinux: $(adb shell getenforce | tr -d '\r'); context: $(adb shell run-as com.termux cat /proc/self/attr/current | tr -d '\r\0')" \
        | tee -a "$S"
    set +e
    adb shell run-as com.termux "$A/usr/bin/env" PREFIX="$A/usr" PATH="$A/usr/bin" \
        LANG=en_US.UTF-8 sh "$A/inside.sh" "$A/mcpp.tar.gz" "$A/home" 2>&1 \
        | tr -d '\r' | tee "$WORK/logs/emulator.log"
    set -e
    ;;
*) echo "unknown mode $MODE"; exit 2 ;;
esac

log=$(ls "$WORK"/logs/*.log | head -1)
{ echo "### Termux ($MODE)"; echo '```'; tail -25 "$log"; echo '```'; } >> "$S"
grep -qx "$WANT" "$log" || { echo "FAIL: the program did not print '$WANT'"; exit 1; }
echo "PASS: mcpp new + run in Termux ($MODE)"
