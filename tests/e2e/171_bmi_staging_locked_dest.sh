#!/usr/bin/env bash
# requires:
# 171_bmi_staging_locked_dest.sh — mcpp#311: staging must survive a destination
# another process is holding, and must fail LOUDLY when it genuinely can't.
#
# The reported failure: mcpp writes the staged std BMI path into
# compile_commands.json so clangd can resolve `import std;`, clangd
# memory-maps that file, and the old `Copy-Item -Force` staging step then tried
# to overwrite it in place → Windows error 1224 (a file with a user-mapped
# section open cannot be replaced) → the whole build reported "build failed".
#
# Windows: reproduce that exactly and WITHOUT clangd — a background PowerShell
# holds a MemoryMappedFile on the staged BMI. The holder signals readiness
# through a sentinel file, so a PowerShell that never started fails the test
# instead of letting it pass for the wrong reason.
#
# Note the holder is STRICTER than clangd: CreateFromFile(path, FileMode.Open)
# takes FileShare.None, so the destination cannot even be OPENED — content
# comparison is impossible. Surviving that is why the std staging edges verify
# by SIZE (fingerprint-scoped ⇒ equal size is equivalence, and size comes from
# directory metadata, which needs no open). Passing here therefore implies the
# real clangd case, which only denies writes.
#
# The load-bearing assertion is platform-neutral and root-proof: an equivalent
# destination must not be written AT ALL (same inode, same mtime, same size).
# Permissions alone would not do — root ignores them, and the container e2e job
# runs as root.
set -e

# build.ninja node names are ninja-ESCAPED: on Windows a drive letter arrives as
# `C$:/Users/...`. Unescape before touching the filesystem (the Windows job
# failed on exactly this).
unescape_ninja() { printf '%s' "$1" | sed 's/\$:/:/g; s/\$\$/$/g'; }
# Compare paths across the Windows fork: MCPP_HOME is `C:\Users\...` while ninja
# writes forward slashes.
norm_path() { printf '%s' "$1" | tr '\\' '/'; }

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
import std;
int main() { std::println("locked"); return 0; }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

cd app
"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "FAIL: initial build"; exit 1; }

NINJA=$(find target -name build.ninja | head -1)
EDGE=$(grep -E '^build [^ ]+ : stage_file ' "$NINJA" | head -1)
[[ -n "$EDGE" ]] || { echo "FAIL: no stage_file edge for the std BMI"; exit 1; }
DST="$(dirname "$NINJA")/$(unescape_ninja "$(echo "$EDGE" | awk '{print $2}')")"
SRC=$(unescape_ninja "$(echo "$EDGE" | awk '{print $NF}')")
[[ -f "$DST" ]] || { echo "FAIL: staged BMI missing at $DST"; exit 1; }

# GNU stat vs BSD/macOS stat (the macOS job failed on `stat -c`).
identity() {
    stat -c '%i %Y %s' "$1" 2>/dev/null || stat -f '%i %m %z' "$1"
}
BEFORE=$(identity "$DST")

# Dirty the staging edge without touching the shared cache's CONTENT.
touch "$SRC"

HOLDER=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        WINDST="$(cygpath -w "$DST")"
        READY="$TMP/mapped.flag"
        WINREADY="$(cygpath -w "$READY")"
        powershell -NoProfile -Command \
            "\$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile('$WINDST', [System.IO.FileMode]::Open); \
             Set-Content -Path '$WINREADY' -Value 'mapped'; Start-Sleep -Seconds 120" &
        HOLDER=$!
        for _ in $(seq 1 40); do [[ -f "$READY" ]] && break; sleep 1; done
        [[ -f "$READY" ]] || {
            kill "$HOLDER" 2>/dev/null || true
            echo "FAIL: could not map the staged BMI — the test would not have proven anything"
            exit 1; }
        echo "holding a user-mapped section on $WINDST (pid $HOLDER)"
        ;;
    *)
        chmod 444 "$DST"
        echo "made $DST read-only"
        ;;
esac

set +e
"$MCPP" build -v > build2.log 2>&1
rc=$?
set -e
# `wait` on a job we just killed returns 143, and under `set -e` that would
# abort the script right here — with the build's own result never asserted.
# (Exactly how this test failed on Windows the first time it got that far.)
[[ -n "$HOLDER" ]] && { kill "$HOLDER" 2>/dev/null || true; wait "$HOLDER" 2>/dev/null || true; }
chmod 644 "$DST" 2>/dev/null || true

if [[ $rc -ne 0 ]]; then
    cat build2.log
    echo "FAIL: staging an already-equivalent BMI failed while the destination was held"
    exit 1
fi
grep -qE "stage .*--output" build2.log || {
    cat build2.log; echo "FAIL: the staging edge did not run"; exit 1; }

# The real assertion: nothing was written. Not "the write succeeded anyway".
AFTER=$(identity "$DST")
[[ "$BEFORE" == "$AFTER" ]] || {
    echo "FAIL: an equivalent destination was rewritten (was '$BEFORE', now '$AFTER')"
    exit 1; }
cmp "$SRC" "$DST" || { echo "FAIL: held destination no longer matches the cache"; exit 1; }

cd "$TMP"

# ── the loud half: a destination that cannot be produced must fail with an
#    actionable diagnostic, never be downgraded to a warning ──
echo payload > src.bin
mkdir -p occupied/child
echo keep > occupied/child/keep.txt
set +e
"$MCPP" stage --output occupied src.bin > stage_err.log 2>&1
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
    cat stage_err.log
    echo "FAIL: staging onto an unusable destination reported success"; exit 1
fi
grep -q "hint:" stage_err.log || {
    cat stage_err.log; echo "FAIL: staging failure carried no hint"; exit 1; }
grep -q "clangd" stage_err.log || {
    cat stage_err.log; echo "FAIL: hint does not name the usual holder (clangd)"; exit 1; }
[[ -f occupied/child/keep.txt ]] || { echo "FAIL: failed staging clobbered the destination"; exit 1; }

echo "OK"
