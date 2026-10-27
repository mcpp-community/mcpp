#!/usr/bin/env bash
# requires: python3
# 841_pack_summarizes_many_outputs.sh -- the human report of `mcpp pack` for a
# format that submits one output per file of a distribution tree.
#
# A format may declare one output per file (the validation project's release
# format declares 1,309 for one program), and a `Packed` line per file then
# buries the rest of the pass.
#
#   P1  more than eight outputs are reported by the entry each lies in below
#       their common parent, with a count;
#   P2  --verbose reports every output;
#   P3  `--message-format json` lists every output.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p app/src && cd app
cat > mcpp.toml <<'EOF'
[package]
name = "app"
version = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("tree");
    if (std::string_view(mcpp::pack_format()) != "tree") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/tree";
    auto copy = [&](const std::string& id, const std::string& dst) {
        mcpp::action a;
        a.id = id.c_str();
        a.role = "artifact";
        a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
         .arg("--output").arg(dst.c_str()).arg("${mcpp.target_file:app}")
         .input("${mcpp.target_file:app}")
         .output(dst.c_str())
         .submit();
    };
    for (int i = 0; i < 6; ++i) {
        copy("a" + std::to_string(i), out + "/bin/f" + std::to_string(i));
        copy("b" + std::to_string(i), out + "/data/f" + std::to_string(i));
    }
    copy("m", out + "/manifest");
    return 0;
}
EOF

"$MCPP" pack --pkg-format tree > p1.log 2>&1 || fail "pack failed" p1.log

# P1
n=$(grep -c "Packed" p1.log)
[ "$n" = 3 ] || fail "P1: $n Packed lines, expected 3" p1.log
grep -qE "Packed .*tree[/\\\\]bin \(6 files\)" p1.log || fail "P1: bin/ is not summarised" p1.log
grep -qE "Packed .*tree[/\\\\]data \(6 files\)" p1.log || fail "P1: data/ is not summarised" p1.log
grep -qE "Packed .*tree[/\\\\]manifest$" p1.log || fail "P1: the single file is not named" p1.log

# P2
"$MCPP" pack --pkg-format tree --verbose > p2.log 2>&1 || fail "pack --verbose failed" p2.log
n=$(grep -c "Packed" p2.log)
[ "$n" = 13 ] || fail "P2: $n Packed lines under --verbose, expected 13" p2.log

# P3
"$MCPP" pack --pkg-format tree --message-format json > p3.json 2> p3.err || fail "pack json failed" p3.err
python3 - p3.json <<'EOF' || fail "P3" p3.json
import json, sys
a = json.load(open(sys.argv[1]))["data"]["artifacts"]
assert len(a) == 13, f"P3: {len(a)} artifacts"
EOF

echo "PASS: 841_pack_summarizes_many_outputs"
