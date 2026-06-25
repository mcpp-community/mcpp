#!/usr/bin/env bash
# scripts/aur/update.sh — bump BOTH AUR packages to a release version.
#
#   mcpp-bin/   prebuilt release binaries (per-arch tarball checksums)
#   mcpp/       built from source via mcpp-bin (source-archive checksum)
#
# Pulls checksums straight from the GitHub release / archive, rewrites each
# PKGBUILD's pkgver + sums, resets pkgrel to 1, regenerates both .SRCINFO
# files, and keeps the shared mcpp.sh wrapper in sync across both dirs.
# Run after a release is published, then publish each package (see README.md).
#
# Usage:
#   scripts/aur/update.sh [VERSION]      # default: [package].version from mcpp.toml
set -euo pipefail

cd "$(dirname "$0")"
REPO="mcpp-community/mcpp"
ARCHES=(linux-x86_64 linux-aarch64)

# Resolve version: explicit arg, else mcpp.toml at repo root.
if [[ $# -ge 1 ]]; then
    VER="$1"
else
    VER=$(grep -m1 -E '^\s*version\s*=' ../../mcpp.toml | sed -E 's/.*"([^"]+)".*/\1/')
fi
[[ -n "$VER" ]] || { echo "error: could not determine version" >&2; exit 1; }
echo ":: targeting mcpp v${VER}"

reldl="https://github.com/${REPO}/releases/download/v${VER}"
archive_url="https://github.com/${REPO}/archive/v${VER}.tar.gz"

# --- prebuilt binary checksums (mcpp-bin) ----------------------------------
declare -A SUMS
for plat in "${ARCHES[@]}"; do
    url="${reldl}/mcpp-${VER}-${plat}.tar.gz.sha256"
    echo ":: fetching ${url}"
    line=$(curl -fsSL --connect-timeout 15 "$url") \
        || { echo "error: cannot fetch sha256 for ${plat} (is v${VER} released?)" >&2; exit 1; }
    SUMS[$plat]=$(awk '{print $1}' <<<"$line")
    [[ -n "${SUMS[$plat]}" ]] || { echo "error: empty sha256 for ${plat}" >&2; exit 1; }
done
x86=${SUMS[linux-x86_64]}
arm=${SUMS[linux-aarch64]}

# --- source-archive checksum (mcpp) ----------------------------------------
echo ":: hashing source archive ${archive_url}"
src=$(curl -fsSL --connect-timeout 30 "$archive_url" | sha256sum | awk '{print $1}')
[[ -n "$src" ]] || { echo "error: cannot hash source archive" >&2; exit 1; }

# --- keep the wrapper in sync ----------------------------------------------
cp -f mcpp-bin/mcpp.sh mcpp/mcpp.sh 2>/dev/null || true

# --- rewrite mcpp-bin/PKGBUILD ---------------------------------------------
sed -i -E \
    -e "s/^pkgver=.*/pkgver=${VER}/" \
    -e "s/^pkgrel=.*/pkgrel=1/" \
    -e "s/^sha256sums_x86_64=\('[^']*'\)/sha256sums_x86_64=('${x86}')/" \
    -e "s/^sha256sums_aarch64=\('[^']*'\)/sha256sums_aarch64=('${arm}')/" \
    mcpp-bin/PKGBUILD

# --- rewrite mcpp/PKGBUILD -------------------------------------------------
sed -i -E \
    -e "s/^pkgver=.*/pkgver=${VER}/" \
    -e "s/^pkgrel=.*/pkgrel=1/" \
    -e "s/^sha256sums=\('[^']*'\)/sha256sums=('${src}')/" \
    mcpp/PKGBUILD

# --- regenerate .SRCINFO files ---------------------------------------------
# Prefer makepkg's own generator on an Arch host; fall back to templates so
# this also works when bumping from a non-Arch machine (e.g. CI / dev box).
_srcinfo() {  # $1 = package dir
    # CI runs as root where makepkg refuses to run; MCPP_AUR_NO_MAKEPKG=1
    # forces the template path (byte-identical output, no makepkg needed).
    if [[ -z "${MCPP_AUR_NO_MAKEPKG:-}" ]] && command -v makepkg >/dev/null 2>&1; then
        ( cd "$1" && makepkg --printsrcinfo > .SRCINFO )
        return
    fi
    case "$1" in
        mcpp-bin)
            cat > mcpp-bin/.SRCINFO <<EOF
pkgbase = mcpp-bin
	pkgdesc = Modern C++ build & package management tool (prebuilt binary)
	pkgver = ${VER}
	pkgrel = 1
	url = https://github.com/${REPO}
	arch = x86_64
	arch = aarch64
	license = Apache-2.0
	depends = git
	provides = mcpp
	conflicts = mcpp
	options = !strip
	source = mcpp.sh
	sha256sums = SKIP
	source_x86_64 = mcpp-${VER}-linux-x86_64.tar.gz::${reldl}/mcpp-${VER}-linux-x86_64.tar.gz
	sha256sums_x86_64 = ${x86}
	source_aarch64 = mcpp-${VER}-linux-aarch64.tar.gz::${reldl}/mcpp-${VER}-linux-aarch64.tar.gz
	sha256sums_aarch64 = ${arm}

pkgname = mcpp-bin
EOF
            ;;
        mcpp)
            cat > mcpp/.SRCINFO <<EOF
pkgbase = mcpp
	pkgdesc = Modern C++ build & package management tool (built from source)
	pkgver = ${VER}
	pkgrel = 1
	url = https://github.com/${REPO}
	arch = x86_64
	arch = aarch64
	license = Apache-2.0
	makedepends = mcpp-bin
	makedepends = git
	depends = git
	conflicts = mcpp-bin
	options = !strip
	source = mcpp-${VER}.tar.gz::${archive_url}
	source = mcpp.sh
	sha256sums = ${src}
	sha256sums = SKIP

pkgname = mcpp
EOF
            ;;
    esac
}
_srcinfo mcpp-bin
_srcinfo mcpp

echo ":: updated to v${VER}"
echo "   mcpp-bin  x86_64   ${x86}"
echo "   mcpp-bin  aarch64  ${arm}"
echo "   mcpp      source   ${src}"
echo ":: review, then publish each package (see scripts/aur/README.md)"
