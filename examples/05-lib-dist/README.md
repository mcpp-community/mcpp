# 05 — Shipping a prebuilt library

A library with **two interfaces at once** — a C header and a C++ module — and
what `mcpp pack` does with them.

```bash
mcpp pack mathkit                      # static library package
mcpp pack mathkit-shared               # dynamic library package (Linux/ELF today)
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target x86_64-linux-musl   # one package, two legs
```

There is no `--lib` and no `--artifact static|shared`. What gets packed is
decided by `[targets.<name>].kind`, which is where mcpp already records what an
artifact is — a second place to say it could only ever disagree with the first.

## What the command prints, and why both lists matter

```
  Packed leg x86_64-linux-gnu  [x86_64-linux-gnu-gcc16-libstdcxx16-c++23]
   Interface mathkit.cppm, api.cppm
    Withheld capi.c, impl.cpp, secret.cppm
      Packed target/dist/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23.tar.gz
```

If you are shipping a closed-source library, **the second list is the one to
read**. Publishing too little fails loudly in your consumer's compile;
publishing too much silently puts your implementation on someone's disk.

## Why `secret.cppm` is not published

`src/secret.cppm` is an *implementation partition* (`module mathkit:secret;`,
no `export`). It produces a BMI and a `.m.o` exactly like the interface units
do — so "publish every module unit" would leak it, and "publish every `.m.o`"
is not a rule mcpp uses.

What travels is the **module closure of the lib root**: `mathkit.cppm` and
what its purview imports (`:api`). Nothing else. Try it:

```bash
mcpp pack mathkit
grep -r house_factor target/dist/*/interface/ ; echo "exit=$?"   # no match
```

**Now break it on purpose.** Add `import :secret;` to `src/mathkit.cppm` and
pack again: the interface now reaches the partition, so it must be published
for a consumer to compile at all — and `mcpp pack` stops and tells you, rather
than shipping a package that cannot be built.

## Static and dynamic from one project

Two targets, not two commands with a flag:

```
bin/libmathkit.a
bin/libmathkit-shared.so
bin/libmathkit.so.1 -> libmathkit-shared.so      # the soname alias
```

The `.so` file carries the target's name; `soname` is what consumers actually
load, and the package records that.

## What ends up in the package

```
mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23/
├── mcpp.toml                     # an ORDINARY manifest — no new section
├── include/mathkit_c.h           # text interface: #include, never compiled
├── interface/mathkit.cppm        # module interface: the consumer compiles it
├── interface/api.cppm
└── lib/x86_64-linux-gnu/libmathkit.a
```

`lib/` is keyed by **triple**, not by OS: MinGW and MSVC are both Windows and
produce `libfoo.a` and `foo.lib` respectively.

Open the generated `mcpp.toml`. Everything in it is a key mcpp already had —
`sources`, `include_dirs`, `[modules] exports`, a `cfg(...)` block per leg, and
`[[runtime.artifacts]]` carrying each artifact's ABI tag and digest. That is
why an older mcpp can still *build* against this package: it just does not run
the checks.

## The tag, and why a C library gets a shorter one

```
x86_64-linux-gnu-gcc16-libstdcxx16-c++23     # a C++ module interface
x86_64-linux-gnu                             # an extern "C" interface only
```

A tag names the dimensions the artifact actually constrains, and unnamed ones
are don't-care. So a C library needs one tag per triple instead of one per
triple per compiler — no flag, no mode: the shape *is* the statement.

See [06-lib-consume](../06-lib-consume/) for the other end.
