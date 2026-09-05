# 17 - The Project Environment

A project can declare the environment it builds in. That one declaration
decides which C library the project links against and which tools its build
programs find — so a `mcpp.toml` means the same build on a developer's laptop
and in CI, whatever else those two machines happen to have installed.

```toml
[xlings]
subos = "tools"

[xlings.workspace]
"xim:qemu-riscv" = "9.2.4-1"
```

Working project: `examples/07-project-subos/`.

## 1. What a SubOS is

A SubOS is a directory that holds a userspace: its own `bin`, its own library
view, its own installed package versions, and a `subos_info` block describing
itself. mcpp treats it as the answer to "what does this project build
against", and it is the only mechanism that answers that question — not the
compiler's path, not `XLINGS_ACTIVE_SUBOS`, not the shell.

Two kinds exist, and the difference is where the directory lives:

| Declaration | Directory | Shared with |
|---|---|---|
| none | mcpp's initialized `subos/default` | every project on the machine |
| `subos = "default"` | the same directory, named explicitly | every project on the machine |
| `subos = "<name>"` | `<project>/.mcpp/.xlings/subos/<name>/` | nothing |

The third row is the isolated one. It belongs to the project, it sits beside
the manifest, and removing the project removes it.

## 2. What the declaration decides

**The C library.** A payload-first build links against one specific glibc, and
which one is a fact about the project rather than about the machine. Chapter 8
covers the binding, the degradation rules, and what a SubOS that does not
describe itself does to them.

**The tools a build program sees** (mcpp 2026.8.25.1+). The declared
environment's `bin` goes to the front of the `PATH` that `build.mcpp` runs
with:

```
PATH=<the declared environment's bin>:<the PATH mcpp itself was started with>
```

A build program that spells `qemu-system-riscv64` as a bare name therefore gets
the copy inside the declared environment. Chapter 7 covers the contract this
rides on.

⚠️ **Only for projects that declare one.** A project with no `[xlings].subos`
gets the `PATH` mcpp was started with, byte for byte. Putting a shared
directory in front of every project would make what a build sees depend on what
else had been installed on that machine — two projects on one machine would
agree with each other, and the same project on two machines would not.
Declaring it is what puts it there.

⚠️ **Prefixed, not replaced.** A build program legitimately calls `git`,
`python3` or a shell, and none of those live in a SubOS. Front position makes
the declared environment the default answer; everything else stays reachable
behind it.

### 2.1 Which version pins apply (2026.9.3+)

Naming an environment also changes where a tool's version comes from. A
project's own `[xlings.workspace]` entries always win; what differs is what
they are laid over:

| The project declares | The version of a tool it did not name comes from |
|---|---|
| `[xlings.workspace]`, no `subos` | the machine's environment |
| `[xlings.workspace]` and `subos = "<name>"` | that environment's own workspace; the machine's does not apply |

The second row is what isolation means. A named environment has its own
installed set, and carrying the machine's versions into it would name versions
that are not there — so a project that relied on the machine's tools has to
declare them once it names an environment.

An `xlings use` performed inside the project outranks both, until mcpp rewrites
the environment: it is the layer merged last, and an action a person took
should beat a file.

## 3. What the declaration does not decide

`[xlings.workspace]` names packages to be present in the environment, and each one's
payload directory is delivered separately as `MCPP_XPKG_<NAME>_DIR`. That is a
different question from `PATH` and stays a different answer: a build program
that needs a package's data files (protoc's well-known `.proto` files, say)
asks for the directory, and one that needs to *run* a program asks `PATH`.

A dependency's own `[xlings]` declaration is never consulted or propagated. In
a workspace build the workspace root owns the selection; a member's declaration
applies only when that member is built as an independent root.

## 4. Reading an environment, never creating one

mcpp resolves a declared name and reads what it finds. A name that does not
resolve is a hard error:

```
error: selected SubOS 'tools' does not exist at …/.mcpp/.xlings/subos/tools;
create/bootstrap that environment instead of falling back to active/default
```

Falling back to the default or to whatever is active would substitute a
different environment for the one the manifest named, which is precisely what
would make one `mcpp.toml` mean two different builds. Creating and populating
a SubOS is xlings' layer — `xlings subos new` — and mcpp managing SubOS state
would invert that layering.

An environment that exists but carries no `subos_info` block **degrades rather
than fails**: the runtime binding reports `inconclusive`, no payload-first
binding is available, a note is printed, and the build continues. Chapter 8
gives the full rule.

## 5. When a private environment is worth it

- **A generator whose version changes what it emits.** `protoc`, `flatc`, a
  shader compiler: the output is an input to everything downstream, so the
  project pins the producer instead of hoping the machine has a compatible one.
- **An emulator a build program runs.** Several bare-metal packages boot an
  artefact under QEMU as part of proving it works; which QEMU is part of what
  was proven.
- **A project whose CI and developer machines differ**, where neither is wrong
  and the build must not notice.
- **Two projects on one machine that need different versions of one tool.**
  Sharing a directory means one of them loses; a private environment means the
  question does not arise.

Against that: an isolated environment is a directory that has to be created and
populated, and the first build pays for it. Since 2026.8.29 mcpp does that
work — a declared `[xlings.workspace]` entry is provisioned on first use, and a named
`[xlings] subos` that does not exist yet is created rather than refused — but
the cost is real: the first build on a clean machine downloads and installs
before it compiles anything. A project whose tools are ordinary and whose
versions do not matter is better off declaring nothing and inheriting the
machine's.

Under `--offline` / `MCPP_OFFLINE` or `MCPP_NO_AUTO_INSTALL`, mcpp refuses
instead of installing, and names the packages so they can be provisioned
out of band — the same two knobs `[toolchain]` honours, for the same reason: an
unasked-for download is not something a build decides on a project's behalf.

The declaration is provisioned on every host that builds the project, and a
package the host cannot install is an error, not a skipped entry. A tool that
exists for one host platform only is therefore declared for that platform
(2026.9.2.1): `deps = [{ linux = "qemu-user-aarch64" }]` declares the emulator
on Linux and nothing elsewhere. The keys and the resolution rule are in
chapter 5, §2.13.

**Which verbs install it.** An entry may name a tier —
`{ version = "0.24.0", when = "run" }` — and a `[feature-xlings.<feature>]`
table gates one on a feature. A tool the project will not use is then not
downloaded: chapter 5, §2.13. Omitting the tier is the historical behaviour.

**The runner.** A program under `[xlings.workspace]` is also where
`[target.<triple>].runner` looks first for its first element, before `PATH`
(chapter 5, §2.7.3). The two keys together provision a user-mode emulator on a
CI host and execute a cross-built artifact through it, without the manifest
naming the payload's path.

## 6. What belongs somewhere else

| Need | Where it goes |
|---|---|
| a library the program links | `[dependencies]` |
| the compiler | `[toolchain]`, chapter 3 |
| a host tool a dependency produces | `tools = [...]`, chapter 7 |
| a tool present in the environment | `[xlings.workspace]` |
| a tool only one verb or one feature needs | `when = "run"`, `[feature-xlings.<f>]` |
| which environment | `[xlings] subos` |

## 7. Related chapters

- [7 - build.mcpp](07-build-mcpp.md) — the contract a build program receives,
  including the `PATH` it runs with.
- [8 - Toolchain Internals](08-toolchain-internals.md) — runtime selection,
  the `RuntimeBinding` snapshot, and the degradation rules.
- [5 - mcpp.toml](05-mcpp-toml.md) — every manifest key, including `[xlings]`.
