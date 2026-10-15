#!/usr/bin/env bash
# F2 / #809 under mcpp: on an NFSv3 mount,
#   - `mcpp new` (mcpp's own renameat2(RENAME_NOREPLACE) in scaffold_fs),
#   - `mcpp toolchain install gcc` with the bundled xlings (2026.10.10.2),
#   - the same with MCPP_VENDORED_XLINGS = xlings $XV,
#   - then build and run a project that lives on NFS.
# Prints observations; never asserts.
set -uo pipefail
. "$(dirname "$0")/lib.sh"
N="${1:?usage: nfs.sh <NFSv3 mount>}"
cd "$T"
obs "mount" bash -c "grep ' $N ' /proc/mounts"
obs "renameat2(RENAME_NOREPLACE) here" python3 - "$N" <<'PY'
import ctypes, errno, os, sys
libc = ctypes.CDLL(None, use_errno=True)
d = sys.argv[1]; a = os.path.join(d, "probe-a"); b = os.path.join(d, "probe-b")
open(a, "w").close()
rc = libc.syscall(316, -100, a.encode(), -100, b.encode(), 1)
print("rc", rc, "errno", errno.errorcode.get(ctypes.get_errno(), ctypes.get_errno()))
for p in (a, b):
    try: os.remove(p)
    except FileNotFoundError: pass
PY

curl -fsSL -o mc.tgz "https://github.com/mcpp-community/mcpp/releases/download/v$MV/mcpp-$MV-linux-x86_64.tar.gz"
tar -xzf mc.tgz
M="$T/mcpp-$MV-linux-x86_64/bin/mcpp"
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
tar -xzf xl.tgz
XB="$T/xlings-$XV-linux-x86_64/bin/xlings"

section "mcpp new on NFS (local MCPP_HOME)"
export MCPP_HOME="$T/mh-local"
m() { clean_env MCPP_HOME="$MCPP_HOME" HOME="$T/home" "$M" "$@"; }
mkdir -p "$N/w1" && cd "$N/w1"
obs "mcpp new demo (cwd on NFS)" m new demo
obs "what was made" bash -c "ls -la '$N/w1' '$N/w1/demo' 2>&1 | head -20"

section "MCPP_HOME on NFS, bundled xlings"
export MCPP_HOME="$N/mh-bundled"
OBS_LINES=20 obs "mcpp toolchain install gcc 16.1.0" m toolchain install gcc 16.1.0

section "MCPP_HOME on NFS, xlings $XV"
export MCPP_HOME="$N/mh-new"
mv_() { clean_env MCPP_HOME="$MCPP_HOME" HOME="$T/home" MCPP_VENDORED_XLINGS="$XB" "$M" "$@"; }
OBS_LINES=20 obs "mcpp toolchain install gcc 16.1.0" mv_ toolchain install gcc 16.1.0
obs "mcpp toolchain default gcc@16.1.0" mv_ toolchain default gcc@16.1.0
obs "xlings in that home" bash -c "'$N/mh-new/registry/bin/xlings' --version"
mkdir -p "$T/local" && cd "$T/local" && mv_ new demo >/dev/null 2>&1
cp -r "$T/local/demo" "$N/w2" 2>/dev/null || { mkdir -p "$N/w2"; cp -r "$T/local/demo/." "$N/w2/"; }
cd "$N/w2"
OBS_LINES=20 obs "build a project on NFS" mv_ build
obs "run" mv_ run
