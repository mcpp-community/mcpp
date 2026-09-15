#!/usr/bin/env bash
# requires: elf
# 731 -- a child started while `emit build-database` plans does not hold the
# caller's pipe (#648 L2).
#
# `emit build-database` sends planning narration to stderr by redirecting
# descriptor 1 and saving the original. The saved copy was inheritable, so a
# build program, a hook or an xlings process started during planning held the
# caller's pipe open, and a caller reading mcpp's output waited for that child
# rather than for mcpp. Criterion: a build program lists its descriptors, and
# none of them names the pipe the reader of mcpp's standard output reads.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > "$TMP/app/src/main.cpp"
cat > "$TMP/app/build.mcpp" <<EOF
import std;
import mcpp;
int main() {
    std::ofstream out("$TMP/fds.txt");
    for (auto const& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        out << e.path().filename().string() << " "
            << std::filesystem::read_symlink(e.path(), ec).string() << "\n";
    }
    return 0;
}
EOF

cd "$TMP/app"
MCPP_OFFLINE=1 "$MCPP" emit build-database --format json 2> err.txt \
    | { readlink /proc/self/fd/0 > "$TMP/reader.txt"; cat > "$TMP/out.json"; }
[ -s "$TMP/fds.txt" ] || fail "the build program did not run" err.txt
PIPE=$(cat "$TMP/reader.txt")
case "$PIPE" in pipe:*) ;; *) fail "the reader's stdin is not a pipe: $PIPE" ;; esac
if grep -F " $PIPE" "$TMP/fds.txt"; then
    fail "a build program holds the caller's pipe $PIPE" "$TMP/fds.txt"
fi
echo "PASS: 731 planning children do not inherit the caller's pipe"
