# Scanner backend abstraction: per-package opt-in, P1689 as lingua franca, plan-vs-ddi reconciliation (Design)

Sister doc to `2026-07-08-index-version-semantics-and-descriptor-grammar-design.md`
(D1–D4 there; this doc is the scanner track). Goal: packages whose module units
legitimately need preprocessing to scan (upstream `src/fmt.cc` with
`#ifdef FMT_IMPORT_STD import std;`) can declare it, instead of the index
maintaining `generated_files` rewrites — while the default build keeps the
zero-cost deterministic text scan, and scan/build consistency becomes
machine-verified rather than rule-trusted.

---

## 1. Current state (grounded, 2026-07-08)

- Two scanners exist and already share one result shape:
  `scan_packages` (M1 regex/text) and `scan_packages_p1689`
  (GCC `-fdeps-format=p1689r5` / clang-scan-deps) — "Same ScanResult shape",
  `src/modgraph/scanner.cppm:81-84`.
- Selection is **global only**: `MCPP_SCANNER=p1689` env var, one lambda in
  `src/build/prepare.cppm:2544-2554`. No manifest/descriptor surface.
- Verified empirically (0.0.81): the same `#ifdef`-guarded `import std;` that
  M1 rejects builds fine under `MCPP_SCANNER=p1689` with `-D` set, and
  correctly drops the import edge without `-D` — the P1689 path evaluates
  macros truthfully.
- `dyndep.cppm` is translation-only (.ddi → ninja dyndep file); the planned
  graph is never compared against what the compiler actually saw.
- **Hazard**: the descriptor mcpp-segment parser silently skips unknown
  top-level keys (`manifest.cppm` final `else` of the key loop — this is why
  `schema = "0.1"` "works"). A `scanner = "p1689"` key on an old client is
  ignored, then the text scanner rejects the source with a confusing error.
  Schema evolution here is invisible, not loud.

## 2. Why the M1 rule is sound at all (context)

C++20 was deliberately shaped for text scanning (P1857R3): `import`/`module`
directives must be literal, line-initial, and **cannot be produced by macro
expansion**. The preprocessor's only levers over the import set are
conditional blocks (M1 bans them textually) and `#include`-carried text
(caught by §5). So "scan OK + any `-D` combination" yields the same phase-4
import set the text scan saw — M1 consumes a language property, it does not
gamble. The rule, restated: **the module graph may not depend on information
the planner cannot evaluate.** Conditions must live in a layer the planner
understands:

