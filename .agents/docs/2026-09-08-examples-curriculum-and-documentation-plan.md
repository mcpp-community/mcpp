# A curriculum for the examples, a reference for the documentation, and a check with a denominator

This plans the `examples/` tree and the `docs/` chapters together, because the
request they answer is one request: that everything about **using** mcpp and
about **developing for** it be reachable by a reader who does not already know
where to look.

It starts from a measurement rather than from an opinion about what is missing,
and it states a criterion for what earns an example so that the plan has a size
rather than an ambition.

Measured 2026-09-08 against `origin/main` at `6e1c65c6` (engine 2026.9.8.1),
`mcpp:plugins` 0.4.0, `mcpp-index` and `xim-pkgindex` at their `origin/main`.

---

## 1. The measurement this starts from

Three surfaces, each with a denominator taken from the tree rather than from a
document.

### 1.1 The manifest surface

Denominator: the sections and keys `docs/05-mcpp-toml.md` documents (34 of them
with a distinct meaning). Numerator: whether any of the 21 example projects
contains that key in its own `mcpp.toml`.

| covered by an example | 21 |
|---|---|
| **zero examples** | **13** |

The thirteen: `[dev-dependencies]`, `[features]`, `[features.<name>]`,
`[feature-deps.<name>]`, `[feature-xlings.<name>]`, `[scan_overrides]`,
`[profile.<name>]`, `[runtime]`, `[package] platforms`, `[resources]`,
`[hooks]`, `cxx_runtime`, `module_extensions` (with `bmi_schedule`,
`build_program_timeout`, `device_extensions` and `rule_module` beside it).

`[features]` is the one worth naming on its own. Every heterogeneous example
**consumes** a feature — `features = ["rules-cuda"]` on a dependency edge — and
**no example in the tree declares one**. The mechanism the entire accelerator
design rests on is visible only from the consuming side.

### 1.2 The `build.mcpp` API

Denominator: the names `docs/07-build-mcpp.md` documents (38). Numerator: the
names any example's `build.mcpp` calls.

| used by an example | 10 |
|---|---|
| **zero examples** | **28** |

Among the twenty-eight: `mcpp::action` (the primitive the whole rule layer is
built on, and the subject of a 136-line section in 07), `mcpp::runner`,
`mcpp::fact` / `mcpp::floor` (the probe channel), `mcpp::warning`,
`mcpp::has_feature`, `mcpp::xpkg_dir`, `mcpp::link_script`, `mcpp::target_os`
and the rest of the target-interrogation family.

`mcpp::action` is reached indirectly — the rule packages call it, and the
examples call the rules — so a reader sees its effect and never its shape. That
is a defensible outcome for a consumer example and not for a repository that
also asks people to write rule packages.

### 1.3 The command surface

Denominator: the commands `print_usage()` in `src/cli.cppm` prints (21
user-facing; the `dyndep` / `bmi-*` / `stage` family is excluded because, as the
source says, "nobody types it, ninja does").

| named in an example's README | 4 (`build`, `run`, `test`, `pack`) |
|---|---|
| **named in no example** | **17** |

Including the entire library-author path: `new`, `add`, `update`, `search`,
`publish`, `emit xpkg`, `xpkg parse`. And the entire diagnosis path: `why`,
`self doctor`, `self env`, `self explain`, `clean --stale`, `cache`.

### 1.4 The documentation's own shape

| | lines |
|---|---|
| `docs/` total (English) | 12,469 |
| `docs/05-mcpp-toml.md` | **3,129 (25%)** |
| `docs/01-examples.md` | 64 |

The chapter that indexes the curriculum is 64 lines and lists directories. The
chapter that documents the manifest is 3,129 lines and is simultaneously a field
reference, four conceptual essays and a compatibility record.

### 1.5 What these numbers do not say

A capability with no example is not automatically a defect. `bmi_schedule` is a
tuning key; an example directory for it would teach nothing a paragraph does not.
The numbers locate the question; §3.2 answers it.

