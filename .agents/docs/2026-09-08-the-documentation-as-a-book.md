---
subject: docs
status: active
---

# The documentation as a book: a chapter-by-chapter design

The previous record in this series
(`2026-09-08-documentation-architecture-three-trees.md`) settled **which tree** a
document belongs to and published that architecture. It did not design the book
inside `docs/`. It grouped the chapters that existed, renumbered them into bands,
and moved six sections out of an over-large reference. That is a
**reorganisation**. This document is the **design**: for every chapter, who reads
it, the one question it answers, what it contains, what it must not contain,
where it sits, why there, and how it is judged.

Written because a reorganisation cannot fix what the measurements below show. A
tree indexed by mechanism gives a *task* no home, and no amount of renumbering
gives it one.

---

## 1. What the measurement says, and why renumbering does not answer it

### 1.1 No topic has an owner

A topic is *owned* by the chapter that carries it as a `##` section.

The first measurement counted headings whose text contained the topic's name,
and **that criterion was wrong in exactly the way this repository's own style
skill forbids** — a substring search standing in for a question about meaning.
It reported testing as a section in seven chapters. Reading those seven:

| chapter | what its "test" section is actually about | verdict |
|---|---|---|
| `90-build-from-source` ×2 | **mcpp's own** test suite, for a contributor | a different subject |
| `50-machine-output` | the `--message-format json` schema | it owns that |
| `04-dependencies` | the `[dev-dependencies]` table | it owns that |
| `01-getting-started` | one step of a tutorial | legitimate; should link |
| `06-workspace` | the workspace fan-out | legitimate; should link |
| `11-publishing-a-library` | path overrides, under a heading that says "testing" | a naming defect |
| `03-mcpp-toml` | a worked example that happens to have tests | worked examples leave `03` |
| **`40-baremetal`** | **how a test runs, restated** | **the one real duplicate** |

So the corrected reading: **one** duplicated explanation, one misnamed heading,
and a worked example that a separate decision moves anyway.

**The conclusion survives the correction, and it is the part that mattered.**
Before `07` existed, no chapter answered "how do I test" — the seven partial
mentions each assumed a reader who already knew, and none of them was the place
to learn it. A topic can be unowned without being duplicated, and this one was.

The same caution applies to the other rows of the first measurement, which are
kept here as what they are — a count of headings, not of explanations:
dependency resolution 7, C++ modules 7, diagnosis 4, caching 4, and **the model
0**. The last is the one that needs no re-reading: a section that does not exist
cannot be miscounted.

### 1.2 There is no model to hang anything on

`00-getting-started` goes from *Installation* to *Creating a Project* with no
section in between. mcpp is a build system, a package manager and a toolchain
provisioner in one program; a reader who does not know that reads every
subsequent chapter as an unrelated feature. The five nouns the whole tree uses
— package, graph, toolchain, target, index — are defined nowhere.

### 1.3 The chapters were never given a shape

| property | chapters that have it |
|---|---|
| an opening that names its reader and its question | **5 of 24** |
| a "Current limitations" section | **6 of 24** |
| length within 2× of the median (≈420 lines) | 18 of 24 |

`02-mcpp-toml` is 1,626 lines and `05-build-mcpp` is 1,218; `51-supported-versions`
is 90. Nothing decided any of those.

### 1.4 The conclusion

> Grouping and renumbering fix the **index**. They do not fix the **book**. A
> chapter that was written because a mechanism existed keeps being about the
> mechanism, and a reader with a task keeps not finding it.

---

## 2. The method: design from the reader's task, not from the mechanism

Three rules produce every decision in §4 and §5.

**R1 — One owner per topic.** Exactly one chapter owns a topic. Every other
chapter that touches it states one sentence and links. A second explanation is a
second thing to keep current, and the two diverge on their own schedule.

**R2 — A chapter exists for a reader with a task, not for a mechanism with a
name.** "Features" is a mechanism; "make part of a package optional" is a task.
Where the two coincide the chapter keeps the mechanism's name, because that is
what the reader searches for — but the *contents* are decided by the task.

