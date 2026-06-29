#!/usr/bin/env bash
# 86_target_cfg_dependencies.sh — L1b platform-conditional DEPENDENCIES: a normal
# mcpp.toml can scope a dependency to a target predicate via
# `[target.'cfg(...)'.dependencies]`, evaluated against the resolved target (here
# the host). A matching predicate's dep resolves like any dep; a non-matching
# predicate's dep is never fetched/resolved. Host-aware so it runs on all three
# CI platforms. See .agents/docs/2026-06-29-manifest-environment-and-platform-design.md (L1b).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# A real path dependency exposing one symbol.
mkdir -p widget/src
cat > widget/mcpp.toml <<'EOF'
[package]
name    = "widget"
version = "0.1.0"
[targets.widget]
kind = "lib"
EOF
cat > widget/src/widget.cppm <<'EOF'
export module widget;
export int widget_anchor() { return 0; }
EOF

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

# widget is pulled under whichever family matches the host — so exactly one of
# these applies and widget is ALWAYS resolved (on any of the 3 platforms).
[target.'cfg(unix)'.dependencies]
widget = { path = "../widget" }
[target.'cfg(windows)'.dependencies]
widget = { path = "../widget" }

# A never-matching predicate (no such arch) points at a non-existent path. If the
# conditional merge wrongly pulled it, resolution of the bogus path would FAIL the
# build — so a clean build proves non-matching deps are excluded.
[target.'cfg(arch = "no_such_arch")'.dependencies]
ghost = { path = "../this_path_does_not_exist" }
EOF
cat > app/src/main.cpp <<'EOF'
import widget;  // fails to build unless the cfg-matched widget dep was resolved
int main() { return widget_anchor(); }
EOF

cd app
"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: conditional dep not resolved, or non-matching dep wrongly pulled"; exit 1; }

# Sanity: the matched dependency really was wired (widget appears in the lockfile).
if [ -f mcpp.lock ]; then
    grep -q 'widget' mcpp.lock || { echo "FAIL: widget missing from lockfile"; cat mcpp.lock; exit 1; }
fi
# The never-matching ghost must NOT be present.
if [ -f mcpp.lock ] && grep -q 'ghost' mcpp.lock; then
    echo "FAIL: non-matching cfg dependency 'ghost' leaked into the resolve"; exit 1; fi

echo "OK"
