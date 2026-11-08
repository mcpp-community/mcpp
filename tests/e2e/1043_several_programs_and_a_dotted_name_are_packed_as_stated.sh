#!/usr/bin/env bash
# requires: elf unix-shell
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1043 -- several programs are packed as stated, and a program whose name has a dot is packed
# (mcpp#911 B1, B3).
#
# A package with several programs and none named after it is not packed by a guess: `mcpp pack`
# refuses and names the two forms, `mcpp pack <program>` and `mcpp pack --all-programs`. With
# one named after the package, that one is packed and the others are said. A manifest `mcpp pack`
# generates quotes a target name with a dot (`COMMAND.COM`), so the generated project parses.
#
# Criteria: the refusal lists the programs; `--all-programs` ships both under bin/; a single-file
# pack of `COMMAND.COM` builds its launcher and the file runs with the program's exit status.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF'
[package]
name    = "multi"
version = "0.1.0"

[targets.alpha]
kind = "bin"
main = "src/alpha.cpp"

[targets."COMMAND.COM"]
kind = "bin"
main = "src/command.cpp"
EOF
echo 'int main() { return 0; }' > "$t/src/alpha.cpp"
echo 'int main() { return 3; }' > "$t/src/command.cpp"
cd "$t"

set +e; out=$("$MCPP" pack 2>&1); rc=$?; set -e
[ "$rc" != 0 ] || fail "several programs were packed by a guess" "$out"
grep -q "several programs (COMMAND.COM, alpha) and none is named after it" <<<"$out" || fail "the refusal does not list the programs" "$out"
grep -q "mcpp pack --all-programs" <<<"$out" || fail "the refusal does not name --all-programs" "$out"
echo "  ok  several programs, none named after the package: refused with both forms"

out=$("$MCPP" pack --all-programs 2>&1) || fail "--all-programs did not pack" "$out"
listing=$(tar tzf target/dist/multi-0.1.0-*.tar.gz)
grep -q '/bin/alpha$' <<<"$listing" && grep -q '/bin/COMMAND.COM$' <<<"$listing" \
    || fail "--all-programs did not ship both programs" "$listing"
echo "  ok  --all-programs ships every program"

rm -rf target/dist
out=$("$MCPP" pack COMMAND.COM --pkg-format single-file 2>&1) || fail "the single file of COMMAND.COM was not made" "$out"
file=$(find target/dist -maxdepth 1 -type f -name 'multi-0.1.0-*' | head -1)
[ -n "$file" ] || fail "no single file under target/dist" "$(ls target/dist)"
set +e; "$file"; rc=$?; set -e
[ "$rc" = 3 ] || fail "the single file exited $rc, not 3"
echo "  ok  a program named with a dot is packed into one file and runs"

sed -i 's/^name    = "multi"/name    = "alpha"/' mcpp.toml
out=$("$MCPP" pack 2>&1) || fail "the program named after the package was not packed" "$out"
grep -q "packing alpha, the program named after the package; its other programs are not packed (COMMAND.COM)" <<<"$out" \
    || fail "the programs left out are not said" "$out"
echo "  ok  the program named after the package is packed and the others are said"
echo "PASS: 1043 several programs and a dotted name are packed as stated"
