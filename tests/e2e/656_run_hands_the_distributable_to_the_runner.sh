#!/usr/bin/env bash
# requires: elf gcc
# 656_run_hands_the_distributable_to_the_runner.sh -- `mcpp run --format
# <name>` (#622 A10, design record
# 2026-09-12-622-a-ui-framework-on-android-ios-and-web.md §2.10).
#
# `mcpp run --format <name>` IS `mcpp pack --format <name>` -- the two
# prepares, the build, the staging, the provider's action -- followed by the
# ORDINARY run, with the artifact the pack pipeline reported as the operand.
# Runner resolution is unchanged: this project's `[target.<triple>] runner`.
#
# What this holds, each with its negative-direction control:
#   1. `--format <name>` hands the runner the DISPATCHED artifact, not the
#      link output -- asserted on the exact operand the runner prints, same
#      technique as 330_runner_hosted_targets.sh (a runner that records and
#      then execs, so the assertion is on the printed argv, not on a message).
#   2. A PLAIN `mcpp run` is unaffected: it still hands the runner the link
#      output.
#   3. An unknown format is refused by the PACK pipeline's own message,
#      naming what the graph provides -- no separate run-side vocabulary.
#   4. `--format` with `--no-runner` is refused before anything is built: a
#      distributable is not a thing this host executes on its own.
#   5. A PLAIN `mcpp build` afterward has a plain graph (`dist=none`) and
#      does not carry the dispatched action's edge -- `--format` on `run`
#      leaves no residue in the ordinary build the way `mcpp pack --format`
#      does not (638).
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p app/src
cd app

cat > mcpp.toml <<'EOF'
[package]
name        = "app"
version     = "1.0.0"
description = "a program with one dispatched distributable"
license     = "Apache-2.0"
authors     = ["Ada <ada@example.org>"]
repo        = "https://example.org/app"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("1-2-3"); return 0; }
EOF

# The artifact action's command: argv only, no shell, exactly the reason
# 188_build_actions.sh and 638_pack_format_dispatch.sh both write a separate
# script rather than a shell one-liner. `chmod +x` on the copy rather than
# relying on `cp` to preserve the executable bit -- the runner below execs
# this file directly.
cat > copy.sh <<'EOF'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
chmod +x "$2"
EOF
chmod +x copy.sh

cat > build.mcpp <<'EOF'
import mcpp;
#include <string>
#include <string_view>
int main() {
    // DECLARE UNCONDITIONALLY, SUBMIT CONDITIONALLY (§1 rule 3): a plain
    // build still declares "blob", so --format bogus can name it as
    // available and a plain `mcpp build` afterward carries no such edge.
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;

    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:app}")
     .arg(out.c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    // A second step that consumes the first: the format's distributable is
    // the TERMINAL artifact (the output no other introduced action consumes),
    // which is what the runner must receive. A provider such as dist-apk is
    // a chain of this shape (link, add libraries, align, sign).
    const std::string fin = std::string(mcpp::out_dir()) + "/app.final";
    mcpp::action b;
    b.id          = "final";
    b.role        = "artifact";
    b.description = "final";
    b.arg((root + "/copy.sh").c_str())
     .arg(out.c_str())
     .arg(fin.c_str())
     .input(out.c_str())
     .output(fin.c_str())
     .submit();
    return 0;
}
EOF

MCPP="${MCPP:-mcpp}"

graph_dist() { sed -n '2p' "$1" | sed 's/.*;dist=//;s/;.*//'; }

# ── Determine the host triple, and declare the runner under it ────────────
# Same technique as 330_runner_hosted_targets.sh: the canonical spelling is
# the name of the directory `mcpp build` actually wrote, read from the
# engine rather than guessed (macOS's status line spells the triple
# differently from the directory key).
"$MCPP" build > b0.log 2>&1 || fail "the initial build failed" b0.log
HOST=$(ls target | head -1)
[ -n "$HOST" ] || fail "could not determine the host triple from target/" b0.log
# THE PLAIN GRAPH'S OWN PATH, captured now rather than re-derived at the
# end. `--format` (a `pack_format` override) resolves to a DIFFERENT
# fingerprint directory than a plain build -- see mcpp.build.plan's
# fingerprint inputs -- so once `run --format blob` has run, target/ holds
# both a plain and a "blob" build.ninja side by side; a plain build never
# touches the "blob" one, and `ninja` files already at the byte content
# being written are not re-touched, so their mtimes do not order the two.
# The plain directory is the one a plain build resolves to EVERY time, so
# capturing it once here and re-reading it in step 5 is exact where
# "most recently modified" is not.
G0=$(ls target/*/*/build.ninja 2>/dev/null | head -1)
[ -n "$G0" ] || fail "no build.ninja after the initial build" b0.log