**R3 — Every chapter states its reader, its question, and its exclusions in its
first fifteen lines.** The exclusions are the load-bearing half: they are what
stops the chapter from re-absorbing the topics R1 assigned elsewhere.

A fourth rule governs the sequence rather than a chapter:

**R4 — A part is a reader's arc, and its order is the order that reader needs
it in.** Not alphabetical, not chronological by when the feature shipped.

Two more govern how a section is written, and both are about not handing the
reader a decision that the design already made:

**R5 — One recommended path in the body; every other spelling in a
`<details>`.** mcpp supports more than one way to say many things, and it has a
style and a semantics of its own, so there is always a default. The body carries
that one. Legacy spellings, escape hatches and platform-specific forms fold
away. The criterion: a reader who reads only the body and opens nothing can do
the thing correctly without choosing. Folding a form away is not deprecation;
deprecation is said in words.

**R6 — An increment is marked beside itself.** A key, flag or behaviour added
later carries its version floor on its own line (`2026.9.6.5+`), not at the top
of the chapter and never as "it used to be X".

All six are recorded in `.agents/skills/mcpp-docs-style` so they outlive this
batch.

---

## 3. The model the book teaches, stated once

Everything in `docs/` is about five nouns and the seams between them. This is
the content of the new chapter §4.1 specifies, and it is written here first
because the chapter list is derived from it.

| noun | what it is | the chapter that owns it |
|---|---|---|
| **package** | a directory with an `mcpp.toml`; identity is `(namespace, name)` | 02 |
| **graph** | what a build is: sources compile, objects link, actions extend | 05 |
| **toolchain** | a payload mcpp installs and pins, not a program found on the machine | 20 |
| **target** | the machine the artifact runs on, which is not the host | 21 |
| **index** | where packages come from, and what a descriptor promises | 11 |

The seams, each of which is a chapter rather than a section, because each is a
place two nouns meet and a reader arrives with a question about the meeting:

| seam | question a reader arrives with |
|---|---|
| features | how do I make part of a package optional |
| workspaces | how do several packages become one build |
| the target side | how does a manifest say "only on this target" |
| rules | how do I teach the graph something it has no rule for |
| devices | how do I run an artifact on a machine that is not this one |
| accelerators | how does part of my program get compiled for a device |

**Why the model is a chapter and not a paragraph in `00`.** `00` is a tutorial:
a reader following it is typing, not building a mental model, and a model
inserted there is read as preamble to skip. The model chapter is short, it is
the first thing the index points at, and every other chapter may assume it.

---

## 4. Chapters this design creates

Three, each answering a question §1.1 measured as unowned.

### 4.1 `00 — How mcpp Works` (new; current `00` becomes `01`)

| | |
|---|---|
| **reader** | anyone, before anything else. Assumed by every other chapter |
| **question** | what are the moving parts, and which one is failing when something fails |
| **contains** | the five nouns of §3 with one paragraph each; the three seams a first build crosses (manifest → graph → toolchain); where each noun's state lives on disk (`target/`, the store, the index cache); the one diagram |
| **excludes** | any field name, any flag, any command beyond `mcpp build`. It is a model, not a reference |
| **why first** | §1.2. Without it every later chapter is an unrelated feature |
| **criterion** | a reader who has read only this chapter can say which noun a given error message is about |

### 4.2 `04 — Dependencies and Resolution` (new)

| | |
|---|---|
| **reader** | someone whose build now has more than their own code in it |
| **question** | where does a dependency come from, what does a version constraint mean, and what happens when two of them disagree |
| **contains** | `[dependencies]` in all its forms (index, path, git); what a bare version pins and what `>=` requires; one package one version, and the refusal when it cannot hold; `mcpp.lock` and `--locked`; `mcpp add` / `update` / `why`; `[dev-dependencies]` and `[build-dependencies]` and the difference in what each reaches |
| **excludes** | the *identity* rules (SPEC-001 owns them) and how to publish (11 owns that) |
| **why here** | it is the second thing every reader does, and today it is seven partial answers |
| **criterion** | a reader can predict which version resolves for a stated graph, and say why |

