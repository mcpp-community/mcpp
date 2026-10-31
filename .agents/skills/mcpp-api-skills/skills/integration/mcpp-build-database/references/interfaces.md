# Build Database Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp emit build-database` | command | Print the build plan as an S1 build database, without writing into the project |
| `mcpp emit build-database --accel` | value | Device backends and architectures (same axis as `mcpp build --accel`) |
| `mcpp emit build-database --cap` | value | Pin capability providers (e.g. blas=openblas,lapack=mkl) |
| `mcpp emit build-database --dev` | flag | Shorthand for --profile dev |
| `mcpp emit build-database --exclude` | value; repeatable | With --workspace (or at a virtual workspace root), leave the named member out; repeatable, refused with -p |
| `mcpp emit build-database --features` | value | Activate root-package features, and a dependency's as <dependency>/<feature> (comma-separated) |
| `mcpp emit build-database --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp emit build-database --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp emit build-database --no-accel` | flag | Describe the variant built for no accelerator |
| `mcpp emit build-database --output` (`-o`) | value | Write to file instead of stdout |
| `mcpp emit build-database --package` (`-p`) | value; repeatable | Describe the named workspace member (namespace.name or package name, then directory); repeat to describe several |
| `mcpp emit build-database --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp emit build-database --profile` | value | Build profile: dev (default) \| release \| dist \| <[profile.*] name> |
| `mcpp emit build-database --release` | flag | Shorthand for --profile release |
| `mcpp emit build-database --spec` | value | Document specification: s1 (default; the S1 IDE profile of P2977R2) \| compile-commands |
| `mcpp emit build-database --static` | flag | Describe the build with --static |
| `mcpp emit build-database --strict` | flag | Treat manifest schema warnings (unknown feature/platform) as errors |
| `mcpp emit build-database --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp emit build-database --target` | value | Describe the build for <triple> (same axis as `mcpp build --target`) |
| `mcpp emit build-database --toolchain` | value | Describe the build with this toolchain, e.g. llvm@22.1.8 |
| `mcpp emit build-database --workspace` | flag | Describe all workspace members in one document |

## Output kinds

| Interface | Details |
| --- | --- |
| `mcpp.build-database` | data version=1 |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
- [src/wire.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/wire.cppm)
