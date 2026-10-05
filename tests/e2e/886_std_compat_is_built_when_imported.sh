#!/usr/bin/env bash
# requires: gcc
# 886 -- `std.compat` on the GCC row, and only when a unit imports it
# (2026.10.5.2).
#
# libstdc++ installs `bits/std.compat.cc` beside `bits/std.cc`. The GCC row
# did not build it, so `import std.compat` failed in the first unit with
# "returning to the gate for a mechanical issue", which SPEC-009 §6.2's
# acceptance of a Default row does not allow. And every row that does build it
# built it for every `import std`, so a library whose `std.compat` failed to
# compile cost every project that never imported it.
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

compat_files() { find "$MCPP_HOME" -path '*build-cache*' -name 'std.compat.*' | wc -l | tr -d ' '; }

mkdir -p "$TMP/only/src" "$TMP/compat/src"
printf '[package]\nname = "only"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "gcc@16.1.0"\n' > "$TMP/only/mcpp.toml"
printf 'import std;\nint main() { std::println("std"); }\n' > "$TMP/only/src/main.cpp"
printf '[package]\nname = "compat"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "gcc@16.1.0"\n' > "$TMP/compat/mcpp.toml"
cat > "$TMP/compat/src/main.cpp" <<'EOF'
import std.compat;
int main() {
    auto* memory = ::malloc(32);
    if (!memory) return 1;
    ::free(memory);
    std::vector<int> values{1, 2, 3};
    ::printf("compat=%zu\n", values.size());
    return values.size() == 3 ? 0 : 1;
}
EOF

cd "$TMP/only"
"$MCPP" run > only.log 2>&1 || fail "import std failed" only.log
grep -q '^std$' only.log || fail "the std program did not run" only.log
[ "$(compat_files)" = 0 ] || fail "std.compat was built for a build that does not import it" only.log

cd "$TMP/compat"
"$MCPP" run > compat.log 2>&1 || fail "import std.compat failed on the GCC row" compat.log
grep -q 'compat=3' compat.log || fail "the std.compat program did not run" compat.log
[ "$(compat_files)" != 0 ] || fail "std.compat was not built in the cache" compat.log

echo "PASS: 886 std.compat is built when imported"
