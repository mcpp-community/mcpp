# 18 — Reaching a Device

This document specifies how mcpp executes, writes, observes and debugs an
artifact that runs somewhere other than the machine that built it, and how a
project selects between an emulator and physical hardware.

Related documents: [13 — Bare-Metal and Freestanding Targets](13-baremetal.md)
covers the targets these actions apply to; [07 — build.mcpp](07-build-mcpp.md)
is the reference for the directive protocol a board-support package speaks;
[11 — Machine Output](11-machine-output.md) is the interface a debugger client
or IDE uses.

## Four actions, one shape

An artifact that cannot run on the build machine needs something to stand in
front of it. Four things are asked of such an artifact, and all four are an
argv that a board knows and a tool performs:

| Command | Slot | What it does |
|---|---|---|
| `mcpp run` | `runner` | executes the artifact |
| `mcpp flash` | `flash` | writes it to the device |
| `mcpp monitor` | `monitor` | observes what the device prints |
| `mcpp debug` | `debug` | starts the device's debug server |

Each is declared the same way, by a board-support package:

```cpp
mcpp::flash("probe-rs");
mcpp::flash("download");
mcpp::flash("--verify");
mcpp::flash("--chip");
mcpp::flash("STM32L475VG");
```

or by a project, overriding what a dependency supplied:

```toml
[target.thumbv7em-none-eabihf]
flash = ["probe-rs", "download", "--verify", "--chip", "STM32L475VG", "{}"]
```

The artifact path is appended, or substituted for `{}` when the template
contains it. One token per call: argv is ordered, and a single string cannot
say where its boundaries are.

The program is located by mcpp rather than by the system: a declared payload's
`bin/` first, then `PATH`. A tool that is nowhere is an error decided before
any process starts, rather than a fallback to bare execution.

## Termination is a property of the slot

The four actions differ in one way the engine must act on, and no argv can
express it.

| Semantics | Slots | Meaning |
|---|---|---|
| `OneShot` | `run`, `flash` | runs to completion; the exit code is the verdict |
| `LongLived` | `monitor`, `debug` | has no natural end; the operator ends it |

`openocd -c "program image.elf verify reset exit"` terminates and `openocd -c
"init"` does not, and the two are spelled alike up to the argument the board
chose. The engine therefore reads termination from the slot, and a board cannot
get it wrong by writing its argv differently.

`mcpp debug` starts a **server** and stops there. The client that attaches is
the user's debugger or their IDE, which learns what it needs through the
machine-output protocol. mcpp does not drive the client.

## Absence is reported, never substituted

`mcpp run` on a hosted target with no runner executes the artifact directly,
because the host can run it. There is no corresponding reading of "no flasher":
nothing else writes an image to a device. An undeclared `flash`, `monitor` or
`debug` is therefore an error on every target, naming the slot and printing the
key to paste.

Succeeding at `mcpp flash` by running the program on the build host would be
the exact failure the slot exists to prevent.

## An exclusive device

A physical board is a mutex; an emulator is not. `mcpp test` runs test binaries
on a worker pool, and two processes reaching for one probe do not fail cleanly
— they interleave, and the verdict describes neither test.

The board states this about itself:

```cpp
mcpp::runner_exclusive();
```

`mcpp test` then runs one test at a time on that target, and reports that it is
doing so. A project never has to remember `-j1`.

## Emulator and hardware are one package

A board reached through an emulator and the same board reached through a debug
probe differ in the argv of their device slots and in nothing else. The linker
script, the startup code, the memory map and the exported module are the same
board. Publishing two packages to vary four strings duplicates all of it and
lets the copies drift.

The choice is therefore a feature of one package:

```toml
[features]
default  = ["emulator"]
emulator = []
hardware = []
```

```cpp
int main() {
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);
        for (auto a : {"probe-rs", "download", "--verify", "--chip", "STM32L475VG"})
            mcpp::flash(a);
        mcpp::runner_exclusive();
    } else {
        mcpp::runner(qemu_path());
        for (auto a : {"-machine", "mps2-an385", "-nographic", "-semihosting",
                       "-no-reboot", "-kernel"})
            mcpp::runner(a);
    }
    return 0;
}
```

The consumer selects an environment where it selects everything else:

```toml
[dependencies]
demo-board-rt = { version = "0.1.0", features = ["hardware"] }
```

A slot the chosen environment does not supply stays absent. An emulator has no
debug probe, so under the emulator feature `mcpp debug` reports that none is
configured rather than inventing one.

This required no engine mechanism. The engine reads slots and knows nothing
about emulators or probes; `mcpp::has_feature` already existed. That the
question is answerable without adding anything is the layering working as
specified.

## Precedence and reporting

Two producers exist for every slot, with ordinary precedence: what the author
of the project wrote beats what a dependency supplied. The override is reported
rather than applied in silence.

```
        note [target.thumbv7em-none-eabihf].flash overrides the flash a dependency supplied
```

Exactly one dependency may supply a given slot. Link flags from two
dependencies concatenate and that is correct; two flashers cannot, and
appending produces an argv that is neither one's. A second provider is an error
naming both packages.