### 4.3 `06 — Testing` (new)

| | |
|---|---|
| **reader** | anyone with code that has to keep working |
| **question** | how do I run tests, what does mcpp consider a test, and how do I test what does not run on this machine |
| **contains** | `tests/**/*.cpp` as the convention; `mcpp test` and its selectors; `[dev-dependencies]`; the worker pool and `run_exclusive`; testing a cross or bare-metal target through a runner; `--message-format json` for a CI consumer, by reference |
| **excludes** | the runner *mechanism* (31 owns it) and the JSON schema (50 owns it) |
| **why here** | §1.1: seven chapters mention it, none owns it |
| **criterion** | a reader can run a test on a target their machine cannot execute |

---

## 5. The book

Bands are meaningful: the first digit is the part. Within a part the order is
the order that part's reader needs, which is R4.

### 5.1 `0x` — Fundamentals

The arc: understand the parts, get one program running, know where the examples
are, write the manifest, add dependencies, make things optional, test.

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 00 | How mcpp Works | anyone | what are the moving parts | fields, flags |
| 01 | Getting Started | a newcomer, typing | how do I get a program running | anything not on the path to a running program |
| 02 | Examples | a reader choosing a starting point | which example teaches what I need | the content of the examples |
| 03 | The mcpp.toml Manifest | an author | what may a manifest say | conditioning (22), features (05), the environment (23) |
| 04 | Dependencies and Resolution | an author with dependencies | where does a dependency come from, and which version wins | identity (SPEC-001), publishing (11) |
| 05 | Features and Capabilities | an author with something optional | how do I make part of a package optional | accelerator selection (32) |
| 06 | Testing | anyone | how do I run tests, including where they cannot run here | runners (31), the JSON schema (50) |
| 07 | Commands by Scenario | anyone, later | which command does the thing I want | everything each command means in depth |

**Why `07` is last and not first.** It is a lookup, used after the reader knows
the nouns. A reader who opens it first gets a list of verbs with no model.

**Why the manifest (03) precedes dependencies (04) and features (05).** Both are
manifest tables; a reader who has not seen a manifest cannot place them.

### 5.2 `1x` — Publishing

The arc: package an application, publish a library's source, publish its
binaries.

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 10 | Packaging an Application for Release | someone shipping a program | how do I produce something another machine can run | libraries (12) |
| 11 | Publishing a Library to mcpp-index | a library author | how does my package become one others can name | the descriptor grammar (SPEC-001) |
| 12 | Distributing a Prebuilt Library | a publisher of binaries | how do I ship compiled artifacts and state what they are compatible with | the compatibility tag's grammar (SPEC-005, planned) |

**Why publishing precedes targets.** A library author publishes before they
cross-compile; an application author packages before they port. The reader who
needs `2x` knows they need it.

### 5.3 `2x` — Toolchains and targets

The arc: what a toolchain is and how it is chosen, how a target is named, how a
manifest conditions on one, what environment the project declares, and one
worked cross-compilation.

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 20 | Toolchain Management | anyone whose compiler matters | which compiler will build this, and how do I choose another | internals (91) |
| 21 | The Target Triple | someone building for another machine | how is a target named, and which are supported | conditioning (22) |
| 22 | The Target Side | an author supporting several targets | how does a manifest say "only there" | the accelerator axis (32) |
| 23 | The Project Environment | an author whose build needs tools | how does a project declare the tools its build runs | build programs (30-band) |
| 24 | Cross-Compilation Over openkal | someone cross-building a hosted target | how do I build for another OS from this one | bare metal (30) |

### 5.4 `3x` — Extending the graph

