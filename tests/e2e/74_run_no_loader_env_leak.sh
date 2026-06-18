#!/usr/bin/env bash
# requires: gcc elf
# 74: `mcpp run` must not leak the target's bundled-glibc LD_LIBRARY_PATH into
# the launcher shell, and the produced binary must use the BUNDLED dynamic
# linker. Regression for the newer-glibc `sh:` crash: a leaked LD_LIBRARY_PATH
# forced the host /bin/sh to load the bundled (older) libc, which could not
# satisfy the host libtinfo's newer GLIBC symbol versions, so sh aborted
# before the target ever ran.
set -e

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

"$MCPP" new leakcheck >/dev/null
cd leakcheck

# 1. Hostile parent env: a bogus LD_LIBRARY_PATH must neither break the launcher
#    nor be required for the target to run. Pre-fix this could crash /bin/sh.
LD_LIBRARY_PATH="/nonexistent/poison:${LD_LIBRARY_PATH:-}" "$MCPP" run

# 2. Self-containment: PT_INTERP points into the mcpp registry (bundled loader),
#    not host /lib64 — proof the run path relies on the artifact, not on env.
"$MCPP" build >/dev/null
bin="$(find target -type f -name leakcheck | head -1)"
[[ -n "$bin" ]] || { echo "leakcheck binary not found"; exit 1; }
interp="$(file "$bin" | grep -o 'interpreter [^,]*' | awk '{print $2}')"
echo "interp=$interp"
case "$interp" in
    */.mcpp/*|*/registry/*|*xpkgs*) echo "OK: bundled loader" ;;
    "")               echo "OK: static (no interp)" ;;
    /lib64/*|/lib/*)  echo "FAIL: host loader $interp"; exit 1 ;;
    *)                echo "WARN: unrecognized interp $interp" ;;
esac

echo "74_run_no_loader_env_leak: OK"
