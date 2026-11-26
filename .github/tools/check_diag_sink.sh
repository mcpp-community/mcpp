#!/usr/bin/env bash
# Every user-visible warning, note and tip goes through mcpp.diag (design 2026-11-21, build
# programs' inputs §3.6): `mcpp::ui::warning`, `mcpp::ui::note` and `mcpp::ui::tip` are its
# renderers, and nothing else calls them or writes a `warning:` line to standard error itself.
#
# WHY THIS EXISTS. A notice outside the sink is not deduplicated, does not reach the machine
# envelope, is not counted by --strict and cannot have a policy (`once`). Before this check there
# were about a hundred such calls and three private "say it once" sets, and each had to be found
# by reading.
set -u
cd "$(dirname "$0")/../.."

pattern='mcpp::ui::(warning|note|tip)\(|println\([^;]*stderr[^;]*"(warning|note|tip): '
hits=$(grep -rnE "$pattern" src modules --include='*.cppm' --include='*.cpp' \
       | grep -vE '^src/(diag|ui)\.cppm:' || true)
if [ -n "$hits" ]; then
    echo "::error::user-visible notices outside mcpp.diag (call mcpp::diag::warning/note/notice/closing instead):"
    echo "$hits"
    exit 1
fi
echo "OK: every warning, note and tip goes through mcpp.diag"