**This part is new as a grouping**, and it is where the design departs most from
what shipped. Build programs and rule packages are the same subject at two
scales: a project that needs the graph to do something it has no rule for, and a
package that supplies that rule to others.

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 30 | Build Programs: `build.mcpp` | an author whose build needs a step mcpp has no rule for | how do I add work to the graph | authoring a reusable rule (31) |
| 31 | Authoring a Rule Package | an ecosystem author | how do I package that step so other projects can use it | the shipped rules' spellings (`mcpp:plugins`) |

**Why they are two chapters and not one.** Different readers with different
questions. The first has a project and a problem; the second has an audience.
The 1,218 lines of the current build-program chapter contain both, and its
"Writing a rule package" section is now a second copy of chapter 31.

### 5.5 `4x` — Devices and accelerators

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 40 | Bare-Metal and Freestanding Targets | an embedded developer | how do I build for a machine with no OS | reaching it (41) |
| 41 | Reaching a Device | anyone whose artifact does not run here | how do I run and test it where it belongs | the target's construction (40) |
| 42 | Heterogeneous Builds | a GPU or accelerator developer | how does part of my program get compiled for a device | rule authoring (31) |

**Why bare metal moved out of `3x`.** It is not a toolchain topic; it is a
target with no operating system, and everything a reader needs after that is
about reaching it and running on it — which is 41. The three chapters are one
arc.

### 5.6 `5x` — Contracts for programs

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 50 | Machine-Readable Output | a tool or CI author | what may a program parse, and what is versioned | human-facing output |
| 51 | Supported Versions and Compatibility | anyone with a policy question | what may change between releases, and what may not | the exit-code table (SPEC-003) |

### 5.7 `9x` — mcpp itself

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 90 | Building from Source and Contributing | a contributor | how do I build and change mcpp | how a user builds their project |
| 91 | Toolchain Internals | a contributor, or a user debugging a toolchain | how does mcpp actually resolve and assemble a toolchain | how to *choose* one (20) |
| 92 | Releasing mcpp | a maintainer | how is a release cut and verified | publishing a package (11) |

---

## 6. What this changes against what shipped

| decision | effect |
|---|---|
| three new chapters (00 model, 04 dependencies, 06 testing) | the three topics §1.1 measured as unowned get an owner |
| build programs move `0x` → `3x`, beside rule authoring | the two scales of one subject become one part |
| bare metal moves `3x` → `4x`, before devices | the embedded arc reads in order |
| every chapter gains a designed opening (reader, question, exclusions) | 5 of 24 have one today |
| every reference chapter gains a limits section | 6 of 24 have one today |
| the manifest reference sheds its worked examples and its appendix | `examples/` owns worked examples; the schema-ownership appendix is contributor material |
| `mcpp test` mentions in six chapters become one sentence and a link | R1 |

**The renumbering this implies is the second in one batch, and that is the cost
of having reorganised before designing.** It is paid once here; §7 states the
order that keeps every citation resolving while it happens.

---

## 7. Order of work

1. Write `00`, `04` and `06` against their specs in §4. New content, no moves.
2. Apply R1: for each of the three topics, cut the six other explanations to a
   sentence and a link.
3. Renumber into §5's bands, in one scripted pass with the link and label
   rewrite the previous batch established.
4. Give every chapter its designed opening (R3) and, where it is a reference, a
   limits section.
5. Split the manifest reference: worked examples out, appendix to `9x` or
   SPEC-004.
6. Re-run the review in `.agents/skills/mcpp-docs-style` §13 against the eight
   dimensions, with §5's table as the criterion for "面向人群" and "梯度".

Steps 1 and 2 are the ones that make the book different. Step 3 is mechanical
and has a tested script. A batch that stops after step 3 has renumbered twice
and designed nothing, which is the failure this document exists to name.

---

## 8. What this design does not claim

- **It does not claim the current chapters are badly written.** Most are
  accurate and several are excellent. The defect is that nothing decided what
  each one is *for*, so topics landed wherever a mechanism needed them.
- **It does not merge chapters to reduce their number.** 24 chapters for a tool
  that is three tools is not too many. Two of them are too long, and one
  grouping was wrong; that is the whole of the structural change.