Two denominators are themselves imperfect and are stated so. The manifest count
is taken from what `docs/05` documents rather than from what the parser accepts,
so a key the parser accepts and the document omits is invisible to it — closing
that is part of §7. The command count excludes internal verbs by judgement, and
the judgement is recorded in the check's own table rather than left in this
paragraph.

---

## 2. The defect the numbers describe

### 2.1 Two structures, each grown along its own axis

The examples are indexed by **build shape**: a program, a program with a
dependency, a static package, a workspace, a library, a cross build, a project
environment, a rule package, a device, a rendering pipeline. That ordering was
right when each new capability was a new shape. It broke at 09, where one number
acquired six sub-examples and the number line stopped being a line.

The documentation is indexed by **mechanism**: the manifest, the build program,
toolchains, the target triple, the target side, devices, heterogeneous builds.
That is the right index for a reference and it is the wrong one for a first read.

Neither is indexed by **the reader**. Someone who wants to publish a library
must know to read 10, then 02, then 12, then the `emit xpkg` paragraphs of 21,
and must discover that no example covers any of it.

### 2.2 Three readers, traced

**The library author.** Wants to publish. There is no example that goes from a
package to a descriptor to a consumer's `mcpp add`. `05-lib-distribution` stops
at the produced artifact; the descriptor half exists only in `docs/10` (160
lines) and in the index repository's own conventions.

**The rule-package author.** Wants a new device language, or a new generator.
The material exists: `docs/07` §"Writing a rule package" (115 lines inside a
944-line chapter), `examples/08-build-rules`, and — for anything about device
languages — `mcpp-plugins/README.md`, in a different repository. There is no
chapter, and `device_extensions` and `rule_module`, the two keys that make a new
device language cost no engine release, appear in no example.

**The reader who wants features.** `docs/05` §2.8 is 130 lines and correct. No
example declares `[features]`, so the reader cannot see one work.

### 2.3 A third teaching surface that nothing names

`docs/13-baremetal.md` is 800 lines and its whole first lesson is:

```bash
mcpp new blinky --template riscv-virt-rt
```

The lesson is a **template**, shipped by a package, not a directory under
`examples/`. `--template ocornut.imgui` is another. Any package may ship
`templates/<name>/`, and `src/scaffold/template.cppm` enumerates them.

So the curriculum is not `examples/` alone, and an index listing only
directories is structurally unable to be complete. This is a decision to state,
not a gap to close: a bare-metal lesson belongs to the board package that
supplies the target world, and duplicating it into this repository would make
two copies that drift. What is missing is that **nothing tells a reader the
template surface exists**.

---

## 3. Three decisions

### 3.1 The examples are a curriculum; the documentation is a reference

Neither should try to be the other. An example is ordered, runnable, and teaches
exactly one new thing relative to its predecessor. A chapter is complete,
indexed by mechanism, and assumes the reader arrives knowing what they want.

The consequence for `docs/05`: the conceptual essays inside it are chapters, not
sections of a field reference (§5.2).

The consequence for `docs/01`: it becomes the curriculum — tracks, order, what
each example is the **first** to teach, and the mapping from an intention to a
path through all three surfaces.

### 3.2 What earns an example

> A capability earns an example when it changes the **shape of a project** — the
> files it contains, the manifest it declares, or the commands its author types.
> A capability that is one line inside an existing project earns a code block in
> its chapter. A capability reached only through a command earns a scenario entry
> in `docs/21`.

Applied to §1's thirteen uncovered manifest keys:

| key | verdict |
|---|---|
| `[features]`, `[features.<n>]`, `[feature-deps]`, `[dev-dependencies]`, `[profile.<n>]` | **example** — they change what a project declares and what its author builds |
| `device_extensions`, `rule_module` | **example** — they are what a rule package IS |
| `[resources]`, `[runtime]` | **example**, folded into the publishing track rather than given their own |
| `[hooks]`, `scan_overrides`, `cxx_runtime`, `module_extensions`, `bmi_schedule`, `build_program_timeout`, `[package] platforms`, `[feature-xlings]` | **code block** — each is one line in a manifest that otherwise looks like an existing example's |

