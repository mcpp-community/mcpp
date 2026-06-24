#!/usr/bin/env bash
# mcpp/install.sh — one-line installer.
#
# Usage:
#   curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
#
# Optional env knobs:
#   MCPP_VERSION   — pin a specific version (default: latest)
#   MCPP_PREFIX    — install root (default: $HOME/.mcpp)
#   MCPP_NO_PATH   — set to skip shell-rc PATH editing
#
# Layout afterwards (PREFIX = $HOME/.mcpp by default):
#   $PREFIX/bin/{mcpp,xlings}     ← binary + pinned bundled xlings
#   $PREFIX/registry/             ← seeded on first `mcpp` invocation
#   $PREFIX/...                   ← mcpp's full self-contained tree
#
# mcpp resolves MCPP_HOME from the binary's location, so $PREFIX IS the
# home — no env var is required after install.
set -euo pipefail

REPO="mcpp-community/mcpp"
VERSION="${MCPP_VERSION:-latest}"
PREFIX="${MCPP_PREFIX:-$HOME/.mcpp}"

# ---- platform detection ---------------------------------------------------
# OS+arch → release asset platform tag. Linux reports the arch as aarch64;
# macOS reports it as arm64 (Apple). Arch Linux et al. are distro-agnostic here
# (uname-based), so x86_64 and aarch64 Arch both work.
uname_s=$(uname -s)
uname_m=$(uname -m)
case "${uname_s}-${uname_m}" in
    Linux-x86_64)                 PLAT="linux-x86_64"  ;;
    Linux-aarch64 | Linux-arm64)  PLAT="linux-aarch64" ;;   # aarch64 / arm64 alias
    Darwin-arm64)                 PLAT="macosx-arm64"  ;;
    *)
        echo "error: unsupported platform ${uname_s}-${uname_m}." >&2
        echo "       Currently supported: linux-x86_64, linux-aarch64, macosx-arm64." >&2
        echo "       Build from source instead:" >&2
        echo "       https://github.com/${REPO}#从源码构建开发者" >&2
        exit 1
        ;;
esac

# ---- resolve + fetch (mirror-aware: GitHub GLOBAL → GitCode CN) ------------
# Most users hit GitHub. On networks where GitHub is blocked/slow (e.g. CN,
# Termux) we fall back to the GitCode mirror (xlings-res/mcpp), so the same
# one-liner works without a manual --mirror flag. MCPP_MIRROR=CN forces GitCode.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

GH_REL="https://github.com/${REPO}/releases"
GTC_API="https://api.gitcode.com/api/v5/repos/xlings-res/mcpp"
GTC_DL="https://gitcode.com/xlings-res/mcpp/releases/download"
CT=8   # connect timeout (s) for the GitHub probe before falling back

fetch_github() {
    local url
    if [[ "$VERSION" == "latest" ]]; then
        url="${GH_REL}/latest/download/mcpp-${PLAT}.tar.gz"   # versionless alias
    else
        url="${GH_REL}/download/v${VERSION}/mcpp-${VERSION}-${PLAT}.tar.gz"
    fi
    echo ":: Trying GitHub: ${url}"
    if curl --fail --location --silent --show-error \
            --connect-timeout "$CT" -o "$WORK/mcpp.tar.gz" "$url"; then
        SHA_URL="${url}.sha256"; SOURCE="GitHub"; return 0
    fi
    return 1
}

fetch_gitcode() {
    local ver="$VERSION"
    if [[ "$ver" == "latest" ]]; then
        # GitCode has no 'latest' redirect — resolve the highest release tag.
        # per_page=100: the default page (20) lists oldest-first and misses
        # recent versions, so a plain sort would pick a stale release.
        ver=$(curl --fail --location --silent --connect-timeout "$CT" \
                "${GTC_API}/releases?per_page=100" 2>/dev/null \
              | grep -oE '"tag_name":"[^"]+"' | sed 's/.*:"//; s/"$//' \
              | sort -V | tail -1)
        [[ -z "$ver" ]] && { echo "error: cannot resolve latest from GitCode" >&2; return 1; }
    fi
    local url="${GTC_DL}/${ver}/mcpp-${ver}-${PLAT}.tar.gz"
    echo ":: Trying GitCode (CN mirror): ${url}"
    if curl --fail --location --silent --show-error -o "$WORK/mcpp.tar.gz" "$url"; then
        SHA_URL="${url}.sha256"; SOURCE="GitCode"; VERSION="$ver"; return 0
    fi
    return 1
}

