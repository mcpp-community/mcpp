#!/usr/bin/env bash
# requires: elf
# Design 2026-10-29 §8.1: a build program's action names `${mcpp.contract}`,
# the configuration's contract (`mcpp-contract.json`, beside its compile
# database): the compiler of each language with the words every unit is
# compiled with, and the link driver, its words and the archiver. The action
# reads it while ninja runs; nothing calls mcpp back.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "contract"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
cat > build.mcpp <<'EOF'
#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/seen-contract.json";
    mcpp::action a;
    a.id   = "copy-contract";
    a.role = mcpp::roles::check;
    a.arg("cp").arg("${mcpp.contract}").arg(out.c_str())
     .output(out.c_str())
     .submit();
}
EOF

"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: build failed"; exit 1; }
seen=$(find target -name seen-contract.json | head -1)
[[ -n "$seen" ]] || { cat b.log; echo "FAIL: the action did not run"; exit 1; }
python3 - "$seen" <<'PY'
import json, os, sys
d = json.load(open(sys.argv[1]))
assert d["schema"] == 1, d
assert d["target"], d
for lang in ("c", "cxx", "asm"):
    t = d["compile"][lang]
    assert os.path.isabs(t["program"]) and isinstance(t["args"], list), (lang, t)
cxx = d["compile"]["cxx"]["args"]
assert any(a.startswith("-O") for a in cxx), cxx
assert not any(a.startswith("-std=") or a == "-fmodules" or a.startswith("-fmodule-") for a in cxx), cxx
assert os.path.isabs(d["link"]["program"]) and isinstance(d["link"]["args"], list), d["link"]
assert "archiver" in d["link"], d["link"]
print("contract:", d["target"], d["dialect"], d["compile"]["cxx"]["program"])
PY
cfg=$(dirname "$(find target -name compile_commands.json -not -path '*/.build-mcpp/*' | head -1)")
[[ -f "$cfg/mcpp-contract.json" ]] || { echo "FAIL: no contract beside the compile database"; exit 1; }
echo "PASS: 982"
