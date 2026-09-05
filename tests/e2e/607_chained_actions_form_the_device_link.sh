#!/usr/bin/env bash
# requires: gcc
# One action may consume what another produced, and the engine only orders and
# fingerprints them. That is the whole engine-side content of a device link
# (multi-device design C-1/C-2): a rule package emits N `artifact` actions
# whose outputs stay out of the link, and one `object` action that reads them
# and produces the object that does join it. Nothing here names a device:
# the "device link" is `cat`, the "device compiler" is the toolchain's own C
# compiler, and the property under test is the graph, not the vendor.
#
#   step 1 (artifact): src/parts/a.inc + src/parts/b.inc -> out/joined.c
#   step 2 (object):   out/joined.c                       -> out/joined.o  (linked)
#
# Also measured: editing an input of step 1 rebuilds step 2 and relinks, which
# is the "changing the device link re-prepares" half of criterion C9.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new chain > /dev/null; cd chain
rm -f src/*.cppm; mkdir -p src/parts
cat > src/main.cpp <<'EOF2'
extern "C" int joined_value();
int main() { return joined_value() == 42 ? 0 : 1; }
EOF2
printf 'extern "C" int joined_value() { return\n' > src/parts/a.inc
printf '42; }\n' > src/parts/b.inc
cat > mcpp.toml <<'EOF2'
[package]
name = "chain"
version = "0.1.0"
[language]
standard = "c++23"
[targets.chain]
kind = "bin"
main = "src/main.cpp"
EOF2
cat > build.mcpp <<'EOF2'
import std;
import mcpp;
int main() {
    mcpp::rerun_if_changed("src/parts/a.inc");
    mcpp::rerun_if_changed("src/parts/b.inc");
    const std::string root = mcpp::manifest_dir(), out = mcpp::out_dir();
    const std::string joined = out + "/joined.cpp", obj = out + "/joined.o";
    {   // Step 1: an ARTIFACT. Its output is data as far as the link is concerned.
        mcpp::action a;
        a.id = "join"; a.role = "artifact"; a.description = "join parts";
        a.arg("sh"); a.arg("-c");
        a.arg(("cat '" + root + "/src/parts/a.inc' '" + root + "/src/parts/b.inc' > '" + joined + "'").c_str());
        a.input((root + "/src/parts/a.inc").c_str());
        a.input((root + "/src/parts/b.inc").c_str());
        a.output(joined.c_str());
        a.submit();
    }
    {   // Step 2: an OBJECT that consumes step 1's output. The engine sees a
        // path; ninja orders the two by it.
        mcpp::action a;
        a.id = "compile-joined"; a.role = "object"; a.description = "compile joined";
        a.arg((std::string(mcpp::toolchain_dir()) + "/bin/g++").c_str());
        a.arg("-c"); a.arg(joined.c_str()); a.arg("-o"); a.arg(obj.c_str());
        a.input(joined.c_str());
        a.output(obj.c_str());
        a.submit();
    }
    return 0;
}
EOF2

"$MCPP" build > build1.log 2>&1 || { cat build1.log; echo "FAIL: the chained build failed"; exit 1; }
"$MCPP" run > run1.log 2>&1 || { cat run1.log; echo "FAIL: the linked object did not carry the joined value"; exit 1; }
echo "PASS: an object action consumed an artifact action's output and joined the link"

# The second half of C9: a change at the head of the chain propagates. The
# joined value becomes 43, main returns 1, and only a relink would notice.
printf '43; }\n' > src/parts/b.inc
"$MCPP" build > build2.log 2>&1 || { cat build2.log; echo "FAIL: rebuild after editing an input failed"; exit 1; }
if "$MCPP" run > run2.log 2>&1; then
    cat build2.log run2.log; echo "FAIL: the edit at the head of the chain did not reach the link"; exit 1
fi
echo "PASS: editing an input of the first action rebuilt the second and relinked"

echo "PASS: chained actions form the device link"
