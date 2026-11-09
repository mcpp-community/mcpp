#!/usr/bin/env bash
# requires: elf llvm
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 1048 -- a git reference with no tag, rev or branch follows the remote's default branch; a
# reference on the command line takes the same forms (SPEC-001 §5; SPEC-016 §8).
#
# The default branch is the one the remote announces (`git ls-remote --symref`), here `trunk`; the
# lock records it and its commit, and an offline build reads them. A dependency that names two of
# rev / tag / branch is refused. `--platform git+<url>#tag=<t>` selects a platform from a
# repository as a manifest's `{ git = ..., tag = ... }` does.
#
# Criteria: the build reports the default branch and runs; mcpp.lock names `#branch=trunk@<sha>`;
# an offline rebuild succeeds; two refs are refused; the platform from git builds its row.
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | tail -20; exit 1; }
g() { git -c user.name=e2e -c user.email=e2e@example.invalid -c init.defaultBranch=trunk "$@"; }
mkdir -p "$t/lib/src" "$t/app/src" "$t/toy-platform" "$t/bare/src"
cat > "$t/lib/mcpp.toml" <<'EOF'
[package]
name    = "gitlib"
version = "0.1.0"

[targets.gitlib]
kind = "lib"
EOF
echo 'int gitlib_value() { return 9; }' > "$t/lib/src/lib.cpp"
(cd "$t/lib" && g init -q . && g add -A && g commit -qm init)
cat > "$t/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[dependencies]
gitlib = { git = "$t/lib" }
EOF
echo 'int gitlib_value(); int main() { return gitlib_value(); }' > "$t/app/src/main.cpp"
cd "$t/app"

out=$("$MCPP" build 2>&1) || fail "the build failed" "$out"
grep -q "the default branch of .* is trunk" <<<"$out" || fail "the default branch is not reported" "$out"
set +e; "$(find target -path '*/bin/app' -type f | head -1)"; rc=$?; set -e
[ "$rc" = 9 ] || fail "the program returned $rc, not 9"
grep -Eq 'source  = "git\+.*#branch=trunk@[0-9a-f]{40}"' mcpp.lock || fail "the lock does not record the branch and its commit" "$(cat mcpp.lock)"
rm -rf target
out=$("$MCPP" build --offline 2>&1) || fail "the offline build did not read the lock" "$out"
echo "  ok  the default branch is followed, locked, and read offline"

sed -i "s|gitlib = { git = \"$t/lib\" }|gitlib = { git = \"$t/lib\", tag = \"v1\", branch = \"trunk\" }|" mcpp.toml
out=$("$MCPP" build 2>&1 || true)
grep -q "names 2 of rev / tag / branch; a git dependency names at most one" <<<"$out" || fail "two refs were not refused" "$out"
echo "  ok  a dependency that names two refs is refused"

cat > "$t/toy-platform/mcpp.toml" <<EOF
[package]
name    = "toy-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@${LLVM_VERSION}"

[[platform.define]]
triple        = "x86_64-toyos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
execution     = "linux"

[[platform.provider]]
serves = ["x86_64-toyos"]

[platform.build]
cflags = ["-ffreestanding", "-fno-stack-protector", "-fno-pic"]

[platform.link]
driver = "compiler"

[platform.layers]
"c-abi" = false
"c++-abi" = false
"compiler-runtime" = false
"kernel-abi" = false
EOF
(cd "$t/toy-platform" && g init -q . && g add -A && g commit -qm toy && g tag v1)
cat > "$t/bare/mcpp.toml" <<'EOF'
[package]
name    = "bare"
version = "0.1.0"

[build]
sources = ["src/*.c"]
ldflags = ["-nostdlib", "-static", "-no-pie"]

[targets.bare]
kind = "bin"
main = "src/main.c"
EOF
printf 'void _start(void) { __asm__ volatile("mov $60, %%eax\\n mov $7, %%edi\\n syscall"); }\n' > "$t/bare/src/main.c"
cd "$t/bare"
out=$("$MCPP" build --target x86_64-toyos --platform "git+$t/toy-platform#tag=v1" 2>&1) || fail "the platform from git did not build its row" "$out"
grep -q "Platform toy-platform 0.1.0" <<<"$out" || fail "the platform is not reported" "$out"
echo "  ok  --platform git+<url>#tag=<t> selects the platform from its repository"
echo "PASS: 1048 a git reference follows the default branch"
