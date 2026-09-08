# 34 — Authoring a Board-Support Package

**Reader:** someone bringing up a board so that projects can target it with
`mcpp build` and reach it with `mcpp run`.

**The question this chapter answers:** what does a board-support package supply,
and how does one package serve both an emulator and the physical board.

**Not here:** using a BSP, which is
[40 — Bare-Metal and Freestanding Targets](40-baremetal.md); the runner
mechanism a consumer sees, which is [41 — Reaching a Device](41-devices.md);
and packaging the emulator or the probe driver themselves, which is
[32 — Authoring a Payload](32-authoring-a-payload.md).

Before: [33 — Authoring a Runtime Adapter](33-authoring-an-adapter.md).

## The contents of a BSP

A freestanding target has no operating system, so everything a hosted program
gets for free has to come from somewhere. A BSP is that somewhere, and it
supplies **the whole target world**:

| | content |
|---|---|
| the memory map | a linker script — the one fact a program can neither derive nor guess |
| the startup code | what runs before `main`, and the vector table |
| an exported module | what the program imports to reach the board's console and peripherals |
| **the runner** | how `mcpp run` and `mcpp test` reach the board at all |

The runner is the one that is easy to leave out, and without it every consumer
writes its own emulator invocation.

## One package, two environments

A board reached through an emulator and the same board reached through a debug
probe differ **in the argv of their runners and in nothing else**. The linker
script, the startup code, the memory map and the exported module are the same
board. Publishing two packages to vary four strings duplicates all of it and
lets the copies drift.

So the environment is a **feature**:

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[feature-xlings.emulator]
"xim:qemu-arm" = { version = "9.2.4-1", when = "run" }

[feature-xlings.hardware]
"xim:probe-rs" = { version = "", when = "run" }
```

**`emulator` is the default, and that is a decision about who is reading.**
Someone meeting the package has no board on their desk; someone who does has a
reason to say so. A default that required hardware would make the first command
fail for everyone who has not bought anything yet.

The two tables are symmetric on purpose: neither environment is the engine's
idea of normal, and a consumer downloads exactly what the feature it selected
needs.

**And both are on the `run` tier, which is a second and independent gate.** The
feature says *who* needs the tool; the tier says *when*. Compiling firmware needs
neither — only reaching the board does — so a CI job that builds and never
flashes downloads nothing at all.

## The C library is a feature too

Every `thumb*-none-eabi*` row carries an empty C-library column, so a project
targeting one begins with **no libc** unless it asks. A BSP stays there: it
references no C library symbol, and its console goes through semihosting rather
than through `stdio`.

```toml
libc = {}

[feature-deps.libc]
picolibc.picolibc = "1.8.12.3"
```

A C library arrives as a **source** package compiled with the program's own
flags, so there is no multilib to match and no ABI convention to get wrong.
`mcpp run --features libc` is the whole of it, and not selecting it leaves the
zero-libc tier exactly as it was.

## The build program

```cpp
int main() {
    mcpp::link_script("cortex-m.ld");
    mcpp::rerun_if_changed("cortex-m.ld");

    const std::string target = mcpp::target() ? mcpp::target() : "";
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);
        mcpp::run_exclusive();
    } else {
        /* the emulator's argv for THIS target */
    }
    return 0;
}
```

**Which machine models a target is a table, not a default.** An image built for
`thumbv6m` does not run on a model that implements `thumbv7em`, and a wrong
guess boots and then faults somewhere unrelated. A target with no row is an
**error that names it**, not a silent absence — a BSP that configured no runner
would leave `mcpp run` reporting a missing runner and advising a `runner` key,
which is true in general and not the cause here.

**Name the program, not a path.** mcpp searches the `bin/` of every payload
declared by any package in the graph, then `PATH`, and reports exactly which
directories it searched. An absolute path computed from an install directory
introduces a failure mode where the lookup returns empty and nothing says why.

**The engine knows none of the runner names.** `flash`, `serve` and `erase` are
this package's vocabulary; a different package supplies `serve` meaning
something else, and neither has to be known to mcpp.

**One probe, one user.** `mcpp::run_exclusive()` states that this target's runs
cannot overlap, and `mcpp test` then serialises them. A project never has to
remember `-j1`.

## Current limitations

- A BSP declares its emulator and its probe driver as payloads, so a board
  whose tooling is not published for a platform cannot be reached from it.
- The machine table is per-triple. A board that needs a model the table does not
  carry is a change to the BSP, not a project-side override.
