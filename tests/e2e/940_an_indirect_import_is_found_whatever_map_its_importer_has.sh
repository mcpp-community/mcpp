#!/usr/bin/env bash
# requires:
# 940 -- an indirect import is found whatever module map its importer has.
#
# A unit of the root package is given a module map only when it reaches a
# module of another package (pack drive and selection design 2026-10-01, B1);
# a unit that imports only the root's modules is compiled with the compiler's
# own mapper. GCC records in a BMI the path of each module that BMI imports, as
# its compile's mapper spelt it, and an importer finds an INDIRECT import at
# that recorded path below its own repository. Until 2026.10.26.2 a map named
# no repository, so a unit with a map that imported a root module compiled
# without one looked for that module's import beside the build directory:
#
#   mcppide.platform.os: error: failed to read compiled module: No such file
#   note: compiled module file is 'mcppide.platform.os.gcm'
#
# Criteria:
#   A. `app.os` (imports nothing), `app.process` (imports app.os; no map),
#      `app.jobs` (imports app.process and a path dependency's module; a map):
#      the build succeeds and the program runs.
#   B. The reverse: `app.view` (no map) imports `app.jobs` (a map), whose BMI
#      records its own imports; the build succeeds.
#   C. `app.compat` (a map) imports `std.compat`, whose BMI the compiler's own
#      mapper built and which records `std` below that mapper's repository;
#      the build succeeds. 2026.10.26.1 failed this in any project with a
#      dependency.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p dep/src app/src
cat > dep/mcpp.toml <<'TOML'
[package]
name    = "dep"
version = "0.1.0"

[targets.dep]
kind = "lib"
TOML
printf 'export module dep.lib;\nexport int dep_value() { return 2; }\n' > dep/src/lib.cppm

cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }
TOML
printf 'export module app.os;\nexport inline constexpr int os_value = 1;\n' > app/src/os.cppm
printf 'export module app.process;\nimport app.os;\nexport int process_value() { return os_value; }\n' > app/src/process.cppm
printf 'export module app.jobs;\nimport app.process;\nimport dep.lib;\nexport int jobs_value() { return process_value() + dep_value(); }\n' > app/src/jobs.cppm
printf 'export module app.view;\nimport app.jobs;\nexport int view_value() { return jobs_value() * 10; }\n' > app/src/view.cppm
printf 'export module app.compat;\nimport std.compat;\nimport dep.lib;\nexport int compat_value() { return static_cast<int>(::strlen("ab")) + dep_value(); }\n' > app/src/compat.cppm
printf '#include <cstdio>\nimport app.view;\nimport app.compat;\nint main() { std::printf("%%d\\n", view_value() + compat_value()); }\n' > app/src/main.cpp

cd app
"$MCPP" build > build.log 2>&1 || fail "a unit with a map could not read a root BMI's import" build.log
echo "ok: A, B, C the build succeeds"
out=$("$MCPP" run 2>run.log | tr -d '\r' | tail -1)
[ "$out" = "34" ] || fail "the program printed '$out', expected 34" run.log
echo "ok: the program runs"

echo "PASS: 940 an indirect import is found whatever map its importer has"
