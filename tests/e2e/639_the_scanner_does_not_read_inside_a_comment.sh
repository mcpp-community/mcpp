#!/usr/bin/env bash
# requires: gcc
# 639_the_scanner_does_not_read_inside_a_comment.sh — comment bodies, comment
# tails and raw-string bodies are not code, and the scanner decides which of
# the three a line is in by whichever opener came first.
#
# THE DEFECT WAS BIDIRECTIONAL, and the reported direction was the cheaper one.
#
# The line loop had two passes -- raw strings, then an unconditional
# `find("//")` -- and no block-comment state at any point in the file's history.
# So a block comment whose opener sits on its own line puts the keyword at the
# start of the next line, and the matcher fires inside a comment:
#
#   /*
#     module (exe)              -> error: '(exe)' is not a module name
#   */
#
# With a WELL-FORMED name that refusal does not fire and the result is worse.
# `export module y;` inside a block comment made a plain `.cpp` the recorded
# producer of `gcm.cache/y.gcm`, a BMI the compiler never writes -- and a file
# that legitimately imports `y` was then told `imports must be built before
# being imported`, an ordering problem that does not exist, while the real
# provider was never searched for.
#
# And in the OTHER direction, a commented-out raw-string opener put the
# raw-string pass into a state it could not leave, blanking every following
# line until a `)"` that never comes:
#
#   // R"(
#   import x;                   -> invisible to the scanner, visible to gcc
#
# That one is a MISSING DEPENDENCY EDGE: the compile is not ordered after the
# BMI it needs, so it fails under parallelism and passes on a retry. The
# reported form is at least deterministic.
#
# CASES 4 AND 5 ARE WHY THIS IS A FIX AND NOT A MUTE. `/* */ import x;` must
# still record the import, which "skip any line starting with /*" would fail;
# and `"a /* b"` must not open a comment, which "treat every /* as an opener"
# would fail. Both were measured before the fix: the first was already broken
# (a fourth wrong answer the issue did not report), the second was correct only
# because ordinary strings were left alone by a line-start matcher.
set -e

t=$(mktemp -d); trap 'rm -rf "$t"' EXIT

# `imported but not provided` is emitted by mcpp's own scanner and by nothing
# else, so it separates "the scanner saw the import" from "the compiler did".
# The module never exists, so the build always fails; what is asserted is the
# WARNING, not the exit code.
scanner_sees_import() {   # $1 = source text
    local d="$t/$RANDOM$RANDOM"
    mkdir -p "$d/src"
    printf '[package]\nname = "s"\nversion = "0.1.0"\n' > "$d/mcpp.toml"
    printf '%b' "$1" > "$d/src/main.cpp"
    ( cd "$d" && "$MCPP" build 2>&1 ) | grep -q "imported but not provided"
}

fail=0
expect_seen() {
    if scanner_sees_import "$2"; then echo "  ok: $1"
    else echo "FAIL: $1 — the scanner did not record the import"; fail=1; fi
}
expect_unseen() {
    if scanner_sees_import "$2"; then
        echo "FAIL: $1 — the scanner recorded an import that is not code"; fail=1
    else echo "  ok: $1"; fi
}

echo "== 639: what is code, and what only looks like it =="

# 1. The reported case. A malformed module name inside a comment is not a
#    scanner error, and the criterion here is the ERROR -- not the warning the
#    other cases use, because there is no import in this file to be seen or
#    missed. The fixture is the four lines from the issue, verbatim.
d="$t/refusal"; mkdir -p "$d/src"
printf '[package]\nname = "r"\nversion = "0.1.0"\n' > "$d/mcpp.toml"
printf '/*\n  module (exe)\n*/\nint main() { return 0; }\n' > "$d/src/main.cpp"
if out=$( cd "$d" && "$MCPP" build 2>&1 ) && ! grep -q "scanner errors" <<<"$out"; then
    echo "  ok: a block comment's body is not a module declaration"
else
    echo "FAIL: the four-line file from the issue was refused:"
    grep -m2 -A1 "scanner errors" <<<"$out" | sed 's/^/    /'
    fail=1
fi

# 2. The same shape with a name that PARSES, so the refusal above cannot fire
#    and an import inside a comment is recorded instead.
expect_unseen "a block comment's body is not an import" \
    '/*\n  import x;\n*/\nint main() { return 0; }\n'

# 3. The other direction: a commented-out raw-string opener must not swallow
#    the code after it.
expect_seen "a // comment does not open a raw string" \
    '// R"(\nimport x;\nint main() { return 0; }\n'
expect_seen "a block comment does not open a raw string" \
    '/*\n  R"(\n*/\nimport x;\nint main() { return 0; }\n'

# 4. A closed block comment leaves the rest of its line as code.
expect_seen "code after a closed block comment is still code" \
    '/* */ import x;\nint main() { return 0; }\n'

# 5. An ordinary string is not a comment opener.
expect_seen "a /* inside a string literal opens nothing" \
    'const char* s = "a /* b";\nimport x;\nint main() { return 0; }\n'

# 6. A raw string still hides what it contains -- the property the raw-string
#    pass existed for, which this must not have cost.
expect_unseen "a raw-string body is still not code" \
    'const char* s = R"(\nimport x;\n)";\nint main() { return 0; }\n'

# 7. THE GRAPH, not the warning. The phantom producer is the expensive form of
#    this defect, and its criterion is an edge that must not exist: the
#    generated ninja file must name no BMI output for a module that only a
#    comment mentions. Asserted on the graph rather than on the build result,
#    because the build SUCCEEDED while the graph was wrong.
d="$t/graph"; mkdir -p "$d/src"
printf '[package]\nname = "g"\nversion = "0.1.0"\n' > "$d/mcpp.toml"
printf '/*\n  export module y;\n*/\nint main() { return 0; }\n' > "$d/src/main.cpp"
( cd "$d" && "$MCPP" build >/dev/null 2>&1 ) || true
graph=$(find "$d/target" -name build.ninja | head -1)
if [ -z "$graph" ]; then
    echo "FAIL: no build.ninja was generated"; fail=1
elif grep -q "y\.gcm\|y\.pcm\|y\.ifc" "$graph"; then
    echo "FAIL: the graph names a BMI for 'y', which only a comment mentions:"
    grep -n "y\.gcm\|y\.pcm\|y\.ifc" "$graph" | head -3 | sed 's/^/    /'
    fail=1
else
    echo "  ok: no BMI edge for a module named only inside a comment"
fi

if [ "$fail" -ne 0 ]; then echo "FAIL: 639"; exit 1; fi
echo "PASS: 639"