- **It does not settle the specifications' language.** That question stands
  where the previous record left it.

---

## 11. What is open after this batch, and the criterion for each

### 11.1 Seventeen reference chapters have no limits section

The style skill calls the section mandatory for a reference chapter. Two
chapters had the content under another heading and are renamed; the scenario,
model, tutorial and index chapters are exempt, because their scope is stated by
the "Not here" line in their opening and they claim no complete surface.

Eight of the original seventeen were written from facts the chapter already
stated somewhere in its body — `05`, `20`, `21`, `22`, `23`, `41`, `50`, `51`.
That is the method that works: **promote a limit the chapter already states into
the section that collects them**, rather than inventing one.

Eight remain: `04`, `07`, `10`, `11`, `24`, `90`, `91`, `92`. Scanning their
text for a limit statement returns nothing usable, which is the honest reading —
each needs a fact its area's owner can state.

**They are open rather than written, and the reason is the rule itself.** A
fabricated limits section satisfies the check and measures nothing, which is
worse than the section being absent — the reader then believes the boundary has
been stated. Writing one requires a fact its area's owner can state and that a
reader can reproduce.

*Criterion for closing one:* the section lists facts, each of which can be
reproduced on the current release, and each of which a reader could otherwise
only discover by hitting it.

### 11.2 Four chapters present several spellings as equals

`04` (50 code blocks), `20` (35), `30` (37) and `40` (33) carry no `<details>`,
which means every spelling in them is offered to the reader at the same weight.
R5 says one recommended path in the body and the rest folded away. `05` is the
one section converted so far, and it is the shape the other four follow.

*Criterion:* a reader who reads only the body and opens nothing can do the thing
correctly without choosing.

### 11.3 The manifest reference still holds worked examples and an appendix

`04` §3 is six worked examples, which `examples/` owns, and Appendix A is the
admission criteria for new manifest fields, which is contributor material and
belongs to `9x` or to SPEC-004. Both were named in §6 and neither is moved yet.

### 11.4 The lookup index has no check

§10's index is hand-built and correct today. Nothing compares it against the
reference chapter's own section list, so a key added to `04` and not indexed is
invisible. The check is the same shape as rule 4 (every specification appears in
every index) and is one loop.

---

## 12. The ecosystem side, which the first design missed

§5 designed the book for three readers — someone using mcpp, someone extending
one project's build, and someone changing mcpp. It missed a fourth, and the
measurement that found it is the sharpest in this record:

> `xim:` payloads are named **84 times across 12 chapters**. How to make one is
> explained **nowhere**. `xim-pkgindex` appears 17 times, all of them in the
> chapter about releasing mcpp itself.

Every toolchain, every device toolkit, every shader compiler and every emulator
in this ecosystem is a payload. The documentation taught the whole of consuming
them and none of producing them, and the omission was hidden by how often they
are mentioned.

Two more of the same shape: a `compat:` runtime adapter — the layer that makes a
host library reachable from an artifact on Linux — is named in four chapters and
authored in none; and a board-support package is described from the consumer's
side in `40` and `41` with no chapter on writing one.

### 12.1 Three chapters, and the band they complete

`3x` was "extending the build graph" and is now **extending mcpp and its
ecosystem**: the two scales of one project's own step, then the three kinds of
package that serve everyone else.

| # | chapter | reader | the one question | excludes |
|---|---|---|---|---|
| 32 | Authoring a Payload | someone packaging a tool or a prebuilt library | what is an `xim:` payload made of, and what must its descriptor say | source packages (11), host libraries (33) |
| 33 | Authoring a Runtime Adapter | someone making a host-supplied library reachable | why an artifact cannot see a library that is installed, and what fixes it | anything redistributable, which is a payload (32) |
| 34 | Authoring a Board-Support Package | someone bringing up a board | what does a BSP supply, and how does one package serve an emulator and hardware | using a BSP (40), the runner a consumer sees (41) |