cat > "$TMP/runner.sh" <<'EOF'
#!/bin/sh
printf 'RUNNER: %s\n' "$1"
exec "$@"
EOF
chmod +x "$TMP/runner.sh"

printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml

# ── 1. `mcpp run --format blob` hands the runner the distributable ────────
out=$("$MCPP" run --format blob 2>&1) || fail "mcpp run --format blob failed" <(echo "$out")
grep -q "RUNNER: .*/app\.final$" <<<"$out" \
    || fail "the runner's operand was not the terminal artifact app.final" <(echo "$out")
grep -q "RUNNER: .*/app\.blob$" <<<"$out" \
    && fail "the runner received the intermediate app.blob" <(echo "$out")
grep -q "Packed .*app\.final" <<<"$out" \
    || fail "the Packed line does not name the terminal artifact" <(echo "$out")
grep -q "Packed .*app\.blob" <<<"$out" \
    && fail "the intermediate app.blob was reported as Packed" <(echo "$out")
grep -q "1-2-3" <<<"$out" \
    || fail "the program's marker did not print through the runner" <(echo "$out")
echo "mcpp run --format blob hands the runner the distributable OK"

# ── 2. a plain `mcpp run` is unaffected: the operand is the link output ───
out=$("$MCPP" run 2>&1) || fail "mcpp run failed" <(echo "$out")
grep -q "RUNNER: .*/bin/app$" <<<"$out" \
    || fail "a plain run's operand was not bin/app" <(echo "$out")
grep -q "1-2-3" <<<"$out" || fail "a plain run lost the program's marker" <(echo "$out")
echo "mcpp run alone is unaffected OK"

# ── 3. an unknown format is refused, naming what the graph provides ───────
set +e
out=$("$MCPP" run --format bogus 2>&1); rc=$?
set -e
[ "$rc" -ne 0 ] || fail "mcpp run --format bogus was accepted" <(echo "$out")
grep -q "unknown --format 'bogus'" <<<"$out" \
    || fail "the refusal does not name the value" <(echo "$out")
grep -q "available in this build:.*blob" <<<"$out" \
    || fail "the refusal does not name what is available" <(echo "$out")
echo "mcpp run --format bogus is refused, naming blob OK"

# ── 4. --format with --no-runner is refused before anything runs ──────────
set +e
out=$("$MCPP" run --format blob --no-runner 2>&1); rc=$?
set -e
[ "$rc" -ne 0 ] || fail "--format with --no-runner was accepted" <(echo "$out")
grep -q -- "--format and --no-runner cannot be combined" <<<"$out" \
    || fail "the refusal does not name the pairing" <(echo "$out")
grep -q "1-2-3" <<<"$out" && fail "the program ran despite the refusal" <(echo "$out")
echo "--format with --no-runner is refused OK"

# ── 5. a plain build afterward has a plain graph, with no blob edge ───────
find target -name 'app.blob' -delete 2>/dev/null || true
"$MCPP" build > b5.log 2>&1 || fail "the build after run --format failed" b5.log
[ -f "$G0" ] || fail "the plain build's own build.ninja disappeared" b5.log
[ "$(graph_dist "$G0")" = "none" ] \
    || { sed -n '2p' "$G0"; fail "the graph after run --format is not dist=none" b5.log; }
grep -q "blob" "$G0" \
    && fail "a plain build's graph still carries the blob edge" b5.log
[ -z "$(find target -name 'app.blob' 2>/dev/null)" ] \
    || fail "a plain build regenerated the distributable" b5.log
echo "a plain build afterward has a plain graph, no blob edge OK"

echo "PASS: 656_run_hands_the_distributable_to_the_runner"
