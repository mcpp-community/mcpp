---
name: mcpp-tests
description: Discover, select, build and run mcpp tests, configure test policy, export test launches or archives, and interpret failures and skipped capabilities.
---

# Testing a Project

## Test programs and discovery

mcpp compiles every discovered file into a separate test program. Each program
normally defines its own `main` and passes by exiting zero. Test-only packages
belong in `[dev-dependencies]`.

The default discovery pattern is `tests/**/*.cpp`. For an existing project with
benchmarks, support sources or vendored code under `tests/`, select the actual
test entry files explicitly:

```toml
[test]
discover = ["tests/test_*.cpp"]
```

For a recursive suite, exclusions remove matching files regardless of which
positive glob found them:

```toml
[test]
discover = ["tests/**/*.cpp", "!tests/support/**", "!tests/third_party/**"]
```

`discover = []` disables discovery. A test's name is its path relative to the
fixed prefix of the first matching glob, without the extension; duplicate names
are refused. Discovery does not inspect whether a matching source defines main.
Keep non-entry support files out of the discovered set. A multi-source suite
can be a workspace member with its own sources and test entry.

```sh
mcpp test --list
mcpp test smoke
mcpp test smoke -- --framework-argument
mcpp test --no-run
```

Check the list against the intended test entries before running it. Success
with no discovered tests does not establish coverage.

When a framework provides the entry point, `[test] main` selects its declared
entry target as `{ from = "dependency/entry" }`; the discovered files then do
not define their own main. The local reference lists this and the per-test
Windows settings.

## Select and bound

- The positional pattern is a name filter, not an arbitrary shell glob.
- `-p` / `--workspace` select member graphs; inspect expected discovery counts.
- `--timeout` bounds an executing test (default 300 seconds; zero disables it).
- `--build-timeout` bounds the compiler/link drive (POSIX only; default zero).
- `--workspace-timeout` stops starting further members after the budget.
- `--features`, `--profile`, `--cap` and `--target` select the tested variant.

When a target cannot execute here, `--no-run` verifies compilation only. A
runner can make a target reachable; `--no-runner` does not emulate it.
Record unavailable capability providers and distinguish skips from passes.

## Program integration

`--message-format json` is per-test NDJSON; it is not the final-build-envelope
format. `test <pattern> --print-launch` builds one selected test and emits
`mcpp.launch` without running it. It requires one member and exactly one match;
do not combine it with `--no-run`, `--list`, `--archive` or JSON test reporting.
It also requires a single target leg, not an expanded multi-target selection.

With `--no-run --archive FILE`, archive the built tests. With `--archive FILE`
alone, run archived tests without building, in a checkout of the same project.

## Expected termination

Native test registration has no `should_fail` option: a nonzero test exit is a
failure. For death tests, use a framework that runs the case in a child process,
or an external harness that validates the expected termination status.

An external harness can obtain argv, cwd and environment changes through
`mcpp test <pattern> --print-launch`, then spawn argv directly with that cwd
and an environment overlay. Establish the expected status for the actual
platform, CRT and runner. Check normal execution and argument handling as
controls; startup errors, timeouts and unrelated nonzero exits are not expected
termination. Report native test results separately from external harness cases.

## Validate changes

Assert discovery, process exit, expected output and relevant failure behavior.
A failing test maps to mcpp test failure, rather than exposing an arbitrary
framework exit code as the mcpp contract. Windows code-page test settings are
distinct from executable target settings. Use `[test] windows_code_page` where
the test itself must run under that declared policy.

## References

[Interface reference](references/interfaces.md).

- [08-testing.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/08-testing.md)
- [50-machine-output.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/50-machine-output.md)
