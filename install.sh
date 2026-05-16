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
uname_s=$(uname -s)
uname_m=$(uname -m)
case "${uname_s}-${uname_m}" in
    Linux-x86_64)   PLAT="linux-x86_64" ;;
    Darwin-arm64)   PLAT="darwin-arm64" ;;
    Darwin-x86_64)  PLAT="darwin-x86_64" ;;
    *)
        echo "error: unsupported platform ${uname_s}-${uname_m}." >&2
        echo "       Currently supported: linux-x86_64, darwin-arm64, darwin-x86_64." >&2
        echo "       Build from source instead:" >&2
        echo "       https://github.com/${REPO}#从源码构建开发者" >&2
        exit 1
        ;;
esac

# ---- resolve download URLs ------------------------------------------------
if [[ "$VERSION" == "latest" ]]; then
    BASE="https://github.com/${REPO}/releases/latest/download"
    ASSET_NAME_GLOB="mcpp-*-${PLAT}.tar.gz"   # `latest` redirect uses fixed asset name
    TARBALL_URL="${BASE}/mcpp-${PLAT}.tar.gz" # versionless alias served by the release
else
    BASE="https://github.com/${REPO}/releases/download/v${VERSION}"
    TARBALL_URL="${BASE}/mcpp-${VERSION}-${PLAT}.tar.gz"
fi
SHA_URL="${TARBALL_URL}.sha256"

# ---- fetch ----------------------------------------------------------------
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
echo ":: Downloading ${TARBALL_URL}"
curl --fail --location --silent --show-error -o "$WORK/mcpp.tar.gz"  "$TARBALL_URL"
curl --fail --location --silent --show-error -o "$WORK/mcpp.sha256" "$SHA_URL" || {
    echo "warning: no .sha256 sidecar found, skipping verification" >&2
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
echo "  Open a new shell (or 'source $rc') and run:  mcpp --help"
