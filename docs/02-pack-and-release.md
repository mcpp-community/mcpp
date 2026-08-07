# 02 — Packaging for Release

> A default dynamically linked binary produced by `mcpp build` has a loader and
> RUNPATH tied to the build sandbox. It is a development artifact, not a
> deliverable. Three routes turn it into one — and none of them uses the host's
> C library.

## Three ways to ship

Every route below produces an artifact whose C runtime comes from the
ecosystem, never from `/lib64`. That is deliberate: mcpp builds against a
private glibc precisely so a binary's behaviour does not depend on which
distribution happens to be underneath it, and reaching back out to the host's
libc to distribute would give that away at the last step.

| | Route | Command | Where its C runtime comes from | Choose it when |
|---|---|---|---|---|
| **A** | Through the ecosystem | `mcpp emit xpkg` → `xlings install <pkg>` | the target machine's own xlings payloads | the target has xlings |
| **B** | One static file | `mcpp build --target x86_64-linux-musl` | nowhere — it is linked in | you want a single file with no runtime at all |
| **C** | Carry the runtime | `mcpp pack --mode self-contained` | shipped inside the bundle | any Linux, including older than the build machine |

**On route A, and the thing that surprises people.** The `PT_INTERP` baked into
a freshly built binary points at *your* machine's payload, so copying that file
to another machine by hand does not work — the path is not there. That is not a
property of the artifact so much as of the copy: installed through `xlings`, the
package's ELF files are repointed at the target machine's own payloads at
install time. The baked path is a build-machine detail, not a distribution
format. If you are hand-copying binaries between machines, you want B or C.

**On route B.** `--target …-musl` implies a static link, so there is no loader,
no RUNPATH and nothing to find at run time. It is the smallest and most
portable result, and the one to reach for first when the program does not need
glibc-specific behaviour (NSS lookups, `dlopen` of host plugins).

**On route C.** The bundle carries this toolchain's glibc and its loader, so it
runs on distributions older than the build machine — the case B cannot cover
when glibc is actually required. Read the `/proc/self/exe` note below before
choosing it: launching through a bundled loader changes what the program sees
about itself.

## Two axes: target (libc) × mode (bundling depth)

Distribution is two orthogonal choices:

- **libc / static** — a *build-target* property: `--target …-linux-gnu` (glibc)
  vs `--target …-linux-musl` (musl, static). `--target …-musl` implies `static`.
- **bundling depth** — a *pack* property: how much of the shared-lib closure
  travels with the artifact. This is what `--mode` selects.

| Mode | Host must provide | Size | Use case |
|---|---|---|---|
| `system` | every `.so` (incl. third-party) | smallest | `.deb`/`.rpm`, same-distro fleet (pkg manager declares deps) |
| `vendored` (default) | libc / libstdc++ / loader | +a few MB | Mainstream distros (Ubuntu 22+, Debian 12+, RHEL 9+) |
| `self-contained` | nothing | +30–50 MB | Any Linux incl. older glibc; bundles closure + `run.sh` wrapper |
| `static` | nothing (single file) | +5–10 MB | musl; matching Linux x86_64 or aarch64 host, Docker scratch, Alpine |

How to choose:

- Distro packages (`.deb`/`.rpm`) or same-distro internal deploy → `system`
- Desktop / server releases for mainstream distros → `vendored` (default)
- Cross-distro / older glibc (legacy CentOS, Kylin) → `self-contained`
- Single portable file, no host deps → `static`

### Mode name compatibility

Canonical names are shown above. The old names remain **permanent aliases**:
`bundle-project` = `vendored`, `bundle-all` = `self-contained`. Tarball-name
suffixes are a frozen wire format (consumed by `install.sh`) and do **not**
follow the rename: `vendored` → no suffix, `self-contained` → `-bundle-all`,
`static` → `-static`, `system` → `-system`.

## Commands

```bash
mcpp pack                          # vendored by default
mcpp pack --mode system
mcpp pack --mode static
mcpp pack --mode self-contained        # alias: --mode bundle-all
mcpp pack --target x86_64-linux-musl   # equivalent to --mode static
mcpp pack --target aarch64-linux-musl  # ARM64 equivalent
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

### Mode `vendored` (default; alias: `bundle-project`)

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

### Mode `self-contained` (alias: `bundle-all`)

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
`self-contained` mode the loader is invoked by absolute path through `run.sh` (and
the top-level wrapper of the same name):

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

The layout and wrapper above use an x86_64 example. The packer derives the
loader name from the target; for aarch64 it is `ld-linux-aarch64.so.1`.

#### Trap: `/proc/self/exe` under the bundled loader

Being started *by* the loader has a consequence the layout above does not
show: the kernel sets `/proc/self/exe` to the **loader**, not to your program,
and `/proc/self/cmdline` carries the `--library-path` argument. Every "find my
resources next to the executable" path therefore resolves against `lib/`
instead of the bundle root — and it does so silently. In practice that means
a GUI toolkit rendering blank text because it cannot find its fonts, an
`assets/` directory that appears to be missing, and helper binaries shipped
alongside the program that cannot be located. Code that parses `argv` from
`/proc/self/cmdline` sees the loader's arguments mixed in.

This affects `self-contained` only. `vendored`, `system` and `static` all
carry a `PT_INTERP` that the kernel can use directly, so `/proc/self/exe` is
correct there.

The wrapper exports **`MCPP_BUNDLE_DIR`** (the bundle root) for this. Resolve
against it first and fall back only when it is unset:

```c
const char *base = getenv("MCPP_BUNDLE_DIR");   /* set by run.sh */
if (!base) {
    /* not launched through the wrapper — /proc/self/exe is trustworthy */
}
```

If the application cannot be changed — a third-party GUI framework doing its
own resolution, say — use `--mode vendored` instead. It repoints `PT_INTERP`
at the host loader, at the cost of requiring the host's glibc to be at least
as new as the one the artifact was built against.

## Configuration

Packaging behavior is configured via the `[pack]` section in `mcpp.toml`. The
common fields are:

```toml
[pack]
default_mode = "static"             # override the normal vendored default for bare `mcpp pack`
include      = ["share/**", "config/*.toml"]   # extra files to bundle
exclude      = ["debug/**"]

# Fine-tune the vendored filtering policy. The configuration key keeps its
# established `bundle-project` spelling.
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # libraries assumed to exist on the target system
force_bundle = ["libfoo.so"]        # bundle even if matched by the PEP 600 list
```

`[pack].default_mode` currently accepts the established manifest spellings
`static`, `bundle-project`, and `bundle-all`; the `system` mode is selected
explicitly with `mcpp pack --mode system`. CLI input accepts both the canonical
and compatibility names described above.

The `static` mode additionally requires a musl toolchain configured under
`[target.<triple>]`; for the full setup, see the `mcpp.toml` in
[`examples/03-pack-static`](../examples/03-pack-static/).

## Planned Support

macOS dylib, Windows DLL, and distribution formats such as `.deb` / `.rpm` /
AppImage are still on the roadmap. This document evolves alongside the
`mcpp pack` implementation; for the latest options, refer to
`mcpp pack --help`.
