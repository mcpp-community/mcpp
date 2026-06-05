# 02 — Packaging for Release

> The binary produced by `mcpp build` only runs on the local machine —— both
> the loader and the RUNPATH point into `~/.mcpp/`. To distribute it to other
> machines or deploy it to a server, use `mcpp pack` to produce a
> self-contained tarball.

## Three Modes

| Mode | Description | Size Increase | Compatibility |
|---|---|---|---|
| `static` | Fully static via musl, no runtime dependencies | +5–10 MB | Any Linux x86_64 |
| `bundle-project` (default) | Bundles only the project's third-party `.so` files | +a few MB | Mainstream distros (Ubuntu 22+, Debian 12+, RHEL 9+, etc.) |
| `bundle-all` | Includes ld-linux, libc, libstdc++ and the project's `.so` files | +30–50 MB | Any Linux, including older versions |

How to choose:

- Command-line tools, or targets like Docker scratch or Alpine minimal images → `static`
- Desktop or server releases targeting mainstream Linux distros → `bundle-project` (default)
- Environments that need compatibility with older glibc versions such as legacy CentOS or Kylin → `bundle-all`

## Commands

```bash
mcpp pack                          # bundle-project by default
mcpp pack --mode static
mcpp pack --mode bundle-all
mcpp pack --target x86_64-linux-musl   # equivalent to --mode static
mcpp pack --format dir                 # output as a directory, no tarball
mcpp pack -o myapp.tar.gz              # filename only: lands at target/dist/myapp.tar.gz
mcpp pack -o /abs/path/myapp.tar.gz    # includes a directory: output to the literal path
```

When `-o` is given a bare filename, the output is placed under `target/dist/`;
when it includes a directory (relative or absolute), the literal path is used.

For the full set of options, see `mcpp pack --help`.

## Output Layout

The tarball contents are wrapped in a single top-level directory whose name
matches the tarball filename (minus the `.tar.gz`) —— this way both a GUI
"right-click extract" and a command-line `tar -xzf` yield the same
self-contained directory, instead of scattering the contents across the current
path.

### Mode `static`

```
target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz
└── myapp-0.1.0-x86_64-linux-musl-static/
    ├── bin/myapp                ← fully static ELF (no PT_INTERP / RUNPATH)
    ├── myapp                    ← top-level entry point (thin shell wrapper, run ./myapp directly)
    ├── README.md                ← copied automatically from the project root
    └── LICENSE
```

### Mode `bundle-project` (default)

```
target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu/
    ├── bin/myapp                ← dynamically linked, RUNPATH=$ORIGIN/../lib
    │                                PT_INTERP=/lib64/ld-linux-x86-64.so.2
    ├── lib/
    │   ├── libcurl.so.4         ← project third-party dependency
    │   ├── libssl.so.3
    │   └── ...
    ├── myapp                    ← top-level entry point
    ├── README.md
    └── LICENSE
```

The skip list follows
[PEP 600 / manylinux2014](https://peps.python.org/pep-0600/) ——
base libraries such as `libc`, `libm`, `libstdc++`, `libgcc_s`, and
`ld-linux-*` are assumed to already exist on the target system and are not
bundled into the tarball.

### Mode `bundle-all`

```
target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu-bundle-all/
    ├── bin/myapp
    ├── lib/
    │   ├── ld-linux-x86-64.so.2  ← complete loader and libc
    │   ├── libc.so.6
    │   ├── libstdc++.so.6
    │   ├── libgcc_s.so.1
    │   └── ...project dependencies
    ├── myapp                     ← one of two entry points
    ├── run.sh                    ← the other entry point (identical contents)
    ├── README.md
    └── LICENSE
```

With `-o foo.tar.gz`, the top-level directory name also becomes `foo` (the
package name and directory name always stay in sync).

The ELF specification forbids `PT_INTERP` from using `$ORIGIN`, so in
bundle-all mode the loader is invoked by absolute path through `run.sh` (and
the top-level wrapper of the same name):

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

## Configuration

Packaging behavior is configured via the `[pack]` section in `mcpp.toml`. The
common fields are:

```toml
[pack]
default_mode = "static"             # default mode when --mode is omitted
include      = ["share/**", "config/*.toml"]   # extra files to bundle
exclude      = ["debug/**"]

# Fine-tune the bundle-project filtering policy
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # libraries assumed to exist on the target system
force_bundle = ["libfoo.so"]        # bundle even if matched by the PEP 600 list
```

The `static` mode additionally requires a musl toolchain configured under
`[target.<triple>]`; for the full setup, see the `mcpp.toml` in
[`examples/03-pack-static`](../examples/03-pack-static/).

## Planned Support

macOS dylib, Windows DLL, and distribution formats such as `.deb` / `.rpm` /
AppImage are still on the roadmap. This document evolves alongside the
`mcpp pack` implementation; for the latest options, refer to
`mcpp pack --help`.
