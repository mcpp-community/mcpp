#!/usr/bin/env bash
# requires:
# 171_bmi_staging_locked_dest.sh — mcpp#311: staging must survive a destination
# that another process is holding, and must fail LOUDLY when it genuinely can't.
#
# The reported failure: mcpp writes the staged std BMI path into
# compile_commands.json so clangd can resolve `import std;`, clangd
# memory-maps that file, and the old `Copy-Item -Force` staging step then tried
# to overwrite it in place → Windows error 1224 (a file with a user-mapped
# section open cannot be replaced) → the whole build reported "build failed".
#
# Windows: reproduce it exactly, WITHOUT needing clangd — a background
# PowerShell holds a MemoryMappedFile on the staged BMI.
# POSIX: the closest analogue an ordinary test can create is an unwritable
# destination (chmod 444); the old `cp -f` silently unlinked and rewrote it,
# the new path must not need to write at all.
set -e

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
DST="$(dirname "$NINJA")/$(echo "$EDGE" | awk '{print $2}')"
SRC=$(echo "$EDGE" | awk '{print $NF}')
[[ -f "$DST" ]] || { echo "FAIL: staged BMI missing at $DST"; exit 1; }

# Dirty the staging edge without touching the shared cache's CONTENT.
touch "$SRC"

HOLDER=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        WINDST="$(cygpath -w "$DST")"
        powershell -NoProfile -Command \
            "\$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile('$WINDST', [System.IO.FileMode]::Open); Start-Sleep -Seconds 90" &
        HOLDER=$!
        # Give the mapping time to exist before the build races it.
        sleep 3
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
[[ -n "$HOLDER" ]] && { kill "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null; }
chmod 644 "$DST" 2>/dev/null || true

if [[ $rc -ne 0 ]]; then
    cat build2.log
    echo "FAIL: staging an already-equivalent BMI failed while the destination was held"
    exit 1
fi
grep -q "stage --output" build2.log || {
    cat build2.log; echo "FAIL: the staging edge did not run"; exit 1; }
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
