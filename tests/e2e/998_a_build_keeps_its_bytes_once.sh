#!/usr/bin/env bash
# requires: gcc
# 998 -- what a build writes is written once, and what no configuration uses
# is collected (mcpp#874: a workspace's target/ of 25 GB, 14x its objects).
#
# Criteria:
#   A. A library's archive holds its own package's objects, not its
#      dependencies' (docs/12).
#   B. On ELF, a program's debug information is compressed.
#   C. A shared library staged as an equal copy is placed again by a link.
#   D. `mcpp clean --stale` removes a `.build-mcpp/out/<hash>` no configuration
#      names in its `uses.list`, and keeps the one the build used.
#   E. `mcpp clean --report` names the areas and the total.
#   F. A path tool's edit supersedes its store entry: one entry remains, and
#      `mcpp cache list` shows it as a tool.
#   G. A generator that runs again and leaves its output as it was rebuilds
#      nothing that includes the output (`restat` on the action's edge).
#   H. A library of a build program that has a build program of its own is
#      built in a group beside the first (`<group>.1`), not in a copy of the
#      host workspace under its own target/, and the program beside its record
#      is a link to the group's binary.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
[ -d "$HOME/.mcpp/registry" ] && ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p dep/src tool/src app/src
cat > dep/mcpp.toml <<'EOF'
[package]
name    = "dep"
version = "0.1.0"

[targets.dep]
kind = "lib"
EOF
echo 'int dep_value() { return 41; }' > dep/src/dep.cpp
write_tool() {
    cat > "$TMP/tool/mcpp.toml" <<'EOF'
[package]
name    = "tool"
version = "0.1.0"

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
EOF
    cat > "$TMP/tool/src/gen.cpp" <<EOF
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int app_extra() { return $1; }\n");
    return std::fclose(f);
}
EOF
}
write_tool 1
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "lib"

[targets.main]
kind = "bin"
main = "src/main.cpp"

[dependencies]
dep = { path = "../dep" }

[build-dependencies]
tool = { path = "../tool", tools = ["gen"] }
EOF
echo 'int dep_value(); int app_value() { return dep_value() + 1; }' > app/src/app.cpp
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int app_value();
int app_extra();
int main() { std::printf("%d\n", app_value() + app_extra()); }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/extra.cpp";
    const std::string cmd = std::string("\"") + mcpp::dep_bin("tool", "gen") + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
EOF
cd app
"$MCPP" build > b1.log 2>&1 || fail "the build failed" b1.log

# A
archive=$(find target -name 'libapp.a' -not -path '*/.build-mcpp/*' | head -1)
[ -n "$archive" ] || fail "no libapp.a" b1.log
members=$(ar t "$archive")
echo "$members" | grep -q 'app' || fail "the archive lacks the package's own object: $members"
echo "$members" | grep -q 'dep' && fail "the archive holds its dependency's object: $members"

# B
if [ "$(uname -s)" = Linux ] && command -v readelf > /dev/null; then
    program=$(find target -path '*/bin/main' | head -1)
    readelf -S -W "$program" | grep -E '\.debug_info' | grep -q ' C ' \
        || fail "the program's .debug_info is not compressed" <(readelf -S -W "$program")
fi

# C
mkdir -p stage
printf 'not really a library' > stage/libx.so
cp stage/libx.so stage/placed.so
"$MCPP" stage --output stage/placed.so stage/libx.so > stage.log 2>&1 || fail "mcpp stage failed" stage.log
[ "$(stat -c %i stage/libx.so)" = "$(stat -c %i stage/placed.so)" ] \
    || fail "an equal copy of a shared library was left a copy"

# D
used=$(sed -n 's/^out //p' target/*/*/uses.list | head -1)
[ -n "$used" ] || fail "the build names no generated-source directory" $(ls target/*/*/uses.list)
mkdir -p target/.build-mcpp/out/0000000000000000/app
touch -d '2 days ago' target/.build-mcpp/out/0000000000000000
"$MCPP" clean --stale > clean.log 2>&1 || fail "clean --stale failed" clean.log
[ -d target/.build-mcpp/out/0000000000000000 ] && fail "an unused generated-source directory was kept" clean.log
[ -d "target/.build-mcpp/out/$used" ] || fail "the generated-source directory in use was removed" clean.log