Applied to §1's twenty-eight uncovered APIs: `mcpp::action`, `mcpp::runner`,
`mcpp::fact`/`floor`, `mcpp::warning` and `mcpp::has_feature` earn an example
because a rule package is a project shape. The target-interrogation family
(`target_os`, `target_arch`, `toolchain_dir`, `sysroot_dir`, …) earns code
blocks: each is one call inside a program the examples already contain.

Applied to the seventeen uncovered commands: `new`, `add`, `search`, `publish`,
`emit xpkg` and `xpkg parse` earn an example because they are a **round trip** a
reader must see completed. `why`, `clean`, `cache`, `toolchain`, `self *` earn
scenario entries in `docs/21`, which is the chapter that exists for exactly this
and is 211 lines.

This criterion is what keeps the plan at **four new examples** rather than
thirteen.

### 3.3 One index over three surfaces

Examples, templates and chapters are indexed from one place — the rewritten
`docs/01` — and the coverage check (§7) reads that index. A capability is
covered when the index names where it is taught, and the check verifies the
named place actually teaches it.

---

## 4. The example tree

### 4.1 Tracks

The number line is replaced by six tracks. Numbering is retained inside a track
so existing links and muscle memory survive where they can.

| track | question it answers |
|---|---|
| **A — The shape of a project** | I am writing a program or a library |
| **B — Publishing** | I want other people to use it |
| **C — The environment** | my build needs tools that are not the compiler |
| **D — Targets** | it does not run on the machine that builds it |
| **E — Devices** | part of it runs on a GPU or an accelerator |
| **F — Authoring for the ecosystem** | I am extending mcpp itself, from outside it |

### 4.2 The tree

| id | directory | status | the first to teach |
|---|---|---|---|
| A1 | `01-hello` | keep | a package, `import std`, `mcpp build` / `run` |
| A2 | `02-with-deps` | keep | `[dependencies]`, the lock file, `mcpp add` |
| A3 | `04-workspace` | keep | `[workspace]`, path dependencies |
| **A4** | **`03-features`** | **new** | `[features]` **declared**, `[feature-deps]`, `[dev-dependencies]`, `[profile.<n>]`, `mcpp test` |
| B1 | `05-pack-static` | move from `03` | `mcpp pack --mode static`, `[resources]` folded in |
| B2 | `06-lib-distribution` | move from `05` | a library's interface and binaries, `[runtime]` folded in |
| **B3** | **`07-to-the-index`** | **new** | `mcpp emit xpkg` → `mcpp xpkg parse` → a descriptor → a consumer's `mcpp add`; the round trip completed |
| C1 | `08-project-subos` | move from `07` | `[xlings]`, `[xlings.workspace]`, a build program's `PATH` |
| D1 | `09-openkal-cross` | move from `06` | `--target`, one source for four machines |
| D2 | *(template)* | index only | bare metal, via `mcpp new … --template riscv-virt-rt`; owned by the board package |
| **E0** | **`10-heterogeneous/boundary`** | **new** | the island ladder: a consumer importing the **generated** module with no hand-written seam (§4.4) |
| E1 | `10-heterogeneous/cuda` | keep | the seam, a generated boundary, the driver as a fact and a floor |
| E2 | `10-heterogeneous/vulkan` | keep | a shader payload reached as a module |
| E3 | `10-heterogeneous/sycl` | keep | a second compiler with its own standard library |
| E4 | `10-heterogeneous/hip` | keep | the hand-written boundary, as the contrast to E1 |
| E5 | `10-heterogeneous/cann` | keep | a vendor outside the NVIDIA and Khronos lineages |
| E6 | `10-heterogeneous/multi-backend` | keep | several backends in one artifact, chosen at run time |
| E7 | `11-graphics/offscreen` | keep | a rendering pipeline whose result is pixels |
| F1 | `12-build-rules` | move from `08` | `host-module = true`, `mcpp::action` with `role = "check"` |
| **F2** | **`12-a-new-device-language`** | **new** | `device_extensions` + `rule_module`: a third-party rule package teaching mcpp a language the engine has never heard of |