SOURCE="" SHA_URL=""
if [[ "${MCPP_MIRROR:-}" == "CN" ]]; then
    fetch_gitcode || fetch_github || { echo "error: download failed (GitCode + GitHub)" >&2; exit 1; }
else
    fetch_github || { echo ":: GitHub unavailable — falling back to GitCode (CN mirror)"; fetch_gitcode; } \
        || { echo "error: download failed (GitHub + GitCode)" >&2; exit 1; }
fi
echo ":: Downloaded from ${SOURCE}"
# .sha256 sidecar is GitHub-only; GitCode mirror ships tarballs without it.
curl --fail --location --silent --show-error -o "$WORK/mcpp.sha256" "$SHA_URL" 2>/dev/null || {
    echo "warning: no .sha256 sidecar (${SOURCE}), skipping verification" >&2
}

# ---- verify ---------------------------------------------------------------
if [[ -s "$WORK/mcpp.sha256" ]]; then
    expected=$(awk '{print $1}' "$WORK/mcpp.sha256")
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$WORK/mcpp.tar.gz" | awk '{print $1}')
    else
        actual=$(shasum -a 256 "$WORK/mcpp.tar.gz" | awk '{print $1}')
    fi
    if [[ "$expected" != "$actual" ]]; then
        echo "error: sha256 mismatch" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual"   >&2
        exit 1
    fi
    echo ":: sha256 verified"
fi

# ---- install --------------------------------------------------------------
# Tarball entries live under a single `<tarball-stem>/` directory so
# extraction lands a self-contained tree. We strip that wrapper here so
# files end up directly under $PREFIX (not $PREFIX/<stem>/...).
mkdir -p "$PREFIX"
echo ":: Extracting to $PREFIX"
tar -xzf "$WORK/mcpp.tar.gz" -C "$PREFIX" --strip-components=1

# ---- PATH integration -----------------------------------------------------
if [[ -z "${MCPP_NO_PATH:-}" ]]; then
    rc=""
    case "${SHELL##*/}" in
        bash) rc="$HOME/.bashrc" ;;
        zsh)  rc="${ZDOTDIR:-$HOME}/.zshrc" ;;
        fish) rc="$HOME/.config/fish/config.fish" ;;
    esac
    if [[ -n "$rc" ]]; then
        if [[ "${SHELL##*/}" == "fish" ]]; then
            line="set -gx PATH \"$PREFIX/bin\" \$PATH"
        else
            line="export PATH=\"$PREFIX/bin:\$PATH\""
        fi
        mkdir -p "$(dirname "$rc")"
        if ! grep -Fqs "$PREFIX/bin" "$rc" 2>/dev/null; then
            printf '\n# mcpp\n%s\n' "$line" >> "$rc"
            echo ":: Added $PREFIX/bin to PATH via $rc"
        else
            echo ":: $PREFIX/bin already on PATH in $rc"
        fi
    else
        echo ":: Unknown shell — add this to your shell rc manually:"
        echo "     export PATH=\"$PREFIX/bin:\$PATH\""
    fi
fi

# ---- verify install -------------------------------------------------------
echo
"$PREFIX/bin/mcpp" --version
echo
echo "✓ mcpp installed at $PREFIX"
if [[ -n "${rc:-}" ]]; then
    echo "  Open a new shell (or 'source $rc') and run:  mcpp --help"
else
    echo "  Add $PREFIX/bin to your PATH, then run:  mcpp --help"
fi
