#!/usr/bin/env bash
# requires:
# 221_subos_without_info_still_builds.sh — absence degrades, it does not
# invalidate.
#
# THE REGRESSION THIS PINS (openxlings/xlings#543)
#
# A SubOS with no `subos_info` block made `resolve_runtime_binding` return an
# error, and every caller of `mcpp build` / `mcpp test` propagated it. On
# Windows — where xlings does not write that block, and where the facts it
# carries (PT_INTERP, a private libc, GL vendor directories) do not exist at
# all — that meant the tool stopped, with a message about graphics drivers:
#
#   error: selected SubOS 'default' cannot provide a RuntimeBinding: … does not
#          describe itself … a GL application will not find its drivers
#
# Shipped in 2026.8.10.2; 2026.8.8.4 was fine. The shape is the index-floor
# incident again: DATA THAT IS MISSING OR NEWER MUST NOT INVALIDATE THE PROGRAM
# THAT READS IT.
#
# WHY THIS TEST DECLARES NO CAPABILITIES
#
# On purpose, and it is the whole point. The regression was Windows-only in
# practice, and a `# requires: elf` or `# requires: gcc` line would skip it on
# exactly the platform it exists to protect — the same way `# requires: gcc`
# kept 217 from ever running on Windows or macOS. It needs no ELF, no graphics,
# and no particular toolchain: only that a build completes.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

# A named SubOS that exists and says nothing about itself. `[xlings] subos`
# resolves to <project>/.mcpp/.xlings/subos/<name>, so the directory is created
# there. Empty is the extreme form of "no subos_info block", and it is a real
# state: a freshly created SubOS, or any SubOS on a platform whose xlings does
# not write the block.
mkdir -p ".mcpp/.xlings/subos/bare"

cat > mcpp.toml <<'EOF'
[package]
name = "bare"
version = "0.1.0"

[xlings]
subos = "bare"
EOF
echo 'int main() { return 0; }' > src/main.cpp

set +e
out="$("$MCPP" build 2>&1)"
rc=$?
set -e
echo "$out"

# ── the assertion that IS the regression ────────────────────────────────────
case "$out" in
    *"cannot provide a RuntimeBinding"*)
        echo "FAIL: an undeclared SubOS still invalidates the build"
        echo "      Absence of a declaration leaves some facts unknown; it does"
        echo "      not make the build wrong. Only a CONTRADICTION (a named"
        echo "      SubOS that is not there) may fail."
        exit 1
        ;;
esac
# A build MAY still fail here, but only for a reason that is about the C
# runtime rather than about the binding — and that distinction is the fix.
#
# An undeclared SubOS names no runtime, so on a platform whose C runtime comes
# from a payload there is nothing to bind to and mcpp declines PayloadFirst
# rather than guessing a version. The hermeticity guard then reports, exactly
# and actionably, that the toolchain resolved its C runtime outside the
# sandbox. That is a different statement from "this SubOS cannot provide a
# RuntimeBinding", and it is one the user can act on.
#
# Anything ELSE failing is not this test passing — it is a new defect wearing
# the old one's clothes.
if [[ $rc -ne 0 ]]; then
    case "$out" in
        *"hermetic link check failed"*) ;;
        *)
            echo "FAIL: mcpp build exited $rc for a reason this test does not"
            echo "      recognise. An undeclared SubOS may cost hermeticity; it"
            echo "      must not cost anything else."
            exit 1
            ;;
    esac
fi

# ── and with the host runtime allowed, it must build everywhere ─────────────
# The green half. Without it the test above could be satisfied by mcpp failing
# for *some* accepted reason on every platform, which is not what "the build is
# unaffected" means.
cat > mcpp.toml <<'EOF'
[package]
name = "bare"
version = "0.1.0"

[xlings]
subos = "bare"

[build]
allow_host_libs = true
EOF
"$MCPP" build > allow.log 2>&1 || {
    echo "FAIL: even with allow_host_libs, an undeclared SubOS blocks the build"
    cat allow.log
    exit 1
}

# ── and it must SAY so ──────────────────────────────────────────────────────
# A degradation nobody prints is indistinguishable from no degradation, which
# is the property that made mcpp#352 expensive. Passing silently here would
# mean the next person cannot tell "my SubOS is fine" from "my SubOS told mcpp
# nothing and mcpp shrugged".
case "$out" in
    *"does not describe itself"*) ;;
    *)
        echo "FAIL: the build succeeded but never reported the degradation."
        echo "      'it did not happen' and 'it succeeded' must not look alike."
        exit 1
        ;;
esac

# ── the contradiction case still fails ──────────────────────────────────────
# The other half of the rule, asserted here so relaxing absence cannot quietly
# relax everything: a SubOS the user NAMED and that does not exist cannot be
# satisfied under any interpretation, and guessing a different one would make
# one mcpp.toml mean different ABIs on different machines.
cat > mcpp.toml <<'EOF'
[package]
name = "bare"
version = "0.1.0"

[xlings]
subos = "definitely-not-created"
EOF
set +e
out2="$("$MCPP" build 2>&1)"
rc2=$?
set -e
[[ $rc2 -ne 0 ]] || {
    echo "FAIL: naming a SubOS that does not exist was accepted"
    echo "$out2"
    exit 1
}
case "$out2" in
    *"does not exist"*) ;;
    *)
        echo "FAIL: the missing-SubOS error no longer says what is wrong"
        echo "$out2"
        exit 1
        ;;
esac

echo "PASS: an undeclared SubOS degrades and reports it; a missing one still fails"
