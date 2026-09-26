#!/usr/bin/env bash
# requires: gcc
# 792_a_rerun_input_inside_a_prepare_directory_is_warned.sh — SPEC-007 R1.3
# (design §5.4 P): a build program's re-run inputs are declared BEFORE
# anything is built, and a `prepare` action's directory is filled AFTER a
# build program has already run once for this build — it is a ninja edge,
# scheduled after `mcpp build`'s configure step ends. A program that also
# names a file or a glob INSIDE such a directory as its own
# `rerun_if_changed`/`rerun_if_changed_glob` reads a CONSTRUCTION RESULT
# while it configures: correct on the second build, once a previous build
# populated the directory, and wrong on the first. This is the pattern of
# watching an install prefix to place a first installation's libraries on the
# NEXT plan (the route mcpp-plugins' deps-vcpkg/deps-cmake used before this
# role existed).
#
# Warned, not refused: the program still configures correctly today. Two
# programs are declared, one for `rerun_if_changed` (a fixed path) and one
# for `rerun_if_changed_glob` (a pattern) — SPEC-007 R1.3 names both.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/prep/src"
cd "$TMP/prep"

cat > mcpp.toml <<'EOF'
[package]
name    = "prep"
version = "0.1.0"

[toolchain]
linux = "gcc@16.1.0"

[targets.prep]
kind = "bin"
main = "src/main.cpp"
EOF

cat > install.sh <<'EOF'
#!/usr/bin/env bash
set -e
PREFIX="$1"
mkdir -p "$PREFIX/include"
echo '#define GREETING 1' > "$PREFIX/include/greet.h"
EOF
chmod +x install.sh

printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); }\n' > src/main.cpp

emit_build_mcpp() {   # $1 = the mcpp::rerun_if_changed* call under test
    cat > build.mcpp <<EOF
import mcpp;
#include <string>
int main() {
    const std::string root   = mcpp::manifest_dir();
    const std::string prefix = std::string(mcpp::out_dir()) + "/prep-install";

    mcpp::action a;
    a.id   = "prep:install";
    a.role = mcpp::roles::prepare;
    a.arg((root + "/install.sh").c_str()).arg(prefix.c_str())
     .input((root + "/install.sh").c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();

    $1

    return 0;
}
EOF
}

MCPP="${MCPP:-mcpp}"

# ── A fixed rerun_if_changed path inside the prepare directory ─────────────
emit_build_mcpp 'mcpp::rerun_if_changed((prefix + "/include/greet.h").c_str());'
rm -rf target
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build with rerun_if_changed inside the prepare dir failed"; exit 1; }
grep -qi "prepare" b1.log \
    || { cat b1.log; echo "FAIL: no warning mentions the prepare action"; exit 1; }
grep -qF "include/greet.h" b1.log \
    || { cat b1.log; echo "FAIL: the warning does not name the re-run path"; exit 1; }
grep -qF "prep-install" b1.log \
    || { cat b1.log; echo "FAIL: the warning does not name the prepare directory"; exit 1; }
grep -qF "prep" b1.log \
    || { cat b1.log; echo "FAIL: the warning does not name the declaring package"; exit 1; }

# ── A glob pattern reaching into the same directory ─────────────────────────
emit_build_mcpp 'mcpp::rerun_if_changed_glob((prefix + "/include/*.h").c_str());'
rm -rf target
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: build with rerun_if_changed_glob inside the prepare dir failed"; exit 1; }
grep -qi "prepare" b2.log \
    || { cat b2.log; echo "FAIL: no warning mentions the prepare action (glob case)"; exit 1; }
grep -qF "prep-install" b2.log \
    || { cat b2.log; echo "FAIL: the glob warning does not name the prepare directory"; exit 1; }

# ── THE CONTROL: a re-run input OUTSIDE the prepare directory is silent ────
# Without this half, any warning at all -- for any reason -- would pass the
# assertions above.
emit_build_mcpp 'mcpp::rerun_if_changed((root + "/install.sh").c_str());'
rm -rf target
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: control build failed"; exit 1; }
if grep -qi "prepare" b3.log; then
    cat b3.log
    echo "FAIL: a re-run input OUTSIDE any prepare directory was warned about"
    exit 1
fi

echo "PASS: 792_a_rerun_input_inside_a_prepare_directory_is_warned"
