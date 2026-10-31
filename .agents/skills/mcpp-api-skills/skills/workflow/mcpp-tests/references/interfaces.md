# Tests Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp test` | command | Build + run all tests/**/*.cpp (after `--`, args go to each test binary) |
| `mcpp test --accel` | value | Device backends and architectures (same axis as `mcpp build --accel`) |
| `mcpp test --archive` | value | With --no-run: write the built tests to FILE. Without: run the tests FILE holds, building nothing (in a checkout of the same project) |
| `mcpp test --build-timeout` | value | Kill a compile/link drive still running after SECS seconds (default 0 = no limit; POSIX only) |
| `mcpp test --cache` | value | Global dependency cache: global (default) \| local \| off |
| `mcpp test --cap` | value | Pin capability providers (e.g. blas=openblas,lapack=mkl) |
| `mcpp test --exclude` | value; repeatable | With --workspace (or at a virtual workspace root), leave the named member out; repeatable, refused with -p |
| `mcpp test --features` | value | Activate root-package features, and a dependency's as <dependency>/<feature>, for the test build (comma-separated) |
| `mcpp test --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp test --list` | flag | List (filtered) tests without building or running them |
| `mcpp test --message-format` | value | Output format: human (default) \| json (NDJSON, one record per test) |
| `mcpp test --no-accel` | flag | Test the variant built for no accelerator (same as `mcpp build --no-accel`) |
| `mcpp test --no-cache` | flag | Deprecated alias for --cache=off (also clears the build dir) |
| `mcpp test --no-run` | flag | Build the tests for --target and stop; a build that succeeds is the result (for a target this host cannot execute) |
| `mcpp test --no-runner` | flag | Run test binaries directly, ignoring any [target.<triple>].runner (a host that runs them natively) |
| `mcpp test --package` (`-p`) | value; repeatable | Run the tests of the named workspace member (namespace.name or package name, then directory); repeat to test several |
| `mcpp test --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp test --play-game` | flag | Play a game in the status row while it builds: --play-game=snake\|stack\|runner, or one at random |
| `mcpp test --print-launch` | flag | Build the one test PATTERN names, then print how it would be started (the mcpp.launch envelope, as `mcpp run --print-launch` prints) instead of running it; arguments after `--` are in its argv |
| `mcpp test --profile` | value | Build profile for the test build: dev (default) \| release \| dist \| <[profile.*] name> |
| `mcpp test --strict` | flag | Treat manifest schema warnings (unknown feature/platform) as errors |
| `mcpp test --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp test --target` | value; repeatable | Cross target triple or target set (same axis as `mcpp build --target`); repeatable: every leg is built, and run where this host reaches it |
| `mcpp test --timeout` | value | Kill a test still RUNNING after SECS seconds (default 300; 0 = no limit) |
| `mcpp test --toolchain` | value | Build the tests with this toolchain for one invocation, e.g. llvm@22.1.8 |
| `mcpp test --workspace` | flag | Run tests for all workspace members |
| `mcpp test --workspace-timeout` | value | Start no further member's tests after SECS seconds since the command began, and report what did not run (default 0 = no limit); the build is bounded by --build-timeout |
| `mcpp test <pattern>` | optional argument | Run only tests whose name contains PATTERN (optional) |

## Manifest fields

| Interface | Details |
| --- | --- |
| `test` | Settings applied to every discovered test program in this package. |
| `test.discover` | Array of nonempty glob strings. Default: `["tests/**/*.cpp"]`. A leading `!` excludes matching files; `[]` discovers none. Each matched file is one test program. |
| `test.main` | Table containing only `from = "dependency/entry"`; selects an entry target for every test. Discovered files become ordinary sources and do not define their own main. |
| `test.start` | String naming the image entry symbol for each test on a freestanding target. |
| `test.windows_code_page` | String selecting the Windows executable code page, using target-setting values such as `"utf-8"`. Unset uses the system code-page policy. |
| `test.windows_long_paths` | Boolean selecting the long-path declaration for each test executable on Windows. |

## Discovery rules

Patterns are resolved from the package root. Exclusions remove matches from the
whole selected set, not only an adjacent include pattern. Names are relative
to the fixed prefix of the first matching include pattern, without extensions.
Two files producing the same name are refused.

A support translation unit without a main is not an independent test entry
unless `[test] main` supplies one. Use a narrower discovery pattern rather than
removing that source from the project. Native tests have no `should_fail`
registration; external termination checks are separate harness results.

## Source

- [modules/manifest/src/key_registry.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/key_registry.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)

- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
