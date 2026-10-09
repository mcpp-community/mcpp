#!/usr/bin/env bash
# Android app sandbox probe (emulator, debuggable Termux APK, run-as).
# Round 6: can llvm@23.1.3 + the managed glibc run inside the sandbox once the
# payloads carry no hard links, does the latest xlings unpack hard links there,
# and is the static-binary exec failure an artifact of run-as? Prints only.
set -u
V="${MCPP_VERSION}"
XV="${XLINGS_LATEST:-2026.10.10.1}"
ARCH="${EMU_ARCH:-x86_64}"
EMU="$RUNNER_TEMP/termux-emu"
LOGS="$RUNNER_TEMP/termux-emu-logs"
mkdir -p "$EMU" "$LOGS"
S="$GITHUB_STEP_SUMMARY"
say() { echo "$*"; echo "$*" >> "$S"; }
A=/data/data/com.termux/files
case "$ARCH" in x86_64) APKARCH=x86_64; MARCH=x86_64;; *) APKARCH=arm64-v8a; MARCH=aarch64;; esac

case "${1:-}" in
download)
  curl -fsSL -o "$EMU/termux.apk" \
    "https://github.com/termux/termux-app/releases/download/v0.118.3/termux-app_v0.118.3+github-debug_${APKARCH}.apk"
  curl -fsSL -o "$EMU/mcpp.tar.gz" \
    "https://github.com/mcpp-community/mcpp/releases/download/v$V/mcpp-$V-linux-$MARCH.tar.gz"
  curl -fsSL -o "$EMU/xlings.tar.gz" \
    "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-$MARCH.tar.gz"
  cp "$GITHUB_WORKSPACE"/.github/probe/termux_inner.sh "$GITHUB_WORKSPACE"/.github/probe/termux_feasibility.sh "$EMU/"
  # Install llvm@23.1.3 + glibc on THIS host at the exact path the app sandbox
  # uses, so every absolute path the install writes is the device's, then pack
  # it with hard links dereferenced -- the state D19 would produce.
  sudo mkdir -p "$A" && sudo chown -R "$(id -u):$(id -g)" /data/data/com.termux
  mkdir -p "$A/homeP/dist" "$A/homeP/hello/src"
  tar -xzf "$EMU/mcpp.tar.gz" -C "$A/homeP/dist"
  D=$(ls -d "$A"/homeP/dist/mcpp-*)
  printf '[package]\nname = "hello"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "llvm@23.1.3"\n\n[targets.hello]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$A/homeP/hello/mcpp.toml"
  printf 'import std;\nint main() { std::println("host-built hello runs on android"); }\n' > "$A/homeP/hello/src/main.cpp"
  (cd "$A/homeP/hello" && MCPP_HOME="$D" "$D/bin/mcpp" build 2>&1 | tail -4)
  find "$A/homeP" -type f -links +1 | wc -l | sed 's/^/hard-linked files before packing: /'
  tar --hard-dereference -czf "$EMU/prebuilt.tgz" -C "$A" homeP
  ls -la "$EMU"
  ;;
run)
  say "## Android app sandbox ($ARCH emulator), round 6"
  adb wait-for-device
  say "- getenforce: $(adb shell getenforce | tr -d '\r'); release: $(adb shell getprop ro.build.version.release | tr -d '\r')"
  adb install -r "$EMU/termux.apk" 2>&1 | tail -1
  adb shell am start -n com.termux/com.termux.app.TermuxActivity 2>&1 | tail -1
  for i in $(seq 1 60); do adb shell run-as com.termux ls $A/usr/bin/bash >/dev/null 2>&1 && break; sleep 5; done
  say "- Termux bootstrap: $(adb shell run-as com.termux ls $A/usr/bin/bash 2>&1 | tr -d '\r')"
  for f in mcpp.tar.gz xlings.tar.gz prebuilt.tgz termux_inner.sh termux_feasibility.sh; do
    adb push "$EMU/$f" /data/local/tmp/ >/dev/null; adb shell chmod 644 /data/local/tmp/$f
  done
  RA() { adb shell run-as com.termux "$@"; }
  RA mkdir -p $A/probe
  for f in mcpp.tar.gz xlings.tar.gz termux_inner.sh termux_feasibility.sh; do RA cp /data/local/tmp/$f $A/probe/$f; done
  RA ls -la $A/probe
  T="PREFIX=$A/usr TMPDIR=$A/usr/tmp PATH=$A/usr/bin LANG=en_US.UTF-8"

  say '### F: llvm@23.1.3 + glibc inside the sandbox (payload without hard links)'
  RA $A/usr/bin/env $T HOME=$A/homeP sh -c "'cd $A && tar -xzf /data/local/tmp/prebuilt.tgz && echo extracted'" 2>&1 | tail -2
  D=$(RA sh -c "'ls -d $A/homeP/dist/mcpp-*'" | tr -d '\r')
  RA $A/usr/bin/env $T HOME=$A/homeP sh $A/probe/termux_feasibility.sh "$D" "$A/homeP/work" 2>&1 | tee "$LOGS/feas.log"
  say '```'; grep '^FEAS' "$LOGS/feas.log" >> "$S"; say '```'
  say "- host-built hello inside the sandbox: $(RA sh -c "'$A/homeP/hello/target/*/*/bin/hello 2>&1'" | tr -d '\r' | head -2)"

  say '### X: the latest xlings unpacking a payload with hard links'
  RA $A/usr/bin/env $T HOME=$A/homeX sh -c "'mkdir -p $A/homeX/dist $A/homeX/xl && tar -xzf $A/probe/mcpp.tar.gz -C $A/homeX/dist && tar -xzf $A/probe/xlings.tar.gz -C $A/homeX/xl && XL=\$(find $A/homeX/xl -path \"*/bin/xlings\" -type f | head -1) && D=\$(ls -d $A/homeX/dist/mcpp-*) && cp \$XL \$D/registry/bin/xlings && \$D/registry/bin/xlings --version && cd $A/homeX && XLINGS_HOME=\$D/registry \$D/registry/bin/xlings install xim:glibc@2.44.3 -y'" 2>&1 | tee "$LOGS/xlings.log" | tail -15
  say '```'; grep -E 'xlings [0-9]|Can.t create|error|installed|hard' "$LOGS/xlings.log" | head -12 >> "$S"; say '```'

  say '### E: termux-exec, with and without its linker exec mode'
  for mode in "" "TERMUX_EXEC__SYSTEM_LINKER_EXEC__MODE=disable"; do
    out=$(RA $A/usr/bin/env $T HOME=$A/homeP LD_PRELOAD=$A/usr/lib/libtermux-exec.so $mode $D/bin/mcpp --version 2>&1 | tr -d '\r' | head -2)
    say "- LD_PRELOAD=libtermux-exec.so ${mode:-<default mode>}: \`$out\`"
  done
  say "- termux-exec package: $(RA $A/usr/bin/env $T $A/usr/bin/dpkg -s termux-exec 2>&1 | grep -E '^Version' | tr -d '\r')"
  say "- process context under run-as: $(RA cat /proc/self/attr/current | tr -d '\r\0')"
  ;;
esac
exit 0
