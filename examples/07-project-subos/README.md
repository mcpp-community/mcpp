# 07 — The Environment This Project Asked For

A build program that finds its tools in an environment the project declared,
instead of asking the machine what it happens to have.

```bash
mcpp build
```

```
warning: project-subos: PATH begins at …/subos/default/bin
warning: project-subos: this project's subos is populated
```

## What the section does

```toml
[xlings]
subos = "default"

[xlings.workspace]
qemu-riscv = "xim:9.2.4-1"
```

`subos` names the environment this project builds in. mcpp already used that
declaration to decide which C library the project links against — one
`mcpp.toml` must not mean different ABIs on different machines — and it now
also puts that environment's `bin` at the front of the `PATH` it runs
`build.mcpp` with.

So `qemu-system-riscv64`, spelled as a bare name in a build program, resolves
inside the environment the project named.

## Only for projects that ask

A project with no `[xlings].subos` gets the `PATH` mcpp was started with,
unchanged. That is deliberate. A build system that put a shared directory in
front of every project would make what a build sees depend on what else had
been installed on that machine — two projects on one machine would agree with
each other, and the same project on two machines would not.

Declaring it is what puts it there.

## Why a bare name and not a constructed path

`MCPP_XPKG_QEMU_RISCV_DIR` gives the payload directory, and a build program can
join `/bin/qemu-system-riscv64` onto it. That works, and it means every build
program in the ecosystem repeats the same joining, each with its own idea of
the layout — one of them will get it wrong on the platform its author does not
have.

Asking `PATH` is what a program would do anyway. What the declaration changed
is the answer.

## The host stays reachable

The directory is **prepended**, not substituted. A build program legitimately
calls `git`, `python3` or a shell, and none of those live in a subos. Front
position makes the declared environment the default answer; everything else is
still behind it.

## A private environment, not the shared one

This example names `"default"` so it builds on a clean checkout. The stronger
form is an environment that belongs to the project:

```toml
[xlings]
subos = "tools"
```

which mcpp resolves to `<project>/.mcpp/.xlings/subos/tools/` — its own `bin`,
its own package versions, isolated from every other project on the machine.

⚠️ **mcpp reads such an environment and never creates one.** A name that does
not resolve is a hard error, not a fallback:

```
error: selected SubOS 'tools' does not exist at …/.mcpp/.xlings/subos/tools;
create/bootstrap that environment instead of falling back to active/default
```

Substituting a different environment is exactly what would make one `mcpp.toml`
mean two different builds. Creating and populating one is xlings' layer
(`xlings subos new`), and a project that ships one puts the directory beside
its manifest.

## When to reach for this

- **A generator whose version changes what it emits.** `protoc`, `flatc`, a
  shader compiler: the output is an input to everything downstream, so the
  project pins the producer rather than hoping.
- **An emulator a build program runs.** Several bare-metal packages boot an
  artefact under QEMU as part of proving it; which QEMU is part of what was
  proven.
- **A project that must build the same way on a developer's machine and in
  CI**, where the two have different things installed and neither is wrong.

What it is *not* for: libraries the program links, which are `[dependencies]`,
and the compiler itself, which is `[toolchain]`.