Four new directories; five renumbered; nothing deleted.

### 4.3 What each new example is for, and its criterion

**A4 `03-features`.** A library with an optional backend. Declares
`[features]` with a default set, `[feature-deps]` bringing a dependency in
behind one, `[dev-dependencies]` for its tests, and a `[profile.release]`
override. Its `build.mcpp` calls `mcpp::has_feature`.
*Criterion:* `mcpp build` with no features resolves a graph that names no
package belonging to the optional backend — the criterion `docs/20` already
states for the framework tier, applied to the smallest project that has it.

**B3 `07-to-the-index`.** Two directories: a library, and a consumer. The
README walks `mcpp emit xpkg` to produce a descriptor, `mcpp xpkg parse
--json` to validate it, a local index registration, and the consumer's `mcpp
add`. *Criterion:* the consumer builds against the descriptor rather than
against a path dependency. This is the one example whose subject is the package
manager rather than the build system, and its absence is why seven commands have
no example.

**E0 `10-heterogeneous/boundary`.** The ladder's bottom rung. `MCPP_EXPORT_C`
entry points, `mcpp.tools.island` generating the header and the module, and a
`main.cpp` that writes `import app.kernels;` — **no hand-written seam anywhere in
the tree**. Its README states what that costs: a C-shaped interface, and no place
for `cfg(accelerator = …)` to apply. E1 then reads as "and here is why you
usually add one".
*Criterion:* the project contains no `.cppm` other than generated ones, and the
program prints the right numbers. The plugins README claims this arrangement was
measured with GCC 16.1; nothing in either tree runs it, so this example is also
that claim's fixture.

**F2 `12-a-new-device-language`.** A rule package in the example tree that
declares `device_extensions = [".toy"]` and `rule_module`, and a consumer whose
`[build] sources` names a `.toy` file. The "compiler" is a shell script that
transforms text, exactly as `tests/e2e/607` uses `cat` as a device linker: the
subject is the graph, not a vendor.
*Criterion:* the `.toy` file is compiled and its output joins the link, on an
engine release that has never heard of `.toy`. That is the property `docs/20`
credits the design with, and today its only instance is `rules-slang` in another
repository.

### 4.4 The island ladder, which this round's discussion surfaced

`mcpp.tools.island` generates two artefacts — the `extern "C"` header the device
compiler reads, and a module over it whose whole content is
`export using ::name;`. Four rungs exist in the mechanism today:

| rung | who writes what | the consumer sees |
|---|---|---|
| L0 | nothing but marked entry points | `import app.kernels` — C-shaped |
| L1 | `scan()` generates; the project writes a seam | `import app.saxpy` — designed |
| L2 | `emit()` takes an explicit list; the project writes a seam | as L1 |
| L3 | the project writes header and module by hand | as L1 |

Three of the four are documented in one paragraph of
`mcpp-plugins/README.md`; `tools-island` appears **zero times** in `docs/`. All
seven current examples are L1 or L3, so a reader cannot see L0 exist.

The plan therefore does two things and not a third: it adds E0 and it documents
the ladder in the new chapter 23. It does **not** propose generating the seam's
C++ shape — `tools/island.cppm:29-31` states that "which functions, which types,
what happens on failure — is a design decision no generator makes well", and
overturning that is a design question of its own, listed in §10.

---

## 5. The documentation chapters

### 5.1 An entry layer indexed by the reader

`docs/README.md` becomes a role index before it is a chapter list. Seven rows,
each naming an ordered path through all three surfaces:

| I want to | read | run |
|---|---|---|
| write a program | 00, 05 §1 | A1, A2 |
| write a library others import | 10, 05 §2.4 | A3, A4 |
| publish it | 02, 10, 12 | B1, B2, B3 |
| build for another machine | 16, 15, 13 | D1, template `riscv-virt-rt` |
| use a GPU or an accelerator | 20, 18 | E0 → E1 → the rest |
| add a rule, a language or a generator | **23 (new)**, 07 | F1, F2 |
| add a package to the index | 10, `specs/package-identity` | B3 |
| change mcpp itself | 04, 09, 19 | — |

### 5.2 Splitting `docs/05`

`05` keeps the field reference and loses the essays, each of which goes to a
chapter that exists and is short:

| section | lines | destination |
|---|---|---|
| §2.8, §2.8.1, §2.8.2 — features and capabilities | 394 | **new 22 — Features and capabilities** |
| §2.13 `[xlings]` — the project's environment | 317 | **17** (currently 199 lines) |
| §2.3 `cxx_runtime` — the C++ runtime contract | 242 | **03 — Toolchains** |
| §2.7.1 `[target.*]` conditioning | 138 | **14 — The target side** |
| §2.14 host tools from a dependency | 274 | **07 — build.mcpp** (which already has a section) |
| §2.16 `[hooks]` | 192 | **21 — Commands by scenario** |

`05` lands at about 1,570 lines and becomes readable as what it is.

### 5.3 Two chapters that must exist

**22 — Features and capabilities.** Assembled from `05` §2.8; gains A4 as its
worked example. Nothing new is written except the example's walkthrough.

**23 — Authoring a rule package.** New, and the chapter whose absence §2.2
traces. It assembles: `docs/07` §"Writing a rule package"; `mcpp::action` roles
and the chained-action shape; `device_extensions` and `rule_module`; the island
ladder (§4.4) with its four rungs; the probe channel (`fact` / `floor`); the
advisory channel (`warning`); and the payload-declaration discipline
(`[feature-xlings]` under a `cfg(accelerator = …)` selector — two gates). It
points at `mcpp-plugins` for the shipped collection rather than restating it,
and it names F1 and F2 as its examples.

### 5.4 The chapter table afterwards

00–21 keep their numbers and meanings. 22 and 23 are added. `01` is rewritten as
the curriculum. `README.md` gains the role index. No chapter is removed.

---

## 6. Defects to fix in the same batch

Found while measuring, each independently verifiable:

1. `examples/09-heterogeneous/README.md` never mentions that the boundary is
   generated in two of its six sub-examples, while `docs/01`'s table does.
2. The same README's prose says "all four" three times and "The four beside it"
   once, over a table listing six.
3. All seven heterogeneous and graphics examples pin
   `plugins = { version = "0.3.0" }`. 0.4.0 is released and is the version in
   which all six rules pass a depfile; a project copied from an example today
   does not rebuild when a shader's `#include` changes.
