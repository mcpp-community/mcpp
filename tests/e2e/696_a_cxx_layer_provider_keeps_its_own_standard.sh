#!/usr/bin/env bash
# requires: llvm
# 696 -- a package that provides the C++ layer compiles its own implementation
# units at the standard it states, while every module unit, the std module
# included, stays at the graph's (#641 item 2).
#
# libc++ 22's sources are written for C++23 (`std::string::resize_and_overwrite`,
# `std::bad_expected_access`), so a c++20 graph over `llvm.libcxx` failed inside
# them. The package fixture is `llvm.libcxx` 22.1.8.2 with its manifest's
# `[build] cxx_standard` (a key no engine reads) restated as
# `[package] standard = "c++23"`. Criteria, read from `mcpp emit build-database`,
# whose arguments are the compile edge's:
#   A. c++20 root: `libcxx/src/new.cpp` ends on -std=c++23, `main.cpp` and the
#      std module carry -std=c++20 only; no "declares standard" warning names
#      the provider; the program builds and runs.
#   B. The unmodified manifest (no statement): `new.cpp` carries -std=c++20 only.
#   C. c++26 root: `new.cpp` ends on -std=c++23 (exactly the stated level, not
#      the higher graph level) and `main.cpp` carries -std=c++26; the program
#      builds and runs.
#   D. A path dependency that states c++23 and is not a C++-layer provider keeps
#      the graph's -std=c++20 and is still reported.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}

