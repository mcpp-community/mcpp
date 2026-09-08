---
subject: docs
status: active
---

# Three documentation trees, three audiences, and the rule for citing between them

This restructures mcpp's documentation as a whole: the user documentation
(`docs/`), the specifications (`docs/specs/`), and the design records
(`.agents/docs/`). Each serves a different reader, so each gets its own
admission criterion, register, language policy, stability promise and
lifecycle — and the relation between them becomes a rule that a check can
enforce rather than a habit.

It supersedes the documentation half of
`.agents/docs/2026-09-08-examples-curriculum-and-documentation-plan.md`. That
plan's example curriculum stands and is referenced here rather than restated;
its chapter-level proposals (splitting `05`, adding chapters 22 and 23) are
carried in unchanged and placed inside the architecture this document defines.

Measured 2026-09-08 against `origin/main` at `6e1c65c6`.

---

## 1. The measurement

### 1.1 The three trees

| tree | files | lines | index |
|---|---|---|---|
| `docs/` chapters (English) | 23 | 11,313 | `docs/README.md`, 35 lines |
| `docs/zh/` | 23 | — | `docs/zh/README.md` |
| `docs/specs/` | 5 | 1,156 | `docs/specs/README.md`, 42 lines |
| `.agents/docs/` | **268** | **109,146** | `.agents/docs/README.md`, **one heading, no body** |
| `.agents/skills/` | 4 | 997 | none |

The design records are **8.8 times** the entire user-facing documentation
(11,313 + 1,156 lines), and their whole index is the single line
`# 开发/方案文档目录`.

### 1.2 Seven defects, each independently verifiable

1. **The architecture is stated exactly once, in a leaf.** The three-way
   division of labour — user docs / specs / design docs, with an audience named
   for each — exists as a table in `docs/specs/README.md`. `docs/README.md`,
   which is where a reader arrives, does not mention `.agents/docs` at all.

2. **The specifications are written in Chinese, inside the English tree, with no
   `docs/zh/` counterpart.** Every chapter under `docs/` has a `docs/zh/` mirror
   whose heading structure CI compares. `docs/specs/` has neither: it is
   Chinese-primary and unmirrored.

3. **The style checker's scope is a glob, and the glob is why.**
   `.github/tools/check_docs_style.sh` iterates `docs/*.md docs/zh/*.md`. That
   pattern does not descend, so `docs/specs/` is exempt from the register rules
   and from the parity loop — by accident rather than by decision. Defect 2 is
   the visible consequence.

4. **`docs/README.md` lists two of four specifications.** SPEC-002
   (`target-side.md`) and SPEC-003 (`exit-codes.md`) exist, are indexed in
   `docs/specs/README.md`, and are absent from the tree's front page.

5. **User documentation cites design records.** Five chapters send a reader to
   `.agents/docs/…` (`05`, `08` twice, `11`, `20`), and four `docs/zh/` chapters
   do the same. A design record carries no stability promise and describes a
   moment; a user chapter that ends in one has delegated a question it should
   have answered.

6. **Six code comments cite chapters that do not exist.**
   `docs/35-pack-design.md`, `docs/04-schema-xpkg-extension.md` and
   `docs/34-release-readiness.md` are named from `modules/manifest/src/`,
   `src/pack/`, `src/pm/` and `src/publish/` — survivors of an earlier numbering.
   Nothing checks that a cited document exists.

7. **The design tree has no taxonomy and no status.** Filenames carry an
   implicit one — 92 contain `design`, 60 `plan`, 20 `analysis`, 7 `review`,
   and **65 contain none of the sixteen classifier words in use** — and nothing
   records whether a record is being executed, has shipped, was superseded, or
   was abandoned. `2026-09-05-heterogeneous-build-ecosystem-design-v2.md` and
   `2026-09-06-ecosystem-plan-v3.md` encode that in the filename, which works
   for the two documents whose authors thought of it.

### 1.3 The renumbering cost, which overturns yesterday's recommendation

| cited as | files citing | where |
|---|---|---|
| `docs/NN-…` | **103** | `.agents/`, `docs/`, `src/`, `modules/` |
| `examples/NN-…` | **59** | `.agents/` 17, `docs/` 18, `examples/` 9, `tests/` 7, `.github/` 4, `src/` 3 |