4. `examples/09-heterogeneous/cuda/app/build.mcpp:20` states that "no source in
   this project names a generated file", and line 51 of the same file states the
   opposite, correctly — `src/cpu/saxpy.cpp:18` includes it.
5. `tools-island` appears zero times in `docs/`; `MCPP_EXPORT_C` appears twice,
   both inside one table row. Closed by chapter 23.
6. `.cl` and `.metal` are in the engine's built-in device-extension table and no
   published rule package claims either, so both produce a refusal. Either a rule
   ships or the refusal says that no package claims the extension. This one is
   engine or ecosystem work rather than documentation, and is recorded here
   because the documentation currently implies support that does not exist.

---

## 7. The check that keeps this true

A capability added after this batch must be classified, or the build fails.

**What it reads.** Three denominators, from the tree:

- manifest keys, from the parse sites in `modules/manifest/src/` — not from
  `docs/05`, which is what makes the check able to catch a key the document
  omits;
- the `build.mcpp` API, from the exported names in `modules/buildmcpp/src/`;
- commands, from the `print_usage()` body in `src/cli.cppm`.

**What it compares them against.** One checked-in table, `docs/coverage.toml`,
with exactly one row per capability and exactly one of three verdicts:

```toml
[manifest."[features]"]
example = "examples/03-features"

[manifest.bmi_schedule]
doc = "docs/05-mcpp-toml.md#build-concurrency"
reason = "one key in a manifest that otherwise looks like 01-hello"

[api."mcpp::action"]
example = "examples/12-build-rules"
```

**What it refuses.**

