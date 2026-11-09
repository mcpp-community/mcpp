#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1042 -- every flag names its layer, and a request its selection (mcpp#911 G2; SPEC-016 §4.5).
#
# `mcpp why flags <unit>` lists each word of the unit's command line and of the link line with the
# layer that gave it: the row's machine description, the platform, the toolchain, the package, the
# engine. `mcpp why target` lists the candidates of a request in the order the selection took them.
#
# Criteria: `why flags` attributes `-march=x86-64` and `-mno-red-zone` to the row, `-ffreestanding`
# to the engine and the package's define to the package; `why target --format json` names the
# request, the identity and the candidates in order.
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
cflags  = ["-DBARE_APP=1"]

[targets.bare]
kind = "bin"
main = "src/main.c"
EOF
cat > "$t/src/main.c" <<'EOF'
void _start(void) { __asm__ volatile("mov $60, %eax\n mov $7, %edi\n syscall"); }
EOF
cd "$t"

why=$("$MCPP" why flags main.c --target x86_64-none-elf 2>/dev/null) || fail "why flags failed" "$why"
grep -Eq '^  row +-march=x86-64 +\[x86_64-none-elf\]' <<<"$why" || fail "the row's word does not name the row" "$why"
grep -Eq '^  row +-mno-red-zone +\[x86_64-none-elf\]' <<<"$why" || fail "the row's flag does not name the row" "$why"
grep -Eq '^  engine +-ffreestanding' <<<"$why" || fail "the engine's convention does not name the engine" "$why"
grep -Eq '^  package +-DBARE_APP=1 +\[bare\]' <<<"$why" || fail "the package's define does not name the package" "$why"
grep -Eq '^  row +-m elf_x86_64' <<<"$why" || fail "the link's emulation does not name the row" "$why"
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
echo "PASS: 1042 each flag names its layer and a request its selection"