| condition kind | belongs in | resolution |
|---|---|---|
| platform/config (`#ifdef _WIN32 import w;`) | manifest data | lift to `cfg()` / features — planner evaluates natively |
| toolchain capability (fmt's `FMT_IMPORT_STD`) | preprocessor | declared per-package preprocess scan (this doc) |
| dead branches / compat shims | text | rewrite (`generated_files`) or upstream convergence |

## 3. Design

### 3-pre. The simplest mechanism first: declared scan overrides

For index packages the sources are **sha256-pinned — their import set is a
constant**. Recomputing a constant with compiler processes at every user's
build is the wrong cost placement; the scan belongs to *packaging time*,
declared as data:

```lua
mcpp = {
    sources  = { "*/src/fmt.cc" },           -- upstream file, verbatim
    cxxflags = { "-DFMT_IMPORT_STD" },       -- author configures the macros
    scan_overrides = {                        -- author asserts the scan result
        ["*/src/fmt.cc"] = { provides = { "fmt" }, imports = { "std" } },
    },
}
```

Files matching an override are not text-scanned; the declared unit enters the
graph directly. No preprocessing, no toolchain capability requirement, no
flag-keyed cache, identical behavior on every platform. **Assertion +
verification replaces computation** — sound because every lying-declaration
failure mode is loud at build (missing BMI / wrong BMI path / unresolvable
extra edge; no silent path), and three auditors stand behind it:

1. index CI builds every package — a wrong declaration cannot merge;
2. plan-vs-ddi reconciliation (§3d) is **mandatory for override files**: the
   compiler's own P1689 output verifies the assertion every build, catching
   even upstream-version drift precisely;
3. `mcpp xpkg parse` (sister doc D2) validates the declaration's shape.

`scan_overrides` is the recommended fix for the fmt case and its class
(pinned third-party module units with guarded imports). It also works in a
project's own `mcpp.toml` for stable files. The p1689 backend below remains
the right tool when the source is *mutable* (a project's own conditional
imports under active development) or when maintaining declarations stops
being worth it — it is the evolution path, not the first move.

Necessity ordering that follows: index floor (sister doc D3, prerequisite for
adding ANY new descriptor key — old clients silently skip unknown keys) →
single-source lint (D2) → `scan_overrides` + reconciliation (this doc) →
long brackets (D1, ergonomics, deferrable) → per-package p1689 backend
(deferred until demanded; the seam below documents where it plugs in).

### 3a. Backend model — P1689 is the wire format, not a backend

```
ScannerBackend
  name        : "text" | "p1689" | (future "exec:<tool>")
  preconds    : text  → M1 rules (unconditional imports, no header units)
                p1689 → toolchain has a P1689 producer (same capability check
                        dyndep uses, ninja_backend.cppm:258-263)
  scan(batch of PackageRoot, ScanContext) -> ScanResult   // shared shape today
```

- **"text"** — the built-in zero-cost producer with strict preconditions; the
  default forever.
- **"p1689"** — one backend, multiple drivers picked per toolchain
  (GCC built-in `-fdeps-*`, clang-scan-deps, MSVC `/scanDependencies` later);
  all emit P1689 JSON.
- **future "exec:<tool>"** — external producer protocol: given file list +
  flags, emit P1689 JSON. P1689 as lingua franca means third-party /
  self-developed scanners plug in without touching the internal model;
  `ScanResult`/modgraph stay the only internal truth.

### 3b. Selection — resolution chain, most specific wins

1. `MCPP_SCANNER` env (whole-build debug override, unchanged semantics)
2. package descriptor `mcpp = { scanner = "p1689" }` / package `mcpp.toml`
   `[build] scanner`
3. root project `[build] scanner`
4. global `~/.mcpp/config.toml`
5. default `"text"`

Granularity is **per-package** (the owner of conditional imports is the
package; it declares its own need). Per-source/per-target is deliberate
non-scope — the partition below just gets finer if ever needed.

### 3c. Execution — partition + merge at the existing seam

The `prepare.cppm` scan lambda becomes: resolve backend per package →
partition → run each backend's batch → concatenate `ScanResult`s → one
`modgraph::validate` over the merged graph. Graphs are logical-name-keyed;
merge is trivial; a text-scanned consumer importing a module provided by a
p1689-scanned package needs nothing special.

Cache keys differ per backend and must not cross-pollute:
- text units → content hash only (graph independent of flags — the whole
  point of the default);
- p1689 units → content hash + canonical flag fingerprint (reuse the plan's
  `canonical_package_build_metadata` encoding — single source, scan-time flags
  provably equal compile-time flags) + toolchain fingerprint
  (`toolchain/fingerprint.cppm`).

Precondition failure is a first-class error: "package X declares
`scanner = "p1689"`; toolchain <tc> has no P1689 producer (clang needs
clang-scan-deps)" — not a silent fallback to text.

### 3d. Consistency — plan-vs-ddi reconciliation (backend-agnostic L3)

Dyndep already makes the compiler emit per-TU P1689 `.ddi` files at build
time. Upgrade `dyndep.cppm` from pure translation to **assertion**: per TU,
diff planned `(provides, requires)` against the `.ddi`'s. Equal → the build is
certified consistent; different → hard error naming the file and the delta
("scanner assumed imports {std}; compiler saw {std, fmt} — conditional or
include-carried import?"). Zero extra compiler invocations; catches every
divergence class (include-carried imports, compiler extensions, future
language drift) without enumerating them. This closes the loop for BOTH
backends: text's construction-based guarantee and p1689's flags-keyed scan
both get observation-based verification.

### 3e. Ecosystem rollout — couples to D2/D3

- The silent-unknown-key skip makes `scanner =` invisible to old clients →
  index packages may only use it once the index floor (`index.toml
  min_mcpp`) is ≥ the version shipping this feature. Same "floor first" rule
  as the long-bracket grammar.
- `mcpp xpkg parse` (D2) learns the field, and gains `--strict`: **warn on
  unknown mcpp-segment keys**, turning the silent-skip hazard into a loud
  lint signal for all future schema evolution.
- Trust/cost: a descriptor opting into p1689 causes compiler-driven scans of
  its own sources on the consumer's machine — no new trust boundary (building
  compiles those sources anyway); cost scales with the opt-in set only.

### 3f. Payoff example

`pkgs/f/fmtlib.fmt.lua` drops its 3.4 KB `generated_files` copy entirely:

```lua
mcpp = {
    language   = "c++23",
    import_std = true,
    modules    = { "fmt" },
    scanner    = "p1689",
    include_dirs = { "*/include", "*/src" },
    sources    = { "*/src/fmt.cc" },        -- upstream file, verbatim
    cxxflags   = { "-DFMT_IMPORT_STD" },
    targets    = { ["fmt"] = { kind = "lib" } },
}
```

(D1's long brackets make `generated_files` readable where still needed;
this makes it unnecessary where the only edit was un-guarding an import.)

## 4. Non-goals / rejected

- **Global default flip to p1689** — costs a compiler-ish pass per TU for
  everyone and makes every graph flags-dependent; the text scan's determinism
  and cache-friendliness stay the default.
- **Partial macro evaluation in the text scanner** — a guessing evaluator
  fails silently; worse than refusing loudly (see sister doc's D1 rationale
  and the M1 verification experiments).
- **Per-source scanner overrides** — speculative; partition granularity can
  be refined later without model change.

## 5. Sketch / order

1. `manifest.cppm`: parse `scan_overrides` (mcpp segment + mcpp.toml);
   validate shape.
2. `scanner.cppm` / `prepare.cppm`: override-matched files bypass the text
   scan, declared units enter the graph.
3. `dyndep.cppm`: plan-vs-ddi reconciliation — mandatory for override files,
   flag-gated elsewhere at first (`MCPP_VERIFY_MODGRAPH=1`), default-on once
   stable.
4. D2 `xpkg parse --strict` unknown-key warning + override shape check.
5. Index side (after floor covers the release): migrate `fmtlib.fmt` off
   `generated_files` onto `scan_overrides`; e2e over a workspace mixing
   scanned and override packages.
6. Deferred (on demand): `scanner = "p1689"` per-package backend via the
   partition + merge seam (§3a–3c).
