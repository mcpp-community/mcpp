#!/usr/bin/env bash
# Build a hosted companion beside an existing runtime checkout/cache. Running
# the resulting binary belongs to the target runner, which needs no toolchain.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 RUNTIME_CHECKOUT TARGET [OUTPUT_BINARY_PATH_FILE]" >&2
    exit 2
fi
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
runtime_root=$(cd "$1" && pwd)
target=$2
output_file=${3:-}
# Actions supplies native drive/UNC paths on Windows, even to a Bash step.
if [ -n "$output_file" ] && command -v cygpath >/dev/null 2>&1; then
    output_file=$(cygpath -u "$output_file")
fi
if [ -n "$output_file" ] && [[ "$output_file" != /* ]]; then
    output_file="$PWD/$output_file"
fi
source "$repo_root/tests/e2e/_toolchain_env.sh"
mcpp_bin=${MCPP:-mcpp}
case "$mcpp_bin" in
    */*|*\\*|[A-Za-z]:*)
        if command -v cygpath >/dev/null 2>&1; then
            mcpp_bin=$(cygpath -u "$mcpp_bin")
        fi
        ;;
esac
if [[ "$mcpp_bin" == */* ]] && [[ "$mcpp_bin" != /* ]]; then
    mcpp_bin="$PWD/$mcpp_bin"
fi
llvm_toolchain=${LLVM_TOOLCHAIN:-llvm@$LLVM_VERSION}
fixture="$runtime_root/examples/mcpp-hosted-threads"
mkdir -p "$fixture/src"
cp "$repo_root/tests/fixtures/openkal-hosted-threads/mcpp.toml" "$fixture/mcpp.toml"
cp "$repo_root/tests/fixtures/openkal-hosted-threads/src/main.cpp.in" "$fixture/src/main.cpp"
cd "$fixture"
"$mcpp_bin" build --target "$target" --toolchain "$llvm_toolchain"
binaries=()
while IFS= read -r path; do
    binaries+=("$path")
done < <(find "target/$target" -type f \( -name openkal-hosted-threads -o -name openkal-hosted-threads.exe \))
if [ "${#binaries[@]}" -ne 1 ]; then
    echo "expected one hosted threads binary for $target, found ${#binaries[@]}" >&2
    exit 1
fi
binary="$fixture/${binaries[0]}"
[ -z "$output_file" ] || printf '%s\n' "$binary" > "$output_file"
printf 'OPENKAL_HOSTED_THREADS_BINARY=%s\n' "$binary"
