#!/usr/bin/env bash
# requires: gcc
# A shader is a device source, and the classification table says so for every
# language a separate compiler consumes -- not only for NVIDIA's two.
#
# THE DEFECT THIS MEASURES. `SourceKind::Device` is documented as a graph role,
# "compiled by a device compiler mcpp does not drive", explicitly so that the
# table does not grow a row per vendor. The extension list nonetheless held
# `.cu` and `.hip` alone, so a shader in a constrained glob was refused:
#
#   'scale.comp' is listed in [build] sources, and mcpp has no role for the
#   extension '.comp'.
#
# and the rule package that exists to compile it was told, in the same run,
# that there were no device sources -- a warning and an error contradicting
# each other about the same file.
#
# Four sections, and the last two are what make the first two a measurement
# rather than a demonstration.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new shaders > /dev/null; cd shaders
rm -f src/*.cppm
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF
mkdir -p shaders

# The build program prints what it was handed, delimited, so a section can
# assert on a whole list rather than on a substring of one name.
# MCPP_DEVICE_SOURCES is newline-separated and the advisory channel is one
# line per message, so the list is flattened before it is printed. Delimited,
# so an empty list is `device=[]` and not the absence of a line -- which is
# what section four asserts on.
cat > build.mcpp <<'EOF'
import std;
import mcpp;

// A `check` action per device source, and the reason it is here rather than a
// bare read of the variable: mcpp refuses a device source that reached no
// action (2026.9.6.5). A build program that only LOOKS at
// `mcpp::device_sources()` models a project whose device files compile to
// nothing, which is the defect that refusal exists to catch -- so this fixture
// declares the edge a real rule package would declare, and asserts on the
// variable as before.
int main() {
    std::string flat(mcpp::device_sources());
    std::size_t n = 0, start = 0;
    while (start <= flat.size()) {
        auto nl = flat.find('\n', start);
        auto one = flat.substr(start, nl == std::string::npos ? flat.size() - start : nl - start);
        start = nl == std::string::npos ? flat.size() + 1 : nl + 1;
        if (one.empty()) continue;
        auto stamp = std::string(mcpp::out_dir()) + "/dev-" + std::to_string(n++) + ".stamp";
        mcpp::action a;
        a.id = "seen";
        a.role = "check";
        a.description = "account for a device source";
        auto abs = std::string(mcpp::manifest_dir()) + "/" + one;
        a.arg("cp").arg(abs.c_str()).arg(stamp.c_str());
        a.input(abs.c_str());
        a.output(stamp.c_str());
        a.submit();
    }
    for (auto& c : flat) if (c == '\n') c = ' ';
    mcpp::warning(("device=[" + flat + "]").c_str());
    return 0;
}
EOF

write_manifest() {   # $1 = accel line, $2 = sources line
    cat > mcpp.toml <<EOF
[package]
name = "shaders"
version = "0.1.0"
[language]
standard = "c++23"

[build]
$1
sources = [$2]

[targets.shaders]
kind = "bin"
main = "src/main.cpp"
EOF
}

# ── One: every shader and kernel language reaches the build program ───────
#
# The denominator is the table itself: each extension gets a file, and the
# assertion is that ALL of them come back. A test naming one extension would
# pass on a table that had gained exactly that one.
exts="comp vert frag geom tesc tese mesh task rgen rint rahit rchit rmiss rcall glsl hlsl cl metal"
for e in $exts; do printf '// %s\n' "$e" > "shaders/s.$e"; done

write_manifest 'accel   = "widget9+{w1}"' '"src/*.cpp", { glob = "shaders/*", accel = "widget9+{w1}" }'
"$MCPP" build > all.log 2>&1 || { cat all.log; echo "FAIL: a project with shader sources failed to build"; exit 1; }
missing=""
for e in $exts; do
    grep -q "shaders/s.$e" all.log || missing="$missing $e"
done
[ -z "$missing" ] || { cat all.log; echo "FAIL: not handed to the build program:$missing"; exit 1; }
echo "PASS: all 18 shader and kernel extensions reach the build program as device sources"

# ── Two: and none of them is compiled ─────────────────────────────────────
#
# The complement of section one. A device source that ALSO went to the C++
# compiler would satisfy section one and produce an object nothing can link.
"$MCPP" build -v > verbose.log 2>&1
for e in $exts; do
    if grep -q "s\.$e -o\|s\.$e\.o" verbose.log; then
        cat verbose.log; echo "FAIL: s.$e was compiled"; exit 1
    fi
done
echo "PASS: no shader is offered to the C++ compiler"

# ── Three: an extension that is NOT in the table is still refused ─────────
#
# Without this the first section would also pass on a table that classified
# every unknown extension as a device source, which is the opposite of what
# the table is for.
printf '// wgsl\n' > shaders/s.wgsl
write_manifest 'accel   = "widget9+{w1}"' '"src/*.cpp", { glob = "shaders/s.wgsl", accel = "widget9+{w1}" }'
if "$MCPP" build > unknown.log 2>&1; then
    cat unknown.log; echo "FAIL: an extension with no role was accepted"; exit 1
fi
grep -q "no role for the extension" unknown.log || {
    cat unknown.log; echo "FAIL: refusal does not say the extension has no role"; exit 1; }
grep -q "\.wgsl" unknown.log || { cat unknown.log; echo "FAIL: refusal does not name the extension"; exit 1; }
rm -f shaders/s.wgsl
echo "PASS: an extension the table does not name is still refused, by name"

# ── Four: the default source globs did not widen ──────────────────────────
#
# The compatibility promise the whole change rests on. A package that vendors
# shaders it builds elsewhere must not start handing them to a build program
# on upgrade, so a project with NO `sources` entry must see none of them.
write_manifest '' '"src/*.cpp"'
mkdir -p src/shaders && cp shaders/s.comp src/shaders/
rm -f mcpp.toml
cat > mcpp.toml <<'EOF'
[package]
name = "shaders"
version = "0.1.0"
[language]
standard = "c++23"

[targets.shaders]
kind = "bin"
main = "src/main.cpp"
EOF
"$MCPP" build > default.log 2>&1 || { cat default.log; echo "FAIL: the default-glob build failed"; exit 1; }
grep -q "device=\[\]" default.log || {
    cat default.log; echo "FAIL: a shader under src/ reached a build program with no sources entry"; exit 1; }
echo "PASS: the default source globs still exclude every device extension"

echo "PASS: shader sources are device sources"
