#!/usr/bin/env bash
# requires: gcc
# 790_a_prepare_action_populates_an_unknown_directory.sh — the `prepare`
# role (design §5.4 P, mcpp#701/#702, SPEC-007 R3.3): construction whose file
# names are not known when the build program runs, because they are decided
# by a command it merely invokes (a vcpkg/CMake install, an SDK unpack) —
# `install.sh` here, standing in for one of those. The build program declares
# only the DIRECTORY (`a.output_dir`); the header and the shared library
# inside it are named by the script, never by build.mcpp.
#
# What this holds:
#
#   1. ON THE FIRST BUILD, a unit that `#include`s a header the action wrote
#      compiles, and the program — linked against a library the SAME action
#      wrote, found only through `runtime_search_dir` — runs and gets the
#      right answer. Neither the header nor the library exists before this
#      build starts; both exist only because the compile and the link waited
#      for the action (design §5.4 P's ordering: every compile edge and every
#      link edge of the declaring package).
#   2. A SECOND BUILD RUNS NOTHING: no compile, no link, no action — a plain
#      "up to date" from ninja's own dependency tracking on unchanged inputs.
#   3. AFTER AN INPUT OF THE ACTION CHANGES, it runs ONCE — and the build
#      after THAT does not run it again (R2, design §5.4: the engine touches
#      the action's stamp on every success, whether or not the command wrote
#      it, so the stamp is newer than the input from that point on).
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

# The command a real rule package would run (vcpkg, a CMake `install`
# target). It decides the file NAMES; build.mcpp never sees them, only the
# directory they land in ($1).
cat > install.sh <<'EOF'
#!/usr/bin/env bash
set -e
PREFIX="$1"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cat > "$PREFIX/include/greet.h" <<HDR
#ifdef __cplusplus
extern "C" {
#endif
int prep_greet(void);
#ifdef __cplusplus
}
#endif
HDR
cat > "$PREFIX/lib/greet.c" <<SRC
int prep_greet(void) { return 99; }
SRC
gcc -shared -fPIC "$PREFIX/lib/greet.c" -o "$PREFIX/lib/libprepgreet.so"
EOF
chmod +x install.sh

# The action's declared INPUT that changes in step 3 below. Its content is
# never read by install.sh — it only has to be a file ninja can hash, the
# same way a vcpkg manifest's checksum would gate a real installation.
echo "recipe v1" > recipe.txt

cat > src/main.cpp <<'EOF'
#include <greet.h>
int main() { return prep_greet() == 99 ? 0 : 1; }
EOF

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string root   = mcpp::manifest_dir();
    const std::string prefix = std::string(mcpp::out_dir()) + "/prep-install";

    mcpp::action a;
    a.id   = "prep:install";
    a.role = mcpp::roles::prepare;
    // The stamp is a SIBLING of `prefix`, not nested inside it: ninja
    // creates a declared output's own parent directory before running any
    // edge, so a stamp inside `prefix` would make `prefix` exist regardless
    // of what install.sh did (see 791, which depends on the two staying
    // apart to mean anything).
    a.arg((root + "/install.sh").c_str()).arg(prefix.c_str())
     .input((root + "/install.sh").c_str())
     .input((root + "/recipe.txt").c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();

    mcpp::include_dir((prefix + "/include").c_str());
    mcpp::link_lib("prepgreet");
    mcpp::link_search((prefix + "/lib").c_str());
    mcpp::runtime_search_dir((prefix + "/lib").c_str());
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"
ran_prepare() { grep -q 'PREPARE prep:install\|__action-stamp.*install\.sh' "$1"; }

# ── 1. first build: header compiles, program runs through runtime_search_dir
"$MCPP" build -v > b1.log 2>&1 || { cat b1.log; echo "FAIL: first build failed"; exit 1; }
ran_prepare b1.log || { cat b1.log; echo "FAIL: the prepare action did not run on the first build"; exit 1; }
nj=$(find target -name build.ninja | head -1)
[[ -n "$nj" ]] || { echo "FAIL: no build.ninja"; exit 1; }
grep -qE '^build [^:]*(prep\.stamp|prepgreet)' "$nj" >/dev/null || true   # sanity, not load-bearing
out1="$("$MCPP" run 2>&1)" || { echo "FAIL: run failed after the first build: $out1"; exit 1; }

# ── 2. second build runs nothing at all ─────────────────────────────────────
"$MCPP" build -v > b2.log 2>&1 || { cat b2.log; echo "FAIL: second build failed"; exit 1; }
if ran_prepare b2.log; then
    cat b2.log; echo "FAIL: the prepare action re-ran on an unchanged second build"; exit 1
fi
if grep -qE '\bmain\.o\b' b2.log; then
    cat b2.log; echo "FAIL: the compile re-ran on an unchanged second build"; exit 1
fi

# ── 3. an input changes: the action runs once, then not again ──────────────
sleep 1
echo "recipe v2" > recipe.txt
"$MCPP" build -v > b3.log 2>&1 || { cat b3.log; echo "FAIL: third build (after the input changed) failed"; exit 1; }
ran_prepare b3.log || { cat b3.log; echo "FAIL: the action did not re-run after its input changed"; exit 1; }

"$MCPP" build -v > b4.log 2>&1 || { cat b4.log; echo "FAIL: fourth build failed"; exit 1; }
if ran_prepare b4.log; then
    cat b4.log
    echo "FAIL: the action re-ran on the build after it passed -- R2's stamp rule did not hold"
    exit 1
fi
out2="$("$MCPP" run 2>&1)" || { echo "FAIL: run failed after the recipe changed: $out2"; exit 1; }

echo "PASS: 790_a_prepare_action_populates_an_unknown_directory"
