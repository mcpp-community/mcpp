#!/usr/bin/env bash
# requires: gcc elf
# 946 -- a program's beginning is a dependency's `kind = "entry"` target
# (design 2026-10-26 §24, mcpp#845).
#
# A framework supplies `main`; the application writes only the callback the
# framework declares. Before, `main` named a file of the program's own package,
# and every application carried the same three-line file.
#
# Criteria:
#   A. `main = { from = "fw/player" }` builds a program that begins in fw's
#      entry, compiled with fw's flags, and reaches the callback the program
#      defines.
#   B. The entry is in no other image: fw built as the root links its library,
#      and a second program with its own `main` over fw links.
#   C. An entry whose `required_features` are inactive is refused naming the
#      feature and the dependency line; with the feature it builds.
#   D. An unknown dependency key and a target that is not an entry are refused
#      with the candidates; flags for the program's own entry beside `from` are
#      refused.
#   E. `windows_entry` follows the entry, and a program stating another one is
#      refused naming both.
#   F. A row replaces the entry's file on the rows it matches.
#   G. `[test] main = { from }`: test files without `main`, the package's own
#      entry named by its own name.
#   H. `start` on a hosted target is refused, naming the C runtime.
#   I. The result record names where the program begins.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# fw: a library, an entry that calls the application's callback, and an entry
# that needs a feature.
mkdir -p "$TMP/fw/src/entry" "$TMP/fw/tests"
cat > "$TMP/fw/mcpp.toml" <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[features]
app = {}

[targets.fw]
kind = "lib"

[targets.player]
kind    = "entry"
main    = "src/entry/player.cpp"
defines = ["FW_PLAYER"]

[targets.app]
kind              = "entry"
main              = "src/entry/app.cpp"
required_features = ["app"]

[target.'cfg(os = "linux")'.targets.app]
main = "src/entry/app_linux.cpp"

[test]
main = { from = "fw/player" }
TOML
cat > "$TMP/fw/src/fw.cppm" <<'CPP'
export module fw;
import std;
// The callback an application defines. Declared with C++ language linkage at
// global scope: a declaration attached to this module could not be defined
// outside it.
export extern "C++" int fw_app();
export namespace fw {
int twice(int v) { return 2 * v; }
}
CPP
cat > "$TMP/fw/src/entry/player.cpp" <<'CPP'
import std;
import fw;
#ifndef FW_PLAYER
#error the entry is compiled with its own target's defines
#endif
int main() {
    std::println("player {}", fw::twice(fw_app()));
    return 0;
}
CPP
cat > "$TMP/fw/src/entry/app.cpp" <<'CPP'
import std;
import fw;
int main() { std::println("app {}", fw_app()); return 0; }
CPP
cat > "$TMP/fw/src/entry/app_linux.cpp" <<'CPP'
import std;
import fw;
int main() { std::println("app-linux {}", fw_app()); return 0; }
CPP
cat > "$TMP/fw/tests/callback.cpp" <<'CPP'
int fw_app() { return 21; }
CPP

# notes: the application, which writes only the callback.
mkdir -p "$TMP/notes/src"
cat > "$TMP/notes/src/app.cpp" <<'CPP'
int fw_app() { return 21; }
CPP
FW_DEP='fw = { path = "../fw" }'
notes() {   # notes <extra target keys> [dependency line]
    local dep="${2:-$FW_DEP}"
    cat > "$TMP/notes/mcpp.toml" <<TOML
[package]
name    = "notes"
version = "0.1.0"

[dependencies]
$dep

[targets.notes]
kind = "bin"
$1
TOML
}

cd "$TMP/notes"

# A
notes 'main = { from = "fw/player" }'
"$MCPP" build > a.log 2>&1 || fail "A: the program over fw's entry did not build" a.log
"$MCPP" run > a-run.log 2>&1 || fail "A: the program did not run" a-run.log
grep -q "player 42" a-run.log || fail "A: the program did not begin in fw's entry with its flags" a-run.log
echo "ok: a program begins in a dependency's entry and reaches its own callback"

# B
(cd "$TMP/fw" && "$MCPP" build > "$TMP/b-fw.log" 2>&1) || fail "B: fw as the root did not build" "$TMP/b-fw.log"
mkdir -p "$TMP/other/src"
cat > "$TMP/other/mcpp.toml" <<'TOML'
[package]
name    = "other"
version = "0.1.0"

