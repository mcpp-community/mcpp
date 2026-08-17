# 06 — Consuming a prebuilt library

Three consumers of the package [05-lib-dist](../05-lib-dist/) produces: one
that only `#include`s, one that only `import`s, and one that does both.

```bash
cd ../05-lib-dist && mcpp pack mathkit          # produce the package
cd ../06-lib-consume
# point [dependencies].mathkit at the directory that appeared under
# ../05-lib-dist/target/dist/, then:
mcpp run consume-header
mcpp run consume-module
mcpp run consume-both
```

A packed library is an **ordinary mcpp package**. It carries a normal
`mcpp.toml`, so a `path` dependency, a downloaded archive and an index entry
all reach it through the same code path — there is nothing new to learn on
this side.

## What the two interface modes cost you

| | `#include <mathkit_c.h>` | `import mathkit;` |
|---|---|---|
| does mcpp compile anything of the package? | no | yes — the published `.cppm` |
| what constrains compatibility | the libc ABI | compiler, C++ stdlib, C++ level |
| the tag the package publishes | `x86_64-linux-gnu` | `x86_64-linux-gnu-gcc16-libstdcxx16-c++23` |

Both work against the *same* package, at the same time.

## What is checked before your build links

Nothing is enforced that the package did not declare, and the two things it
does declare are the two that fail silently otherwise.

**The interface still matches its binaries.** Edit one line of
`interface/api.cppm` in the package and rebuild:

```
error: mcpp.mathkit@0.1.0: 'interface' does not match what was packaged.
  recorded fnv1a:25b2cf2a79d71c40
  found    fnv1a:fe404d5be85118ff
```

That refusal exists because the alternative was measured: swap two `int`
members of a struct in a shipped interface — which the Itanium ABI does not
mangle — and the consumer compiles, links, runs, and prints transposed data,
with no diagnostic from any tool.

**The binaries were built for your toolchain.** Switch `[toolchain]` to another
compiler and rebuild:

```
error: mcpp.mathkit@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags :
                   x86_64-linux-gnu-gcc15-libstdcxx15-c++23
  closest is x86_64-linux-gnu-gcc15-libstdcxx15-c++23, and it differs on:
    compiler  needs gcc15, this build has gcc16
    stdlib    needs libstdcxx15, this build has libstdcxx16
```

The tags it *does* have are part of the message on purpose: "not found" would
send you looking for a package that is already on your disk.

## One thing that will not work, and should not

```bash
cd ../05-lib-dist/target/dist/mathkit-0.1.0-*/ && mcpp build
error: … is a distribution package produced by `mcpp pack`, not a source tree.
```

Its `interface/` holds declarations whose definitions are in the archive
beside them. Building there compiles the declarations, produces a near-empty
library and reports success — a failure that looks exactly like a success.
