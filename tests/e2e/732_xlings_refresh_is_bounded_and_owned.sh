#!/usr/bin/env bash
# requires: unix-shell
# 732 -- an xlings child is bounded, and it does not outlive mcpp (#648 L3).
#
# The xlings binary is a stub named through `[xlings] binary`; its `update`
# writes its pid and sleeps. Criteria:
#   A. `mcpp index update` under `[index] refresh_timeout = 3` returns within the
#      bound plus a margin, says which bound stopped it, and the stub is gone;
#   B. with a long bound, SIGTERM to mcpp takes the stub with it: the stub's
#      process group is registered with mcpp's signal guard.
set -e

TMP=$(mktemp -d)
cleanup() {
    for f in "$TMP"/stub.pid "$TMP"/stub2.pid; do
        [ -s "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null || true
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

# `timeout` is GNU coreutils and is absent on macOS; `gtimeout` is there when
# coreutils is installed. Without either, the bound under test is the engine's
# own, and the suite's per-test bound is the backstop.
bounded() {   # <seconds> <command...>
    local secs="$1"; shift
    if command -v timeout  >/dev/null 2>&1; then timeout  "$secs" "$@"; return $?; fi
    if command -v gtimeout >/dev/null 2>&1; then gtimeout "$secs" "$@"; return $?; fi
    "$@"
}

mkdir -p "$TMP/bin" "$TMP/home"
cat > "$TMP/bin/xlings" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  update) echo "$$" > "${STUB_PID:?}"; exec sleep 1000 ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/xlings"

write_config() {
    cat > "$TMP/home/config.toml" <<EOF
[xlings]
binary = "$TMP/bin/xlings"

[index]
refresh_timeout = $1
EOF
}

# A. the bound
write_config 3
start=$(date +%s)
set +e
STUB_PID="$TMP/stub.pid" MCPP_HOME="$TMP/home" bounded 60 "$MCPP" index update > "$TMP/a.log" 2>&1
set -e
elapsed=$(( $(date +%s) - start ))
[ -s "$TMP/stub.pid" ] || fail "A: the stub's update never ran" "$TMP/a.log"
[ "$elapsed" -lt 30 ] || fail "A: index update returned after ${elapsed}s under a 3 s bound" "$TMP/a.log"
grep -q "did not finish within 3 seconds" "$TMP/a.log" || fail "A: no line names the bound" "$TMP/a.log"
sleep 1
if kill -0 "$(cat "$TMP/stub.pid")" 2>/dev/null; then fail "A: the stub outlived its bound"; fi

# B. ownership
write_config 600
STUB_PID="$TMP/stub2.pid" MCPP_HOME="$TMP/home" "$MCPP" index update > "$TMP/b.log" 2>&1 &
mcpp_pid=$!
for _ in $(seq 1 150); do [ -s "$TMP/stub2.pid" ] && break; sleep 0.1; done
[ -s "$TMP/stub2.pid" ] || { kill -9 "$mcpp_pid" 2>/dev/null; fail "B: the stub's update never ran" "$TMP/b.log"; }
kill -TERM "$mcpp_pid"
wait "$mcpp_pid" 2>/dev/null || true
for _ in $(seq 1 30); do kill -0 "$(cat "$TMP/stub2.pid")" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$(cat "$TMP/stub2.pid")" 2>/dev/null; then fail "B: the stub outlived mcpp after SIGTERM"; fi

echo "PASS: 732 xlings refresh is bounded and owned (A ${elapsed}s)"