[dependencies]
fw = { path = "../fw" }
TOML
printf 'import std;\nimport fw;\nint fw_app() { return 1; }\nint main() { std::println("own {}", fw::twice(3)); }\n' > "$TMP/other/src/main.cpp"
(cd "$TMP/other" && "$MCPP" run > "$TMP/b-other.log" 2>&1) || fail "B: a program with its own main over fw did not link" "$TMP/b-other.log"
grep -q "own 6" "$TMP/b-other.log" || fail "B: the program with its own main did not run" "$TMP/b-other.log"
echo "ok: the entry is in no image that does not select it"

# C
notes 'main = { from = "fw/app" }'
"$MCPP" build > c.log 2>&1 && fail "C: an entry with an inactive required feature was linked" c.log
grep -q 'requires the feature "app"' c.log || fail "C: the refusal does not name the feature" c.log
grep -q 'fw = { ..., features = \["app"\] }' c.log || fail "C: the refusal does not give the dependency line" c.log
notes 'main = { from = "fw/app" }' 'fw = { path = "../fw", features = ["app"] }'
"$MCPP" run > c-run.log 2>&1 || fail "C: the entry with its feature did not build" c-run.log
echo "ok: an entry's required features gate its selection"

# D
notes 'main = { from = "nope/player" }'
"$MCPP" build > d1.log 2>&1 && fail "D: an unknown dependency was accepted" d1.log
grep -q "'nope' is neither a dependency of 'notes' nor its own name (its dependencies: fw)" d1.log \
    || fail "D: the unknown dependency is not named with the candidates" d1.log
notes 'main = { from = "fw/fw" }'
"$MCPP" build > d2.log 2>&1 && fail "D: a library target was accepted as an entry" d2.log
grep -q "declares no entry target 'fw' (its \`kind = \"entry\"\` targets: app, player)" d2.log \
    || fail "D: the entry targets are not listed" d2.log
notes 'main = { from = "fw/player" }
defines = ["X=1"]'
"$MCPP" build > d3.log 2>&1 && fail "D: flags for the program's own entry were accepted beside from" d3.log
grep -q "targets.notes.defines applies to the target's own entry translation unit" d3.log \
    || fail "D: the flags refusal does not say why" d3.log
echo "ok: a selection that names nothing is refused with the candidates"

# E
sed -i 's/^defines = \["FW_PLAYER"\]/&\nwindows_entry = "main"/' "$TMP/fw/mcpp.toml"
notes 'main = { from = "fw/player" }
windows_entry = "wmain"'
"$MCPP" build > e.log 2>&1 && fail "E: two entry functions were accepted" e.log
grep -q 'windows_entry = "wmain", and its beginning .fw/player. defines `main`' e.log \
    || fail "E: the conflict does not name both statements" e.log
echo "ok: the entry function follows the entry"

# F
notes 'main = { from = "fw/app" }' 'fw = { path = "../fw", features = ["app"] }'
grep -q "app-linux 21" c-run.log || fail "F: the row did not replace the entry's file" c-run.log
echo "ok: a row replaces the entry's file"

# G
(cd "$TMP/fw" && "$MCPP" test > "$TMP/g.log" 2>&1) || fail "G: the test over the package's own entry failed" "$TMP/g.log"
grep -q "callback" "$TMP/g.log" || fail "G: the test did not run" "$TMP/g.log"
echo "ok: test programs begin in an entry named by the package's own name"

# H
notes 'main  = { from = "fw/player" }
start = "_start"'
"$MCPP" build > h.log 2>&1 && fail "H: start on a hosted target was accepted" h.log
grep -q "the image is entered by the C runtime" h.log || fail "H: the refusal does not name the C runtime" h.log
echo "ok: start on a hosted target is refused"

# I
notes 'main = { from = "fw/player" }'
"$MCPP" build --message-format json > i.json 2>/dev/null || fail "I: the json build failed" i.json
grep -q '"entry":{"from":"fw/player"}' i.json || fail "I: the result record does not name where the program begins" i.json
echo "ok: the result record names the program's beginning"

echo "PASS: 946 a program begins in a dependency entry"