1. A capability in the tree with no row — a new key cannot ship unclassified.
2. A row naming an `example` whose project does not actually contain the
   capability. For a manifest key the check parses that example's `mcpp.toml`
   and asserts the key is present; for an API name it asserts the call appears
   in that example's `build.mcpp`; for a command it asserts the README's command
   block contains it.
3. A row naming a `doc` anchor that does not resolve.

**Why the second refusal is written that way.** The obvious check — grep the
README for the key's name — passes on a document that merely mentions the key,
and fails the day someone rewords a heading. This repository has paid for that
shape before: a criterion that greps prose when it means to ask about state. The
check therefore reads the example's **manifest and sources**, and the prose is
not its subject.

**What it cannot decide.** That an example teaches its subject *well*. The check
establishes that the capability is present where the index says it is; review
establishes the rest.

---

## 8. Staging

Each stage is independently mergeable and leaves the tree better than it found
it.

| stage | content | why here |
|---|---|---|
| **1** | §6 defects 1–5; bump the seven examples to `plugins` 0.4.0 | pure corrections, no restructuring, and 3 is a live defect in copied projects |
| **2** | rewrite `docs/01` as the curriculum; add the role index to `docs/README.md`; name the template surface | the index must exist before things are moved into it |
| **3** | E0 and chapter 23 | the two halves of the gap this round's discussion found; E0 is also the fixture for a measured claim that nothing runs |
| **4** | A4 and chapter 22; split `[features]` out of `05` | the largest single uncovered mechanism |
| **5** | F2; finish the `05` split | rule authoring, which chapter 23 has by then described |
| **6** | B3 | the round trip; largest new example, and it needs a local index fixture |
| **7** | `docs/coverage.toml` and the check | last, because it is only enforceable once the rows it would demand exist |

Renumbering (B1, B2, C1, D1, F1) happens in stage 2 with redirects left in
`docs/01`, so no stage both moves directories and adds content.

---

## 9. What this deliberately does not do

- **It does not delete an example.** Every current example teaches something no
  other one does; the defect is the index, not the set.
- **It does not add an example per uncovered capability.** §3.2 is what keeps
  four from becoming thirteen, and the criterion is stated so a future addition
  is argued rather than assumed.
- **It does not duplicate the bare-metal lesson into `examples/`.** It belongs to
  the board package that supplies the target world; what changes is that the
  index names the template surface.
- **It does not restate `mcpp-plugins` in `docs/`.** Chapter 23 describes the
  authoring surface and points at the collection; a second copy of a rule
  collection's reference would drift on its own schedule.
- **It does not touch the Chinese translations' content policy.** Every chapter
  added or split needs its `docs/zh/` counterpart in the same change, which CI
  already enforces; the cost is counted in each stage.

---

## 10. Open questions for review

1. **Renumbering.** Stage 2 moves five directories. The alternative is to keep
   today's numbers and let the tracks be a documentation-only grouping. Moving
   makes the tree self-describing and breaks external links; not moving keeps
   `03-pack-static` sitting between two basics. *Recommendation: move, with a
   redirect table in `docs/01`.*

2. **Whether the seam's C++ shape should ever be generated.** §4.4 leaves this
   open deliberately. Generating it requires the generator to read intent it
   cannot see today — that a pointer and a count are a span, that a returned
   `int` is an error code — which is an IDL by another name. The question worth
   deciding first is not "can we" but "how much convention before the generator
   becomes a thing the design says it is not".

3. **B3's index fixture.** The round trip needs an index to register against. A
   local file-backed index in the example directory is self-contained and is not
   what a real publisher does; using `mcpp-index` makes the example unrunnable
   for a reader without publish rights. *Recommendation: local index, with the
   README stating exactly which step differs in the real one.*

4. **Whether `docs/21` should absorb the diagnosis commands or gain a sibling.**
   Seventeen commands land there under §3.2. At 211 lines it can take them; at
   double that it becomes the chapter nobody reads. *Recommendation: absorb now,
   split when it passes 400 lines.*

5. **Defect 6 (`.cl` and `.metal`).** Ship a rule package for one of them, or
   change the refusal to state that no package claims the extension? The second
   is small and honest; the first is ecosystem work with its own schedule.