`docs/05-mcpp-toml` alone is cited 90 times.

Yesterday's plan proposed renumbering five example directories, estimating the
cost as "external links". The measurement says otherwise: the example numbers
are cited from **seven test files, four CI workflows and three source files**,
where a stale path is a broken job rather than a broken link. Both trees are
therefore treated the same way in §6: **numbers are stable; grouping happens in
the index.**

---

## 2. Diagnosis

### 2.1 The division of labour is right and is unreachable

`docs/specs/README.md` already says what the three trees are for, and its table
is correct. The defect is placement: it sits two levels down, in the tree whose
audience is the narrowest of the three, in a language the enclosing tree does
not use. A contributor who wants to know where a document belongs will not find
it, so documents land where the last similar one landed.

That is the whole mechanism behind defects 1, 5 and 7. Nothing enforced the
division because nothing published it.

### 2.2 `docs/` already contains three audiences

The numbered sequence reads as one audience and is not:

| chapters | audience |
|---|---|
| 00, 01, 02, 03, 05, 06, 07, 10, 12–18, 20, 21 | people using mcpp |
| 08 (toolchain internals), 11 (machine output) | people writing against a mechanism |
| 04 (build from source), 09 (releasing mcpp) | people changing mcpp |

`09-release.md` documents how a maintainer cuts a release. It is chapter nine of
a sequence whose first three chapters are hello-world, dependencies and
packaging. Nothing marks the transition.

### 2.3 Citation direction is a habit

Four directions are in use and only one of them is examined:

- spec → design record, in a metadata row: **correct**, and it is the
  convention `docs/specs/README.md` already prescribes.
- user chapter → spec: **correct** and underused.
- user chapter → design record: **wrong**, and present five times.
- code comment → chapter: **unchecked**, and wrong six times.

---

## 3. The design: three classes

Each class is defined by its reader. Everything else — register, language,
stability, lifecycle — follows from that and is stated so the answer to "where
does this belong" is mechanical.

| | **用户文档** `docs/` | **规范** `docs/specs/` | **设计记录** `.agents/docs/` |
|---|---|---|---|
| **reader** | someone with a task in hand | someone implementing against the mechanism: index authors, downstream tools, contributors | whoever works on that change, and whoever later asks why it is like this |
| **question it answers** | how do I do X | what exactly is guaranteed, and is it implemented yet | why is it this way, and what was refuted |
| **emphasis** | completing the task; the shortest correct path | precision; every rule tagged with implementation status | the reasoning and the measurements, including the ones that overturned the plan |
| **register** | declarative reference; tutorials may address the reader | RFC 2119 (必须 / 应当 / 可以) | narrative permitted; "why" is the content |
| **language** | English + `docs/zh/` parity, CI-checked | **decision in §6.2** | the language of the round; new writing in English academic register |
| **stability** | additive; a spelling is kept as an alias | numbered, versioned, state machine | **immutable once the change lands** |
| **lifecycle** | kept current with the implementation | Draft → Review → Accepted → Superseded | active → landed → superseded / abandoned |
| **history** | none — a chapter describes today | a change record at the end | it *is* history |
| **admission criterion** | a reader with this task cannot finish without it | two independent implementations could disagree without it | a decision was made whose reasoning would otherwise be lost |
| **who reviews** | anyone who has done the task | whoever owns the mechanism | whoever did the work |

Three consequences worth naming because they are the ones that get violated:

**A user chapter never records history.** "This was a bug until 2026.8.16" is a
design record's sentence. A chapter states what is true and, where a version
matters, states the floor: "2026.9.6.5+".

**A design record is never edited after its change lands** — except to add a
status line or a correction block that says what later measurement overturned.
The alternative is a document that silently becomes a claim about the present,
and the repository has already met the failure that produces: a decision
written a second time without reading the first.

**A specification is the only tree with a normative voice.** If a user chapter
finds itself writing 必须, the content belongs in a spec and the chapter should
cite it.

### 3.1 The fourth surface, named so it stops being invisible

`.agents/skills/` holds four procedure documents for agents:
`mcpp-usage`, `mcpp-contributing`, `mcpp-release`, `mcpp-docs-style`. They are
not a fourth class of documentation; they are **executable procedure** —
ordered steps with criteria, addressed to an agent rather than a reader.

