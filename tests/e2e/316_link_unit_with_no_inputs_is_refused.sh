#!/usr/bin/env bash
# requires: gcc
# 316_link_unit_with_no_inputs_is_refused.sh — a target with nothing to link is
# an error that names the target (mcpp#533).
#
# WHERE THIS CAME FROM. A dependency whose `install()` was skipped over a
# package-identity collision left a version directory with no source tree. mcpp
# planned a shared-library target for it anyway, emitted a link edge with zero
# inputs, and the user was shown:
#
#   /bin/sh: 1: -shared: not found
#
# — four layers from the cause and naming nothing that had anything to do with
# it. (`$cc` was emitted only when the compile set held a C or asm unit; a
# package with no sources has neither, so the variable expanded to nothing and
# the shell was handed `-shared` as a program name.)
#
# THE STATIC CASE IS THE ONE THAT MATTERED. `ar rcs libfoo.a` with no
# members exits 0 and writes an 8-byte archive, so before this the build
# REPORTED SUCCESS and every consumer failed later with undefined symbols. Both
# kinds are asserted here, and the static one is why the check is at plan time
# rather than a better linker diagnostic.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

check_kind() {   # $1 = mcpp kind, $2 = human name, $3 = artifact glob
    local dir="$TMP/$1"
    mkdir -p "$dir/src"
    cat > "$dir/mcpp.toml" <<EOF
[package]
name    = "ghost"
version = "0.1.0"

[targets.ghost]
kind = "$1"
EOF
    cd "$dir"
    if "$MCPP" build > b.log 2>&1; then
        echo "FAIL($1): a target with no sources built successfully"
        find . -name "$3" -printf '  produced %p (%s bytes)\n' 2>/dev/null
        exit 1
    fi

    # The message names the target, and says which kind — a user reading it has
    # to be able to find the thing in their manifest.
    grep -q "target 'ghost'" b.log || {
        echo "FAIL($1): the error does not name the target:"; cat b.log; exit 1; }
    grep -q "$2" b.log || {
        echo "FAIL($1): the error does not say it is a $2:"; cat b.log; exit 1; }

    # THE NEGATIVE HALF. This is the assertion that catches a regression back
    # to the reported symptom: if the refusal is ever removed, the build reaches
    # the linker again and this is what comes out.
    if grep -q -- '-shared: not found' b.log; then
        echo "FAIL($1): the build reached the shell and reported the old symptom:"
        cat b.log; exit 1
    fi
    if grep -qi 'no input files' b.log; then
        echo "FAIL($1): the build reached the compiler driver instead of being"
        echo "          refused at plan time:"; cat b.log; exit 1
    fi

    # Nothing was written. For the static kind this is the whole point.
    if find . -name "$3" | grep -q .; then
        echo "FAIL($1): an artifact was produced by a refused target:"
        find . -name "$3" -printf '  %p (%s bytes)\n'
        exit 1
    fi
}

check_kind shared "shared library" 'libghost.so*'
check_kind lib    "static library" 'libghost.a'

# ── the control: a target WITH sources still builds ─────────────────────────
#
# Without this, a refusal that fired on every library target would pass
# everything above.
mkdir -p "$TMP/ok/src"
cat > "$TMP/ok/mcpp.toml" <<'EOF'
[package]
name    = "ok"
version = "0.1.0"

[targets.ok]
kind = "shared"
EOF
printf 'int ok_value(void) { return 7; }\n' > "$TMP/ok/src/ok.c"
cd "$TMP/ok"
"$MCPP" build > b.log 2>&1 || {
    cat b.log; echo "FAIL: a library target WITH sources was refused"; exit 1; }
find . -name 'libok.so*' | grep -q . || {
    echo "FAIL: the control target produced no library"; exit 1; }

echo "PASS: 316 (empty shared and static targets refused by name; populated one builds)"
