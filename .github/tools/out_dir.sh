#!/bin/sh
# Prints where `mcpp::out_dir()` pointed for the root package in the newest
# build of the package in the current directory: its `outDir` in
# resolution.json. Each configuration has its own output directory, so a check
# reads it instead of naming target/.build-mcpp/out.
r=$(ls -t target/*/*/resolution.json 2>/dev/null | head -1)
[ -n "$r" ] || { echo "out_dir.sh: no resolution.json under target/" >&2; exit 1; }
py=$(command -v python3 || command -v python)
d=$("$py" -c 'import json, sys
for p in json.load(open(sys.argv[1])).get("graph", {}).get("packages", []):
    if p.get("root"):
        print(p.get("outDir", ""))
        break' "$r")
[ -n "$d" ] || { echo "out_dir.sh: $r records no outDir for the root package" >&2; exit 1; }
echo "$d"