The rule that keeps them from becoming a fourth copy: **a skill states the
procedure and cites the chapter for the explanation; it does not restate the
explanation.** `mcpp-release` and `docs/09-release.md` are the pair to watch —
the skill is the checklist, the chapter is the reasoning, and the version-number
rules must exist in exactly one of them.

---

## 4. The citation rule

Six edges; four allowed, two forbidden.

```
    docs/  ────────────────▶  docs/specs/          allowed  (cite for exact semantics)
    docs/  ─ ─ ─ ─ ─ ─ ─ ▶  .agents/docs/        FORBIDDEN
docs/specs/ ──────────────▶  .agents/docs/        allowed, metadata row only (provenance)
docs/specs/ ──────────────▶  docs/                allowed  (point at the how-to)
.agents/docs/ ────────────▶  anything             allowed
   code    ──────────────▶  docs/ or docs/specs/  allowed, and the target must exist
```

**Why the forbidden edge is forbidden.** A design record describes a moment and
carries no stability promise. Sending a user into one means either the chapter
is incomplete, or the record holds something that has become normative. Both
have a fix, and neither is a link:

> When a user chapter wants to cite a design record, the content is **promoted**
> — into the chapter if it is how-to, into a spec if it is a guarantee. The
> design record is then cited by the spec's metadata row, where provenance
> belongs.

The five existing leak sites are the promotion worklist, and each names what it
would promote: schema ownership (→ SPEC-004), toolchain naming and the hermetic
link model (→ `08` or a spec), the machine-output protocol's design (→ `11`,
which already has SPEC-003 beside it), the heterogeneous design v2 (→ `20`).

**Code comments cite documents, and the document must exist.** Six do not
today. The check is one line and is listed in §7.

---

## 5. What changes in `docs/`

### 5.1 Numbers stay; the index groups

Renumbering is refused on the measurement in §1.3. `docs/README.md` becomes a
grouped index over the numbers that exist, and gains the two new chapters at the
next free numbers.

| part | chapters | reader |
|---|---|---|
| **I — Using mcpp** | 00, 01, 05, 06, 07, 21, **22 (new)** | someone building something |
| **II — Shipping what you built** | 02, 10, 12 | someone publishing |
| **III — Toolchains and targets** | 03, 16, 14, 15, 13, 17 | someone whose target is not the host |
| **IV — Devices and accelerators** | 18, 20 | someone with a GPU or a board |
| **V — Extending mcpp from outside** | **23 (new)** | rule-package and index authors |
| **VI — Machine interfaces and compatibility** | 11, 19, SPEC-003 | tool authors, release engineers |
| **VII — Contributing to mcpp itself** | 04, 08, 09 | maintainers |

Part VII is the change that fixes §2.2: `04`, `08` and `09` keep their numbers
and stop appearing to be step four, step eight and step nine of a user's path.

### 5.2 The role index sits above the parts

Before the parts, one table mapping an intention to a path across all three
teaching surfaces — chapters, examples and templates. This is the table
yesterday's plan specified; it is unchanged and belongs here because it is the
entry point for the whole documentation set, not for the examples alone.

### 5.3 Splitting `05`, and the two new chapters

Carried unchanged from the earlier plan, restated here as a table only:

| moved out of `05` | lines | to |
|---|---|---|
| features and capabilities (§2.8–2.8.2) | 394 | **22 — Features and capabilities** (new) |
| `[xlings]`, the project environment (§2.13) | 317 | **17** |
| the C++ runtime contract (§2.3 `cxx_runtime`) | 242 | **03** |
| `[target.*]` conditioning (§2.7.1) | 138 | **14** |
| host tools from a dependency (§2.14) | 274 | **07** |
| `[hooks]` (§2.16) | 192 | **21** |

`05` lands at about 1,570 lines. **23 — Authoring a rule package** is assembled
from `docs/07` §"Writing a rule package", the `mcpp::action` roles, the island
ladder's four rungs, `device_extensions` / `rule_module`, and the probe and
advisory channels.

---

## 6. What changes in `docs/specs/`

### 6.1 Four specs, and what is missing from the set

SPEC-001 identity, SPEC-002 target side, SPEC-003 exit codes, SPEC-004 manifest
semantics. Two are missing from `docs/README.md` (defect 4) — a one-line fix.

