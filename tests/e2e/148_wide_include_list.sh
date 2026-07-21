#!/usr/bin/env bash
# #261: a package with a wide include-dir list must scan, compile and link.
#
# The failure this guards against is Windows-only in its symptom but not in
# its cause: every -I lands on the scan AND compile command lines, which have
# a hard ceiling on Windows (8191 through cmd.exe before #261, 32767 through
# CreateProcess after). The regression was found from the CONSUMER side —
# the package's own CI was green because its paths were short, and only a
# deep dependency path pushed the same command over the limit.
#
# Running it on every platform keeps the shape honest (POSIX proves the
# inline form still works, Windows proves the response-file form does).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new wideinc > /dev/null
cd wideinc

# 64 include dirs, each with a header that only that dir provides, so the
# build genuinely depends on every entry being present on the command line.
N=64
INCS=""
for i in $(seq 1 $N); do
    d="vendor/dir_with_a_deliberately_long_name_$i/include"
    mkdir -p "$d"
    echo "#define WIDE_INC_$i $i" > "$d/wide_$i.h"
    INCS="$INCS\"$d\", "
done

python3 - "$N" <<'PY' > src/wide.cpp
import sys
n = int(sys.argv[1])
for i in range(1, n + 1):
    print(f'#include <wide_{i}.h>')
print('extern "C" int wide_sum() { return 0')
for i in range(1, n + 1):
    print(f'    + WIDE_INC_{i}')
print('    ; }')
PY

cat > src/main.cpp <<'EOF'
import std;
extern "C" int wide_sum();
int main() {
    std::println("wide_sum = {}", wide_sum());
    return wide_sum() == (64 * 65) / 2 ? 0 : 1;
}
EOF

python3 - "$INCS" <<'PY'
import sys, re, pathlib
incs = sys.argv[1].rstrip(', ')
p = pathlib.Path('mcpp.toml')
s = p.read_text()
s = re.sub(r'(?m)^\[build\]$', f'[build]\ninclude_dirs = [{incs}]', s, count=1)
if 'include_dirs' not in s:
    s += f'\n[build]\ninclude_dirs = [{incs}]\n'
p.write_text(s)
PY

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "wide include build failed"; exit 1; }
out=$("$MCPP" run 2>&1) || { echo "$out"; echo "wide include run failed"; exit 1; }
echo "$out" | grep -q "wide_sum = 2080" || {
    echo "$out"; echo "expected wide_sum = 2080 (every -I must be present)"; exit 1; }

echo "PASS: 148_wide_include_list"
