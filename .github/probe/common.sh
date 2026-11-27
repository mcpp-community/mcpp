# Shared by the probe scripts: fetch the released mcpp and xlings, print helpers.
# Sourced; prints and never asserts.

say()  { printf '\n===== %s =====\n' "$*"; }
show() { printf '$ %s\n' "$*"; "$@" 2>&1; printf '[exit %s]\n' "$?"; }
tailed() {   # $1 = lines, rest = command; prints the tail and the exit status
    local n="$1"; shift
    printf '$ %s\n' "$*"
    "$@" > "$PROBE_TMP/last.log" 2>&1
    local rc=$?
    tail -n "$n" "$PROBE_TMP/last.log"
    printf '[exit %s]\n' "$rc"
    return 0
}

PROBE_TMP="${RUNNER_TEMP:-/tmp}/probe"
mkdir -p "$PROBE_TMP"

fetch_mcpp() {
    local os arch asset
    case "$(uname -s)" in Linux) os=linux ;; Darwin) os=macosx ;; esac
    case "$(uname -m)" in x86_64) arch=x86_64 ;; arm64|aarch64) arch=$([ "$os" = macosx ] && echo arm64 || echo aarch64) ;; esac
    asset="mcpp-${os}-${arch}.tar.gz"
    curl -fsSL -o "$PROBE_TMP/$asset" \
        "https://github.com/mcpp-community/mcpp/releases/download/v${MCPP_VERSION}/${asset}"
    mkdir -p "$PROBE_TMP/mcpp"
    tar -xzf "$PROBE_TMP/$asset" -C "$PROBE_TMP/mcpp"
    MCPP="$(find "$PROBE_TMP/mcpp" -type f -name mcpp -perm -u+x | head -1)"
    export MCPP
    echo "MCPP=$MCPP"
    "$MCPP" --version
}

fetch_xlings() {
    local asset="xlings-${XLINGS_VERSION}-linux-x86_64.tar.gz"
    curl -fsSL -o "$PROBE_TMP/$asset" \
        "https://github.com/openxlings/xlings/releases/download/v${XLINGS_VERSION}/${asset}"
    mkdir -p "$PROBE_TMP/xlings"
    tar -xzf "$PROBE_TMP/$asset" -C "$PROBE_TMP/xlings"
    XLINGS_BIN="$(find "$PROBE_TMP/xlings" -type f -name xlings -perm -u+x | head -1)"
    export XLINGS_BIN
    echo "XLINGS_BIN=$XLINGS_BIN"
    find "$PROBE_TMP/xlings" -maxdepth 3 | head -30
    "$XLINGS_BIN" --version
}

hello_project() {   # $1 = directory
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<'TOML'
[package]
name    = "hello"
version = "0.1.0"

[targets.hello]
kind = "bin"
main = "src/main.cpp"
TOML
    printf '#include <cstdio>\nint main() { std::puts("hello"); return 0; }\n' > "$1/src/main.cpp"
}
