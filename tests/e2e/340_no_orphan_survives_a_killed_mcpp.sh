#!/usr/bin/env bash
# requires: gcc unix-shell
#
# NO BUILD PROCESS SURVIVES THE mcpp THAT STARTED IT.
#
# Measured before the fix, in the ecosystem sandbox: every `timeout`-terminated
# `mcpp run` left an orphaned ninja spinning at 100% of a core. One of them
# outlived the removal of the entire sandbox it belonged to — its working
# directory read `(deleted)` and it was still burning a core half an hour
# later. Any CI that wraps mcpp in `timeout` leaked a busy core per timeout.
#
# THE SIGNAL GOES TO mcpp ALONE, AND THAT IS THE WHOLE TEST.
#
# `timeout` and an interactive Ctrl-C both signal the process GROUP, which
# reached ninja even before the fix — so a test built on `timeout` passes
# either way and proves nothing. Signalling mcpp's pid alone is what separates
# "the child was told by someone else" from "mcpp took its child down".
#
# SIGKILL RATHER THAN SIGTERM IS WHY THE FIX WORKS AT ALL. ninja records a
# signal in a flag and acts on it where it waits for a subprocess; a ninja with
# no command running never reaches that check, so a polite signal is recorded
# and never obeyed. That is the state the orphans were in.
#
# THE BUILD MUST STILL BE RUNNING WHEN THE SIGNAL ARRIVES. An earlier draft of
# this test killed mcpp after the build had already finished and passed without
# testing anything; the assertion below therefore requires that ninja was seen
# alive first, and fails if it never was.
set -euo pipefail
fail() { echo "FAIL: $*"; exit 1; }
MCPP="${MCPP:?set MCPP to the mcpp binary under test}"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p src

# Counting by executable name, not by a `pgrep -f` pattern: this script's own
# command line contains the word, and a pattern match finds itself.
count_ninja() {
  local n=0 p
  for p in /proc/[0-9]*; do
    [[ "$(cat "$p/comm" 2>/dev/null)" == "ninja" ]] && n=$((n+1))
  done
  echo "$n"
}

# `-j1` IS WHAT MAKES THE WINDOW DETERMINISTIC, NOT THE NUMBER OF FILES.
#
# An earlier draft used 200 parallel translation units and PASSED AGAINST A
# BUILD THAT HAS THE DEFECT: on a 32-core host the batch finished inside the
# ten seconds between the signal and the check, so nothing survived either way
# and the control could not tell the two apart. Serialising the build makes it
# outlast the check on every host, by construction rather than by hoping the
# machine is slow.
n_tu=60
for i in $(seq 0 $((n_tu-1))); do
  cat > "src/u$i.cpp" <<EOF
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
template<int N> struct T_$i { static int v(){ return N + T_$i<N-1>::v(); } };
template<> struct T_$i<0> { static int v(){ return 0; } };
int f$i() {
  std::map<std::string, std::vector<std::pair<int,std::string>>> m;
  std::set<std::string> s;
  for (int k = 0; k < 800; ++k) { m[std::to_string(k)].push_back({k, std::to_string(k*2)}); s.insert(std::to_string(k*3)); }
  std::vector<std::string> keys;
  for (auto& kv : m) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return (int)m.size() + T_$i<400>::v() + (int)keys.size() + (int)s.size();
}
EOF
done
printf 'int main() { return 0; }\n' > src/main.cpp

cat > mcpp.toml <<'EOF'
[package]
name    = "orphan"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]
EOF

baseline=$(count_ninja)

"$MCPP" build --jobs 1 >/dev/null 2>&1 &
mcpp_pid=$!

seen=0
for _ in $(seq 1 60); do
  if [[ "$(count_ninja)" -gt "$baseline" ]]; then seen=1; break; fi
  sleep 0.25
done
[[ "$seen" -eq 1 ]] \
  || { kill -9 "$mcpp_pid" 2>/dev/null || true
       fail "ninja never started: this test measured nothing"; }

sleep 2
kill -TERM "$mcpp_pid" 2>/dev/null || true
wait "$mcpp_pid" 2>/dev/null || true

# Long enough that a ninja which merely finished on its own is indistinguishable
# from one that was killed — and short enough that the suite does not stall. The
# build takes far longer than this, so anything still alive here is an orphan.
sleep 8
after=$(count_ninja)
if [[ "$after" -gt "$baseline" ]]; then
  for p in /proc/[0-9]*; do
    [[ "$(cat "$p/comm" 2>/dev/null)" == "ninja" ]] \
      && echo "  orphan $(basename "$p") cwd=$(readlink "$p/cwd" 2>/dev/null)"
  done
  fail "ninja survived a killed mcpp: $after alive, baseline $baseline"
fi

echo "  ok  ninja was running, mcpp was signalled alone, nothing survived"
echo "PASS: 340 no orphan survives a killed mcpp"
