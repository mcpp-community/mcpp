# 32 — Authoring a Payload

**Reader:** someone packaging a tool or a prebuilt library so that mcpp projects
can declare it and mcpp installs it.

**The question this chapter answers:** what is an `xim:` payload made of, and
what must its descriptor say for a consumer to name it and get a working
program.

**Not here:** publishing a **source package** others `import`, which is
[11 — Publishing a Library](11-publishing-a-library.md); making a **host**
library reachable from an artifact, which is
[33 — Authoring a Runtime Adapter](33-authoring-an-adapter.md); and consuming a
payload, which is [23 — The Project Environment](23-the-project-environment.md).

Before: [31 — Authoring a Rule Package](31-authoring-a-rule-package.md), whose
rules declare the payloads they drive. After:
[33 — Authoring a Runtime Adapter](33-authoring-an-adapter.md).

## The definition of a payload

Everything mcpp installs and does not compile: a compiler, a shader compiler, a
device toolkit, an emulator, a probe driver, a prebuilt C library. A project
names one in `[xlings.workspace]`, or a rule package names it in
`[feature-xlings.<f>]`, and mcpp provisions it before the build runs.

A payload lives in `xim-pkgindex` as one Lua file: a `package` table that
describes it, and two functions that place and register it.

The shortest one that works:

```lua
package = {
    spec = "2",
    name = "glslang",
    description = "Khronos reference GLSL/ESSL front end and validator",
    licenses = {"BSD-3-Clause", "Apache-2.0", "MIT"},
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            ["latest"]  = { ref = "15.1.0" },
            ["15.1.0"]  = {
                url = {
                    GLOBAL = "https://github.com/…/glslang-15.1.0-linux-x86_64.tar.gz",
                    CN     = "https://gitcode.com/…/glslang-15.1.0-linux-x86_64.tar.gz",
                },
                sha256 = "87167c9cb32f258addbedb607639b2c1f484c029ba91542a92f19ead21d65d13",
            },
        },
    },
}

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mv("glslang-15.1.0", dir)
    return true
end

function config()
    xvm.add(package.name)
    return true
end
```

`install()` places the extracted tree where mcpp will look for it; `config()`
registers what the payload offers. Everything else in this chapter is one of
those two doing more.

## The four things a descriptor must get right

**One version, two URLs.** Every version carries a `GLOBAL` and a `CN` URL and
one `sha256`. The two mirrors serve the same bytes; a consumer behind either
mirror resolves the same hash, and a descriptor with one URL is unusable for
half the ecosystem.

**`latest` is a reference, not a version.** `["latest"] = { ref = "15.1.0" }`.
It is what a consumer gets when it names no version, and moving it is a
deliberate act — a consumer pinned to `15.1.0` is unaffected.

**`archs` and the platform table are what a resolution reads.** A payload
published only for `linux` and `x86_64` says so, and a consumer on another
platform is refused by name rather than handed something that will not run.

**Dependencies are `xim:` addresses with floors.**

```lua
deps = { "xim:gcc-runtime@>=15", "xim:glibc@>=2.38" },
```

## Making the payload reachable

A payload that only unpacks is not usable. Three declarations turn a directory
into something a build can consume.

**A program on the path.** `xvm.add(package.name)` registers the payload's
`bin/` so mcpp can find the program by its bare name. A rule package should
**name the program, not a path** — mcpp searches the `bin/` of every declared
payload and then `PATH`, and can report exactly which directories it searched.

**Libraries a consumer will link or load.**

```lua
exports = {
    runtime = { libdirs = { "lib" } },
},
```

`elfpatch` reads this from each dependency and writes the consumer's `RPATH`,
which is what makes a stack of payloads resolve without anyone setting
`LD_LIBRARY_PATH`.

**Headers, so a compiler in this environment can build against it.**
`sysroot.declare_libs(...)` and the header declaration place the payload into
the SubOS sysroot view. **Declared rather than copied** — xlings removes them
with the package, and a copy would outlive its owner.

## The tier: the commands that need a payload

```toml
"xim:qemu-arm" = { version = "9.2.4-1", when = "run" }
```

`when` is a second, independent gate beside the feature that selects the
payload. The feature says **who** needs the tool; the tier says **when**. An
emulator is needed to run and not to compile, so a CI job that builds firmware
and never flashes it downloads nothing.

## Current limitations

- A payload is published to `xim-pkgindex`, which is a separate repository with
  its own review; nothing in mcpp publishes one.
- `latest` and "the highest version in the table" are two different questions,
  and a consumer that wants the newest published version names `latest`.
- A payload's own CI cannot verify that a consumer resolves it: that is what a
  sandbox check against the published descriptor is for, and it is the only
  thing that verifies the published bytes rather than the working tree.