Each is written from a real published package rather than from the mechanism:
`xim-pkgindex/pkgs/g/glslang.lua`, `mcpp-index/pkgs/c/compat.vulkan-runtime.lua`,
and `mcpplibs/cortex-m-rt`. Every fact in them was read out of those files.

### 12.2 The decision the adapter chapter records, which is not a packaging choice

A proprietary driver's userspace is in ABI lockstep with a kernel module and its
licence forbids redistribution. Neither is solved by effort, so it is modelled
as a **host capability** and the adapter is how an artifact reaches it. An open
driver takes the other answer — it is a payload, and a machine using one needs
no adapter. **Which answer applies is decided by the licence and the ABI, not by
preference**, and that is the sentence a reader of scenario 10 leaves with.

### 12.3 What this changes about §5's claim

§5 said 24 chapters for a tool that is three tools is not too many. The count is
31 now, and the reason is that the tool is four things rather than three: a
build system, a package manager, a toolchain provisioner, **and an ecosystem
other people publish into**. The fourth had no chapters at all.

---

## 13. Chapter 00 was designed wrong, and the review said so

§4.1 specified `00 — How mcpp Works`: five nouns, three seams, where state lives
on disk. It was written, it was accurate, and it was the wrong chapter.

**The review's verdict was that a general user does not care about the operating
principle.** What a reader opening the first chapter wants is what mcpp is, what
it can do, what its advantage is, and one example they can actually run.

The design failed two of its own rules to get there:

- **R2** — a chapter exists for a reader with a task. "Understand the machinery"
  is not a task a first-time reader has; it is a task a maintainer has, and it
  already has chapters in `9x`.
- **R7** — an advantage is shown by the artifact, not by the mechanism. A model
  chapter explains how the advantage is produced and never demonstrates it.

`00 — What mcpp Is` replaces it, and its shape came from the review too:
**background → who pays → what mcpp is → the guarantee → the smallest example
that shows it.** A first chapter earns the definition by first stating the
problem, and only then shows it solved.

1. **The definition**, in the form the reviewer gave: mcpp = build system +
   build plugins + package manager + toolchain management + the environment and
   runtime (xlings), in one program.
2. **An analogy table** — CMake/Meson, `build.zig` and xmake rules, Conan/vcpkg,
   Zig's bundled toolchain and rustup, Nix/conda — so a reader with existing
   tools can place each part, with the disclaimer that places rather than
   equates. Named beside it: **Cargo and Zig are the closest single-tool
   analogues**, and for different halves of the same idea — Cargo for one
   program being build, packages, lock and tests, Zig for the toolchain shipping
   with the tool. The part neither has is the environment layer, which is why a
   project can declare the non-compiler tools its build needs.
3. **The guarantee, stated once**: clone any mcpp project and `mcpp build`
   works, without installing a compiler, configuring an environment, or hunting
   dependencies. Plus the two boundaries that make it trustworthy.
4. **A session that was run**, with its real output: a five-line manifest, no
   declared compiler or standard, `import std` compiling on a machine whose own
   `g++` is 13.3.0 and cannot, and `mcpp self env` showing the GCC 16 mcpp
   installed. 1.25 s of wall clock including the first run.
5. **What mcpp is for**: modules and the newest language features, and the
   ecosystems that follow — embedded, heterogeneous and GPU, graphics, kernel
   work.

The one part of the old chapter a *user* wanted — the table from a message's
shape to the stage that produced it — moved to `09`, beside the other
diagnosis scenarios. The rest is deleted rather than relocated.

**What this says about the method.** The seven-cell spec was filled for the old
chapter and it still produced the wrong chapter, because the cell that decides
everything — the reader — was answered with "anyone" and then served as if that
meant "someone who wants the model". A reader cell that names no task is not
filled in.

**And the opening block came off.** Every other chapter opens with reader,
question and exclusions; on the front door that block reads as machinery. `00`
has no "not here" to declare because everything else *is* elsewhere, which its
closing paragraph says in a sentence instead. Rule 11 exempts `00` by name, with
that reason in the script.
