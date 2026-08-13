#!/usr/bin/env bash
# newest_artifact.sh <target-dir> <basename> — print the most recently built
# copy of a binary under a target/ tree.
#
# WHY THIS IS NOT `find ... | head -1`. mcpp lays artifacts out under
# target/<triple>/<toolchain-fingerprint>/bin/, and the fingerprint changes
# whenever the toolchain, the standard or the flags do. A tree that has been
# built more than once therefore holds SEVERAL binaries with the same name, and
# `find | head -1` picks whichever the filesystem happens to list first.
#
# That is not hypothetical: it picked a two-and-a-half-hour-old bench binary on
# the first machine it ran on, and the run that followed silently exercised code
# that had already been replaced. In CI the same line would benchmark a stale
# mcpp and report the numbers as the new one's — a wrong answer with no symptom,
# which is the only kind this suite really has to defend against.
#
# `-printf` is GNU-only and macOS ships BSD find, so the mtime comes from a
# per-file `stat` call whose flag differs by platform. Both spellings are here
# because the alternative is a script that works on Linux and silently returns
# the wrong file everywhere else.
set -euo pipefail

dir="${1:?usage: newest_artifact.sh <target-dir> <basename>}"
name="${2:?usage: newest_artifact.sh <target-dir> <basename>}"

[ -d "$dir" ] || { echo "newest_artifact: no such directory: $dir" >&2; exit 1; }

mtime() {
    # GNU coreutils first, then BSD/macOS. Windows runners use git-bash, which
    # ships GNU stat.
    stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null || echo 0
}

best=""
best_t=-1
# `bin/<name>` and `bin/<name>.exe` — anchored on the bin/ directory so a
# same-named object or intermediate elsewhere in target/ cannot win.
while IFS= read -r f; do
    [ -f "$f" ] || continue
    t=$(mtime "$f")
    if [ "$t" -gt "$best_t" ]; then best_t=$t; best=$f; fi
done <<EOF
$(find "$dir" -type f \( -path "*/bin/$name" -o -path "*/bin/$name.exe" \) 2>/dev/null)
EOF

if [ -z "$best" ]; then
    echo "newest_artifact: no '$name' under $dir/*/*/bin/" >&2
    find "$dir" -maxdepth 4 -type d -name bin >&2 2>/dev/null || true
    exit 1
fi

printf '%s\n' "$best"
