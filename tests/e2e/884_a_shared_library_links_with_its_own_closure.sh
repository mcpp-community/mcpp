#!/usr/bin/env bash
# requires: elf
# 884 -- mcpp#771: a shared library that is not the root's links with the
# graph's flags and the flags of the packages its owner reaches, and with no
# other package's (2026.10.5.2).
#
# Read from the produced images: every flag carries an `-rpath` marker, and
# `readelf -d` lists the RUNPATH entries that reached each link.
#
#   A  a workspace member's shared library takes the libraries its own
#      `build.mcpp` declares, and no other member's flags; `-p` agrees.
#   B  a path dependency's shared library outside a workspace takes its own
#      flags and the profile's, and not the root's or a sibling's.
#   C  a search path only the root names no longer reaches a dependency's
#      shared library; the build states this once beside the library.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF

runpath() { readelf -d "$1" | grep -E 'RUNPATH|RPATH' || true; }
lib() { find target -name "$1" -type f -path '*/bin/*' | head -1; }

# ── A ──────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/ws/player/src" "$TMP/ws/other/src"
cd "$TMP/ws"
printf '[workspace]\nmembers = ["player", "other"]\n' > mcpp.toml
cat > player/mcpp.toml <<'EOF'
[package]
name = "player"
version = "0.1.0"

[targets.player_dll]
kind = "shared"

[targets.player]
kind = "bin"
main = "src/main.cpp"
EOF
cat > player/build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::link_lib("m");
    mcpp::link_flag("-Wl,-rpath,/t884-player-program");
    return 0;
}
EOF
printf 'export module t884_player;\nexport int answer() { return 42; }\n' > player/src/player.cppm
printf 'import t884_player;\nint main() { return answer() == 42 ? 0 : 1; }\n' > player/src/main.cpp
cat > other/mcpp.toml <<'EOF'
[package]
name = "other"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,/t884-other"]

[targets.other]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > other/src/main.cpp

for selection in "" "-p player"; do
    rm -rf target
    # shellcheck disable=SC2086
    "$MCPP" build $selection > a.log 2>&1 || fail "A: build ${selection:-of the workspace} failed" a.log
    so=$(lib libplayer_dll.so)
    [ -n "$so" ] || fail "A: no libplayer_dll.so" a.log
    runpath "$so" > a.dyn
    grep -q '/t884-player-program' a.dyn \
        || fail "A (${selection:-workspace}): the member's build.mcpp flag did not reach its shared library" a.dyn
    if grep -q '/t884-other' a.dyn; then
        fail "A (${selection:-workspace}): another member's flag reached the shared library" a.dyn
    fi
done

# ── B and C ────────────────────────────────────────────────────────────────
mkdir -p "$TMP/b/app/src" "$TMP/b/dep/src" "$TMP/b/sib/src" "$TMP/b/ext"
cd "$TMP/b"
cat > dep/mcpp.toml <<'EOF'
[package]
name = "dep"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,/t884-dep"]

[targets.dep]
kind = "shared"
EOF
printf 'export module t884_dep;\nexport int dv() { return 1; }\n' > dep/src/dep.cppm
cat > sib/mcpp.toml <<'EOF'
[package]
name = "sib"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,/t884-sib"]

[targets.sib]
kind = "lib"
EOF
printf 'export module t884_sib;\nexport int sv() { return 2; }\n' > sib/src/sib.cppm
cat > app/mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,/t884-root"]

[profile.release]
ldflags = ["-Wl,-rpath,/t884-profile"]

[dependencies]
dep = { path = "../dep" }
sib = { path = "../sib" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'import t884_dep;\nimport t884_sib;\nint main() { return dv() + sv() == 3 ? 0 : 1; }\n' > app/src/main.cpp
cd app
"$MCPP" build --release > b.log 2>&1 || fail "B: build failed" b.log
runpath "$(lib libdep.so)" > b.dyn
grep -q '/t884-dep' b.dyn     || fail "B: the dependency's own flag did not reach its shared library" b.dyn
grep -q '/t884-profile' b.dyn || fail "B: the profile's flag did not reach the dependency's shared library" b.dyn
for foreign in /t884-root /t884-sib; do
    if grep -q "$foreign" b.dyn; then fail "B: $foreign reached the dependency's shared library" b.dyn; fi
done
runpath "$(lib app)" > app.dyn
grep -q '/t884-root' app.dyn || fail "B: the root's own flag did not reach the root's program" app.dyn

# C: the root names a search path the dependency's library needs.
printf 'int ext_fn(void) { return 1; }\n' > ../ext/ext.c
gcc -shared -fPIC ../ext/ext.c -o ../ext/libt884ext.so 2>/dev/null || cc -shared -fPIC ../ext/ext.c -o ../ext/libt884ext.so
sed -i 's|ldflags = \["-Wl,-rpath,/t884-dep"\]|ldflags = ["-Wl,-rpath,/t884-dep", "-lt884ext"]|' ../dep/mcpp.toml
sed -i "s|ldflags = \\[\"-Wl,-rpath,/t884-root\"\\]|ldflags = [\"-Wl,-rpath,/t884-root\", \"-L$TMP/b/ext\"]|" mcpp.toml
rm -rf target
if "$MCPP" build > c.log 2>&1; then
    fail "C: the dependency's library linked with a search path only the root names" c.log
fi
grep -q "no longer reach the shared" c.log || fail "C: the change of rule was not stated" c.log
grep -q -- "-L$TMP/b/ext" c.log || fail "C: the statement does not name the root's search path" c.log
# The dependency stating its own search path is the migration.
sed -i "s|\"-lt884ext\"\\]|\"-L$TMP/b/ext\", \"-Wl,-rpath,$TMP/b/ext\", \"-lt884ext\"]|" ../dep/mcpp.toml
"$MCPP" build > c2.log 2>&1 || fail "C: the dependency's own search path did not fix the link" c2.log

echo "PASS: 884 a shared library links with its own closure"
