#!/usr/bin/env bash
# requires: llvm
# 785_cdb_interface_flag_module_extensions.sh — C4 (design 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.4): the
# record states a module interface's language explicitly
# (BmiTraits::moduleInterfaceLangFlag), at the position the build uses --
# immediately before `-c` -- for every unit that provides a module, so a
# reader does not have to guess it from the extension. Clang does not know
# `.ixx` on its own and hands it to the linker with no `-x`, so this is a real
# failure there, not a cosmetic one. Must FAIL on 2026.9.26.1.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
mkdir -p app/src
cd app
cat > mcpp.toml <<'EOF'
[package]
name = "ixxtest"
version = "0.1.0"
standard = "c++23"

[build]
module_extensions = [".ixx"]
EOF
cat > src/greet.ixx <<'EOF'
export module ixxtest.greet;
export int answer() { return 42; }
EOF
cat > src/main.cpp <<'EOF'
import ixxtest.greet;
int main() { return answer() == 42 ? 0 : 1; }
EOF

"$MCPP" build --toolchain llvm@22.1.8 > build.log 2>&1 || {
    cat build.log; echo "FAIL: build failed"; exit 1; }

python3 - <<'PY'
import json, os, subprocess, sys

entries = json.load(open("compile_commands.json"))
iface = next(e for e in entries if e["file"].endswith("greet.ixx"))
args = iface["arguments"]

# The flag sits immediately before `-c` (GNU dialects read it positionally).
c_at = args.index("-c")
assert c_at >= 2, args
assert args[c_at - 2:c_at] == ["-x", "c++-module"], args

# And replays: the same argv, run from `directory`, must actually produce a
# BMI+object -- the measured failure this criterion guards is clangd's
# `[fe_expected_compiler_job]`, which is exactly "no compiler job ran".
obj = iface["output"]
if os.path.exists(obj):
    os.remove(obj)
rc = subprocess.run(args, cwd=iface["directory"], stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE)
if rc.returncode != 0 or not os.path.exists(obj):
    sys.stderr.write(rc.stderr.decode(errors="replace"))
    print("FAIL: the interface entry did not replay into its object")
    sys.exit(1)

# The non-module unit carries no such flag at all.
main = next(e for e in entries if e["file"].endswith("main.cpp"))
assert "-x" not in main["arguments"], main["arguments"]
print("ok: the interface entry carries -x c++-module immediately before -c, and replays")
print("ok: the non-module unit carries no language flag")
PY

echo "PASS: 785 the compile database states a module interface's language explicitly"