Two candidates for SPEC-005 and SPEC-006 emerge from §4's promotion worklist and
from the accelerator work:

- **The artifact compatibility tag**, including the `accel` field's grammar,
  the coverage relation (family targets, portable-form floors) and the matching
  algorithm. It is normative, it has a second implementer today (`mcpp-index`
  descriptors are written by hand against it), and it currently lives in
  `docs/20` §"What a prebuilt artifact states" — a user chapter writing rules.
- **The `build.mcpp` directive protocol**, whose version number already
  functions as a normative contract (`kProtocolVersion`; an engine refuses a
  program declaring a higher one) and whose only description is a section of
  `docs/07`.

Both are promotions of existing text rather than new writing, and both are
listed as staged work rather than decided here.

### 6.2 The language decision

Today: Chinese, unmirrored, unchecked. Three options.

| | cost | consequence |
|---|---|---|
| **(a) English primary + `docs/zh/specs/` mirror** | translate 1,156 lines once, then parity forever | uniform with the enclosing tree; reachable by the downstream tool authors the specs name as their audience |
| (b) move to a top-level `specs/`, keep Chinese | rewrite paths in ~15 citation sites | admits that specs are not user docs, and abandons the audience that cannot read them |
| (c) English only, no mirror | translate once | breaks the tree's own parity policy in the other direction |

**Recommendation: (a).** The audience the specs themselves name — index authors
and downstream tooling — is the least likely of the three audiences to be
Chinese-reading, and the specs are the documents where a misreading is most
expensive.

### 6.3 The metadata contract becomes checkable

`docs/specs/README.md` already requires a metadata table and a change record in
every spec. Nothing checks it. §7 adds that, and the same check verifies that
every spec in the directory appears in both indexes.

---

## 7. What changes in `.agents/docs/`

268 files and 110,143 lines are **not rewritten**. Three additions, all of which
apply to new documents and are backfilled only where a reader needs them.

### 7.1 Front matter, required on new records

```yaml
---
subject: heterogeneous            # one of a short controlled list
status: landed                    # active | landed | superseded | abandoned
superseded_by: 2026-09-07-module-first-heterogeneous-surface.md
implements: [docs/20-heterogeneous-builds.md, docs/specs/SPEC-005]
---
```

`status` is what the tree lacks most. A reader opening
`2026-09-05-accelerator-support-design.md` today cannot tell from the document
that `…-v2` and then `ecosystem-plan-v3` moved past it; the filename carries it
for the two authors who thought of the convention.

### 7.2 A generated index

`.agents/docs/README.md` is generated: grouped by `subject`, ordered newest
first within each, showing status and title. For the 268 existing records the
title comes from the first heading and the status is `landed` unless the
document says otherwise — a mechanical default that is right for almost all of
them, since they describe changes that shipped.

The generator is a script, and the index is checked in so that reading the tree
on GitHub works.

### 7.3 The subject list, and why it is short

Derived from the filenames rather than invented: `toolchain`, `packaging`,
`resolution`, `target`, `freestanding`, `heterogeneous`, `ecosystem`,
`performance`, `platform`, `docs`, `process`. Eleven. A twelfth is added when a
document does not fit, which is a decision someone makes rather than a field
someone fills in freely — a free-text subject would reproduce the
sixty-five-uncategorised state in a new column.

### 7.4 Two housekeeping items

Four records carry no date prefix (`fix-xlings-package-home-detection.md`,
`llvm-install-failure-analysis.md`, `platform-abstraction-plan.md`,
`platform-remaining-ifdefs-report.md`) and `todos/` holds four more under a
directory the convention does not mention. Both are absorbed by the front matter
and the generated index without moving a file.

---

## 8. The checks

`.github/tools/check_docs_style.sh` keeps its three rules and gains a scope; a
new `check_docs_structure.sh` carries the rules that are about the architecture
rather than the prose.

