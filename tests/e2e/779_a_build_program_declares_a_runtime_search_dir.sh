#!/usr/bin/env bash
# requires: pack gcc
# 779_a_build_program_declares_a_runtime_search_dir.sh --
# `mcpp::runtime_search_dir(dir)`: the build-program form of
# `runtime_search_dirs` (docs/04 §2.11). A dependency that brings a prebuilt
# shared library (a vcpkg prefix's `bin/`, a Qt SDK's `bin/`) knows where it
# lives only at build-program time, and the manifest key cannot be computed --
# it is a fixed TOML array. This is the directive that closes that gap
# (design §5.4 R1', mcpp#701/#702).
#
# What this holds, each with the wrong answer it excludes:
#
#   1. THE DIRECTIVE GETS THE SAME RUNPATH TREATMENT THE MANIFEST KEY DOES:
#      `-Wl,-rpath,<dir>` in build.ninja, never `-L` -- a launch-time search
#      directory is not a link-library search path (docs/04 §2.11's own
#      table), read from the emitted graph rather than inferred from a green
#      build, the same way 62_runtime_library_dirs.sh checks the manifest key.
#   2. `mcpp run` FINDS THE LIBRARY ONLY THROUGH THAT RUNPATH: the executable
#      is linked against it (DT_NEEDED), and the directory is nowhere else on
#      any search path this build would otherwise consult.
#   3. THE CACHE TAG IS NON-EMPTY, SO A REPLAY CARRIES THE ENTRY: rebuilding
#      on a build.mcpp CACHE HIT (not a re-run) must keep the RUNPATH and keep
#      `mcpp run` working -- the same replay criterion `mcpp:deploy=` is held
#      to in 651_a_build_program_deploys_what_it_generated.sh.
#   4. `mcpp pack --format dir` STAGES IT into the bundle's `lib/` -- the
#      same closure search `runtime_search_dirs` already feeds. Checked
#      LAST: `mcpp pack` builds under a different profile and would otherwise
#      invalidate the `dev`-profile build.mcpp cache the replay check (3)
#      depends on.
#   5. A DEPENDENCY'S DECLARATION REACHES THE CONSUMER'S EXECUTABLE: the
#      directive is `Scope::LinkGlobal`, folded into `LinkIntent
#      .runtimeSearchDirs` by the SAME per-package merge (`resolve_runtime_
#      contract`, plan.cppm) the manifest key goes through, so a path
#      dependency that calls `runtime_search_dir` in its OWN build.mcpp makes
#      its consumer's binary run without the consumer naming the directory at
#      all.
#
# It lands DIRECTLY on `LinkIntent::runtimeSearchDirs`, not the retiring
# `RuntimeConfig::libraryDirs` (docs/04 §2.11) -- #702 as first written joined
# the wrong field (design §5.3 point 1); nothing here would tell them apart
# if it had not been fixed, since every reader below is fed FROM
# `runtimeSearchDirs` either way once the manifest key itself is also routed
# there.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src app/rtlib

# The prebuilt-plugin shape a real build.mcpp would locate (a vcpkg/Qt-style
# `bin/`): compiled directly with the toolchain here, standing in for that
# discovery the same way 62_runtime_library_dirs.sh's precompiled .so stands
# in for the manifest key's case.
cat > app/rtlib/plugin.c <<'EOF'
int runtime_plugin_answer(void) { return 42; }
EOF
gcc -shared -fPIC app/rtlib/plugin.c -o app/rtlib/libruntime_plugin.so

cat > app/src/main.cpp <<'EOF'
extern "C" int runtime_plugin_answer();
int main() { return runtime_plugin_answer() == 42 ? 0 : 1; }
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[toolchain]
linux = "gcc@16.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

# The directive under test. `link_lib`/`link_search` also come from the
# program, on purpose: the point is a dependency the build.mcpp DISCOVERS,
# not one written into mcpp.toml by hand -- the case `runtime_search_dirs`
# cannot cover.
cat > app/build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::link_lib("runtime_plugin");
    mcpp::link_search((root + "/rtlib").c_str());
    mcpp::runtime_search_dir((root + "/rtlib").c_str());
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"
cd app
find_graph() { find target -name build.ninja | head -1; }

# ── 1. a plain build gets the RUNPATH treatment, never -L ──────────────────
"$MCPP" build > b1.log 2>&1 || fail "build failed" b1.log
G=$(find_graph)
[ -n "$G" ] || fail "no build.ninja" b1.log
RTDIR=$(realpath rtlib)
grep -F -- "-Wl,-rpath,$RTDIR" "$G" >/dev/null \
  || fail "runtime-search-dir directive missing from RUNPATH intent" "$G"

