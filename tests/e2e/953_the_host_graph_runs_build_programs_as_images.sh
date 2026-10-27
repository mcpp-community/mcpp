#!/usr/bin/env bash
# requires: llvm python3
# 953 -- the host graph's platform (SPEC-013 §6): a project whose `[host]`
# selects openkal-platform builds its build program as an ok image of this
# machine's architecture and runs it through the platform's runner; the build
# program's library is an image of the same graph; the platform's own tools are
# built for this machine as before (the bootstrap boundary).
#
# Criteria:
#   A. The build program runs as an image: the report names the host platform,
#      and what the program defines reaches the project's unit.
#   B. A library of `[build-dependencies]` the build program imports is built
#      into the same image graph, and its answer reaches the program.
#   C. The runner of the host graph is a program of this machine (the platform's
#      `okrun`, built natively): the bootstrap boundary holds.
#   D. `--host-platform none` runs the build program as a program of this
#      machine: no host platform is reported, and the answer is the same.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64) ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac

mkdir -p "$TMP/app/src" "$TMP/app/helper/src"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "hostgraph"
version = "0.1.0"

[host]
platform = "openkal-platform@0.17.1"

[build-dependencies]
helper = { path = "helper" }
TOML
printf '[package]\nname = "helper"\nversion = "0.1.0"\n\n[targets.helper]\nkind = "lib"\n' > helper/mcpp.toml
cat > helper/src/helper.cppm <<'CPP'
export module helper;
import std;
export namespace helper {
int answer() { return static_cast<int>(std::string_view("forty-two").size()) * 4 + 6; }
}
CPP
cat > build.mcpp <<'CPP'
import mcpp;
import std;
import helper;
int main() {
    mcpp::define("BUILT_BY_AN_IMAGE=1");
    mcpp::define(std::format("HELPER_ANSWER={}", helper::answer()).c_str());
    return 0;
}
CPP
cat > src/main.cpp <<'CPP'
#include <cstdio>
int main() { std::printf("host graph: %d, answer %d\n", BUILT_BY_AN_IMAGE, HELPER_ANSWER); return 0; }
CPP

# A, B
"$MCPP" run > run.log 2>&1 || fail "A: the project did not build or run" run.log
grep -qE "Host platform +openkal-platform" run.log || fail "A: the report does not name the host platform" run.log
grep -q "host graph: 1, answer 42" run.log || fail "A/B: the build program's definitions did not arrive" run.log
echo "ok: the build program and its library run as an image"

# C
"$MCPP" self env --host --format json > host.json 2> host.err || fail "C: self env --host" host.err host.json
python3 - host.json <<'PY' || fail "C: the host graph's runner" host.json
import json, sys
d = json.load(open(sys.argv[1]))["data"]
p = d["platform"]
assert p and p["name"].endswith("openkal-platform"), p
okrun = d["tools"].get("okrun")
assert okrun, d["tools"]
assert d["runner"] and d["runner"]["program"] == okrun, d["runner"]
# A program of this machine, not an image: an executable's magic, and no
# image name.
magic = open(okrun, "rb").read(4)
assert magic in (b"\x7fELF", b"\xcf\xfa\xed\xfe", b"MZ\x90\x00") or magic[:2] == b"MZ", magic
assert not okrun.endswith(".oke"), okrun
PY
echo "ok: the host graph's runner is a program of this machine"

# D
rm -rf target
"$MCPP" run --host-platform none > none.log 2>&1 || fail "D: --host-platform none" none.log
grep -q "Host platform" none.log && fail "D: a host platform is reported under --host-platform none" none.log
grep -q "host graph: 1, answer 42" none.log || fail "D: the answer differs without the host platform" none.log
echo "ok: --host-platform none runs the build program natively"

echo "PASS: 953 the host graph runs build programs as images"
