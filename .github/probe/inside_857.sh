#!/system/bin/sh
# Runs inside the app sandbox (run-as com.termux): the failing case of #857,
# mcpp new + mcpp build, the build traced by a static strace.
A=/data/data/com.termux/files
D="$1"
export HOME="$A/home" TMPDIR="$A/home/tmp"
mkdir -p "$TMPDIR"
cd "$HOME" && rm -rf hello
"$D/bin/mcpp" new hello 2>&1 | tail -5
cd hello
"$A/strace" -f -e trace=execve,execveat,openat,fchdir,clone,clone3,readlinkat,newfstatat,statx \
    -o "$HOME/trace.log" "$D/bin/mcpp" build 2>&1 | tail -15
echo "--- trace: failing calls ---"
grep -E "= -1 (ENOENT|EACCES|EPERM|EBADF)" "$HOME/trace.log" | grep -v "/proc/sys\|locale\|zoneinfo\|\.mcpp-" | tail -60
echo "--- trace: every exec ---"
grep -E "execve" "$HOME/trace.log" | tail -30
