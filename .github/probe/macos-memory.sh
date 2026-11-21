#!/usr/bin/env bash
# Prints, never asserts: the counts behind mcpplibs.os memory_available on macOS while three and
# six compilers run, and the kernel's own measure beside them.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
sw_vers; sysctl hw.memsize hw.activecpu hw.physicalcpu kern.memorystatus_level
clang -O1 -o "$work/sampler" "$here/sampler.c" || exit 0
echo "== idle"; "$work/sampler" 5
for n in 3 6; do
  echo "== $n compilers"
  "$work/sampler" 120 > "$work/samples-$n.txt" &
  sampler=$!
  start=$(date +%s)
  for i in $(seq 1 "$n"); do clang++ -std=c++23 -O2 -c "$here/heavy.cpp" -o "$work/heavy-$i.o" & done
  wait $(jobs -p | grep -vx "$sampler")
  echo "compiles took $(( $(date +%s) - start )) s"
  kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
  sort -t'|' -k1,1 "$work/samples-$n.txt" | awk '{print $2}' | sort -n | head -1 | sed 's/^/min free+inactive MiB: /'
  awk -F'level ' '{print $2}' "$work/samples-$n.txt" | sort -n | head -1 | sed 's/^/min memorystatus_level: /'
  head -3 "$work/samples-$n.txt"; echo ...; tail -3 "$work/samples-$n.txt"
done
echo "== after"; "$work/sampler" 3
