# openkal: implementation plan and outcome

This document records the plan by which openkal 0.1 was implemented, the
dependencies among its tasks, the criteria applied to each, and the result. It
accompanies the design analysis in
[`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md) and the
specification itself, which is maintained in the `mcpplibs/openkal` repository.

## 1. Deliverables

| Repository | Contents | Version |
| --- | --- | --- |
| `mcpplibs/openkal` | the specification, the modules that declare it, the surface checker, and a substitution example | 0.1.0 |
| `mcpplibs/openkal-linux` | the reference implementation for Linux, its conformance suite, and an example | 0.1.0 |
| `mcpplibs/mcpp-index` | descriptors for both packages | pull request 220 |

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

An interface is the unit of provision and of versioning. An implementation
provides an interface in whole or not at all, and an interface that is not
provided is absent as a module rather than present and refusing.

The consequence for the module layout is stated in clause 4 of the
specification: the implementation owns the name a consumer imports, because
argument-dependent lookup does not reach a module the translation unit has not
imported. The alternative arrangement, in which the specification package owns
that name, was rejected because it would have made optional capabilities
undetectable.

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

The capability mechanism uses argument-dependent lookup and a fallback overload.
An earlier design used a record of capability flags together with a separate
configuration file, and both were removed. A record can disagree with the code
it describes; a declaration cannot. The removal also eliminated a second
configuration format from the ecosystem, in which every other fact about a
package resides in `mcpp.toml`.

### 3.4 User experience

A consumer that calls an operation the implementation does not provide is
rejected during compilation, and the diagnostic carries the wording the
specification supplies. This is the default behaviour and requires nothing of
the consumer: no capability test, no configuration, and no annotation.

A consumer that wishes to adapt rather than fail uses the concept the interface
provides, which evaluates to false when the operation is absent.

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
