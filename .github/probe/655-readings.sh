#!/usr/bin/env bash
# #655 measurement: how the released mcpp's build hands each flag spelling to
# the compiler on this host, and what the compile database lists for it.
# One project per spelling, because a spelling that splits into a stray word
# fails the compile and must not take the others with it.
set +e
M="${MCPP:?}"
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
  std::printf("V=[%s]\n", STR(V));
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
    args=$(python3 - "$cdb" <<'PY' 2>/dev/null || python - "$cdb" <<'PY2'
import json,sys
e=json.load(open(sys.argv[1]))
a=[x for x in e[0]["arguments"] if "V=" in x or x.startswith(("-DV","/DV"))]
i=e[0]["arguments"].index(a[0]) if a else -1
print(json.dumps(e[0]["arguments"][i:i+2] if a else []))
PY
import json,sys
e=json.load(open(sys.argv[1]))
a=[x for x in e[0]["arguments"] if "V=" in x]
print(json.dumps(a))
PY2
)
  fi
  reading "$RUNNER_OS $id $channel $element => build: $v | cdb: $args"
}
"$M" --version
case_one esc      cflags   '"-DV=\\\"esc.h\\\""'
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