source "$(dirname "$0")/_host_path.sh"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# The -std= tokens of the translation unit whose source ends with $2, in
# argument order, from an S1 document. Keys are sorted, so a unit's
# "arguments" array precedes its "source".
std_tokens() {
    awk -v want="$2\"" '
        /^[ \t]*"arguments": \[/ { args = ""; inargs = 1; next }
        inargs && /^[ \t]*\],?[ \t]*$/ { inargs = 0; next }
        inargs {
            if (match($0, /"-std=[^"]*"/)) args = args " " substr($0, RSTART + 1, RLENGTH - 2)
            next
        }
        /^[ \t]*"source": / { if (index($0, want)) { print args; found = 1; exit } }
        END { if (!found) print "NOT-FOUND" }' "$1"
}

cd "$TMP"
git clone -q --depth 1 --branch 22.1.8.2 https://github.com/mcpplibs/libcxx.git upstream \
    2>/dev/null || fail "cannot clone mcpplibs/libcxx 22.1.8.2"
rm -rf upstream/.git
grep -q '^cxx_standard = "c++23"$' upstream/mcpp.toml \
    || fail "the fixture no longer has the line this test restates" upstream/mcpp.toml
cp -r upstream stated
awk '/^cxx_standard = "c\+\+23"$/ { next }
     { print }
     /^version     = "22\.1\.8\.2"$/ { print "standard    = \"c++23\"" }' \
    upstream/mcpp.toml > stated/mcpp.toml
grep -q '^standard    = "c++23"$' stated/mcpp.toml || fail "the restated manifest has no standard" stated/mcpp.toml

make_root() {   # $1 directory, $2 standard, $3 libcxx path
    local LIBCXX_HOST
    LIBCXX_HOST=$(host_path "$3")
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<TOML
[package]
name     = "app"
version  = "0.1.0"
standard = "$2"

[toolchain]
default = "llvm@22.1.8"

[dependencies.llvm.libcxx]
path = "$LIBCXX_HOST"
TOML
    cat > "$1/src/main.cpp" <<'CPP'
import std;
int main() {
    std::unordered_map<std::string, int> m;
    m["one"] = 1;
    try { throw std::runtime_error("x"); } catch (const std::runtime_error&) { m["two"] = 2; }
    std::cout << std::format("{}-{}", m["one"], m["two"]) << std::endl;
}
CPP
}

# --- A ----------------------------------------------------------------------
make_root a c++20 ../stated
( cd a && "$MCPP" emit build-database > ../a.json 2> ../a.err ) || fail "A: emit build-database failed" a.err
got=$(std_tokens a.json "llvm/libcxx/src/new.cpp")
[ "$got" = " -std=c++20 -std=c++23" ] || fail "A: new.cpp std tokens were '$got', expected ' -std=c++20 -std=c++23'" a.err
got=$(std_tokens a.json "a/src/main.cpp")
[ "$got" = " -std=c++20" ] || fail "A: main.cpp std tokens were '$got', expected ' -std=c++20'"
got=$(std_tokens a.json "stated/generated/std.cppm")
[ "$got" = " -std=c++20" ] || fail "A: the std module's std tokens were '$got', expected ' -std=c++20'"
grep -q 'declares standard' a.err && fail "A: the provider is reported as not applied" a.err
echo "ok: A the provider's implementation units are planned at c++23 under a c++20 graph"
( cd a && "$MCPP" build > ../a-build.log 2>&1 ) || fail "A: the c++20 build over the provider failed" a-build.log
out=$(a/target/*/*/bin/app) || fail "A: the program exited non-zero"
[ "$out" = "1-2" ] || fail "A: expected 1-2, got: $out"
echo "ok: A the c++20 program over the provider builds and runs"

# --- B ----------------------------------------------------------------------
make_root b c++20 ../upstream
( cd b && "$MCPP" emit build-database > ../b.json 2> ../b.err ) || fail "B: emit build-database failed" b.err
got=$(std_tokens b.json "llvm/libcxx/src/new.cpp")
[ "$got" = " -std=c++20" ] || fail "B: without a statement new.cpp std tokens were '$got', expected ' -std=c++20'"
echo "ok: B a provider that states nothing keeps the graph's level"

# --- C ----------------------------------------------------------------------
make_root c c++26 ../stated
( cd c && "$MCPP" emit build-database > ../c.json 2> ../c.err ) || fail "C: emit build-database failed" c.err
got=$(std_tokens c.json "llvm/libcxx/src/new.cpp")
[ "$got" = " -std=c++26 -std=c++23" ] || fail "C: new.cpp std tokens were '$got', expected ' -std=c++26 -std=c++23'"
got=$(std_tokens c.json "c/src/main.cpp")
[ "$got" = " -std=c++26" ] || fail "C: main.cpp std tokens were '$got', expected ' -std=c++26'"
echo "ok: C the provider keeps exactly its stated level under a higher graph level"
( cd c && "$MCPP" build > ../c-build.log 2>&1 ) || fail "C: the c++26 build over the provider failed" c-build.log
out=$(c/target/*/*/bin/app) || fail "C: the program exited non-zero"
[ "$out" = "1-2" ] || fail "C: expected 1-2, got: $out"
echo "ok: C the c++26 program over the provider builds and runs"

# --- D ----------------------------------------------------------------------
mkdir -p plain/src d/src
printf '[package]\nname     = "plain"\nversion  = "0.1.0"\nstandard = "c++23"\n\n[targets.plain]\nkind = "lib"\n' > plain/mcpp.toml
printf 'int plain_answer() { return 42; }\n' > plain/src/plain.cpp
printf '[package]\nname     = "d"\nversion  = "0.1.0"\nstandard = "c++20"\n\n[dependencies]\nplain = { path = "../plain" }\n' > d/mcpp.toml
printf 'int plain_answer();\nint main() { return plain_answer() == 42 ? 0 : 1; }\n' > d/src/main.cpp
( cd d && "$MCPP" emit build-database > ../d.json 2> ../d.err ) || fail "D: emit build-database failed" d.err
got=$(std_tokens d.json "plain/src/plain.cpp")
[ "$got" = " -std=c++20" ] || fail "D: an ordinary dependency's std tokens were '$got', expected ' -std=c++20'"
grep -q 'dependency `plain` declares standard = "c++23"' d.err \
    || fail "D: the ordinary dependency's higher standard is no longer reported" d.err
echo "ok: D an ordinary dependency keeps the one-standard rule and its warning"

echo "PASS: a C++-layer provider compiles its implementation units at its own standard"
