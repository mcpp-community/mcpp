#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1042 -- the row's defaults are replaced by name, and every flag names its layer (mcpp#911 G2;
# SPEC-016 §4.5, §5.2).
#
# `[target.<selector>.replace]` names a word the row gives (`-march=`, `-mcmodel=`, its flags,
# `-m <emulation>`) and the words in its place; an empty list removes it, and a word the row does
# not give is reported with the row's words. `mcpp why flags <unit>` lists each word of the unit's
# command line with the layer that gave it, and `mcpp why target` the request's selection.
#
# Criteria: the compile line has `-march=x86-64-v2` and no `-mcmodel=small`; the build reports each
# replacement and warns about the unmet one; `why flags` attributes the replaced word to mcpp.toml
# and `-mno-red-zone` to the row; `why target --format json` names the request, the identity and
# the candidates in order.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF'
[package]
name    = "bare"
version = "0.1.0"

[build]
sources = ["src/*.c"]

[targets.bare]
kind = "bin"
main = "src/main.c"

[target.x86_64-none-elf.replace]
"-mcmodel=small" = []
"-march=x86-64"  = "-march=x86-64-v2"
"-mno-such"      = []
EOF
cat > "$t/src/main.c" <<'EOF'
void _start(void) { __asm__ volatile("mov $60, %eax\n mov $7, %edi\n syscall"); }
EOF
cd "$t"

out=$("$MCPP" build --target x86_64-none-elf 2>&1) || fail "the build did not succeed" "$out"
grep -q 'Replaces `-march=x86-64` → `-march=x86-64-v2` (mcpp.toml:' <<<"$out" || fail "the replacement is not reported" "$out"
grep -q 'Replaces `-mcmodel=small` → nothing' <<<"$out" || fail "the removal is not reported" "$out"
grep -q "replaces \`-mno-such\`, which the row x86_64-none-elf does not give" <<<"$out" \
    || fail "a replacement that meets no word is not reported" "$out"
ninja=$(find target/x86_64-none-elf -name build.ninja | head -1)
cflags=$(grep '^cflags' "$ninja")
grep -q -- '-march=x86-64-v2' <<<"$cflags" || fail "the replacement is not on the compile line" "$cflags"
grep -q -- '-mcmodel=small' <<<"$cflags" && fail "the removed word is still on the compile line" "$cflags"
echo "  ok  the row's words are replaced and removed by name, and an unmet one is reported"

why=$("$MCPP" why flags main.c --target x86_64-none-elf 2>/dev/null) || fail "why flags failed" "$why"
grep -Eq '^  replace +-march=x86-64-v2 +\[mcpp.toml:[0-9]+, in place of `-march=x86-64`\]' <<<"$why" \
    || fail "the replaced word does not name its statement" "$why"
grep -Eq '^  row +-mno-red-zone +\[x86_64-none-elf\]' <<<"$why" || fail "the row's word does not name the row" "$why"
grep -Eq '^  engine +-ffreestanding' <<<"$why" || fail "the engine's convention does not name the engine" "$why"
echo "  ok  why flags names the layer of each word"

json=$("$MCPP" why target --target x86_64-windows --format json 2>/dev/null) || fail "why target failed" "$json"
python3 - "$json" <<'PY' || fail "why target --format json does not state the selection" "$json"
import json, sys
t = json.loads(sys.argv[1])["data"]["target"]
assert t["request"] == "x86_64-windows", t
assert t["identity"] in ("x86_64-windows-msvc", "x86_64-windows-musl", "x86_64-windows-gnu"), t
ids = [c["identity"] for c in t["candidates"]]
assert ids[0] == "x86_64-windows-msvc", ids
assert any(c["identity"] == t["identity"] and c["provider"] == t["provider"] for c in t["candidates"]), t
PY
echo "  ok  why target states the request, the identity and the candidates in order"
echo "PASS: 1042 a row default is replaced and each flag names its layer"
