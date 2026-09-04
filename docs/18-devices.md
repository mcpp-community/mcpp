# 18 — Reaching a Device

This document specifies how mcpp executes an artifact that runs somewhere other
than the machine that built it, how a package supplies additional ways of
reaching it, and how a project selects between an emulator and physical
hardware.

Related documents: [13 — Bare-Metal and Freestanding Targets](13-baremetal.md)
covers the targets this most often applies to; [07 — build.mcpp](07-build-mcpp.md)
is the reference for the directive protocol a package speaks; [11 — Machine
Output](11-machine-output.md) is the interface a debugger client or IDE uses.

## One command, and named exceptions

An artifact that cannot run on the build machine needs something to stand in
front of it. That thing is a **runner**: an argv the package supplies and a tool
performs, with the artifact appended or substituted for `{}`.

```bash
mcpp run                        # the default runner
mcpp run --runner flash         # a named one
mcpp run --list-runners         # what this project supplies
```

`mcpp run` is the whole of the common case, including on real hardware. On a
device, running a program means writing it, resetting, attaching to its output
and reading its exit status — which is one command (`probe-rs run`, `qemu-system-*
-kernel`), not several. A board therefore supplies that as its **default**
runner, and the command a developer types does not change when they move from an
emulator to a board.

Named runners exist for what remains: writing an image without running it,
observing a console, starting a debug server, erasing a part, deploying without
starting.

⚠️ **The engine knows no runner names.** `flash`, `serve`, `deploy`, `submit`
and `logcat` are equally unknown to it: it knows only that a package may supply
named runners, and performs the argv it finds. A fixed set of names in the
engine would decide, in the engine, which domains are expressible.

## What a package supplies

```cpp
mcpp::runner("qemu-system-arm");        // the default: argv token by token
mcpp::runner("-machine"); mcpp::runner("mps2-an385"); …

mcpp::runner("flash", "probe-rs");      // a named runner
mcpp::runner("flash", "download"); …

mcpp::runner_longlived("monitor");      // no natural end
mcpp::run_exclusive();                  // this target's runs cannot overlap
```

⭐ **Name the program, not its path.** mcpp locates it: the `bin/` of a payload
declared under `[xlings] deps` by **any package in the graph** — the consuming
project first, then its dependencies — and then `PATH`. A board-support package
is precisely the thing that knows which emulator or probe reaches its machine,
so it declares that payload itself and the consumer declares nothing. Writing an
absolute path computed from `mcpp::xpkg_dir` is unnecessary, and it introduces a
failure mode — a declaration is not an install, so the lookup can return empty
and leave no runner configured with nothing said about why. Naming the program
lets mcpp report exactly which directories it searched.

## What a project overrides

```toml
[target.thumbv7em-none-eabihf]
runner = ["qemu-system-arm", "-machine", "mps2-an385", "-kernel"]

[target.thumbv7em-none-eabihf.runners]
flash   = ["probe-rs", "download", "--verify", "--chip", "STM32L475VG", "{}"]
monitor = ["probe-rs", "attach", "--chip", "STM32L475VG"]
```

Precedence is the ordinary one: what the author of the project wrote beats what
a dependency supplied, and the override is reported rather than applied in
silence. Exactly one dependency may supply a given name; a second is an error
naming both packages.

## Termination is declared, not inferred

| | Meaning |
|---|---|
| default | runs to completion; the exit code is the verdict |
| `runner_longlived(name)` | has no natural end; the operator ends it |

`openocd -c "program image.elf verify reset exit"` terminates and `openocd -c
"init"` does not, and the two are spelled alike up to the argument the package
chose. No argv can express which is which, and the engine has no list of names
to infer it from — so the package states it.

`mcpp run --runner debug` starts a **server** and stops there. The client that
attaches is the user's debugger or IDE, which learns what it needs through the
machine-output protocol.

## Runs that cannot overlap

`mcpp test` runs test binaries on a worker pool. One board on one probe, one
GPU, one serial port, or a tool with a single-seat licence admits one user at a
time, and two workers reaching for it do not fail cleanly — they interleave, and
the verdict describes neither test.

The package states this about itself with `mcpp::run_exclusive()`, and `mcpp
test` then serialises. A project never has to remember `-j1`.

Named for the property rather than for the hardware: nothing here is about
devices.

## Emulator and hardware are one package

A board reached through an emulator and the same board reached through a debug
probe differ in the argv of their runners and in nothing else. The linker
script, the startup code, the memory map and the exported module are the same
board. Publishing two packages to vary a few strings duplicates all of it and
lets the copies drift.

The choice is therefore a feature of one package:

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}
```

```cpp
int main() {
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);                       // the DEFAULT moves
        for (auto a : {"probe-rs", "gdb", "--chip", "STM32L475VG"})
            mcpp::runner("debug", a);
        mcpp::runner_longlived("debug");
        mcpp::run_exclusive();
    } else {
        for (auto a : {"qemu-system-arm", "-machine", "mps2-an385", "-nographic",
                       "-semihosting", "-no-reboot", "-kernel"})
            mcpp::runner(a);
    }
    return 0;
}
```

```toml
[dependencies]
cortex-m-rt = { version = "0.1.0", features = ["hardware"] }
```

The consumer's command does not change. A runner the chosen environment does not
supply stays absent: an emulator has no debug probe, so under the emulator
feature `mcpp run --runner debug` reports that no such runner exists and lists
the ones that do.

⭐ This required no engine mechanism. The engine reads runners and knows nothing
about emulators or probes; `mcpp::has_feature` already existed. That the
question is answerable without adding anything is the layering working as
specified.
