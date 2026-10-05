#!/usr/bin/env bash
# requires:
# A sibling input directory must invalidate both build.mcpp's cache and the
# project fast path. The generated executable is the observable result.
set -euo pipefail
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME" "$TMP/ws/app/src" "$TMP/logs"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF

cd "$TMP/ws/app"
cat > mcpp.toml <<'EOF'
[package]
name = "parent-glob"
version = "0.1.0"

[targets.parent-glob]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
int count_inputs();
int main() { std::printf("COUNT=%d\n", count_inputs()); }
EOF
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    namespace fs = std::filesystem;
    const fs::path inputs = fs::path(mcpp::manifest_dir()) / "../inputs";
    mcpp::rerun_if_changed_glob("../inputs/**/*.in");
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(inputs, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".in") ++count;
    const std::string output = std::string(mcpp::out_dir()) + "/count.cpp";
    { std::ofstream out(output); out << "int count_inputs() { return " << count << "; }\n"; }
    mcpp::generated(output.c_str());
    return 0;
}
EOF

fail() { echo "FAIL: $1"; cat "$TMP/logs"/*.log; exit 1; }
build_count() {
    local step=$1 expected=$2
    "$MCPP" build > "$TMP/logs/$step.log" 2>&1 || fail "build $step failed"
    local output
    output=$("$MCPP" run 2>&1 | grep '^COUNT=' | tail -1)
    [[ "$output" == "COUNT=$expected" ]] || fail "$step expected COUNT=$expected, got $output"
}

# The literal prefix initially does not exist. Creating the sibling directory
# and its first input must rerun without touching a source or manifest.
build_count missing 0
mkdir -p ../inputs
printf 'a\n' > ../inputs/a.in
build_count appeared 1
mkdir -p ../inputs/nested
printf 'b\n' > ../inputs/nested/b.in
build_count added 2
rm ../inputs/nested/b.in
build_count removed 1

# Glob inputs track membership only. A content edit, a nonmatching file, and
# an unchanged build must preserve the fast path instead of always rerunning.
printf 'different content\n' > ../inputs/a.in
printf 'not an input\n' > ../inputs/ignored.txt
build_count content 1
build_count unchanged 1
for step in content unchanged; do
    if grep -qE '^ *build\.mcpp .* ran [0-9]' "$TMP/logs/$step.log"; then
        fail "$step reran the build program despite unchanged membership"
    fi
done


# An absolute pattern is matched against absolute paths (#766): the root
# package may watch a directory outside its tree by its absolute name.
INPUTS_ABS=$(cd ../inputs && pwd -P)
sed -i.bak "s|\"../inputs/\\*\\*/\\*.in\"|\"$INPUTS_ABS/**/*.in\"|" build.mcpp
grep -q "$INPUTS_ABS/\*\*/\*.in" build.mcpp || fail "the fixture did not take the absolute pattern"
build_count absolute 1
printf 'c\n' > ../inputs/c.in
build_count absolute-added 2
rm ../inputs/c.in
mv build.mcpp.bak build.mcpp

# A pattern no walk enters is reported, not silently kept as an empty set.
cp build.mcpp build.mcpp.bak
awk '{ print } /rerun_if_changed_glob\("\.\.\/inputs/ { print "    mcpp::rerun_if_changed_glob(\"target/**/*.in\");" }' \
    build.mcpp.bak > build.mcpp
grep -q 'rerun_if_changed_glob("target/' build.mcpp || fail "the fixture did not take the second glob"
"$MCPP" build > "$TMP/logs/unwatchable.log" 2>&1 || fail "build with an unwatchable glob failed"
grep -q 'rerun_if_changed_glob("target/\*\*/\*.in") can never re-run it' "$TMP/logs/unwatchable.log" \
    || fail "the unwatchable glob was not reported"
mv build.mcpp.bak build.mcpp

# A git dependency is sealed: its program may not watch outside its own tree.
mkdir -p "$TMP/origin/src" "$TMP/consumer/src"
cd "$TMP/origin"
git init --quiet
git config user.email "test@local"
git config user.name test
cat > mcpp.toml <<'EOF'
[package]
name = "sealed"
version = "0.1.0"

[targets.sealed]
kind = "lib"
EOF
printf 'export module t882_sealed;\nexport int sealed_value() { return 1; }\n' > src/sealed.cppm
cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::rerun_if_changed_glob("../outside/**/*.in");
    return 0;
}
EOF
git add -A >/dev/null
git commit --quiet -m init
REV=$(git rev-parse HEAD)
ORIGIN_HOST=$(host_path "$TMP/origin")
cd "$TMP/consumer"
cat > mcpp.toml <<EOF
[package]
name = "consumer"
version = "0.1.0"

[dependencies]
sealed = { git = "$ORIGIN_HOST", rev = "$REV" }
EOF
printf 'import t882_sealed;\nint main() { return sealed_value() == 1 ? 0 : 1; }\n' > src/main.cpp
if "$MCPP" build > "$TMP/logs/sealed.log" 2>&1; then
    fail "a git dependency's glob that leaves its package was accepted"
fi
grep -q "which leaves the package" "$TMP/logs/sealed.log" \
    || fail "the refusal did not name the escaping glob"

echo "PASS: parent-directory glob inputs invalidate builds on membership changes"
