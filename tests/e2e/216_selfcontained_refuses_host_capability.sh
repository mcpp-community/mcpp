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
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

# ── the capability comes from a DEPENDENCY, not from this project ───────────
#
# THIS IS THE SHAPE REAL PROJECTS HAVE, and getting it wrong is how the first
# version of this gate shipped half-working. Almost no application declares
# `capability:opengl.glx.driver` itself — it depends on glfw / an SDL wrapper /
# a GL runtime that does, and the resolver stamps each requirement with its
# requester. A gate that reads the ROOT manifest answers "did the author write
# it down" (nearly always no) instead of "does the resolved graph need it".
#
# Measured on a real imgui project: `mcpp why runtime` listed
# `capability:opengl.glx.driver [run] <- compat.glfw@3.4 (required)`, and
# `--mode self-contained` packaged it happily — while THIS test passed, because
# its fixture declared the capability at the root. The fixture had the one
# shape real projects do not.
INDEX_DIR="$TMP/local-index"
# The manifest is read by mcpp, not by the shell, so the path has to be in HOST
# spelling — an MSYS `/c/...` written into mcpp.toml is a path Windows cannot
# open. `00_fixture_path_hygiene` enforces this statically.
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/g"
cat > "$INDEX_DIR/pkgs/g/gfx-runtime.lua" <<'EOF'
package = {
    spec = "1",
    name = "gfx-runtime",
    description = "A dependency that needs the host to provide a driver",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/gfx-runtime-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["gfx-runtime"] = { kind = "lib" } },
        deps = {},
        runtime = {
            requirements = {
                { kind = "capability", value = "opengl.glx.driver", phase = "run",
                  required = true, discovery = "rpath-of-dispatch" },
            },
        },
    },
}
EOF

mkdir -p "$TMP/gfxapp/src"
mkdir -p "$TMP/gfxapp/.mcpp/.xlings/data/xpkgs/local-dev.gfx-runtime/1.0.0/src"
cd "$TMP/gfxapp"
cat > .mcpp/.xlings/data/xpkgs/local-dev.gfx-runtime/1.0.0/src/lib.cppm <<'EOF'
export module gfx.runtime;
export int gfx_ready() { return 1; }
EOF
cat > src/main.cpp <<'EOF'
import gfx.runtime;
int main() { return gfx_ready() == 1 ? 0 : 1; }
EOF
# The application itself declares NOTHING. That is the point.
cat > mcpp.toml <<EOF
[package]
name = "gfxapp"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[dependencies]
"local-dev.gfx-runtime" = "1.0.0"

[targets.gfxapp]
kind = "bin"
main = "src/main.cpp"
EOF

# Guard: if the requirement never reaches the resolved graph, everything below
# would pass by refusing nothing — so assert it arrived first.
"$MCPP" build > "$TMP/build.log" 2>&1 || { cat "$TMP/build.log"; exit 1; }
"$MCPP" why runtime > "$TMP/why.log" 2>&1 || { cat "$TMP/why.log"; exit 1; }
grep -q 'capability:opengl.glx.driver' "$TMP/why.log" || {
    echo "FAIL: the dependency's capability never reached the resolved graph —"
    echo "      this test would then prove nothing about the gate"
    cat "$TMP/why.log"
    exit 1
}
grep -q 'gfx-runtime' "$TMP/why.log" || {
    echo "FAIL: the requirement is not attributed to the dependency"
    cat "$TMP/why.log"
    exit 1
}
echo "  requirement arrives from the dependency, not from this project"

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
