#!/usr/bin/env bash
# requires: gcc unix-shell
# 896 -- an action's output is written by the action and by nothing else
# (D6, design 2026-10-10 §4; #778).
#
# Prepare used to write an empty placeholder for every generated translation
# unit, so the scan had a file to read. The placeholder was newer than the
# action's inputs: whenever a ninja log already held the action, ninja took
# the placeholder for the generated file, compiled it empty, and the link
# failed with `undefined reference to resource()` (#778).
#
#   O1  `--configure-only` leaves no file where the action will write.
#   O2  A generated file removed after a build is generated again, not
#       replaced by an empty one.
#   O3  A generated file that exists (here a legitimately empty one written by
#       the action) is not rewritten by prepare.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/ws/app" && cd "$TMP/ws"
printf '[workspace]\nmembers = ["app"]\n' > mcpp.toml
cat > app/mcpp.toml <<'TOML'
[package]
name = "app"
version = "0.0.1"
[build]
sources = []
[targets.app]
kind = "bin"
main = "main.cpp"
TOML
printf 'extern const char* resource();\nint main() { return resource()[0] == %sR%s ? 0 : 1; }\n' "'" "'" > app/main.cpp
printf '#!/bin/sh\nprintf %sconst char* resource() { return "RESOURCE"; }\\n%s > "$1"\nprintf "" > "$2"\n' "'" "'" > app/gen.sh
cat > app/build.mcpp <<'CPP'
import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = std::filesystem::path(mcpp::out_dir());
    const auto gen = (root / "gen.sh").generic_string();
    const auto cpp = (out / "generated.cpp").generic_string();
    const auto empty = (out / "empty.cpp").generic_string();
    mcpp::action a;
    a.id = "generate:resource";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(cpp.c_str()).arg(empty.c_str())
     .input(gen.c_str()).output(cpp.c_str()).output(empty.c_str()).submit();
    return 0;
}
CPP

# ── O1 ────────────────────────────────────────────────────────────────────
"$MCPP" build -p app --configure-only > configure.log 2>&1 || fail "configure-only" configure.log
if find . -name generated.cpp | grep -q .; then
    fail "prepare wrote the action's output: $(find . -name generated.cpp)" configure.log
fi
echo "ok O1"

# ── O2 ────────────────────────────────────────────────────────────────────
"$MCPP" build -p app > b1.log 2>&1 || fail "first build" b1.log
gen=$(find . -path '*/out/*' -name generated.cpp | head -1)
[ -s "$gen" ] || fail "the action did not write its output" b1.log
rm -f "$gen"
"$MCPP" build -p app > b2.log 2>&1 || fail "the build after the generated file was removed failed (#778)" b2.log
[ -s "$gen" ] || fail "the generated file was not generated again" b2.log
echo "ok O2"

# ── O3 ────────────────────────────────────────────────────────────────────
empty=$(find . -path '*/out/*' -name empty.cpp | head -1)
[ -f "$empty" ] && [ ! -s "$empty" ] || fail "the action's empty output is missing or not empty" b2.log
before=$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "$empty")
sleep 1
"$MCPP" build -p app > b3.log 2>&1 || fail "the third build" b3.log
after=$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "$empty")
[ "$before" = "$after" ] || fail "an existing output was rewritten by prepare" b3.log
echo "ok O3"

echo "PASS: 896_an_action_output_has_one_writer"
