#!/bin/sh
# #655 probe: the command the build runs against the `arguments` both databases list.
# Usage: MCPP=<mcpp binary> sh 2026-09-17-655-probe.sh <scratch dir>
set -eu
M=${MCPP:-mcpp}; S=${1:?scratch dir}
rm -rf "$S"; mkdir -p "$S/src"; cd "$S"
cat > mcpp.toml <<'TOML'
[package]
name = "i655"
version = "0.1.0"

[build]
sources = ["src/*.c"]
cflags  = ["-DESC=\\\"esc.h\\\"", "-DMID=\"mid\"", "-DSQ='sq'"]
defines = ["DEF=\"def\"", "SPACE=\"a b\""]
TOML
printf 'int main(void){return 0;}\n' > src/main.c
"$M" build >/dev/null 2>&1
"$M" emit build-database --format json > db.json 2>/dev/null
D=$(ls -d target/*/*/ | head -1)
CMD=$(cd "$D" && ninja -t commands obj/main.o | tail -1 | sed 's/ -MMD .*$/ -E -dM -x c \/dev\/null/')
(cd "$D" && /bin/sh -c "$CMD") | grep -E 'define (ESC|MID|SQ|DEF|SPACE) ' | sed 's/^/READING build: /'
python3 - <<'PY'
import json, subprocess
a = json.load(open('compile_commands.json'))[0]['arguments']
i = a.index('-c')
r = subprocess.run(a[:i] + ['-E', '-dM', '-x', 'c', '/dev/null'], capture_output=True, text=True)
print('READING database exec rc=%d' % r.returncode)
for l in r.stdout.splitlines():
    w = l.split()
    if len(w) > 1 and w[1] in ('ESC', 'MID', 'SQ', 'DEF', 'SPACE'):
        print('READING database:', l)
PY
