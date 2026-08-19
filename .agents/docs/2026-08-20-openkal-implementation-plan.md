# openkal: implementation plan and outcome

This document records the plan by which openkal 0.1 was implemented, the
dependencies among its tasks, the criteria applied to each, and the result. It
accompanies the design analysis in
[`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md) and the
specification itself, which is maintained in the `mcpplibs/openkal` repository.

## 1. Deliverables

| Repository | Contents | Version |
| --- | --- | --- |
| `mcpplibs/openkal` | the specification, the modules that declare it, the surface checker, and a substitution example | 0.2.0 |
| `mcpplibs/openkal-linux` | the reference implementation for Linux, its conformance suite, and an example | 0.2.0 |
| `mcpplibs/mcpp-index` | descriptors for both packages | pull requests 220 and 221 |

Both packages are mirrored to GitCode, and the mirrored archives were verified
to be byte-identical to those served by GitHub.

## 2. Task dependencies

The work divides into five groups. Groups A and B are independent of each
other; C depends on both; D and E follow C.

```
A. specification text ────┐
                          ├──► C. reference implementation ──► D. conformance
B. declaration modules ───┘                                    │
                                                               └► E. distribution
```

The order is not arbitrary. Writing the reference implementation before
completing the specification would have produced a specification describing one
implementation, which clause 7.1 of the specification exists to prevent.
Deferring the reference implementation until after distribution would have
deferred the discovery reported in section 4.3.

| Group | Tasks | Depends upon |
| --- | --- | --- |
| A | scope, interface inventory, capability model, conformance procedure | the design analysis |
| B | `openkal.decl.types`, `.abort`, `.stream`, `.memory` | A |
| C | `openkal.abort`, `.stream`, `.memory` for Linux | A, B |
| D | behavioural suite, absence assertions, exported-surface comparison | C |
| E | repositories, tags, mirrors, index descriptors, verification | D |

## 3. Criteria applied

The following criteria were fixed before implementation and applied to each
decision. Where a decision satisfied one criterion at the expense of another,
the resolution is recorded.

### 3.1 Architecture

⚠️ **A correction applied after 0.1 was published.** Version 0.1 placed the
module a consumer imports under the control of the implementation: the
specification package provided `openkal.decl.<name>` and the implementation
re-exported it as `openkal.<name>`. Review identified this as a contradiction of
what a specification is for, and the identification was correct. The arrangement
existed to support detection of optional operations through argument-dependent
lookup, which requires an implementation's declarations to be visible to the
consumer — and version 0.1 defined no optional operation, so the mechanism
served nothing that existed.

Version 0.2 restores the intended layering: the specification package provides
the interface, and an implementation contributes definitions and exports no
module. The cost is that a missing implementation is reported by the linker
rather than by the compiler, and the diagnostic names the undefined functions.

An interface is the unit of provision and of versioning. An implementation
provides an interface in whole or not at all, and an interface that is not
provided is absent as a module rather than present and refusing.

The consequence for the module layout is stated in clause 4 of the
specification: the specification package owns every module, and an
implementation exports none. An implementation therefore cannot extend the
interface, and this is not a rule that must be enforced — it follows from the
arrangement.

### 3.2 Stability

The layout of every structure is frozen at 0.1, and the evolution rule admits
new declarations while excluding changes to existing ones. The C surface carries
no version in its symbol names; the specification records this as an unsettled
matter rather than concealing it.

The reference implementation retries interrupted operations rather than
reporting them. The alternative produces short transfers on any system that
delivers asynchronous notifications, and such a defect is unlikely to be
reproduced by a test suite.

### 3.3 Simplicity

Version 0.2 defines no optional operation, and therefore no mechanism for
expressing one. Two designs were built and removed before this was recognised: a
record of capability flags with a separate configuration file, and a set of
fallback overloads detected through argument-dependent lookup. Each solved a
problem the specification did not yet have.

Clause 6.3 records the alternatives and the measurements that constrain them, so
that the choice is informed when an optional operation is first defined.

### 3.4 User experience

A consumer imports the interface and names no implementation. Which
implementation supplies the definitions is decided in the manifest, and changing
it is a change to one line.

A consumer that depends upon no implementation compiles and fails to link, with
the undefined functions named. That is later than a compilation failure and is
legible; clause 4.2 records it rather than concealing it.

### 3.5 Compatibility

A consumer declares two dependencies. The second selects an implementation; the
first fixes the version of the contract, and converts a mismatch between
consumer and implementation into a failure of dependency resolution rather than
a collection of signature errors at compile time.

Neither package raises the index floor, and neither requires a version of mcpp
beyond one already published.

### 3.6 Portability

The handle is one machine word and opaque. An implementation stores a
descriptor, an operating-system handle, a pointer to a driver structure, or a
capability index, and none is required to maintain a translation table. This is
the property that permits an implementation to be placed above a C library,
beneath one, or without one.

The specification does not assume a process model, a division between
privileged and unprivileged execution, or a namespace shared by all callers.

### 3.7 Consistency

The specification adopts the vocabulary already established in this ecosystem:
capability absence is expressed at compile time, backend selection is a
conditional dependency, and no new axis is introduced into the build system. The
implementation of openkal required no change to mcpp.

### 3.8 Upgrade without disruption

Version 0.1 introduces two packages and modifies none. No existing project is
affected, and the criterion is satisfied trivially. It is recorded so that
subsequent versions, which will not satisfy it trivially, are measured against
it.

## 4. Verification performed

### 4.1 Language constructs

Exporting `extern "C"` declarations from a module, and calling them from a
consumer that imports the module, was verified before the interface was written.

### 4.2 Substitution

One application source was compiled against two implementations, one writing to
descriptors and one discarding every transfer. The observable behaviour differed
and the source did not: its checksum was taken before the first build and
compared after the second. The example is retained in the specification
repository and is exercised by its continuous integration.

### 4.3 A decomposition confirmed by implementation

Writing the Linux implementation confirmed clause 6.3 independently of the
reasoning that produced it. On Linux, whether a stream can be repositioned is a
property of the individual descriptor: the same implementation succeeds for a
regular file and fails for a pipe. Had `openkal.stream` offered positioning, the
implementation could have neither claimed it honestly nor withheld it usefully.

This is the argument for writing a complete reference implementation rather than
a sketch. A decomposition error of this kind is not visible in the
specification text, and a conformance suite would find it later.

### 4.4 The surface checker

The exported-surface comparison required by clause 9.3 was verified in both
directions: it accepts the reference implementation, and it rejects the same
implementation after an unspecified name is added. The negative direction is the
one that establishes the check is not vacuous, and an earlier version of it was
vacuous, comparing a set of C++ symbols that inline functions never emit.

### 4.5 Conformance

The suite verifies that the operations provided behave as specified and that the
operation not provided is absent, the latter as a compile-time assertion.

## 5. Matters deferred

| Matter | Reason |
| --- | --- |
| `openkal.time`, `.task`, `.fs`, `.net`, `.channel` | 0.1 specifies the core set; the remaining names are reserved |
| Concurrency upon one handle | unavoidable once `openkal.task` is specified, and not before |
| Symbol versioning | the evolution rule protects the interface by prohibiting change; an ecosystem that outgrows the prohibition will need a mechanism this version does not define |
| A shared conformance package | the suite presently resides with the implementation; a reusable package requires a mechanism for a test package to be compiled against an implementation chosen by a third project |

## 6. The gate that governs what follows

The design analysis records that the criterion for continuing beyond D0 is not
technical: it is whether a third party implements a third backend. The reference
implementation does not satisfy that criterion and does not alter it. Its effect
is to reduce the work such a party must perform from inferring a shape and
writing an implementation to writing an implementation.
