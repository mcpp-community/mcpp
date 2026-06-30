# Bare OS-alias sugar for `[target.*]` conditional tables (Design)

Addresses the ergonomic complaint that `[target.'cfg(linux)'.dependencies.compat]`
is visually noisy. Lets the common single-OS case drop the `cfg(...)` wrapper (and
its mandatory TOML quotes) while keeping the full `cfg(...)` grammar for compound
predicates. Ships in mcpp 0.0.80.

## Before / after

```toml
# before — quotes (TOML-mandated around cfg(...)) + ceremony
[target.'cfg(windows)'.dependencies.compat]
openblas = "0.3.33"
[target.'cfg(windows)'.build]
ldflags = ["-Llib", "-llibopenblas"]

# after — bare alias for the 90% case, no quotes
[target.windows.dependencies.compat]
openblas = "0.3.33"
[target.windows.build]
ldflags = ["-Llib", "-llibopenblas"]

# compound predicates STILL use cfg(...) (arch / env / all / any / not)
[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-march=x86-64-v2"]
```

## Why this is unambiguous

A mcpp target triple is always `<arch>-<os>[-<env>]` (`x86_64-linux-musl`,
`x86_64-pc-windows-msvc`). The bare OS/family aliases **`windows` / `linux` /
`macos` / `unix` are never valid triples** (no dash), so `[target.linux]` can only
mean the `cfg(linux)` predicate — there is no collision with the exact-triple
namespace (`[target.x86_64-linux-musl]`). This is the same alias set already
accepted *inside* `cfg(...)` (`cfgpred::match_alias`), now also accepted as a bare
section key.

This is a deliberate, small divergence from Cargo (which always requires
`cfg()`/triple) — justified because mcpp's alias set is unambiguous, and the win is
removing the quote-noise from the overwhelmingly common per-OS case.

## Implementation (one evaluator branch)

The parser already does the right thing: `manifest.cppm`'s `[target]` loop reads
`build` / `dependencies` / … for **every** `[target.<key>]` and stores a
`ConditionalConfig{ predicate = <key> }` (manifest.cppm:1242-1275). So
`[target.linux.dependencies.compat]` is *already* parsed into a conditional config
with `predicate = "linux"`. The only gap is evaluation.

`cfgpred::matches()` (`prepare.cppm:139-146`) currently:
```cpp
inline bool matches(const std::string& predicate, const Ctx& c, std::string_view triple) {
    std::string_view k = predicate;
    if (k.starts_with("cfg(") && k.ends_with(")")) { Parser p{...}; return p.expr(); }
    return !triple.empty() && predicate == triple;   // bare-triple exact match
}
```
A bare `"linux"` falls through to the triple branch and never matches (host build →
`triple` empty; cross build → `"linux" != "x86_64-linux-gnu"`). Fix — add the
alias branch **before** the triple fallback:
```cpp
    if (predicate == "windows" || predicate == "linux" ||
        predicate == "macos"   || predicate == "unix") {
        Parser p{ predicate, 0, c };   // evaluate as the cfg bareword
        return p.expr();
    }
```
That's the whole behavioral change. Evaluation stays **target-resolved** (the
`Ctx` is built from the resolved `--target`, else host) — identical semantics to
`cfg(linux)`. No parser change, no schema change, fully backward-compatible
(`cfg(...)` and exact triples unchanged).

## Scope boundary (documented)

The sugar covers the **L1 conditional config**: `.dependencies` /
`.dev-dependencies` / `.build-dependencies` / `.build` (cflags/cxxflags/ldflags).

`[target.<triple>].toolchain` and `.linkage` remain **exact-triple only** — they
describe a *specific* cross target, not an OS family, and are looked up by exact
triple in `prepare_build`. Writing `toolchain`/`linkage` under a bare alias (or a
`cfg(...)` key) has no effect today; a follow-up may add a schema warning to flag
that footgun. Out of scope here.

## Tests

`tests/e2e/91_target_bare_alias.sh`: a project with `[target.linux.build] cxxflags`
+ `[target.windows.dependencies]`; assert on Linux the cxxflag define reaches the
TU and the windows-only dep is NOT pulled; assert `[target.'cfg(linux)']` and the
bare `[target.linux]` produce identical results (parity).

## Docs

`docs/05-mcpp-toml.md` (+ zh): the `[target.*]` section gains the bare-alias form
as the recommended spelling for single-OS, with `cfg(...)` shown for compound
predicates and the triple form for exact targets.
