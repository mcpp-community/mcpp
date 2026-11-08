#!/usr/bin/env bash
# requires: elf gcc
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1047 -- an artifact compiles the shared sources it links with its own flags (mcpp#911 G9).
#
# `[targets.<name>.build]` states defines and flags for every translation unit of the package the
# artifact links: its entry and the shared implementation sources, compiled as the artifact's own
# variant under obj/_variant/<name>/ where they differ. The other artifacts link the package's
# objects as before; a module interface stays one per package.
#
# Criteria: the program with the define returns the shared source's defined value (5), the other
# returns the plain one (4); a variant object exists for the varied artifact only; an
# implementation unit that imports the package's module builds in both.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
mkdir -p "$t/src"
cat > "$t/mcpp.toml" <<'EOF'
[package]
name    = "shell"
version = "0.1.0"

[targets.cmd]
kind = "bin"
main = "src/cmd.cpp"

[targets.COMMAND]
kind = "bin"
main = "src/command.cpp"

[targets.COMMAND.build]
defines = ["ENABLE_AUTOEXEC=1"]
EOF
cat > "$t/src/base.cppm" <<'EOF'
export module shell.base;
export int base_value() { return 4; }
EOF
cat > "$t/src/shell.cpp" <<'EOF'
import shell.base;
int autoexec() {
#ifdef ENABLE_AUTOEXEC
    return base_value() + 1;
#else
    return base_value();
#endif
}
EOF
echo 'int autoexec(); int main() { return autoexec(); }' > "$t/src/cmd.cpp"
echo 'int autoexec(); int main() { return autoexec(); }' > "$t/src/command.cpp"
cd "$t"

out=$("$MCPP" build 2>&1) || fail "the build failed" "$out"
dir=$(dirname "$(find target -path '*/bin/cmd' -type f | head -1)")
set +e; "$dir/cmd"; a=$?; "$dir/COMMAND"; b=$?; set -e
[ "$a" = 4 ] || fail "cmd returned $a, not the shared source's plain 4"
[ "$b" = 5 ] || fail "COMMAND returned $b, not 5 from its own define"
[ -n "$(find target -path '*/obj/_variant/COMMAND/*' -name 'shell.o' | head -1)" ] || fail "no variant object of COMMAND"
[ -z "$(find target -path '*/obj/_variant/cmd*' | head -1)" ] || fail "an artifact without flags of its own has a variant"
echo "  ok  COMMAND compiles the shared source with its define; cmd links the package's object"
echo "PASS: 1047 an artifact compiles its shared sources with its own flags"
