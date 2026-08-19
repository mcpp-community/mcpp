# openkal: plan for industrial completeness

This document plans the extension of openkal from its core set to a
specification sufficient to host a C library, and through it the ordinary
software of a hosted system. It does not restate the decisions already taken;
those are recorded in [`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md)
and in the specification itself, and this plan treats them as given.

## 1. Method

The interfaces are derived from the kinds of resource an execution environment
provides. They are not derived from what a particular program calls.

The distinction is not stylistic, and the specification already records why: a
boundary shaped by one client acquires that client's assumptions and obliges
every other environment to reproduce them. POSIX was shaped by the C library of
its time; the first preview of the WebAssembly System Interface was shaped by
POSIX; an openkal shaped by a compiler would be the third instance of the same
error.

Programs are therefore used to **validate** the derivation and never to produce
it. A program that cannot be hosted indicates that a resource kind has been
omitted or decomposed incorrectly. A program that can be hosted only by
reproducing its own assumptions indicates nothing about openkal, and something
about the program.

### 1.1 The two validation subjects, and why they form a stack

```
    an ordinary POSIX program, such as the GCC toolchain
                    │  the C standard library and POSIX
                    ▼
    musl, ported to openkal
                    │  the openkal C application binary interface
                    ▼
    an openkal implementation: Linux, Windows, a microkernel, bare metal
```

The two subjects are not parallel. Porting a C library is the enabling work,
and porting it once causes the software above it to run on every openkal
implementation without further porting. The compiler is then an end-to-end
check of that claim rather than a separate exercise.

The arrangement also settles a question the derivation raises. A compiler
assumes a global namespace of paths; that assumption is not universally
satisfiable, since capability-based kernels and the second preview of the
WebAssembly System Interface have no such namespace. The resolution of a path
against a root the environment supplies is therefore work that belongs in the C
library, performed once, rather than in every program or in the specification.

### 1.2 Evidence gathered

Measurements taken while preparing this plan. They inform coverage, which is a
question about sufficiency; they do not inform shape.

| Subject | Measurement |
| --- | --- |
| GCC driver and `cc1plus` | 189 undefined symbols, of which 149 face the environment |
| Process creation in GCC | `posix_spawn` and `posix_spawnp`; neither `fork` nor `execve` appears |
| Path operations in GCC | `open`, `stat`, `lstat`, `mkdir`, `unlink` alongside `fstatat`: a global namespace is assumed |
| musl, static archive | file and directory operations 48, process and signal 21, memory 7, threads 81, time 8, network 23 |
| musl thread construction | the pthread layer is built upon a wait-and-wake primitive rather than upon kernel mutexes |

Two of these are load-bearing.

The first is that GCC creates processes by spawning rather than by duplicating
the calling image. A specification that copied the POSIX decomposition would
have obliged an implementation on Windows to reproduce `fork`, which cannot be
done faithfully, and would therefore have failed the specification's own
admission criterion. The resource-derived form and the observed usage agree,
which is corroboration rather than derivation.

The second is that musl constructs mutexes and condition variables from a
primitive that suspends an execution context until another context wakes it.
The kernel boundary is therefore the primitive, and the synchronisation objects
are library constructions above it. An interface offering mutexes would be
placing a library at the kernel boundary.

## 2. The four decisions this plan settles

The core set is specified and published. Four questions were deferred, and each
must be answered before the specification can host a C library.

### 2.1 Optional capabilities

An earlier draft deferred the mechanism. The rule adopted here follows from a
distinction the specification already draws for another purpose.

> An **operation** that an implementation may lack becomes an interface of its
> own. A **property** that varies between implementations is reported by a
> capability word.

The justification is that the two are not alike. An operation that is present
and always fails is the defect the specification rejects in clause 6.4; the
remedy is that its absence is expressed by its absence, which at the granularity
of an interface means the linker reports it. A property, by contrast, cannot be
called. Whether paths are compared case-sensitively, whether a clock advances
while the machine is suspended, what the granularity of an allocation is: these
are facts, and a program adapts to them rather than invoking them.

Capability words are the ordinary mechanism of every system that has faced this
question, including `sysconf`, `pathconf`, the auxiliary vector and the
processor identification instruction. They are also the only mechanism available
once the implementation is chosen at link time, which the specification's
layering requires.

Information therefore becomes available at three times, each being the earliest
at which it exists.

| Time | Mechanism | What it answers |
| --- | --- | --- |
| Dependency resolution | the implementation package declares the interfaces it provides | may this program be built against this implementation |
| Link | an undefined symbol | was an interface used that the implementation does not provide |
| Run | a capability word | how does this implementation behave within an interface it provides |

### 2.2 Interfaces beyond the core

Derived from resource kinds. The core set is unchanged.

| Interface | Resource | Class |
| --- | --- | --- |
| `openkal.abort` | termination | core |
| `openkal.memory` | a region of the address space | core |
| `openkal.stream` | a byte stream | core |
| `openkal.env` | the parameters a program receives at inception | standard |
| `openkal.time` | a time source | standard |
| `openkal.fs` | a directory, and an open file | standard |
| `openkal.process` | a program image that has been started | standard |
| `openkal.task` | an execution context, and a suspension primitive | standard |
| `openkal.net` | an endpoint | optional |
| `openkal.module` | a code image that has been loaded | optional |
| `openkal.entropy` | a source of unpredictable bits | optional |
| `openkal.event` | readiness of a set of resources | reserved |

*Standard* denotes an interface an implementation hosting a C library provides.
*Optional* denotes one it may omit without ceasing to host a C library, at the
cost of the facilities built upon it. *Reserved* denotes a name whose contents
are not yet normative.

`openkal.event` is reserved rather than specified because readiness
notification is the interface at which environments differ most, and because a
C library can be hosted without it: blocking operations suffice, and the
facilities that require readiness are those a program uses when it declines to
block. Specifying it prematurely would produce an interface shaped by whichever
environment was consulted first.

#### 2.2.1 `openkal.fs`

The resource is a directory or an open file, and operations are relative to a
directory the program holds. There is no global namespace of paths.

The reason is the admission criterion. A global namespace is not available in a
capability-based kernel, and an implementation on such a kernel would have to
construct one. Relative operations, by contrast, are natural on every
environment considered: they are the primitive on capability systems, and on
POSIX they are the `…at` family, which musl already uses to implement the
global forms.

The consequence is that the resolution of an absolute path is work performed by
the C library against a root directory the environment supplies at inception.
This is the arrangement the second preview of the WebAssembly System Interface
adopts, and it is what allows a program to be confined without its cooperation.

#### 2.2.2 `openkal.process`

The resource is a program image that has been started. The operations are to
start one, to wait for it, and to request its termination.

Duplication of the calling image is not among them. It is not universally
implementable, and the observed behaviour of the validation subject does not
require it.

#### 2.2.3 `openkal.task`

The resource is an execution context sharing the address space, together with a
primitive that suspends a context until another wakes it.

Mutexes and condition variables are not among the operations. They are
constructions above the primitive, as the measurement of musl demonstrates, and
placing them at the kernel boundary would place a library there.

### 2.3 Concurrency

An implementation shall permit concurrent operations upon distinct handles.

Concurrent operations upon one handle shall not damage the implementation's own
state. The order in which they take effect, and whether the bytes of one
transfer may be separated by those of another, are unspecified.

Atomicity below a threshold, which POSIX guarantees for pipes, is not required.
It is not universally implementable, and a specification that required it would
oblige an implementation to introduce buffering it does not otherwise need.

### 2.4 Ownership

Handles obtained from the core interfaces are borrowed and are not released.

Handles obtained from `openkal.fs`, `openkal.process`, `openkal.net` and
`openkal.module` are owned, and each of those interfaces provides the operation
that releases one. An implementation shall not treat a released handle as valid.

The recommended construction divides the handle word into an index and a
generation, incrementing the generation on release. The specification does not
require it: it requires only the property, which this construction achieves
without a lookup table and therefore without the compatibility layer that clause
7.1 excludes.

## 3. Work breakdown

### 3.1 Groups and their dependencies

```
S. specification: clauses for the four decisions ─┬─► I. interface modules ─┬─► L. Linux implementation ─┬─► M. musl port ──► G. compiler validation
                                                  │                        │                            │
                                                  └─► C. conformance ──────┴────────────────────────────┘
```

| Group | Tasks | Depends upon |
| --- | --- | --- |
| S | capability rule, interface inventory, concurrency, ownership | this plan |
| I | `env`, `time`, `fs`, `process`, `task` declaration modules | S |
| L | the same five for Linux | I |
| C | behavioural suites, surface comparison, capability-word agreement | I |
| M | musl retargeted onto openkal | L, C |
| G | the compiler built and run above the ported library | M |

Groups I and C are independent of each other and both depend upon S. Group L
may begin as soon as the declarations of a given interface exist, so the five
interfaces proceed in parallel rather than in sequence.

### 3.2 Sequence within an interface

Each interface is taken through the same five steps, and the order is not
interchangeable.

1. State the resource and the operations that follow from it.
2. Determine which operations may be absent, and separate those into their own
   interface.
3. Determine which properties vary, and assign them positions in the capability
   word.
4. Write the Linux implementation, which is where a decomposition error becomes
   visible.
5. Write the conformance suite, which is where a claim becomes checkable.

Step 4 precedes step 5 deliberately. The implementation of the core set
demonstrated that a decomposition error is visible to an implementer and not to
a reader of the specification: positioning was excluded from the stream
interface by argument, and the exclusion was confirmed when the Linux
implementation showed that positioning is a property of the individual
descriptor.

## 4. Assessment

### 4.1 Architecture

The interface remains the unit of provision and of versioning, and the
specification retains ownership of every module. The extension adds interfaces;
it does not add mechanisms, with the single exception of the capability word,
which is confined to properties.

### 4.2 Stability

Structure layouts remain frozen. The capability word introduces a new obligation:
a position once assigned retains its meaning, and a property that ceases to vary
is not reclaimed. Unassigned positions are reserved and read as zero, so that a
program compiled against a later specification behaves correctly against an
earlier implementation.

### 4.3 Simplicity

The rule distinguishing operations from properties replaces the three mechanisms
that were drafted and withdrawn: a record of capability flags, a configuration
file that accompanied it, and a set of fallback overloads detected through
argument-dependent lookup. Each was more elaborate than the rule that replaces
them, and each solved a problem that a correct decomposition does not have.

### 4.4 Consumer experience

A program declares the interfaces it requires. A program built against an
implementation that lacks one fails during dependency resolution, with the
interface named. A program that reaches an interface it did not declare fails
at link, with the function named. Neither failure is deferred to run time.

### 4.5 Compatibility

The core set does not change. An implementation of the core set that predates
this extension remains conforming, and a program that uses only the core set is
unaffected.

### 4.6 Portability

The interfaces are derived rather than borrowed, and each is checked against
four environments before it is specified: a hosted system with a global
namespace, a hosted system without one, a capability-based kernel, and a
freestanding target. An interface that any of the four could satisfy only by
constructing a compatibility layer is decomposed again rather than admitted.

### 4.7 Consistency

The extension introduces no vocabulary the ecosystem does not already have. An
implementation is selected by a conditional dependency, an interface it does not
provide is absent as a module, and the exported surface is compared against a
list of names. None of these is new, and none required a change to the build
system: openkal was implemented, twice, without modifying mcpp.

The constraint that admits no exception is that every declaration a project makes
resides in its `mcpp.toml`. A configuration file accompanying the capability
record was drafted and removed for violating it, and the rule that replaced the
record was chosen partly because it requires no file at all.

### 4.8 Upgrade

An implementation adds interfaces without altering those it already provides. A
program observes the addition through dependency resolution. No existing
manifest requires modification.

## 5. Criteria for completeness

The extension is complete when the following hold. Each is an observation rather
than a judgement.

1. musl, retargeted onto openkal, builds and passes its own test suite against
   the Linux implementation.
2. The compiler toolchain, built against that library, compiles and links a
   program, and the program runs.
3. A second implementation exists for an environment without a global path
   namespace, and the same library binary interface is satisfied by it.
4. The conformance suite verifies, for every interface, the behaviour of the
   operations provided, the absence of those not provided, and the agreement
   between each capability word and the behaviour it describes.
5. No implementation requires a table, a registry or a name resolver in order to
   satisfy the specification.

Criterion 3 is the one that distinguishes this work from a portable C library.
A specification satisfied only by environments resembling the one it was written
against has not been validated, however many programs it hosts.

### 5.1 State against these criteria, recorded 2026-08-20

| Criterion | State |
| --- | --- |
| 1. A C library retargeted onto openkal | Partially met. `openkal-libc` 0.2.0 performs the two adaptations the specification places outside itself — resolving a global name against the supplied directories, and constructing synchronisation objects from the suspension primitive — and a program above it reads a file by global path, consults a variable, measures an interval and starts another program without containing any of that. Its output agrees with the system's own counter field by field. musl itself has not been retargeted. |
| 2. A compiler toolchain above that library | Not met, and not attempted. It follows criterion 1 and is the subject of the next round. |
| 3. A second implementation for an environment without a global path namespace | Not met. `openkal-macos` 0.2.0 is a second implementation and records four divergences from the first, but both environments have a global path namespace and both were written by one author. Section 5.2 records what that limits. |
| 4. A conformance suite covering behaviour, absence, and the capability words | Met for the operations and the exported surface: five suites per implementation, and a surface comparison in `--complete` mode confirming all 47 names. The agreement between a capability word and the behaviour it describes is asserted for `openkal.time` and not yet for the others. |
| 5. No implementation requires a table, a registry or a name resolver | Met. Neither implementation maintains a translation table; the handle carries an index and a generation directly. Name resolution resides in `openkal-libc`, which is where the specification places it. |

### 5.2 What two implementations by one author establish, and what they do not

`openkal-macos` records four divergences from the Linux implementation, and each
is a place where an interface could have assumed a mechanism: the monotonic clock
continues during suspension where the other stops, names are compared without
regard to case, the spawn has no attribute setting the working directory, and
there is no suspension primitive a program may use. These establish that the
capability words and the interface shapes that accommodate them are necessary.

The two implementations also agreed on a question the specification had not
settled, and their agreement was worth nothing: both prepended the path to the
argument vector, because both were written by one author from one reading. A
second implementation by the same author establishes less than a second
implementation by another, and criterion 3 is written as it is for that reason.

The consequence for the conformance suite is recorded in
[the portable-program findings](2026-08-20-openkal-portable-program-findings.md):
a test that does not observe the thing cannot detect the thing. The suite started
a program that ignores its arguments and read its status, which produced the same
result whether the vector arrived intact or shifted by one.

## 6. Matters this plan does not settle

| Matter | Status |
| --- | --- |
| `openkal.event` | reserved; see 2.2 |
| Symbol versioning | the evolution rule prohibits change rather than permitting coexistence, and an ecosystem that outgrows the prohibition will require a mechanism this specification does not define |
| Locale and character encoding | properties of a C library rather than of a kernel boundary; the specification does not address them |
| Signals as a general mechanism | the process interface provides termination; asynchronous delivery to a running program is deferred with `openkal.event` |
