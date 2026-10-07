#!/usr/bin/env bash
# Positive admission for the native Linux ARM64 LLVM payload.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || {
    echo 'FAIL: this gate requires a native Linux aarch64 host'; exit 1;
}
MCPP="${MCPP:-mcpp}"
"$MCPP" toolchain install llvm 23.1.3
MCPP_E2E_LLVM_VERSION=23.1.3 source tests/e2e/_toolchain_env.sh
[[ -x "$LLVM_ROOT/bin/clang++" ]] || { echo 'FAIL: LLVM frontend missing'; exit 1; }
file -L "$LLVM_ROOT/bin/clang++" | grep -q 'ARM aarch64' || {
    echo 'FAIL: LLVM frontend is not native ARM64'; exit 1;
}
"$LLVM_ROOT/bin/clang++" --version
work="$(mktemp -d)"
report="${MCPP_NATIVE_REPORT_DIR:-${RUNNER_TEMP:-$work}/native-llvm-arm64}"
mkdir -p "$report"
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/mcpp-home"
"$MCPP" self config --mirror "${MCPP_E2E_MIRROR:-GLOBAL}"
"$MCPP" self env --format json | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["data"]["defaultToolchain"] == "llvm@23.1.3", d'
"$MCPP" new "$work/native-probe"
mkdir -p "$work/nativeabi/src"
cat > "$work/nativeabi/mcpp.toml" <<'TOML'
[package]
name = "nativeabi"
version = "0.1.0"
[build]
sources = ["src/*.c"]
[targets.nativeabi]
kind = "shared"
soname = "libnativeabi.so.1"
TOML
printf 'int native_answer(int x) { return x + 1; }\n' > "$work/nativeabi/src/answer.c"
cd "$work/native-probe"
cat >> mcpp.toml <<'TOML'

[dependencies]
nativeabi = { path = "../nativeabi" }
TOML
cat > src/main.cpp <<'CPP'
import std;
import std.compat;
extern "C" int native_answer(int);
int native_headers();
struct alignas(16) Pair { unsigned long a, b; };
static_assert(sizeof(Pair) == 16);
int main() {
    auto memory = std::make_unique<int>(41);
    int value = 0;
    std::thread worker([&] { value = native_answer(*memory); });
    worker.join();
    bool caught = false;
    try { throw std::runtime_error("native"); }
    catch (const std::runtime_error&) { caught = true; }
    std::atomic<Pair> atom;
    atom.store(Pair{0, 0});
    Pair expected{0, 0};
    bool exchanged = atom.compare_exchange_strong(expected, Pair{42, 7});
    auto result = atom.load();
    void* raw = ::malloc(32);
    bool allocated = raw != nullptr;
    ::free(raw);
    bool ok = value == 42 && caught && exchanged && result.a == 42
        && result.b == 7 && allocated && native_headers() == 1;
    ::printf("native-stdlib-cabi=%s\n", ok ? "ok" : "failed");
    return ok ? 0 : 1;
}
CPP
cat > src/headers.cpp <<'CPP'
#include <features.h>
#include <stddef.h>
#include <unistd.h>
#include <pthread.h>
int native_headers() { return 1; }
CPP
"$MCPP" build
"$MCPP" run | tee "$report/runtime.log"
grep -qF 'native-stdlib-cabi=ok' "$report/runtime.log"
binary="$(find target/aarch64-linux-gnu -type f -path '*/bin/native-probe' | head -1)"
[[ -n "$binary" ]] || { echo 'FAIL: default did not produce a native GNU artifact'; exit 1; }
readelf -l "$binary" > "$report/program-headers.txt"
readelf -d "$binary" > "$report/dynamic.txt"
grep -q 'ld-linux-aarch64.so.1' "$report/program-headers.txt" || {
    echo 'FAIL: native default is not glibc-linked'; exit 1;
}
interpreter="$(sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p' "$report/program-headers.txt")"
case "$interpreter" in
  "$MCPP_HOME"/registry/data/xpkgs/xim-x-glibc/*/lib*/ld-linux-aarch64.so.1) ;;
  *) echo "FAIL: GNU default uses an ambient loader: $interpreter"; exit 1 ;;
