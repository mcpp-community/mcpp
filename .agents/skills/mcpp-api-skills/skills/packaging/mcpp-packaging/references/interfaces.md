# Packaging Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp pack` | command | Build + package: a program becomes a self-contained bundle, a library an interface + prebuilt binaries |
| `mcpp pack --compress` | value | The container's compression: gzip (tar default) \| zstd \| xz \| none; a zip deflates unless none |
| `mcpp pack --debug-symbols` | value | Write the separated *.debug files here (default: discard) |
| `mcpp pack --dev` | flag | Shorthand for --profile dev |
| `mcpp pack --exclude` | value; repeatable | With --workspace, leave the named member out; repeatable, refused with -p |
| `mcpp pack --features` | value | Activate root-package features, and a dependency's as <dependency>/<feature>, for every build pass of the pack (comma-separated) |
| `mcpp pack --format` | value | Earlier spelling of --pkg-format |
| `mcpp pack --host-platform` | value | The host graph's platform for this invocation (= MCPP_HOST_PLATFORM) |
| `mcpp pack --linkage` | value | The program's libraries in it (static) or beside it (shared); rebuilds with [build] dependency_linkage (default: as the project builds) |
| `mcpp pack --list-formats` | flag | Print the formats --format accepts here, the engine's and those the resolved graph provides, as the mcpp.pack.formats envelope |
| `mcpp pack --merge` | value; repeatable | Merge packages this command wrote (legs, systems) into one; repeatable, nothing is built |
| `mcpp pack --message-format` | value | Output format: human (default) \| json (one mcpp.pack envelope on stdout; narration on stderr) |
| `mcpp pack --mode` | value | What the machine that runs it provides: system \| vendored (default) \| self-contained \| static (= --linkage static --mode self-contained) |
| `mcpp pack --no-strip` | flag | Ship the artifacts as built (default: strip debug info) |
| `mcpp pack --no-universal` | flag | Deprecated: a pack of one target packs that target alone |
| `mcpp pack --output` (`-o`) | value | Override output path; a directory (ending in /) when the pack writes several files |
| `mcpp pack --package` (`-p`) | value; repeatable | Pack the named workspace member (namespace.name or package name, then directory), as if run in its directory; repeat to pack several, planned and built once |
| `mcpp pack --pkg-format` | value | The package format: tar \| zip \| dir \| single-file \| polyglot \| any format the resolved graph provides (e.g. appimage, msi, okb), or a list of containers of one tree (tar,okb); default: zip for a Windows program, tar elsewhere |
| `mcpp pack --platform` | value | The target's platform for this invocation (= MCPP_PLATFORM): <name>@<version>, path:<dir>, none |
| `mcpp pack --profile` | value | Build profile (default: [build] default-profile, else release) |
| `mcpp pack --release` | flag | Shorthand for --profile release |
| `mcpp pack --sysroot` | value | The target's base for this invocation (= MCPP_SYSROOT): a package, glibc@system, path:<dir>, none |
| `mcpp pack --systems` | value; repeatable | The runners a self-contained package carries, or the systems single files are made for: <os>, <os>-<arch>, all (default: this machine's) |
| `mcpp pack --target` | value; repeatable | A target (x86_64-linux-gnu) or a target set (openkal, linux-musl); repeatable: the legs |
| `mcpp pack --toolchain` | value | Build with this toolchain for one invocation, e.g. llvm@22.1.8 |
| `mcpp pack --workspace` | flag | Pack every workspace member that has a program target, planned and built once |
| `mcpp pack <target>` | optional argument | Target name from [targets.*] (optional) |

## Manifest fields

| Interface | Details |
| --- | --- |
| `pack.bundle-project.also_skip` | Array of strings. |
| `pack.bundle-project.force_bundle` | Array of strings. |
| `pack.compress` | String. |
| `pack.debug_symbols` | String. |
| `pack.default_mode` | String. |
| `pack.exclude` | Array of strings. |
| `pack.format` | Array of strings. String. |
| `pack.include` | Array of strings. |
| `pack.linkage` | String. |
| `pack.sign` | Array of strings. |
| `pack.strip` | Boolean. |
| `pack.systems` | Array of strings. String. |
| `pack.targets` | Array of strings. String. |
| `pack.universal` | Boolean. |

## Source

- [modules/manifest/src/toml.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/modules/manifest/src/toml.cppm)
- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
