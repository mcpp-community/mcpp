#!/usr/bin/env bash
# requires: gcc unix-shell
# 897 -- `mcpp::out_dir()` is per configuration (D7, D25; design 2026-10-10
# §4.2), and `resolution.json` says where it is.
#
# One `target/.build-mcpp/out` used to serve every configuration. A generator
# whose output depends on `mcpp::profile()` wrote one `gen.cpp`; each
# configuration's ninja log believed it current, so a dev build after a
# release build ran with the release text (R2: `GEN=release`).
#
#   G1  dev, release, dev: each binary prints its own profile.
#   G2  the two configurations' `outDir` (resolution.json, graph.packages)
#       differ, exist, and hold the generated file.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/gp/src" && cd "$TMP/gp"
cat > mcpp.toml <<'TOML'
[package]
name = "gp"
version = "0.1.0"
[build]
sources = ["src/main.cpp"]
[targets.gp]
kind = "bin"
main = "src/main.cpp"
TOML
cat > src/main.cpp <<'CPP'
#include <cstdio>
const char* gen_profile();
int main() { std::printf("GEN=%s\n", gen_profile()); }
CPP
printf '#!/bin/sh\nprintf %sconst char* gen_profile() { return "%%s"; }\\n%s "$1" > "$2"\n' "'" "'" > gen.sh
cat > build.mcpp <<'CPP'
import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = (std::filesystem::path(mcpp::out_dir()) / "gen.cpp").generic_string();
    const auto gen = (root / "gen.sh").generic_string();
    mcpp::action a;
    a.id = "gen";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(mcpp::profile()).arg(out.c_str())
     .input(gen.c_str()).output(out.c_str()).submit();
    return 0;
}
CPP

outdir() {  # $1 = a configuration directory: the root package's outDir
    python3 - "$1/resolution.json" <<'PY'
import json, sys
for p in json.load(open(sys.argv[1]))["graph"]["packages"]:
    if p.get("root"):
        print(p.get("outDir", "")); break
PY
}

for p in dev release dev; do
    if [ "$p" = release ]; then "$MCPP" build --release > "b-$p.log" 2>&1 || fail "$p" "b-$p.log"
    else "$MCPP" build > "b-$p.log" 2>&1 || fail "$p" "b-$p.log"; fi
done

# Each configuration directory: what its binary prints, and what its outDir's
# generated file says. They must agree, and the two must be dev and release.
seen=""
for r in $(find target -name resolution.json); do
    d=$(dirname "$r")
    out=$(outdir "$d")
    [ -n "$out" ] || fail "resolution.json records no outDir: $r"
    [ -s "$out/gen.cpp" ] || fail "outDir $out does not hold the generated file"
    text=$(grep -o '"[a-z]*"' "$out/gen.cpp" | tr -d '"')
    got=$("$d/bin/gp" | tr -d '\r')
    [ "$got" = "GEN=$text" ] || fail "the binary in $d prints '$got' and its outDir was generated for '$text'"
    seen="$seen $text:$out"
done
case "$seen" in
    *dev:*release:* | *release:*dev:*) ;;
    *) fail "expected one dev and one release configuration, saw:$seen" ;;
esac
dev=$(echo "$seen" | tr ' ' '\n' | grep '^dev:' | cut -d: -f2-)
rel=$(echo "$seen" | tr ' ' '\n' | grep '^release:' | cut -d: -f2-)
[ "$dev" != "$rel" ] || fail "dev and release share one out_dir: $dev"
echo "ok G1 G2"

echo "PASS: 897_each_configuration_has_its_own_generated_output"