# ── 2. mcpp run finds the library only through that RUNPATH ────────────────
"$MCPP" run > run1.log 2>&1 \
  || fail "run failed to find the plugin through the directive's RUNPATH" run1.log b1.log

# ── 3. the replay criterion: a build.mcpp cache HIT keeps the directive ────
# `mcpp pack` builds under a different profile and would invalidate this
# package's build.mcpp cache (a different ctx hash) before the replay is
# exercised, so the cache-hit rebuild happens BEFORE pack, on the same `dev`
# profile `mcpp build` and `mcpp run` already used.
touch src/main.cpp   # past the whole-project no-op fast path, without
                      # touching build.mcpp itself
"$MCPP" build > b2.log 2>&1 || fail "second build failed" b2.log
grep -q "up to date (cached)" b2.log \
  || fail "the second build re-ran build.mcpp; the replay path was not exercised" b2.log
G2=$(find_graph)
[ -n "$G2" ] || fail "no build.ninja after the second build" b2.log
grep -F -- "-Wl,-rpath,$RTDIR" "$G2" >/dev/null \
  || fail "the RUNPATH directive did not survive a build.mcpp cache hit" "$G2"
"$MCPP" run > run2.log 2>&1 \
  || fail "run failed after the directive was replayed from a build.mcpp cache hit" run2.log b2.log

# ── 4. mcpp pack --format dir stages it into the bundle's lib/ ─────────────
"$MCPP" pack --format dir > pack1.log 2>&1 || fail "pack --format dir failed" pack1.log
STAGED=$(find target/dist -name 'libruntime_plugin.so' | head -1)
[ -n "$STAGED" ] || fail "the runtime-search-dir closure was not staged by mcpp pack" pack1.log
case "$STAGED" in
  */lib/libruntime_plugin.so) : ;;
  *) fail "the staged library is not under the packed bundle's lib/" pack1.log ;;
esac

cd "$TMP"

# ── 5. a dependency's declaration reaches the consumer's executable ────────
# `libdep` is a path dependency whose OWN build.mcpp declares the plugin's
# directory; `consumer` names none of this -- no `link_search`, no
# `runtime_search_dir`, not even `link_lib` in its own manifest or program --
# and still runs, because the directive is LinkGlobal and the merge that
# folds a dependency's LinkIntent into its consumer's is the SAME one the
# manifest key goes through (plan.cppm ~:911-948).
mkdir -p libdep/src libdep/plugin

cat > libdep/plugin/dep_plugin.c <<'EOF'
int dep_plugin_answer(void) { return 7; }
EOF
gcc -shared -fPIC libdep/plugin/dep_plugin.c -o libdep/plugin/libdep_plugin.so

cat > libdep/src/lib.cpp <<'EOF'
extern "C" int dep_plugin_answer();
int libdep_touch() { return dep_plugin_answer(); }
EOF

cat > libdep/mcpp.toml <<'EOF'
[package]
name    = "libdep"
version = "0.1.0"

[modules]
sources = ["src/**/*.cpp"]

[targets.libdep]
kind = "lib"
EOF

cat > libdep/build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::link_lib("dep_plugin");
    mcpp::link_search((root + "/plugin").c_str());
    mcpp::runtime_search_dir((root + "/plugin").c_str());
    return 0;
}
EOF

mkdir -p consumer/src
cat > consumer/src/main.cpp <<'EOF'
extern "C" int dep_plugin_answer();
int main() { return dep_plugin_answer() == 7 ? 0 : 1; }
EOF

cat > consumer/mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"

[toolchain]
linux = "gcc@16.1.0"

[dependencies]
libdep = { path = "../libdep" }

[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

cd consumer
DEPDIR=$(realpath ../libdep/plugin)
"$MCPP" build > b3.log 2>&1 || fail "consumer build failed" b3.log
G3=$(find_graph)
[ -n "$G3" ] || fail "no build.ninja for consumer" b3.log
grep -F -- "-Wl,-rpath,$DEPDIR" "$G3" >/dev/null \
  || fail "the dependency's runtime-search-dir did not reach the consumer's link" "$G3"
"$MCPP" run > run3.log 2>&1 \
  || fail "consumer run failed to find the dependency's plugin" run3.log b3.log

echo "PASS: 779_a_build_program_declares_a_runtime_search_dir"
