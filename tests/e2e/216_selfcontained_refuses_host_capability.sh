#!/usr/bin/env bash
# requires: pack patchelf elf
# 216_selfcontained_refuses_host_capability.sh — a bundle that carries its own
# libc cannot consume a capability the host must satisfy.
#
# WHY BOTH MODES
#
# `static` has no libc to share; `self-contained` brings its own. For a library
# the target must supply, the consequence is the same: that .so arrives with
# its own requirements on the HOST's libc, and the process does not have that
# libc. Measured in both directions as mcpp#392 / mcpp#401 — a private glibc
# meeting host-loaded objects dies during relocation, before main.
#
# The graphics drivers are the everyday case: they cannot be bundled at all
# (version-locked to the running kernel module; redistribution not permitted),
# so they are always the host's.
#
# Today both modes link and then fail at startup, or silently fall back to
# software rendering — which is worse than not building, because nothing says
# so.
#
# THE REFUSAL MUST NAME A WAY FORWARD. A check that only says no is a check
# users route around; `vendored` is the mode that actually works here, and the
# test asserts the message says so.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

cd "$TMP"
"$MCPP" new gfxapp > /dev/null
cd gfxapp
cat >> mcpp.toml <<'EOF'

[[runtime.requirements]]
kind      = "capability"
value     = "opengl.glx.driver"
phase     = "run"
# DECLARED by the package, not inferred by mcpp: which mechanism a capability
# uses is the provider's property. GLX is reached through the dispatch
# library's own DT_RPATH; EGL through a JSON file holding an ABSOLUTE path — so
# the two are not interchangeable, and a row without this is not actionable.
discovery = "rpath-of-dispatch"
EOF

# ── the two modes that carry their own libc must refuse ─────────────────────
for mode in self-contained static; do
    if "$MCPP" pack --mode "$mode" > "$TMP/$mode.log" 2>&1; then
        echo "FAIL: --mode $mode packed a program that needs a host capability"
        echo "      it would link and then fail at startup on the user's machine"
        tail -20 "$TMP/$mode.log"
        exit 1
    fi
    grep -q 'opengl.glx.driver' "$TMP/$mode.log" || {
        echo "FAIL: --mode $mode refused without naming the capability"
        cat "$TMP/$mode.log"
        exit 1
    }
    grep -q 'vendored' "$TMP/$mode.log" || {
        echo "FAIL: --mode $mode refused without naming a way forward"
        echo "      a refusal with no next step is a refusal users disable"
        cat "$TMP/$mode.log"
        exit 1
    }
    echo "  $mode: refused, named the capability and the alternative"
done

# ── vendored must WORK, and must state what the host has to provide ─────────
"$MCPP" pack --mode vendored > "$TMP/vendored.log" 2>&1 || {
    echo "FAIL: --mode vendored refused a program it can package"
    cat "$TMP/vendored.log"
    exit 1
}
TARBALL="$(ls target/dist/*.tar.gz | head -1)"
mkdir -p "$TMP/x" && tar -xzf "$TARBALL" -C "$TMP/x"
REQ="$(find "$TMP/x" -name HOST-REQUIREMENTS | head -1)"
[[ -n "$REQ" ]] || {
    echo "FAIL: vendored bundle does not state what the host must provide"
    find "$TMP/x" -type f | sed "s|$TMP/x/||"
    exit 1
}
cat "$REQ"

grep -q 'capability=opengl.glx.driver' "$REQ" || {
    echo "FAIL: HOST-REQUIREMENTS does not list the capability"
    exit 1
}
# The discovery mechanism is the actionable half. GLX is found through the
# dispatch library's own DT_RPATH, while EGL is found through a JSON file whose
# library_path is ABSOLUTE — so "copy the directory over" fixes one and not the
# other. A row without it is not actionable.
grep -q 'discovery=rpath-of-dispatch' "$REQ" || {
    echo "FAIL: HOST-REQUIREMENTS states the capability but not how it is found"
    cat "$REQ"
    exit 1
}

echo "PASS: modes that carry their own libc refuse host capabilities; vendored packages and declares them"
