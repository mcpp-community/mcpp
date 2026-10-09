#!/usr/bin/env bash
# O8 / D19 probe: the Android app sandbox on an x86_64 emulator. The debuggable
# Termux APK gives an app uid and SELinux app domain that `run-as` can enter,
# which is where the hard-link rule the Termux report hit applies. The published
# x86_64 mcpp is static, so it runs there without Termux's own bootstrap.
# Prints only.
set -u
V="${MCPP_VERSION}"
EMU="$RUNNER_TEMP/termux-emu"
LOGS="$RUNNER_TEMP/termux-emu-logs"
mkdir -p "$EMU" "$LOGS"
S="$GITHUB_STEP_SUMMARY"
say() { echo "$*"; echo "$*" >> "$S"; }

case "${1:-}" in
download)
  apk_url=$(curl -fsSL -H "Authorization: Bearer ${GITHUB_TOKEN:-}" \
      https://api.github.com/repos/termux/termux-app/releases/latest \
    | python3 -c 'import json,sys; a=[x["browser_download_url"] for x in json.load(sys.stdin)["assets"] if x["name"].endswith("github-debug_x86_64.apk")]; print(a[0] if a else "")')
  echo "apk: $apk_url"
  curl -fsSL -o "$EMU/termux.apk" "$apk_url"
  curl -fsSL -o "$EMU/mcpp.tar.gz" \
    "https://github.com/mcpp-community/mcpp/releases/download/v$V/mcpp-$V-linux-x86_64.tar.gz"
  cp /etc/ssl/certs/ca-certificates.crt "$EMU/cacert.pem"
  cp "$GITHUB_WORKSPACE/.github/probe/termux_inner.sh" "$EMU/"
  ls -la "$EMU"
  ;;
run)
  say "## Termux app sandbox on an API 34 x86_64 emulator (mcpp $V)"
  adb wait-for-device
  say "- getenforce: $(adb shell getenforce | tr -d '\r')"
  adb install -r "$EMU/termux.apk" 2>&1 | tail -1
  for f in mcpp.tar.gz cacert.pem termux_inner.sh; do adb push "$EMU/$f" /data/local/tmp/ >/dev/null; done
  adb shell chmod 644 /data/local/tmp/mcpp.tar.gz /data/local/tmp/cacert.pem /data/local/tmp/termux_inner.sh
  A=/data/data/com.termux/files
  adb shell run-as com.termux sh -c "'mkdir -p $A/home $A/probe && cp /data/local/tmp/mcpp.tar.gz /data/local/tmp/cacert.pem /data/local/tmp/termux_inner.sh $A/probe/'" 2>&1
  adb shell run-as com.termux sh -c "'id; cat /proc/self/attr/current; echo; ls -la $A'" 2>&1 | tee -a "$LOGS/sandbox.txt"
  # The bash in termux_inner.sh is not there without Termux's bootstrap; run it
  # with the system sh, which handles everything the script uses.
  start=$(date +%s)
  timeout 5400 adb shell run-as com.termux sh -c \
    "'PROBE_OUT=$A/probe/out PROBE_TARBALL=$A/probe/mcpp.tar.gz PROBE_HOME=$A/home SSL_CERT_FILE=$A/probe/cacert.pem sh $A/probe/termux_inner.sh'" \
    2>&1 | tee "$LOGS/inner.log"
  say "- wall: $(( $(date +%s) - start )) s"
  for f in first-build.log; do
    adb shell run-as com.termux cat "$A/probe/out/$f" > "$LOGS/$f" 2>/dev/null
  done
  adb shell run-as com.termux sh -c "'cat $A/probe/out/mcpp-log/*.log'" > "$LOGS/mcpp.log" 2>/dev/null
  say '### sandbox'
  say '```'
  cat "$LOGS/sandbox.txt" >> "$S"
  grep -E '^(ENV|UNAME|ID|HARDLINK|MCPP|TIMING)' "$LOGS/inner.log" >> "$S"
  say '```'
  say '### first run: key lines'
  say '```'
  grep -E 'First run|Resolving|Installing|Downloading|error|warning|hint|Finished|glibc|Can.t create' \
    "$LOGS/inner.log" | head -60 >> "$S"
  say '```'
  say '### plan stage timings'
  say '```'
  grep 'build/stage' "$LOGS/mcpp.log" | tail -30 >> "$S"
  say '```'
  ;;
esac
exit 0
