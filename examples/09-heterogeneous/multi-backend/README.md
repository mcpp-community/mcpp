# Several backends in one artifact, chosen at run time

The four examples beside this one are each **one seam**: a device file and a CPU
file define the same symbol and are never in one link, so exactly one exists and
the choice is made at build time. That is the right shape for a program.

A library cannot make that choice. It is compiled once and consumed by people
whose machines differ, so its backends are **additive** — several land in one
artifact and the choice moves to run time. This is that shape, in the smallest
form that still shows it.

## What it demonstrates

| | |
|---|---|
| `accel` is a **set** | `--accel "cuda12.9+{sm_89}, vulkan1.2"` compiles both islands into one artifact |
| A constrained glob gates itself | `{ glob = "…/*.cu", accel = "…" }` reaches the build program only when this build's `accel` accepts it, so device sources need no `cfg` block |
| `cfg(accelerator = "none")` | the CPU-only variant, selected without enumerating the backends it is not |
| `cfg(not(accelerator = "none"))` | the dispatcher, built whenever **some** backend is named |
| A module seam | `main.cpp` does `import opkit;`; the C boundary exists only where an island requires it |

## Building it

```bash
mcpp run                                          # CPU only, no payloads
mcpp run --accel "vulkan1.2"                      # + the Vulkan island
mcpp run --accel "cuda12.9+{sm_89}"               # + the CUDA island
mcpp run --accel "cuda12.9+{sm_89}, vulkan1.2"    # both, one artifact
```

The first line needs nothing installed, which is why CI builds it: the CPU-only
path is where `cfg(accelerator = "none")` is exercised, and it costs no payload.

The program prints the backend **before** the numbers, because every backend
returns the same four numbers — the numbers alone cannot separate a device run
from the reference one, and that is exactly the confusion an example about
heterogeneous compute must not teach.

## Why the dispatcher's predicate matters

The registry has to be built for cuda, for vulkan, for both, and for a backend
that does not exist yet. Written as

```toml
[target.'cfg(not(any(accelerator = "cuda", accelerator = "vulkan")))'.build]
```

it would have to be edited every time the ecosystem gains a backend — and
`accelerator` is an **open** vocabulary, so a third backend is a package rather
than an engine change. The edit that is forgotten is silent: the CPU-only
dispatcher and the registry would both compile, or neither would.

`accelerator = "none"` says "this build named no backend" directly, and keeps
saying it after the vocabulary grows.

## Where the rule package is declared

```toml
[build-dependencies.mcpp]
plugins = { version = "0.2.1", features = ["rules-spirv"], host-module = true }
```

`[build-dependencies]`, not `[dependencies]`: a rule package's library must
never reach the target while its rule is still wanted, which is the case
docs/05 section 2.6.1 exists for. `host-module = true` says which build-time
product is wanted; the section says whether the package reaches the target.
Two axes, and a rule package answers "no" on the second.
