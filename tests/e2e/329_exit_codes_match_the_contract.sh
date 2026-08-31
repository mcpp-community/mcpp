#!/usr/bin/env bash
# 329_exit_codes_match_the_contract.sh — #540, SPEC-003.
#
# The machine-output chapter shipped an exit-code table carrying 0 / 2 / 70 /
# 127 — which is the usage/internal half of what the protocol design record
# (§R4) asked for. The runtime half was never written down, so `1` — the code
# `mcpp xpkg parse` returns for every descriptor it rejects — was undocumented,
# and an audit reading that table concluded the missing code was `4`. It is not:
# no enveloped command can return 4, because `self env --format json` is built
# specifically to avoid the `load_or_init` path that produces it.
#
# docs/spec/exit-codes.md is now the contract. This holds it.
#
# ⚠️ ASSERTS THE CODE, NOT "STDERR IS NON-EMPTY". A criterion whose "no" is also
# what a crash produces cannot tell the two apart — and the whole point of the
# table is that a client can distinguish a rejection from a broken mcpp.
#
# requires: unix-shell
set -e

MCPP="${MCPP:-mcpp}"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

run_rc() {  # run_rc <expected> <label> -- <argv...>
    local want="$1" label="$2"; shift 3
    set +e
    "$MCPP" "$@" > out.txt 2> err.txt
    local rc=$?
    set -e
    [ "$rc" -eq "$want" ] || {
        echo "FAIL: $label — expected exit $want, got $rc"
        echo "--- stdout:"; cat out.txt
        echo "--- stderr:"; cat err.txt
        exit 1; }
    echo "  ok  $label → $rc"
}

# ── 0: success ─────────────────────────────────────────────────────────────
run_rc 0 "self env --format json" -- self env --format json
grep -q '"schemaVersion"' out.txt || { cat out.txt; echo "FAIL: no envelope on stdout"; exit 1; }

# ── 2: usage error — an unsupported --format value ─────────────────────────
# Nothing on stdout: a request that does not yet know what it will be given
# must not write into the channel the protocol owns.
run_rc 2 "self env --format yaml" -- self env --format yaml
[ ! -s out.txt ] || { cat out.txt; echo "FAIL: a usage error wrote to stdout"; exit 1; }

# ── 127: unknown command ───────────────────────────────────────────────────
run_rc 127 "an unknown subcommand" -- definitely-not-a-subcommand-540

# ── 1: the command ran and failed ──────────────────────────────────────────
# The code the table omitted. An unreadable file is the simplest instance.
run_rc 1 "xpkg parse on a missing file" -- xpkg parse ./no-such-descriptor.lua --format json
[ -s err.txt ] || { echo "FAIL: a non-zero exit left stderr empty (SPEC-003 §3.5)"; exit 1; }

# …and the shape SPEC-003 §3.4 exists for: a failure that is ALSO a document.
# A client treating any non-zero exit as "no output" would discard an answer it
# was handed.
# A short name that still carries a dot: identity is (namespace, name) where
# `name` is ONE atomic segment (SPEC-001 §2/§3.2), so this is INV-NAME's
# violating shape and `xpkg parse` reports it as a document.
cat > split.lua <<'EOF'
package = {
    name      = "acme.sub.widget",
    namespace = "acme",
}
xpm = { linux = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0" } } }
EOF
set +e
"$MCPP" xpkg parse ./split.lua --format json > out.txt 2> err.txt
rc=$?
set -e
[ "$rc" -eq 1 ] || { echo "FAIL: a rejected descriptor should exit 1, got $rc"; cat out.txt err.txt; exit 1; }
grep -q '"schemaVersion"' out.txt || {
    echo "FAIL: exit 1 with --format json must still carry the envelope on stdout"
    cat out.txt; exit 1; }
echo "  ok  a rejected descriptor exits 1 AND puts its envelope on stdout"

echo "PASS: 329 exit codes match the contract"
