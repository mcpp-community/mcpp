#!/usr/bin/env bash
# O7 / O8 probe: the published aarch64 mcpp, first run, inside a Termux userland
# (termux/termux-docker on a native arm64 runner). The container has Termux's
# bionic userland and paths but not Android's SELinux, so it shows host
# recognition, the default toolchain and where Planning spends its time, not
# the app-sandbox hard-link rule (see termux_emulator.sh). Prints only.
set -u
V="${MCPP_VERSION}"
DL="$RUNNER_TEMP/termux-dl"
LOGS="$RUNNER_TEMP/termux-logs"
mkdir -p "$DL" "$LOGS"; chmod 777 "$LOGS"
S="$GITHUB_STEP_SUMMARY"
say() { echo "$*"; echo "$*" >> "$S"; }

curl -fsSL -o "$DL/mcpp.tar.gz" \
  "https://github.com/mcpp-community/mcpp/releases/download/v$V/mcpp-$V-linux-aarch64.tar.gz"
cp "$GITHUB_WORKSPACE/.github/probe/termux_inner.sh" "$DL/"
chmod -R a+rX "$DL"

say "## termux-docker aarch64 (mcpp $V)"
docker pull -q termux/termux-docker:aarch64 2>&1 | tail -1
docker image inspect termux/termux-docker:aarch64 --format '{{json .Config.Entrypoint}} {{json .Config.Cmd}} {{json .Config.User}}' | tee -a "$S"

start=$(date +%s)
docker run --rm --privileged \
  -v "$DL:/mnt/dl:ro" -v "$LOGS:/mnt/logs" \
  termux/termux-docker:aarch64 bash /mnt/dl/termux_inner.sh 2>&1 | tee "$LOGS/container.log"
say "- container exit: ${PIPESTATUS[0]}, wall: $(( $(date +%s) - start )) s"

say '### environment'
say '```'
grep -E "^(ENV|UNAME|ID|HARDLINK|XLINGS|MCPP)" "$LOGS/container.log" | head -40 >> "$S"
say '```'
say '### first run: key lines'
say '```'
grep -E 'First run|Resolving|Installing|Downloading|Planning|error|warning|hint|Finished|TIMING|glibc|llvm|musl' \
  "$LOGS/container.log" | grep -v '^\s*$' | head -60 >> "$S"
say '```'
say '### plan stage timings'
say '```'
grep -h 'build/stage' "$LOGS"/mcpp-log/*.log 2>/dev/null | tail -40 >> "$S"
say '```'
exit 0
