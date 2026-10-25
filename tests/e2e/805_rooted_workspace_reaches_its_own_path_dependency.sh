#!/usr/bin/env bash
# requires: gcc python3
# 805 -- a rooted workspace (root [package] + [workspace]) built as itself
# carries its workspace context to a member reached through its OWN [dependencies]
# path entry (mcpp#725).
#
# Before the fix, `state.wsManifest` / `state.runtimeWorkspaceRoot` were left
# unset on this exact branch (src/build/prepare/manifest.cpp), so
# `depIsMember` (src/build/prepare/graph.cpp) read the member as a stranger:
#
#   A. its `x.workspace = true` entry, pinned in [workspace.dependencies] to a
#      version that is NOT the latest, was either refused (2026.9.27.1) or
#      resolved as an unconstrained dependency that happened to land on the
#      latest version anyway (2026.9.26.1) -- the pin must be honoured;
#   B. its omitted `package.version`, supplied by [workspace.package], made the
#      manifest parser itself refuse the member (it was not loaded
#      `insideWorkspace`);
#   C. it received none of [workspace.build]'s flags.
#
# The dependency is served from a project-local path index and pre-extracted
# into the private xlings data directory `install_path_from_project_data`
# reads, so the build touches no network: only the PINNED version (0.0.1) is
# pre-extracted, and the index's "latest", 0.0.2, is not -- so an unpinned
# resolution fails loudly (a network fetch of an https://example.invalid URL)
# rather than silently passing, which is what let 2026.9.26.1 look correct.
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# A gcc payload already installed on this machine, pinned explicitly at the
# workspace root so the whole graph is compiled with a toolchain member `a`'s
# own (nonexistent) declaration would not choose. The HIGHEST installed
# version, not merely the first: an old gcc (13.x) has no `-fmodules` at all,
# and this fixture needs C++23 modules to build.
GCC_VER=""
if [ -d "$MCPP_HOME/registry/data/xpkgs/xim-x-gcc" ]; then
    GCC_VER="$(ls -1 "$MCPP_HOME/registry/data/xpkgs/xim-x-gcc" | sort -V | tail -1)"
fi
[ -n "$GCC_VER" ] || fail "no gcc payload found under \$MCPP_HOME to pin [toolchain]"

mkdir -p "$TMP/ws" && cd "$TMP/ws"

# ── a project-local index serving two versions of one package ──────────────
mkdir -p local-index/pkgs/f
cat > local-index/pkgs/f/fx.pinned.lua <<'EOF'
package = {
    spec = "1",
    namespace = "fx",
    name = "fx.pinned",
    description = "fixture package pinned to a non-latest version (#725)",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.0.1"] = {
                url = "https://example.invalid/fx-pinned-0.0.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["0.0.2"] = {
                url = "https://example.invalid/fx-pinned-0.0.2.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/pinned.cppm" },
        targets = { ["pinned"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# Only the PINNED version (0.0.1) is pre-extracted -- 0.0.2 ("latest") is
# deliberately absent, so resolving the wrong version fails loudly instead of
# silently succeeding (the 2026.9.26.1 shape).
mkdir -p ".mcpp/.xlings/data/xpkgs/fx.pinned/0.0.1/src"
cat > ".mcpp/.xlings/data/xpkgs/fx.pinned/0.0.1/src/pinned.cppm" <<'EOF'
export module pinned;
export int pinned_value() { return 1; }
EOF

# ── the rooted workspace: root has BOTH [package] and [workspace] ──────────
mkdir -p src a/src
cat > mcpp.toml <<EOF
[package]
name    = "ws"
version = "0.1.0"

[workspace]
members = ["a"]

[workspace.package]
version = "0.2.0"

[workspace.dependencies.fx]
pinned = "0.0.1"

[workspace.build]
cxxflags = ["-DWS_FLAG=1"]

[indices]
fx = { path = "local-index" }

[toolchain]
default = "gcc@$GCC_VER"

[dependencies]
a = { path = "a" }

[targets.ws]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
import a;
int main() { return a_value() == 2 ? 0 : 1; }
EOF

# `a` is reached ONLY through the root's own [dependencies] path entry, never
# through `-p`: it omits `package.version` (supplied by [workspace.package]),
# and its `x.workspace = true` entry names a package this workspace's own
# [indices] resolves.
cat > a/mcpp.toml <<'EOF'
[package]
name = "a"

[dependencies.fx]
pinned = { workspace = true }

[targets.a]
kind = "lib"

[build]
sources = ["src/a.cppm"]
EOF
cat > a/src/a.cppm <<'EOF'
export module a;
import pinned;
#if !defined(WS_FLAG)
#error "a.cppm: [workspace.build] cxxflags did not reach this member (#725)"
#endif
export int a_value() { return pinned_value() + WS_FLAG; }
EOF

"$MCPP" build > build.log 2>&1 || fail "a rooted workspace's own path dependency did not build" build.log

# ── B: `a` was recognised as a member (no missing-version refusal happened) ─
grep -qE "missing required field 'package.version'" build.log \
    && fail "a's omitted package.version was not supplied by [workspace.package]" build.log

# ── A: the pinned, non-latest version was locked and used ──────────────────
grep -q 'version = "0.0.1"' mcpp.lock \
    || fail "mcpp.lock does not record the pinned version 0.0.1" mcpp.lock
grep -q 'version = "0.0.2"' mcpp.lock \
    && fail "mcpp.lock records 0.0.2 -- the pin (not \"latest\") should have won" mcpp.lock

# ── C, and the shared root-position toolchain: read compile_commands.json ──
python3 - "$GCC_VER" > cdb_check.log 2>&1 <<'EOF' || fail "compile_commands.json did not show a's inherited flags/toolchain" cdb_check.log
import json, sys
gcc_ver = sys.argv[1]
entries = json.load(open("compile_commands.json"))
def find(suffix):
    for e in entries:
        if e["file"].replace("\\", "/").endswith(suffix):
            return e
    sys.exit(f"no compile_commands.json entry ending in {suffix}")
a_entry = find("a/src/a.cppm")
main_entry = find("src/main.cpp")
if "-DWS_FLAG=1" not in a_entry["arguments"]:
    sys.exit("[workspace.build] cxxflags did not reach a.cppm's compile command")
a_cxx = a_entry["arguments"][0]
main_cxx = main_entry["arguments"][0]
if a_cxx != main_cxx:
    sys.exit(f"member 'a' and the root were not compiled with the same "
             f"toolchain ({a_cxx!r} vs {main_cxx!r})")
if gcc_ver not in a_cxx:
    sys.exit(f"member 'a' was not compiled with the workspace's "
             f"[toolchain] gcc@{gcc_ver} ({a_cxx!r})")
EOF

"$MCPP" run > run.log 2>&1 || fail "the program did not return 0 (pinned_value() + WS_FLAG != 2)" run.log

echo "PASS: 805_rooted_workspace_reaches_its_own_path_dependency"
