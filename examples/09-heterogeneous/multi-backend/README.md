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

Measured, on a machine with an RTX 4080 and a 12.4 driver:

| build | prints |
|---|---|
| `mcpp run` | `backend: cpu (only backend in this build)` |
| `--accel "vulkan1.2"` | `backend: vulkan (NVIDIA GeForce RTX 4080)` |
| `--accel "cuda12.9+{sm_89}"` | `backend: cuda` |
| both | `backend: cuda` — the chain's first entry answers |

All four print `12 24 36 48`, which is exactly why the backend is printed
first: every backend returns the same four numbers, so the numbers alone cannot
separate a device run from the reference one.

**Naming a subset is not a mismatch.** `--accel "vulkan1.2"` leaves the `.cu`
glob out the way `--no-accel` leaves both out, and the `cfg(accelerator =
"cuda")` section carrying that backend's host half does not activate either, so
the two halves stay together. Only an accelerator this build *does* name whose
architecture it does not cover is refused (mcpp 2026.9.6.5).

**Nothing is installed for a device this build did not name.** The payloads sit
under `[target.'cfg(accelerator = ...)'.xlings.workspace]`, so `mcpp run`
fetches neither the CUDA toolkit nor the shader compiler. That gating needs
mcpp 2026.9.6.5; before it, the only spellings available were "unconditionally"
and "not at all", and the cheapest build paid for the most expensive one.

## The CUDA leg takes the clang route

`[toolchain] default = "llvm@22.1.8"`, and the reason is measured rather than
stylistic. On the 12.9 line the nvcc route is refused by nvcc's own front end:
the toolkit headers redeclare the C23 `cospi`, `sinpi` and `rsqrt` for the host
without `noexcept` while the C library declares them with it. Driving an older
`xim:gcc` payload does not help — the declarations come from the C library, not
from the host compiler, and this was tried. The 13.x line fixes it and raises
the driver floor to r580, which is a requirement on the machine rather than a
decision the project gets to make. The clang route never includes that header
and runs on any driver from r525 onward.

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
plugins = { version = "0.2.2", features = ["rules-cuda", "rules-spirv"], host-module = true }
```

Two rules, in one build program, which is what an additive-backend package
needs and what no other example here has. `mcpp::device_sources()` is the
package's whole device set, so in a build naming both backends that one list
holds a `.cu` and a `.comp`: each rule takes the extensions it claims and
leaves the rest, which is what 0.2.2 fixed. A device source no rule claims is
not silently dropped either — mcpp refuses a device source that reached no
action, naming the file.

`[build-dependencies]`, not `[dependencies]`: a rule package's library must
never reach the target while its rule is still wanted, which is the case
docs/05 section 2.6.1 exists for. `host-module = true` says which build-time
product is wanted; the section says whether the package reaches the target.
Two axes, and a rule package answers "no" on the second.
