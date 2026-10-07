#!/usr/bin/env bash
# Run only the exact public release inside the real native ARM64 SubOS.
set -euo pipefail
phase="${1:?usage: ecosystem-cn.sh release VERSION}"
version="${2:?mcpp version under test required}"
base="${XLINGS_HOME:?explicit isolated XLINGS_HOME required}"
[[ "$(uname -m)" == aarch64 ]] || { echo "native ARM64 required"; exit 1; }
[[ "$phase" == release ]] || { echo "published release only"; exit 1; }
[[ ! -d "${CN_HOST_CHECKOUT:?host checkout path required}" ]] || {
    echo 'host checkout must not be visible in the SubOS'; exit 1;
}
xl="${CN_PUBLISHED_XLINGS:?published client path required}"
work="${CN_PROBE_RUN_DIR:-$base/probes/runs/$phase-$version-$(date +%Y%m%dT%H%M%S)}"
[[ "$work" == "$base/probes/runs/"* ]] || exit 1
mkdir -p "$work"
exec > >(tee -a "$work/probe.log") 2>&1
printf 'phase=%s version=%s XLINGS_HOME=%s sandbox_HOME=%s\n' "$phase" "$version" "$base" "$HOME"
cat /proc/self/mountinfo > "$work/subos-mountinfo.txt"
export MCPP_HOME="$work/mcpp-home"
export MCPP_VENDORED_XLINGS="$xl"
export MCPP_E2E_MIRROR=CN MCPP_E2E_LLVM_VERSION=23.1.3 MCPP_E2E_EXPECT_ARCH=aarch64
unset LD_LIBRARY_PATH LD_PRELOAD MCPP_TOOLCHAIN
"$xl" config --mirror CN
"$xl" install "mcpp@$version" -y -u
MCPP="$base/data/xpkgs/xim-x-mcpp/$version/bin/mcpp"
[[ -x "$MCPP" ]] || { echo "mcpp not executable: $MCPP"; exit 1; }
export MCPP
"$MCPP" --version | tee "$work/mcpp-version.txt"
got=$(grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' "$work/mcpp-version.txt" | head -1)
[[ "$got" == "$version" ]] || { echo "unexpected mcpp version $got"; exit 1; }
sha256sum "$MCPP" > "$work/mcpp-binary.sha256"
[[ "$(sha256sum "$MCPP" | cut -d ' ' -f 1)" == "${CN_EXPECTED_BINARY_SHA:?CN archive identity required}" ]] || exit 1
"$MCPP" self config --mirror CN
# No default setter or explicit install may conceal the engine's ARM64 default.
[[ "$(file -b "$MCPP")" == *"ARM aarch64"* ]] || exit 1
case "$(uname -m)" in
 x86_64) native=x86_64-linux-gnu ;;
 aarch64) native=aarch64-linux-gnu ;;
 *) echo 'unsupported native host'; exit 1 ;;
esac
cd "$work"
project="${CN_PROBE_PROJECT:-hello}"
"$MCPP" new "$project"
cd "$work/$project"
"$MCPP" build 2>&1 | tee "$work/native-default-build.log"
grep -q 'Resolved llvm@23.1.3' "$work/native-default-build.log"
grep -Eq 'aarch64-(unknown-)?linux-gnu' "$work/native-default-build.log"
mapfile -t native_bins < <(find target -type f -path "*/bin/$project")
[[ "${#native_bins[@]}" == 1 ]] || exit 1
readelf -hW "${native_bins[0]}" | tee "$work/native-header.txt"
grep -q 'Machine:.*AArch64' "$work/native-header.txt"
readelf -lW "${native_bins[0]}" > "$work/native-program-headers.txt"
python3 - "$MCPP_HOME" "$work/native-program-headers.txt" <<'PYLOADER'
import pathlib,re,sys
home=pathlib.Path(sys.argv[1]).resolve()
text=pathlib.Path(sys.argv[2]).read_text()
m=re.search(r'Requesting program interpreter: ([^\]]+)',text)
assert m,text
loader=pathlib.Path(m[1]).resolve()
loader.relative_to(home/'registry/data/xpkgs/xim-x-glibc')
assert loader.name=='ld-linux-aarch64.so.1',loader
PYLOADER
"$MCPP" run | tee "$work/native-run.log"
grep -q "Hello from $project!" "$work/native-run.log"
"$MCPP" pack --mode self-contained --format dir --message-format json > "$work/pack.json"
bundle=$(python3 - "$work/pack.json" <<'PY'
import json, sys
artifact=json.load(open(sys.argv[1]))['data']['artifacts'][0]
assert artifact['type']=='directory', artifact
print(artifact['path'])
PY
)
cp -a "$bundle" "$work/deployed"
(cd / && env -u LD_LIBRARY_PATH "$work/deployed/$project") | tee "$work/deployed.log"
grep -q "Hello from $project!" "$work/deployed.log"
cd "$work"
git clone --depth 1 --branch "v$version" https://github.com/mcpp-community/mcpp mcpp-source
git -C mcpp-source rev-parse HEAD | tee "$work/mcpp-source-sha.txt"
cd mcpp-source
bash tests/e2e/286_the_openkal_stack_still_builds.sh | tee "$work/openkal-native.log"
! grep -q 'SKIP' "$work/openkal-native.log"
grep -qF 'OK: the openkal stack builds, links statically and runs' "$work/openkal-native.log"
cd "$work"
git clone --depth 1 https://github.com/mcpp-community/mcpp-index index-source
git -C index-source rev-parse HEAD | tee "$work/index-source-sha.txt"
export CN_PROBE_MCPP="$MCPP" CN_PROBE_TARGET="$native" CN_PROBE_WORK="$work"
cat > "$work/llvm-consumer" <<'ADAPTER'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in build|test|run) set -- "$@" --toolchain llvm@23.1.3 --target "$CN_PROBE_TARGET" ;; esac
{ printf 'argv:'; printf ' %q' "$@"; printf '\n'; } >> "$CN_PROBE_WORK/consumer-argv.log"
exec "$CN_PROBE_MCPP" "$@"
ADAPTER
chmod +x "$work/llvm-consumer"
cd "$work/index-source"
MCPP="$work/llvm-consumer" MCPP_VERBOSE=1 MCPP_TIMINGS="$work/members.tsv" \
    bash tests/run_members.sh cjson sqlite3 fmtlib.fmt nlohmann.json | tee "$work/members.log"
