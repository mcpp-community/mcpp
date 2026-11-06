#!/usr/bin/env bash
# A PACKAGE AND A NAMESPACED PACKAGE OF ONE NAME BOTH LINK (mcpp#896).
# `foo` and `ns.foo` are two packages. The plan found a package by its bare
# name too, and the bare `foo` of `ns.foo` replaced `foo`'s, so a member that
# depended on both reached `ns.foo` twice and its link left `foo`'s objects
# out (`undefined reference to foo::one`). Each package is now reached by the
# edge that names it.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

cd "$TMP"
mkdir -p foo/src nsfoo/src app/src
printf '[workspace]\nmembers = ["foo", "nsfoo", "app"]\n' > mcpp.toml
printf '[package]\nname = "foo"\nversion = "0.1.0"\n[targets.foo]\nkind = "lib"\n[build]\nsources = ["src/*.cppm", "src/*.cpp"]\n' > foo/mcpp.toml
printf '[package]\nnamespace = "ns"\nname = "foo"\nversion = "0.1.0"\n[targets.ns-foo]\nkind = "lib"\n[build]\nsources = ["src/*.cppm", "src/*.cpp"]\n' > nsfoo/mcpp.toml
printf '[package]\nname = "app"\nversion = "0.1.0"\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n[dependencies]\nfoo = { path = "../foo" }\nns.foo = { path = "../nsfoo" }\n' > app/mcpp.toml
printf 'export module foo;\nexport namespace foo { int one(); }\n' > foo/src/foo.cppm
printf 'module foo;\nnamespace foo { int one() { return 1; } }\n' > foo/src/foo.cpp
printf 'export module ns.foo;\nexport namespace ns::foo { int two(); }\n' > nsfoo/src/foo.cppm
printf 'module ns.foo;\nnamespace ns::foo { int two() { return 2; } }\n' > nsfoo/src/foo.cpp
printf 'import std;\nimport foo;\nimport ns.foo;\nint main() { std::println("sum {}", foo::one() + ns::foo::two()); }\n' > app/src/main.cpp

"$MCPP" run -p app > run.log 2>&1 || fail "the member reaching foo and ns.foo did not link and run" run.log
grep -q "sum 3" run.log || fail "the program's output" run.log
echo "PASS: 1018 a package and a namespaced package of one name both link"
