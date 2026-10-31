# Build And Run Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp build` | command | Build the current package |
| `mcpp build --accel` | value | Device backends and architectures, e.g. 'cuda12.8+{sm_89}'; overrides [build] accel |
| `mcpp build --cache` | value | Global dependency cache: global (default) \| local \| off |
| `mcpp build --cap` | value | Pin capability providers (e.g. blas=openblas,lapack=mkl) |
| `mcpp build --configure-only` | flag | Generate compile_commands.json without compiling or linking |
| `mcpp build --dev` | flag | Shorthand for --profile dev (-O0 -g) |
| `mcpp build --exclude` | value; repeatable | With --workspace (or at a virtual workspace root), leave the named member out; repeatable, refused with -p |
| `mcpp build --features` | value | Activate root-package features, and a dependency's as <dependency>/<feature> (comma-separated) |
| `mcpp build --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp build --jobs` (`-j`) | value | Concurrent compiles: a number, or 'auto' to size from cores + free RAM |
| `mcpp build --message-format` | value | human (default) \| json: NDJSON events on stdout, then the mcpp.build envelope |
| `mcpp build --no-accel` | flag | Target no accelerator, ignoring [build] accel |
| `mcpp build --no-cache` | flag | Deprecated alias for --cache=off (also clears the build dir) |
| `mcpp build --package` (`-p`) | value; repeatable | Build the named workspace member (namespace.name or package name, then directory); repeat to build several |
| `mcpp build --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp build --play-game` | flag | Play a game in the status row while it builds: --play-game=snake\|stack\|runner, or one at random |
| `mcpp build --print-fingerprint` | flag | Show toolchain fingerprint and 11 inputs |
| `mcpp build --profile` | value | Build profile: dev (default) \| release \| dist \| <[profile.*] name> |
| `mcpp build --release` | flag | Shorthand for --profile release |
| `mcpp build --static` | flag | Force static linking (-static). On Linux, prefer pairing with --target <arch>-linux-musl |
| `mcpp build --strict` | flag | Treat manifest schema warnings (unknown feature/platform) as errors |
| `mcpp build --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp build --target` | value; repeatable | Build for <triple> (e.g. x86_64-linux-musl), or for a target set (e.g. openkal, linux-musl); repeatable: one leg per target |
| `mcpp build --toolchain` | value | Build with this toolchain for one build, e.g. llvm@22.1.8 |
| `mcpp build --workspace` | flag | Build all workspace members |
| `mcpp run` | command | Build + run a binary target (after `--`, args are passed to it) |
| `mcpp run --accel` | value | Device backends and architectures (same axis as `mcpp build --accel`) |
| `mcpp run --cache` | value | Global dependency cache: global (default) \| local \| off |
| `mcpp run --dev` | flag | Shorthand for --profile dev |
| `mcpp run --features` | value | Activate features (comma/space separated), same axis as `mcpp build --features` |
| `mcpp run --format` | value | Earlier spelling of --pkg-format |
| `mcpp run --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp run --list-runners` | flag | List the named runners this project supplies, and exit |
| `mcpp run --no-accel` | flag | Run the variant built for no accelerator (same as `mcpp build --no-accel`) |
| `mcpp run --no-cache` | flag | Deprecated alias for --cache=off (also clears the build dir) |
| `mcpp run --no-runner` | flag | Execute the artifact directly, ignoring any [target.<triple>].runner (a host that runs it natively) |
| `mcpp run --package` (`-p`) | value; repeatable | Run only the named workspace member (namespace.name or package name, then directory; one member: a second -p is refused) |
| `mcpp run --pkg-format` | value | Run the distributable a `--pkg-format <name>` pack would produce (same values as `mcpp pack --pkg-format`); refused together with --no-runner |
| `mcpp run --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp run --play-game` | flag | Play a game in the status row while it builds: --play-game=snake\|stack\|runner, or one at random |
| `mcpp run --print-launch` | flag | Build, then print how the program would be started (argv, the environment mcpp adds, cwd, runner) as the mcpp.launch envelope, instead of starting it |
| `mcpp run --profile` | value | Build profile to run (dev \| release \| <custom>) |
| `mcpp run --release` | flag | Shorthand for --profile release |
| `mcpp run --runner` | value | Reach the artifact by a named runner a package supplied; see --list-runners |
| `mcpp run --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp run --target` | value; repeatable | Cross target triple or target set (same axis as `mcpp build --target`); of several, the first this host reaches, its own architecture first |
| `mcpp run --target-triple` | value | Alias for --target |
| `mcpp run --toolchain` | value | Build with this toolchain for one invocation, e.g. llvm@22.1.8 |
| `mcpp run <bin>` | optional argument | Binary name (optional) |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
