# 26 — Build Optimizations

**Reader:** someone whose build compiles more than the edit seems to ask for, or
who wants to know what mcpp does to a build beyond running the compiler.

**The question this chapter answers:** which optimizations mcpp applies, which are
on, what each one changes, and how one is stated otherwise.

**Not here:** how to lay code out so that edits stay local, which is
[13 — Code and Project Style](13-code-and-project-style.md); the keys of a profile,
which are [04 — mcpp.toml](04-mcpp-toml.md); the exact rules, which are
[SPEC-019](specs/build-optimizations.md) and [SPEC-018](specs/module-rebuilds.md).

A build with none of what this chapter lists is the **standard build**: one compile
per unit, every file the compiler writes stands, and every step after it runs. mcpp
adds to that in three places — the sources, the graph, and each compile — and every
addition is an entry of one table. An entry of low risk and measured benefit is on
by default; the others are stated by a parameter.

## The entries

| Entry | Default | Parameter | Effect |
|---|---|---|---|
| `keep-bmi` | on | — | A module interface compiled to the BMI it had: its importers are not compiled |
| `keep-object` | on | — | An object compiled to the bytes it had: nothing is archived or linked for it |
| `gcm-stamps` | on | — | GCC writes two clocks into a BMI; they are not a difference |
| `gcm-locations` | on | `[profile.<name>] bmi_cutoff`, `MCPP_BMI_CUTOFF` | GCC: a BMI whose line maps alone moved is kept; importers name the earlier lines |
| `scan-pass` | on | — | The scans of a graph run before its compiles, as a pass of their own |
| `split-schedule` | off | `[build] bmi_schedule`, `MCPP_BMI_SCHEDULE` | Importers start when a BMI exists, not when its compiler exits |
| `private-fragment` | on | `[build] private_fragment`, `MCPP_PRIVATE_FRAGMENT` | A private module fragment is built as a unit of its own |

`MCPP_BUILDOPT=off mcpp build` is the standard build for one invocation: every entry
off. An entry with no parameter produces the bytes the standard build produces, so
there is nothing to choose, and that statement alone turns it off.

## An edit and the units it compiles

Measured with clang 23.1.3 and gcc 16.1.0 on a module `c` imported by `b`, which
`main` imports; the edit is to a function of `c` that is not inline.

| `c` is written as | Edit | clang compiles | gcc compiles |
|---|---|---|---|
| one file, bodies in the interface | a function body | `c`, `b`, `main` | `c` |
| one file, bodies in the interface | a comment line above a body | `c`, `b`, `main` | `c`, `b`, `main` |
| one file, bodies after `module :private;` | anything in the fragment | the fragment | the fragment |
| an interface file and an implementation file | anything in the implementation | the implementation | the implementation |

A touched file with no change compiles its own unit and nothing else, on every
compiler and host: the BMI and the object are the bytes they were.

## A private module fragment

```cpp
export module shapes;
export int area(int w, int h);
export inline int twice(int v) { return 2 * v; }

module :private;                    // from here on, nothing reaches an importer
int area(int w, int h) { return w * h; }
```

The language says the fragment is not reachable from an importer. mcpp builds it that
way: the text before `module :private;` is the interface unit, and the fragment is an
implementation unit of the same module. An edit below the line compiles that unit and
links; an edit above it reaches the importers, as it must.

- A diagnostic, debug information and `__FILE__` name the file as written.
- GCC does not implement `module :private;`. Built as a unit of its own, the file builds
  with GCC as well.
- A fragment that reads what is local to the interface's translation unit — a macro
  defined in the module's purview, an unnamed namespace, a `static` at namespace scope,
  a using-directive at namespace scope — is built as written, and `--verbose` says which
  line decided it. With a compiler that builds no fragment as written, that is a warning.
- `[build] private_fragment = "as-written"` hands the file to the compiler unchanged.

## The notes of a build

```
note  hub.cppm is the whole of module hub, which 46 units import: its function bodies are
      compiled with its interface, so an edit of one can compile all 46 again. After
      `module :private;` they are a unit of their own, and an edit of them compiles that
      unit alone
```

The note is given when the rebuild started at a module of one file with at least five
importing units, once a build directory. Beside it, a build that replaced or kept BMIs
states which ([04](04-mcpp-toml.md), `bmi_cutoff`).

## Stating an entry otherwise

```toml
[build]
private_fragment = "as-written"   # "unit" (default) | "as-written"
bmi_schedule     = "on"           # "auto" (default, = off) | "on" | "off"

[profile.dev]
bmi_cutoff = "exact"              # "ignore-locations" (default) | "exact"
```

Each has an environment name for one invocation (`MCPP_PRIVATE_FRAGMENT`,
`MCPP_BMI_SCHEDULE`, `MCPP_BMI_CUTOFF`), and `MCPP_BUILDOPT=off` covers all of them.

## Recorded, not implemented

| Optimization | Benefit expected | Needs |
|---|---|---|
| Splitting a one-file module with no `module :private;` | The effect of the fragment without the line | Semantic analysis of the source, from a language server |
| clang: keeping a BMI whose observable interface is unchanged | Body and comment edits stop at the module | A semantic difference of two versions of an interface |
| The tests a change affects, by symbol | `mcpp test --since` runs fewer tests | References across imports |
| Two versions of one library in one program | A dependency chain naming two versions builds | Renaming modules apart; headers and macros cannot be |