python3 - "$work/members.tsv" <<'PY'
import pathlib,sys
rows=[line.split('\t') for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
assert len(rows)==4 and {r[1] for r in rows}=={'cjson','sqlite3','fmtlib.fmt','nlohmann.json'},rows
assert all(r[2]=='ok' for r in rows),rows
PY
# The ecosystem's own same-source example generates its target runtimes from
# the graph. Foreign artifacts are inspected here; target OS execution is a
# separate acceptance gate, never implied by this Linux sandbox.
cd "$work"
git clone --depth 1 --branch "${OPENKAL_SOURCE_REF:-main}" https://github.com/mcpplibs/openkal-llvm-runtime openkal-source
git -C openkal-source rev-parse HEAD | tee "$work/openkal-source-sha.txt"
cp "$work/openkal-source-sha.txt" "$work/source-sha.txt"
cd "$work/openkal-source/examples/same-source"
readobj="$MCPP_HOME/registry/data/xpkgs/xim-x-llvm/23.1.3/bin/llvm-readobj"
[[ -x "$readobj" ]] || { echo 'LLVM readobj unavailable'; exit 1; }
mkdir -p "$work/cross"
for target in x86_64-linux-gnu aarch64-macos x86_64-windows-gnu; do
    rm -rf target
    "$MCPP" build --target "$target" --toolchain llvm@23.1.3 | tee "$work/cross/$target.build.log"
    mapfile -t artifacts < <(find target -type f \( -name openkal-same-source -o -name openkal-same-source.exe \))
    [[ "${#artifacts[@]}" == 1 ]] || { echo 'cross artifact missing or ambiguous'; exit 1; }
    "$readobj" --file-headers "${artifacts[0]}" > "$work/cross/$target.headers.txt"
    case "$target" in
      x86_64-linux-gnu) grep -q 'Format: elf64-x86-64' "$work/cross/$target.headers.txt"; grep -q 'Arch: x86_64' "$work/cross/$target.headers.txt" ;;
      aarch64-macos) grep -q 'Format: Mach-O' "$work/cross/$target.headers.txt"; grep -q 'Arch: aarch64' "$work/cross/$target.headers.txt" ;;
      x86_64-windows-gnu) grep -q 'Format: COFF' "$work/cross/$target.headers.txt"; grep -q 'Arch: x86_64' "$work/cross/$target.headers.txt" ;;
      *) echo "unreviewed selected target: $target"; exit 1 ;;
    esac
    case "$target" in
      x86_64-linux-gnu) output=linux ;;
      aarch64-macos) output=macos ;;
      x86_64-windows-gnu) output=windows.exe ;;
    esac
    cp "${artifacts[0]}" "$work/cross/$output"
    (cd "$work/cross" && sha256sum "$output") >> "$work/cross/SHA256SUMS"
done
echo 'RELEASE BUILD PASS: CN install/default/new/build/run/pack, native openkal, four consumers and three target artifacts.'
cp "$work/openkal-source-sha.txt" "$work/cross/source-sha.txt"
printf '%s\n' "$work" > "$base/probes/last-run.txt"
echo 'Foreign target execution is pending the three target-system jobs.'