| # | rule | reads |
|---|---|---|
| 1 | the existing three: heading register, second person in reference docs, bilingual heading parity | text |
| 2 | **scope extended to `docs/specs/`** | the glob, corrected to descend |
| 3 | no `docs/**` file cites `.agents/` | the forbidden edge in §4 |
| 4 | a spec cites `.agents/` only inside its metadata table | line position |
| 5 | every `docs/…md` path named anywhere in the repository resolves | catches defect 6, in code as well as prose |
| 6 | every spec in `docs/specs/` appears in `docs/README.md` and in `docs/specs/README.md` | catches defect 4 |
| 7 | every spec has a metadata table and a change record | the contract `docs/specs/README.md` already states |
| 8 | a new `.agents/docs/*.md` has front matter with a known `subject` and `status` | new files only, by comparing against the merge base |
| 9 | `.agents/docs/README.md` matches what the generator would produce | the index cannot drift |

Rule 5 is the one with reach beyond documentation: a comment in `src/` naming a
chapter is a citation, and today six of them are stale. Rule 8 is scoped to new
files on purpose — a rule that demanded front matter on 268 existing records
would be satisfied by a mechanical pass that adds a field nobody chose.

**What none of them check.** Whether a chapter is *right*, whether a spec's
rules are complete, or whether a design record's reasoning holds. The checks
establish that each document is in the tree its content belongs to and is
reachable from that tree's index; a reader establishes the rest.

---

## 9. Staging

| stage | content | independent? |
|---|---|---|
| **1** | publish the architecture: the three-way table moves to `docs/README.md`; role index; parts I–VII; the two missing specs listed | yes — index only, no content moves |
| **2** | close defects 4, 5 and 6: promote the five leak sites, fix the six code citations | yes |
| **3** | checks 2–7 (scope, citation direction, resolvable paths, spec index and metadata) | after stage 2, so the tree is already clean when the check turns on |
| **4** | translate the specs, mirror to `docs/zh/specs/`, put them under the parity loop | yes; the largest single translation cost |
| **5** | front matter, subject list, generated index, checks 8–9 | yes |
| **6** | split `05`; add chapters 22 and 23 | after stage 1, which is where their entries in the index go |
| **7** | SPEC-005 (compatibility tag) and SPEC-006 (directive protocol), each a promotion | last; each is its own review |

Stages 1–3 are a week's worth of work and close five of the seven defects.
Stage 4 is the largest and is separable. Stage 6 is the chapter work carried
from the earlier plan and is where its example work rejoins.

---

## 10. What this deliberately does not do

- **It does not renumber anything.** §1.3 measured the cost in test and CI
  files, not link rot, and this reverses a recommendation made yesterday on an
  estimate.
- **It does not rewrite the 268 design records.** Their value is that they were
  written when the reasoning was fresh; a pass to normalise them would edit
  documents whose defining property is that they are not edited.
- **It does not merge `.agents/skills/` into any tree.** They are procedure, and
  §3.1 states the rule that keeps them from duplicating explanation.
- **It does not introduce a documentation site generator.** Every tree stays
  readable as Markdown on GitHub, which is where its readers are; a generated
  site is a separate decision with its own hosting and staleness questions.
- **It does not change what is *in* a chapter beyond the moves listed.** The
  register work landed in #452 and the content is largely correct; this is an
  architecture change, not a rewrite.

---

## 11. Open questions for review

1. **The specs' language (§6.2).** Recommendation is English primary with a
   `docs/zh/specs/` mirror, at 1,156 lines of translation. The alternative that
   is cheapest — leave them Chinese and exempt them explicitly rather than by
   glob accident — is defensible if the specs' real audience is this ecosystem's
   own contributors rather than the downstream tool authors they name.

2. **Whether `08-toolchain-internals` belongs in part VII.** It documents
   mechanism for people writing against it as much as for people changing mcpp.
   Placing it under "Contributing" may hide it from the first group.

3. **SPEC-005's scope.** The compatibility tag alone, or the tag plus the
   prebuilt-artifact selection algorithm? The algorithm is implemented once, in
   `src/pack/prebuilt.cppm`, and has no second implementer today — which is the
   admission criterion in §3 arguing against including it.

4. **Whether `status: landed` is the right default for the backfill.** It is
   right for almost all of the 268 and wrong for the handful that were written
   and never executed. The alternative is `unknown`, which is honest and makes
   the generated index less useful on its first day.

5. **Whether design records should carry a correction block.** §3 says a record
   is immutable except for a status line. Several records in the tree have been
   corrected in place by later measurement, which is how their own value was
   preserved. Making that a named, dated block at the end — rather than an edit
   in the body — would keep both properties.
