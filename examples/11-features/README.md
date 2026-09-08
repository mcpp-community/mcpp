# 11 — Features: what a package declares, and what a consumer pays for

Every heterogeneous example in this repository *consumes* a feature
(`features = ["rules-cuda"]` on a dependency edge). This one **declares** them.

```
cd examples/11-features/greeter
mcpp run                      # HELLO, WORLD!   metrics: off
mcpp run --features metrics   # HELLO, WORLD!   metrics: on
mcpp test                     # the dev-dependency reaches the test
```

## Three shapes of feature, in one manifest

```toml
[features]
default = ["shout"]

shout = {}                              # a compile macro and nothing else

[features.metrics]                      # ...and a source
sources = ["src/metrics.cpp"]

[feature-deps.metrics]                  # ...and a dependency
counters = { path = "../counters" }
```

| shape | what it adds | reached in the source as |
|---|---|---|
| `shout = {}` | `MCPP_FEATURE_SHOUT` | `#ifdef MCPP_FEATURE_SHOUT` |
| `[features.metrics] sources` | one file in the source set | the file is compiled or it is not |
| `[feature-deps.metrics]` | a package in the graph | `import counters;` inside that file |

Two more keys are here because a library has them and no other example does:

```toml
[dev-dependencies]                      # reaches tests, never the artifact
counters = { path = "../counters" }

[profile.release]
opt_level = 3
```

## The criterion

Not "the default build still works" — that passes when the optional dependency
is resolved and simply unused. The criterion is that **the default build's
resolution does not name the optional package**:

```
mcpp build                    counters appears 0 times in the build output
mcpp build --features metrics counters appears 2 times
```

Measured on 2026.9.8.1. That is the property `[feature-deps]` exists for: a
consumer that does not ask for the backend does not download it, does not
compile it, and does not link it.

## A build program sees the resolved feature set

```cpp
if (mcpp::has_feature("metrics"))
    mcpp::warning("the metrics feature is active; `counters` is in the graph");
```

A rule or a generator acts on the feature set without the project stating the
decision a second time. This is the mechanism the accelerator lanes use: a rule
package declares its toolkit under the feature that selects it, so a build that
does not name the feature installs nothing.

## Current limitations

**A default feature is turned off in the manifest, not on the command line.**
There is no `--no-default-features`. `mcpp run --features metrics` yields
`default ∪ {metrics}`; to build without `shout`, `[features] default` is
edited. With `default = []` this example prints `Hello, world.` instead of
`HELLO, WORLD!`.
