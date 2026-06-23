#!/usr/bin/env bash
# 45_install_platform_mapping.sh — install.sh platform IDs must match release assets.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALL_SH="$ROOT/install.sh"
RELEASE_YML="$ROOT/.github/workflows/release.yml"

grep -q 'Darwin-arm64).*PLAT="macosx-arm64"' "$INSTALL_SH" || {
    echo "FAIL: Darwin-arm64 must map to the released macosx-arm64 asset"
    exit 1
}

# Linux aarch64 (also reported as arm64 on some kernels) → linux-aarch64 asset.
grep -qE 'Linux-aarch64 \| Linux-arm64\).*PLAT="linux-aarch64"' "$INSTALL_SH" || {
    echo "FAIL: Linux-aarch64 must map to the released linux-aarch64 asset"
    exit 1
}

! grep -q 'Darwin-x86_64).*PLAT=' "$INSTALL_SH" || {
    echo "FAIL: install.sh advertises Darwin-x86_64, but release CI publishes no macOS x86_64 asset"
    exit 1
}

grep -q 'Currently supported: linux-x86_64, linux-aarch64, macosx-arm64' "$INSTALL_SH" || {
    echo "FAIL: supported platform message must match published install assets"
    exit 1
}

grep -q 'mcpp-macosx-arm64.tar.gz' "$RELEASE_YML" || {
    echo "FAIL: release workflow must publish the macosx-arm64 alias used by install.sh"
    exit 1
}

grep -q 'mcpp-linux-aarch64.tar.gz' "$RELEASE_YML" || {
    echo "FAIL: release workflow must publish the linux-aarch64 alias used by install.sh"
    exit 1
}
