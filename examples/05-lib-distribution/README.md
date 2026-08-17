# 05 — Distributing a prebuilt library

Both halves of one story, because they are only interesting together:

```
producer/    a library with TWO interfaces — a C header and a C++ module
consumer/    three programs using it: header only, module only, both
```

```bash
cd producer  && mcpp pack mathkit          # → target/dist/mathkit-0.1.0-<tag>/
cd ../consumer                              # point [dependencies] at it, then:
mcpp run consume-header
mcpp run consume-module
mcpp run consume-both
```

Full reference: [docs/12 — Distributing a Prebuilt Library](../../docs/12-binary-distribution.md).

## There is no `--lib` flag

What `mcpp pack` produces is decided by `[targets.<name>].kind` — `bin` gives an
application bundle, `lib` and `shared` give library packages. That is already
where mcpp records what an artifact is, and a flag would be a second place to
say it. Publishing both forms means declaring both targets, which the producer
does:

```toml
[targets.mathkit]         kind = "lib"
[targets.mathkit-shared]  kind = "shared"   soname = "libmathkit.so.1"
```

## Read both lists it prints

```
  Packed leg x86_64-linux-gnu  [x86_64-linux-gnu-gcc16-libstdcxx16-c++23]
   Interface mathkit.cppm, api.cppm
    Withheld capi.c, impl.cpp, secret.cppm
```

If you are shipping closed source, **the second line is the one that matters**.
Publishing too little fails loudly in your consumer's compile; publishing too
much silently puts your implementation on someone's disk.

`producer/src/secret.cppm` is an *implementation partition*
(`module mathkit:secret;`, no `export`). It produces a BMI and an object exactly
like the interface units do — so "publish every module unit" would leak it, and
"publish every `.m.o`" is not a rule mcpp uses. What travels is the **module
closure of the lib root**: `mathkit.cppm` and what its purview imports.

Prove it:

```bash
cd producer && mcpp pack mathkit
grep -r house_factor target/dist/*/interface/ ; echo "no match = nothing leaked"
```

**Then break it on purpose.** Add `import :secret;` to `src/mathkit.cppm` and
pack again: the interface now reaches the partition, so a consumer needs that
source to compile at all — and `mcpp pack` says so instead of shipping a package
nobody can build.

## What the consumer's build checks

Two things, both of which fail silently without a check.

**The interface still matches its binaries.** Edit one line of
`interface/api.cppm` inside the package and rebuild:

```
error: mcpp.mathkit@0.1.0: 'interface' does not match what was packaged.
  recorded fnv1a:25b2cf2a79d71c40
  found    fnv1a:fe404d5be85118ff
```

That refusal exists because the alternative was measured: swap two `int` members
of a struct in a shipped interface — the Itanium ABI does not mangle field order
— and the consumer compiles, links, runs, and prints transposed data, with no
diagnostic from any tool.

**The binaries were built for your toolchain.** Switch `[toolchain]` and rebuild;
the refusal lists the tags the package *does* have, because "not found" would
send you looking for a package already on your disk.

## And one thing that will not work

```bash
cd producer/target/dist/mathkit-0.1.0-*/ && mcpp build
error: … is a distribution package produced by `mcpp pack`, not a source tree.
```

Its `interface/` holds declarations whose definitions are in the artifact beside
them. Building there compiles the declarations, produces a near-empty library
and reports success — a failure shaped exactly like a success.

## Why the tag is shorter for a C-only library

```
x86_64-linux-gnu-gcc16-libstdcxx16-c++23     # a C++ module interface
x86_64-linux-gnu                             # an extern "C" interface only
```

A tag names the dimensions the artifact actually constrains, and unnamed ones
are don't-care. So an `extern "C"` library needs one tag per triple instead of
one per triple per compiler — no flag, no mode: the shape *is* the statement.
