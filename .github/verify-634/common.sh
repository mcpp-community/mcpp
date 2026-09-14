# Shared helpers for the #634 measurement jobs. Sourced, never executed.
#
# A measurement prints READING lines and does not fail its step on a reading
# it did not expect: the job exists to record what each platform does. A step
# fails only when the probe itself cannot run (its setup broke), which keeps
# "the platform said no" distinguishable from "the probe is broken".

reading() {  # reading <id> <text...>
    local id="$1"; shift
    printf 'READING %s: %s\n' "$id" "$*"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        printf -- '- `%s`: %s\n' "$id" "$*" >> "$GITHUB_STEP_SUMMARY"
    fi
}

# first_line <file>: the first non-empty line, for a one-line reading.
first_line() { grep -m1 -v '^[[:space:]]*$' "$1" 2>/dev/null | tr -d '\r'; }

# A file's contents on one line, for readings that quote short output.
one_line() { tr '\n' '|' < "$1" 2>/dev/null | tr -d '\r' | cut -c1-600; }

# The measured mcpp, fetched by setup-mcpp.sh.
: "${MCPP:?MCPP must be set by setup-mcpp.sh}"

# Git Bash on Windows hands RUNNER_TEMP over as `D:\a\_temp`; every script
# works in POSIX paths and converts only what a native program must read.
if command -v cygpath >/dev/null 2>&1 && [ -n "${RUNNER_TEMP:-}" ]; then
    RUNNER_TEMP=$(cygpath -u "$RUNNER_TEMP")
fi
