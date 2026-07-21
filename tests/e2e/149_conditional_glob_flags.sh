#!/usr/bin/env bash
# #258: per-glob `flags` inside `[target.'cfg(<os>)'.build]`.
#
# What this unblocks, in the reporter's words: `sources` is OS-conditional
# but `flags` was not, so one manifest covering three OSes had to present
# every OS with the other two OSes' flag entries — each matching zero sources
# by construction. The vendored-opencv port worked around it with 703
# committed stub files whose only purpose was to give windows TUs OS-unique
# PATHS that a global flag table could key on, plus 32 globs pointing at
# them, plus ~23 structurally-guaranteed dead-glob warnings on every build.
#
# Three assertions, matching the three costs:
#   1. a matching-OS conditional flag entry reaches the TU;
#   2. it can OVERRIDE the unconditional base table (the removal case:
#      unix defines HAVE_UNISTD_H, windows must not);
#   3. an off-OS entry produces NO dead-glob warning — the entry simply does
#      not exist on this OS, the same way #253 made a feature-off entry not
#      exist. No suppression mechanism, no `optional = true` escape hatch.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new condflags > /dev/null
cd condflags

case "$(uname -s)" in
    Linux)  THIS_OS=linux;  OTHER_OS=windows ;;
    Darwin) THIS_OS=macos;  OTHER_OS=windows ;;
    *)      THIS_OS=windows; OTHER_OS=linux ;;
esac

mkdir -p src/zlib

# Probes the three-way outcome: the base define must be overridden by the
# conditional entry that lands after it, and the conditional-only define
# must be present.
cat > src/zlib/probe.cpp <<'EOF'
#ifndef BASE_DEFINE
#error "base [build].flags entry did not reach the TU"
#endif
#ifndef COND_DEFINE
#error "conditional [target.cfg(os)] flags entry did not reach the TU"
#endif
#ifdef REMOVED_DEFINE
#error "conditional entry failed to override the base table (removal case)"
#endif
extern "C" int probe_ok() { return 1; }
EOF

cat > src/main.cpp <<'EOF'
import std;
extern "C" int probe_ok();
int main() { std::println("probe = {}", probe_ok()); return 0; }
EOF

# The base table defines REMOVED_DEFINE for everyone; the matching-OS
# conditional entry appends -U for it (last flag wins) and adds its own.
# The off-OS entry points at a glob that matches nothing on this host and
# must stay silent.
cat > mcpp.toml <<EOF
[package]
name    = "condflags"
version = "0.1.0"

[build]
flags = [
  { glob = "src/zlib/**", defines = ["BASE_DEFINE=1", "REMOVED_DEFINE=1"] },
]

[target.'cfg(${THIS_OS})'.build]
flags = [
  { glob = "src/zlib/**", defines = ["COND_DEFINE=1"], cxxflags = ["-UREMOVED_DEFINE"] },
]

[target.'cfg(${OTHER_OS})'.build]
flags = [
  { glob = "src/${OTHER_OS}_only/**", defines = ["OFF_OS=1"] },
]
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; echo "conditional glob flags build failed"; exit 1; }

out=$("$MCPP" run 2>&1 | tail -1)
[[ "$out" == "probe = 1" ]] || { echo "unexpected output: $out"; cat build.log; exit 1; }

# Assertion 3 — the whole point of the "23 warnings" half of the report.
if grep -q "matched no source file" build.log; then
    cat build.log
    echo "FAIL: an off-OS conditional flags entry produced a dead-glob warning;"
    echo "      off-OS entries must not exist on this OS at all"
    exit 1
fi

# Control: a genuinely dead glob in the UNCONDITIONAL table must still warn.
# The fix removes structural noise, not real signal.
cat >> mcpp.toml <<'EOF'

[[build.flags]]
glob = "src/nonexistent_dir/**"
defines = ["NEVER=1"]
EOF
"$MCPP" build > build2.log 2>&1 || { cat build2.log; exit 1; }
grep -q "matched no source file" build2.log || {
    cat build2.log
    echo "FAIL: a real dead glob in [build].flags must still warn"
    exit 1
}

echo "OK"
