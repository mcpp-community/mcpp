#!/usr/bin/env bash
# #655 measurement: how the released mcpp's build hands each flag spelling to
# the compiler on this host, and what the compile database lists for it.
# One project per spelling, because a spelling that splits into a stray word
# fails the compile and must not take the others with it.
set +e
M="${MCPP:?}"
# The shim refuses outside the repository's pinned subos; address the store copy.
for cand in "$HOME"/.xlings/data/xpkgs/xim-x-mcpp/*/bin/mcpp "$HOME"/.xlings/data/xpkgs/xim-x-mcpp/*/bin/mcpp.exe; do
  [ -x "$cand" ] && M="$cand"
done
PY=$(command -v python3 || command -v python)
ROOT="$(mktemp -d)"
reading() { echo "READING $*"; echo "- \`$*\`" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"; }
case_one() {
  id="$1"; channel="$2"; element="$3"
  d="$ROOT/$id"; mkdir -p "$d/src"
  printf '[package]\nname = "p%s"\nversion = "0.1.0"\n\n[build]\n%s = [%s]\n' "$id" "$channel" "$element" > "$d/mcpp.toml"
  cat > "$d/src/main.cpp" <<'CPP'
#include <cstdio>
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
int main() {
#ifdef V
  std::printf("V=[");
  for (const char* c = STR(V); *c; ++c)
    if (*c >= 0x20 && *c < 0x7f) std::putchar(*c); else std::printf("<%02x>", (unsigned char)*c);
  std::printf("]\n");
#else
  std::printf("V undefined\n");
#endif
  return 0;
}
CPP
  out=$(cd "$d" && "$M" run 2>&1 | tr -d '\r')
  v=$(printf '%s\n' "$out" | grep -E '^V(=| undefined)' | tail -1)
  if [ -z "$v" ]; then
    err=$(printf '%s\n' "$out" | grep -iE 'error|FAILED' | head -2 | tr '\n' ' ')
    v="build-failed: ${err:0:200}"
  fi
  cdb=$(find "$d" -name compile_commands.json | head -1)
  args=""
  if [ -n "$cdb" ]; then
    args=$("$PY" -c 'import json,sys; a=json.load(open(sys.argv[1]))[0]["arguments"]; i=[k for k,x in enumerate(a) if "V=" in x]; print(json.dumps(a[i[0]:i[0]+2]) if i else "[]")' "$cdb")
  fi
  reading "$RUNNER_OS $id $channel $element => build: $v | cdb: $args"
}
"$M" --version
case_one esc_cxx  cxxflags '"-DV=\\\"esc.h\\\""'
case_one mid      cxxflags '"-DV=\"mid\""'
case_one sq       cxxflags "\"-DV='sq'\""
case_one sqsp     cxxflags "\"-DV='a b'\""
case_one dqsp     cxxflags '"-DV=\"a b\""'
case_one bsletter cxxflags '"-DV=a\\b"'
case_one bsbs     cxxflags '"-DV=a\\\\b"'
case_one bsspace  cxxflags '"-DV=a\\ b"'
case_one dollar   cxxflags '"-DV=a$b"'
case_one plainsp  cxxflags '"-DV=a b"'
case_one def_q    defines  '"V=\"def\""'
case_one def_sp   defines  '"V=a b"'
case_one def_bs   defines  '"V=a\\\\b"'
case_one def_sq   defines  "\"V='c'\""
exit 0
