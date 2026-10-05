#!/usr/bin/env bash
# requires: msvc
# 887 -- `lto = true` on cl.exe, and its meeting with export discovery
# (2026.10.5.2, #770).
#
#   A  a program: `/GL` on the compile, `/LTCG` on the link, `+ lto`.
#   B  a DLL whose exports are discovered (key omitted): its packages compile
#      with `/GL-`, the build says so once, the summary says `+ lto (partial)`,
#      and a consumer links against the discovered exports.
#   C  the same DLL stating `windows_auto_export = true`: refused.
#   D  stating `false` with `__declspec(dllexport)`: full `/GL`, and it links.
#   E  `/GL` written into the DLL's flags with discovery on: refused.
#   F  `/bigobj` with discovery on: read, and the consumer links.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF
cd "$TMP"
ninja_of() { find target -name build.ninja | head -1; }

# ── A ──────────────────────────────────────────────────────────────────────
mkdir -p a/src
cat > a/mcpp.toml <<'EOF'
[package]
name = "a"
version = "0.1.0"
[toolchain]
windows = "msvc@system"
[profile.release]
lto = true
EOF
printf 'int main() { return 0; }\n' > a/src/main.cpp
(cd a && "$MCPP" build --release > ../a.log 2>&1) || fail "A: build failed" a.log
grep -q '+ lto\]' a.log || fail "A: the summary does not say + lto" a.log
(cd a && grep -q '/GL' "$(ninja_of)" && grep -q '/LTCG' "$(ninja_of)") \
    || fail "A: /GL or /LTCG is missing from the build" a.log

# ── the DLL and its consumer, for B to F ───────────────────────────────────
write_dll() {   # $1 = extra [targets.dll] lines, $2 = extra [build] lines, $3 = source
    mkdir -p dll/src app/src
    cat > dll/mcpp.toml <<EOF
[package]
name = "dll"
version = "0.1.0"
[toolchain]
windows = "msvc@system"
[build]
$2
[targets.dll]
kind = "shared"
$1
EOF
    printf '%s\n' "$3" > dll/src/dll.cpp
    cat > app/mcpp.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"
[toolchain]
windows = "msvc@system"
[profile.release]
lto = true
[dependencies]
dll = { path = "../dll" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'int twice(int);\nint main() { return twice(21) == 42 ? 0 : 1; }\n' > app/src/main.cpp
    rm -rf app/target
}
PLAIN='int twice(int x) { return 2 * x; }'
ANNOTATED='__declspec(dllexport) int twice(int x) { return 2 * x; }'

# B
write_dll "" "" "$PLAIN"
(cd app && "$MCPP" run --release > ../b.log 2>&1) || fail "B: the consumer of a discovered DLL did not link or run" b.log
grep -q 'LTO is not applied to the packages linked into the shared libraries' b.log \
    || fail "B: the downgrade was not stated" b.log
grep -q 'lto (partial)' b.log || fail "B: the summary does not say + lto (partial)" b.log
(cd app && grep -q '/GL-' "$(ninja_of)") || fail "B: the DLL's package was not compiled with /GL-" b.log

# C
write_dll "windows_auto_export = true" "" "$PLAIN"
if (cd app && "$MCPP" build --release > ../c.log 2>&1); then fail "C: a stated windows_auto_export = true was not refused under LTO" c.log; fi
grep -q 'states `windows_auto_export = true`' c.log || fail "C: the refusal does not name the key" c.log

# D
write_dll "windows_auto_export = false" "" "$ANNOTATED"
(cd app && "$MCPP" run --release > ../d.log 2>&1) || fail "D: an annotated DLL under full LTO did not link or run" d.log
(cd app && ! grep -q '/GL-' "$(ninja_of)") || fail "D: a DLL that opted out of discovery was compiled with /GL-" d.log

# E
write_dll "" 'cxxflags = ["/GL"]' "$PLAIN"
sed -i 's/^lto = true/lto = false/' app/mcpp.toml
if (cd app && "$MCPP" build > ../e.log 2>&1); then fail "E: /GL beside export discovery was not refused" e.log; fi
grep -q 'compiles with /GL' e.log || fail "E: the refusal does not name /GL" e.log

# F
write_dll "" 'cxxflags = ["/bigobj"]' "$PLAIN"
sed -i 's/^lto = true/lto = false/' app/mcpp.toml
(cd app && "$MCPP" run > ../f.log 2>&1) || fail "F: a /bigobj DLL's discovered exports did not link" f.log

echo "PASS: 887 msvc lto and export discovery"