esac
"$interpreter" --list "$binary" > "$report/loader-resolution.txt"
python3 - "$report/loader-resolution.txt" <<'PYLOADER'
import pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text()
assert not re.search(r'=> /(?:usr/lib|lib64?|usr/local/lib)/', text), text
print('PASS: the native loader resolves no ambient system library')
PYLOADER
grep -q 'libnativeabi.so.1' "$report/dynamic.txt" || {
    echo 'FAIL: the C ABI consumer does not depend on the shared library'; exit 1;
}
# Replay the engine's effective header compile as a preprocess trace. The trace
# must resolve libc headers through the payload, without an ambient /usr tree.
python3 - "$report" <<'PYTRACE'
import json, os, pathlib, re, shlex, subprocess, sys
report = pathlib.Path(sys.argv[1])
cdb = next(pathlib.Path('target/aarch64-linux-gnu').rglob('compile_commands.json'))
entries = json.loads(cdb.read_text())
entry = next(e for e in entries if e['file'].endswith('/headers.cpp'))
args = entry.get('arguments') or shlex.split(entry['command'])
# The driver named by the cold project's CDB must itself have a managed
# ARM64 loader and dependency closure, independently of the program it emits.
store = (pathlib.Path(os.environ['MCPP_HOME']) / 'registry/data/xpkgs').resolve()
compiler = pathlib.Path(args[0]).resolve()
relative = compiler.relative_to(store)
assert relative.parts[:2] == ('xim-x-llvm', '23.1.3'), compiler

driver_env = dict(os.environ)
driver_env.pop('LD_LIBRARY_PATH', None)
driver_env.pop('LD_PRELOAD', None)

def capture(name, command):
    result = subprocess.run(command, text=True, capture_output=True, env=driver_env)
    text = result.stdout + result.stderr
    (report / name).write_text(text)
    assert result.returncode == 0, (command, text)
    return text

header = capture('compiler-elf-header.txt', ['readelf', '-hW', str(compiler)])
assert re.search(r'Machine:\s+AArch64', header), header
version = capture('compiler-version.txt', [str(compiler), '--version'])
assert re.search(r'clang version 23\.1\.3(?:\s|$)', version), version
program_headers = capture('compiler-program-headers.txt', ['readelf', '-lW', str(compiler)])
match = re.search(r'Requesting program interpreter: ([^\]]+)', program_headers)
assert match, program_headers
loader = pathlib.Path(match.group(1)).resolve()
loader_relative = loader.relative_to(store)
assert loader_relative.parts[0] == 'xim-x-glibc', loader
assert loader.name == 'ld-linux-aarch64.so.1', loader
closure = capture('compiler-loader-resolution.txt', [str(loader), '--list', str(compiler)])
assert not re.search(r'=> /(?:usr/lib|lib64?|usr/local/lib)/', closure), closure
for path in re.findall(r'=> (/\S+)', closure):
    pathlib.Path(path).resolve().relative_to(store)
print('PASS: cold LLVM frontend has native ARM64 managed loader and libraries')
clean = []
i = 0
while i < len(args):
    arg = args[i]
    if arg in ('-o', '-MF', '-MT', '-MQ'):
        i += 2
        continue
    if arg in ('-c', '-MMD', '-MD', '-MP'):
        i += 1
        continue
    clean.append(arg)
    i += 1
result = subprocess.run(clean + ['-E', '-H'], cwd=entry['directory'],
                        text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
(report / 'include-trace.txt').write_text(result.stderr)
assert result.returncode == 0, result.stderr
headers = [m.group(1) for line in result.stderr.splitlines()
           if (m := re.match(r'^\.+ (.+)$', line))]
assert any('xim-x-glibc' in path and path.endswith('/features.h') for path in headers), headers
assert not any(re.match(r'/usr/(include|lib/gcc|local/include)(/|$)', path) for path in headers), headers
print('PASS: libc include trace uses the managed payload')
PYTRACE
cat "$report/program-headers.txt" "$report/loader-resolution.txt" \
    "$report/compiler-version.txt" "$report/compiler-elf-header.txt" \
    "$report/compiler-program-headers.txt" "$report/compiler-loader-resolution.txt" \
    "$report/include-trace.txt"
"$MCPP" pack --mode self-contained --format dir --message-format json > "$report/pack.json"
bundle="$(python3 - "$report/pack.json" <<'PYPACK'
import json, sys
artifact = json.load(open(sys.argv[1]))['data']['artifacts'][0]
assert artifact['type'] == 'directory', artifact
print(artifact['path'])
PYPACK
)"
# Directory deployment exercises the documented portable bundle entry point.
cp -a "$bundle" "$work/deployed"
(cd / && env -u LD_LIBRARY_PATH "$work/deployed/native-probe") | tee "$report/deployed.log"
grep -qF 'native-stdlib-cabi=ok' "$report/deployed.log"
"$MCPP" new "$work/musl-probe"
cd "$work/musl-probe"
"$MCPP" build --target aarch64-linux-musl --toolchain gcc@16.1.0-musl
"$MCPP" run --target aarch64-linux-musl --toolchain gcc@16.1.0-musl
printf '%s\n' 'PASS: native ARM64 LLVM installs, builds and runs'