# E
"$MCPP" clean --report > report.log 2>&1 || fail "clean --report failed" report.log
grep -q '.build-mcpp/out' report.log && grep -q '^target/' report.log || fail "the report lacks its areas" report.log

# F
sleep 1
write_tool 2
"$MCPP" build > b2.log 2>&1 || fail "the build after the tool's edit failed" b2.log
versions=$(find "$MCPP_HOME/build-cache/v1/tool" -mindepth 2 -maxdepth 2 -name 'tool@*' | wc -l)
[ "$versions" -eq 1 ] || fail "the superseded tool entry was kept ($versions)" \
    <(find "$MCPP_HOME/build-cache/v1/tool")
"$MCPP" cache list > cache.log 2>&1 || fail "cache list failed" cache.log
grep -E '\btool\b' cache.log | grep -q 'tool@' || fail "cache list does not show the tool entry" cache.log

# G
mkdir -p "$TMP/gen/src"
cd "$TMP/gen"
cat > mcpp.toml <<'EOF'
[package]
name    = "gen"
version = "0.1.0"
EOF
cat > gen.sh <<'EOF'
#!/bin/sh
printf '#define VALUE 7\n' > "$2.tmp"
if cmp -s "$2.tmp" "$2"; then rm "$2.tmp"; else mv "$2.tmp" "$2"; fi
EOF
chmod +x gen.sh
echo one > value.in
cat > build.mcpp <<'EOF'
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = mcpp::out_dir();
    mcpp::action a;
    a.id   = "gen:value";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg((root + "/value.in").c_str())
     .arg((out + "/value.h").c_str())
     .input((root + "/value.in").c_str())
     .output((out + "/value.h").c_str())
     .submit();
    mcpp::include_dir(out.c_str());
}
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include "value.h"
int main() { std::printf("%d\n", VALUE); }
EOF
"$MCPP" build > g1.log 2>&1 || fail "the generator's build failed" g1.log
log=$(find target -name .ninja_log -not -path '*/.build-mcpp/*' | head -1)
before=$(grep -c 'main' "$log")
sleep 1
echo two > value.in
"$MCPP" build > g2.log 2>&1 || fail "the second build failed" g2.log
grep -q 'value.h' <(tail -n +2 "$log" | tail -5) || fail "the generator did not run again" "$log"
[ "$(grep -c 'main' "$log")" = "$before" ] || fail "an unchanged generated header rebuilt its includer" "$log"

# H
mkdir -p "$TMP/genlib/src" "$TMP/user/src"
cd "$TMP/genlib"
cat > mcpp.toml <<'EOF'
[package]
name    = "genlib"
version = "0.1.0"

[targets.genlib]
kind = "lib"
EOF
printf 'export module genlib;\nexport int genlib_value();\n' > src/genlib.cppm
cat > build.mcpp <<'EOF'
#include <fstream>
#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/genlib_impl.cpp";
    std::ofstream(out) << "module genlib;\nint genlib_value() { return 5; }\n";
    mcpp::generated(out.c_str());
}
EOF
cd "$TMP/user"
cat > mcpp.toml <<'EOF'
[package]
name    = "user"
version = "0.1.0"

[build-dependencies]
genlib = { path = "../genlib" }
EOF
echo 'int main() { return 0; }' > src/main.cpp
cat > build.mcpp <<'EOF'
import mcpp;
import genlib;
int main() { return genlib_value() == 5 ? 0 : 1; }
EOF
"$MCPP" build > h1.log 2>&1 || fail "the build with a nested build program failed" h1.log
host=target/.build-mcpp/host
nested=$(find "$host" -path '*/target/.build-mcpp/host' | head -1)
[ -z "$nested" ] || fail "the host workspace holds a copy of itself: $nested" <(find "$host" -maxdepth 3)
ls -d "$host"/*.1 > /dev/null 2>&1 || fail "the library's build program has no group beside the first" <(ls "$host")
copy=target/.build-mcpp/build.mcpp.bin
binary=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(next(iter(d.values()))['binary'])" "$(ls "$host"/*/programs.json | grep -v '\.1/' | head -1)")
[ "$(stat -c %i "$copy")" = "$(stat -c %i "$binary")" ] || fail "the program beside its record is a copy"

echo "OK"
