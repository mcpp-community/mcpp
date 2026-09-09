---
subject: heterogeneous
status: landed
---

# A dlopen surface no closure walks, and a process with two unwinders

mcpp#596 reports that `examples/09-heterogeneous/sycl` builds cleanly and then
aborts with exit 134 on an NVIDIA machine, printing no exception text. The
reporter located the trigger exactly: `compat.sycl-runtime`'s farm carries
`libcuda.so.1` and not `libnvidia-ml.so.1`, so the SYCL runtime's CUDA adapter
cannot load. That is correct, and it is one of three mechanisms.

This record separates them, because they are repaired in three different
repositories and only one of them is about NVML:

* **A.** The libraries a package publishes through `runtime.library_dirs` exist
  precisely because something will `dlopen` them. No closure mcpp walks reaches
  them, so a farm that cannot satisfy its own members measures green. Two of
  this farm's twenty-five members cannot.
* **B.** `compat.sycl-runtime`'s `install()` **enumerates** the payload's
  library directory and **hand-writes** the driver's name. The hand-written
  half is the half that is wrong, and the package's own criterion hand-writes
  the same three names it checks.
* **C.** The artifact links LLVM libunwind statically and loads libgcc_s at run
  time. Ten of libgcc's eighteen `_Unwind_*` entry points are interposed by the
  executable and eight are not, so one throw is processed by two unwinders and
  reaches `std::terminate` past a matching handler. **This is why the failure
  prints nothing**, and the reporter set it aside as an unrelated build
  warning.

A is the reason mcpp did not catch it. B is the reason it exists. C is the
reason it presented as a silent abort rather than as one line of text.

## 1. What was measured

Host: Linux 6.8, x86_64, RTX 4080 (sm_89), NVIDIA driver 550.144.03. mcpp
`2026.9.8.1` (released binary, its own provisioned registry), `mcpp:plugins`
0.5.2, `xim:dpcpp` 7.1.0, `compat.sycl-runtime` 2026.09.07. The project is
`examples/09-heterogeneous/sycl`, unmodified. The reporter's host is a
different GPU (4070 Ti SUPER) on a different driver (610.57.04) with mcpp
`2026.9.9.1`; the readings below agree with theirs except where noted in §2.3.

| # | Input | Reading |
|---|---|---|
| 1 | `mcpp build` | green in 2.70 s, no runtime-closure diagnostic |
| 2 | `./bin/sycl-saxpy` | `terminate called ...` / `terminate called recursively`, exit **134** |
| 3 | `SYCL_UR_TRACE=1` | cuda adapter fails on `libnvidia-ml.so.1`; opencl adapter fails on `libOpenCL.so.1`; both level_zero adapters load |
| 4 | `ln -s /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1 <farm>/` then run | `12 24 36 48` / `device: NVIDIA GeForce RTX 4080` |
| 5 | remove that link, run again | back to row 2 |
| 6 | `<glibc payload>/ld.so --help` | search path is the literal `/nonexistent/xlings-use-rpath-not-default-search/lib` |
| 7 | `nm -D` on the artifact vs. libgcc_s | 10 of 18 `_Unwind_*` names defined by the executable |

Rows 4 and 5 are the pair that matters: one symbolic link, both directions,
same binary, same machine. Row 6 is what makes row 3 fatal rather than
cosmetic — the private loader consults no host directory on any distribution,
so "absent from the farm" is "absent from the machine".

The same objects were then relinked by hand, four ways, and each variant was
run in both situations — the unrepaired farm (no device reachable) and the repaired
one (the 4080 reachable). This is the evidence §6's R6 rests on:

| Variant | Unwinders | Exported / overlapping | When it fails | With a device |
|---|---|---|---|---|
| V0 — as mcpp links today | 2 | 89 / 68 | `terminate`, **exit 134**, no text | `12 24 36 48`, exit 0 |
| V1 — `--unwindlib=libgcc`, no `libunwind.a` | 1 | 58 / 58 | **`sycl: no usable device: No device of requested type available.` / `device unavailable`, exit 1** | `12 24 36 48`, exit 0 |
| V2 — V1 + `-Wl,--exclude-libs,ALL` | 1 | **0 / 0** | same as V1, exit 1 | `12 24 36 48`, exit 0 |
| V3 — `--exclude-libs,ALL` alone | 2 | 0 / 0 | **SIGSEGV, exit 139** | `12 24 36 48`, exit 0 |

V3 is the variant that looks right. It removes every duplicate symbol, its
run with a device is perfect, and it turns a silent abort into a silent
segfault. The failing run is the only place the difference is visible, which is the same
property that let V0 ship.

Rows 4, 5 and the variant runs were performed against a scratchpad registry and
reverted; the working tree carries no change from this investigation.

## 2. Mechanism A — the declared dlopen surface is outside every closure

### 2.1 The walk

`resolve_runtime_closure` (`src/runtime/elf.cppm:920`) seeds its queue with the
artifact and one thing only (`elf.cppm:943`), then follows `DT_NEEDED`. An
unresolvable SONAME is recorded (`elf.cppm:979`) and, under a hermetic binding,
becomes `Unresolvable` with the message that names the private loader
(`elf.cppm:1229`).

`libur_adapter_cuda.so.0` is never a `DT_NEEDED` of anything in that closure.
`libsycl.so.9` loads `libur_loader.so.0`, which `dlopen`s each adapter — the
trace in row 3 shows it trying the bare SONAME first and then the absolute path
inside the farm. Nothing in the artifact's link-time graph names it, so it is
outside the walk **by construction**, not by oversight.

Measured, with the artifact's real `DT_RPATH` as the search path:

```
libur_adapter_cuda.so.0         FAIL libnvidia-ml.so.1
libur_adapter_opencl.so.0       FAIL libOpenCL.so.1
libur_adapter_level_zero.so.0   ok
libur_loader.so.0               ok
libsycl.so.9                    ok
```

### 2.2 mcpp already holds every input this check needs

`runtime_search_dirs` (`src/build/runtime_validation.cppm:449`) assembles the
exact directory list the artifact will use, and it is already passed to the
closure walk at `runtime_validation.cppm:676`. `plan.depRuntimeLibraryDirs`
(`src/build/plan.cppm:265`, filled at `plan.cppm:1124`) is the set of
dependency `runtime.library_dirs` — that is, the set of directories a package
published *because* its contents are reached by `dlopen` rather than by a
header or a link line.

So the missing check is not a missing capability. It is a missing edge: the one
surface mcpp itself put on the search path is the one surface it does not walk.

### 2.3 The second gap, and one disagreement between hosts

`libur_adapter_opencl.so.0` needs `libOpenCL.so.1`, which the farm does not
carry either. On this host that adapter fails; on the reporter's host the trace
shows it **loading**, and the Intel CPU device it enumerated is what their
default selector then chose. Some path on their machine supplies
`libOpenCL.so.1` and it is not visible from here. The gap is real on both — the
farm carries no ICD loader — but its consequence is host-dependent, and that
disagreement is unexplained. See §7.

This also refines the reporter's causal chain in one place. The abort does not
depend on a wrong device being selected. On this host **no device existed at
all** and the first thrown exception is `No device of requested type
available.`; the observable outcome is identical, down to the exit code. "The
default selector picked the CPU" is a companion symptom of the same missing
adapter, not a link in the chain.

### 2.4 Why the package's own criterion is green

`mcpp-index`'s `tests/examples/sycl-runtime/tests/farm.cpp` exists to assert
that this farm works. It `dlopen`s three sonames: `libsycl.so.9`,
`libur_loader.so.0`, `libumf.so.1`. The farm has twenty-five entries. The two
that are broken are not among the three.

The reason its comment gives for not testing the adapters is that "a machine
with no GPU is a legitimate configuration and is what every runner in this
repository is". That reason does not hold: **whether an adapter can be
`dlopen`ed is a packaging property, not a device property.** This host has no
Level Zero device and both Level Zero adapters load. The two that fail report
`cannot open shared object file`, which is a statement about the farm, not
about the hardware.

There is a real obstacle behind the wrong reason, and it must be handled or the
repaired test will be red on every runner and will be reverted: on a machine
with no NVIDIA driver the farm's `libcuda.so.1` is a **deliberately dangling**
symlink — `xim:libcuda-host-link` creates it pointing at the canonical path so
that installing a driver later self-heals every consumer. A naive "dlopen
everything" is red there. §5 states the three-state rule that both this test and
the mcpp-side check need.

## 3. Mechanism B — a hand-written name where the same function enumerates

`compat.sycl-runtime`'s `install()` builds the farm in two halves:

* the payload's libraries are **enumerated** — `ls -1 <payload>/lib`, link
  every versioned SONAME;
* the driver is **hand-written** — one `ls` for
  `xim-x-libcuda-host-link/*/lib/libcuda.so.1`, one link, one name.

The recipe's own header explains, at length and correctly, why the driver has
to be in this farm: once the payload's libraries acquired `RUNPATH = $ORIGIN`,
a non-empty RUNPATH switched off the inherited `DT_RPATH` for their
dependencies (mcpp#460), so the adapter can no longer reach a driver two farms
away. Every word of that reasoning applies to `libnvidia-ml.so.1` unchanged.
The defect is not in the reasoning; it is that the conclusion was written as a
name instead of as a set.

Two ecosystem constraints bear on the repair, and they rule out one of the two
options the issue proposes.

**GPU-related index packages are not permitted to probe the host.** The
sentinels `xim:libcuda-host-link` and `xim:nvidia-gl-host-link` are the single
source of truth for where the driver is; `hostlib.lua` records the history —
four independent probes, three of them wrong, one returning a 32-bit
`libcuda.so.1` on a biarch host and failing three layers away (mcpp#352). The
issue's option 2 — copy `farm_libc_stubs` and change the library name — looks
like precedent but is not: that helper searches the **xpkg store**, and
`libnvidia-ml.so.1` is not in any store. Copying it would produce a host probe,
which is the forbidden shape, and would bypass the ELF-class check and the
self-healing dangling-link semantics the sentinel already implements.

**A version key selects the anchor URL and does not freeze behaviour.** There is
one `install()` in that recipe and it never reads `pkginfo.version()`, so
installing `2026.09.06` today builds the farm `2026.09.07` builds. The
consequence for this repair is the opposite of what it sounds like: existing
pins pick the fix up automatically, but a machine that **already has** the
directory does not reinstall, so a new version key is still required to reach
installed hosts.

## 4. Mechanism C — two unwinders in one process

This is the part the issue does not contain, and it is the reason the failure is
as hard to diagnose as it is.

The island declares a synchronous failure path
(`examples/09-heterogeneous/sycl/app/src/kernels/saxpy.sycl:114`):

```cpp
} catch (const sycl::exception& e) {
    std::fprintf(stderr, "sycl: no usable device: %s\n", e.what());
    rc = -1;
}
```

On this host that path should run — there is no usable device — and `main`
should print `device unavailable` and return 1. It aborts instead. The throw is
on the main thread, three frames below the handler:

```
#0 __cxa_throw (tinfo = typeinfo for sycl::_V1::exception)   libstdc++
#1 sycl::_V1::detail::select_device …                        libsycl.so.9
#3 sycl::_V1::queue::queue<…>
#4 saxpy_device                                              the frame whose caller has the catch
#6 main
```

At `abort`, the reason is explicit:

```
#3 __cxa_call_terminate
#4 __gxx_personality_v0 (actions=6)      libstdc++
#5 unwind_phase2 ()                      0x555555618… — the executable's LLVM libunwind
#6 _Unwind_RaiseException ()             the executable's
```

and one frame deeper, during phase 1:

```
#0 _Unwind_GetIPInfo (context=0x7fffffffc930)  libgcc/unwind-dw2.c:360   libgcc_s
#1 __gxx_personality_v0 (actions=1, …)         eh_personality.cc:457     libstdc++
#2 _Unwind_RaiseException ()                   0x5555556181f6            LLVM libunwind
```

libstdc++'s personality routine reads an `_Unwind_Context` built by **LLVM
libunwind** through **libgcc's** accessor. The two structures are unrelated. The
IP it recovers is meaningless, the LSDA lookup at that IP finds no landing pad,
and phase 2 ends in `__cxa_call_terminate`. `__verbose_terminate_handler` then
rethrows in order to print the exception's type — through libgcc's
`_Unwind_Resume_or_Rethrow`, which is not interposed — and terminates again,
which is the `terminate called recursively` line and the reason no message is
printed.

The split is measurable and is an artifact of static-archive granularity:

```
libgcc_s _Unwind_* entry points           18
defined by the executable (libunwind.a)   10
still resolved in libgcc_s                 8   incl. _Unwind_GetIPInfo,
                                               _Unwind_GetCFA, _Unwind_Resume_or_Rethrow
```

The link line is `-nostdlib++` plus `libc++.a libc++abi.a libunwind.a` named
explicitly, under `-stdlib=libc++ -rtlib=compiler-rt --unwindlib=libunwind`; the
artifact's `DT_NEEDED` is `libsycl.so.9`, `libstdc++.so.6`, `libm.so.6`,
`libc.so.6`, and libgcc_s arrives underneath libstdc++. The linker pulls from
`libunwind.a` only the members that something references and exports them
because a loaded shared library has undefined references to them; the other
eight names were never referenced by libc++abi and so were never pulled in.
Nothing chose ten; ten is what the archive resolution happened to need.

### 4.1 The other fifty-eight

The warning says 68, and the unwinder family is only ten of them. The rest are
libc++abi's and libc++'s copies of the `std::` exception root — `vtable`,
`typeinfo`, `typeinfo name`, the destructors and `what()` for `std::exception`,
`bad_alloc`, `bad_cast`, `bad_typeid`, `bad_exception`, `bad_array_new_length`
and `type_info` — plus, for `std::logic_error` and `std::runtime_error`, their
**constructors and `operator=`**.

Those last two are the same partial-interposition shape as the unwinder, moved
from control flow to object layout. `std::runtime_error`'s constructors are
exported and its **destructor is not**. libstdc++-compiled code that constructs
one therefore builds a libc++ object — whose payload is a
`__libcpp_refstring` — and destroys it through libstdc++'s destructor, which
expects a `__cow_string`. Nothing in this example exercises that path, so it is
a hazard rather than a measured failure; it is recorded because the repair that
fixes the unwinder does not by itself remove it, and V2 does.

Three consequences worth stating separately:

1. **The condition is already detected.** mcpp's duplicate-symbol warning names
   these symbols and names `libgcc_s.so.1` as the other provider. It classifies
   the impact as "the library's own copy is never called". For the unwinder
   family the impact is that exception handling does not work.
2. **The seam discipline cannot fix it.** The island rule — device code is
   reached only through `extern "C"` — is about symbols crossing a boundary the
   author writes. The unwinder is reached through the process-global symbol
   namespace by code neither side wrote.
3. **It only shows on the error path.** When nothing throws, this artifact runs
   correctly and prints `12 24 36 48`. That is why it shipped.

The island's comment (`saxpy.sycl:22-31`) says the one failure it cannot turn
into a return code is a missing device image, "which is why the manifest names
the device — and why `mcpp.rules.sycl` warns at build time when it does not".
That mitigation was written against one cause. This issue is the second: the
manifest named the device, the image was compiled for that device, and the back
end that consumes it never loaded. The mitigation is conditioned on the
manifest, and the failure is conditioned on the runtime.

## 5. The three-state rule

Both repairs that walk a farm need the same distinction, and it is stated once
here so that each can cite it and neither can be folded into the other:

| State of a farm member's `DT_NEEDED` SONAME | Meaning | Report |
|---|---|---|
| resolves on the artifact's search path | nothing to say | silent |
| no entry anywhere on that path | **packaging gap** | name the member, the SONAME, and the package that published the directory |
| an entry exists in the farm but is a dangling symlink | **machine gap** — the sentinel's self-healing shape, no driver installed | distinct wording, never an error |

A `DT_NEEDED` that is an absolute path (`nvidia-gl-host-link` deliberately
patches `/lib/x86_64-linux-gnu/libGLX_nvidia.so.0` into the GL farm) is resolved
as a path, not searched. A survey that searched it reported the GL and Vulkan
farms as broken; they are not, and that survey is not evidence about them.

## 6. Repairs

Seven, in three repositories. Each carries its own criterion, because a
requirement that shares another repair's criterion disappears when that repair
ships.

### R1 — `xim-pkgindex`: the sentinel names a set

`pkgs/l/libcuda-host-link.lua` probes and links one name. Make the name a list
— `libcuda.so.1`, `libnvidia-ml.so.1` — through the same `hostlib.path_of`, the
same ELF-class check, and the same canonical-path fallback for the
not-yet-installed case, so both links self-heal identically.

*Criterion.* On a host with a driver, both files exist under the sentinel's
`lib/` and both resolve. On a host without one, both exist and both dangle, and
the package still installs successfully. Denominator: the recipe's name list,
asserted by count, so that a list that silently became empty is visible.

### R2 — `mcpp-index`: `compat.sycl-runtime` enumerates the sentinel too

Replace the hand-written `libcuda.so.1` lookup in `install()` with an
enumeration of the sentinel's `lib/` directory, filtered by the same
versioned-SONAME rule the payload half already uses (an unversioned name would
reach the linker, which the header explains). Publish a new version key so hosts
that already hold `2026.09.07` reinstall. `compat.cuda-runtime` carries the same
hand-written shape and should be converted with it.

*Criterion.* Two checks, and the second is the one that fails today:
1. after install, `<farm>/libnvidia-ml.so.1` exists;
2. the private loader resolves the cuda adapter's whole `DT_NEEDED` against the
   artifact's real search path — `ld.so --library-path "$RPATH" --list
   <farm>/libur_adapter_cuda.so.0` prints no `cannot open shared object file`.
   This must be run with the fix removed as well; it is red today and must be
   red again if R2 is reverted.

### R3 — `mcpp-index`: the farm's criterion enumerates its own members

`tests/examples/sycl-runtime/tests/farm.cpp` checks three hand-written names.
Change it to enumerate the farm directory and apply §5 per member: dangling
entries are skipped with a printed note, missing SONAMEs fail. Print the number
of members examined.

*Criterion.* The test reports a member count greater than zero (a farm that
failed to build must not read as a pass), fails on today's `2026.09.07` farm
naming `libur_adapter_cuda.so.0` and `libnvidia-ml.so.1`, and passes after R1
and R2. It must also pass unchanged on a GPU-less runner, which is the check
that decides whether the three-state rule was implemented or merely written
down.

*`dlopen` OF EACH MEMBER IS NOT ENOUGH, and this was measured rather than
foreseen.* The first implementation enumerated the farm and `dlopen`ed every
member, which is the obvious reading of "enumerate the population". It passed on
a farm with NVML removed, because `dlopen` measures the PROCESS: another farm on
the same search path supplied the missing soname (see R7). A package's test has
to be able to fail on that package alone, so the test reads each member's
DT_NEEDED itself and resolves it against the farm plus the three sonames the
artifact has already loaded. The three states are then decided from the farm's
own contents rather than from whatever else the run happened to have.

*Measured, all three situations:*

| situation | reading |
|---|---|
| repaired farm, driver present | PASSED, members 26, walked 26 |
| NVML removed from the farm | FAILED, names the member and the soname |
| driver links made dangling | PASSED, members 26, walked 24, two noted |

### R4 — `mcpp`: the closure check reaches the declared dlopen surface

Extend the artifact verdict: for each directory in `plan.depRuntimeLibraryDirs`,
read each ELF member and resolve its `DT_NEEDED` against the same `searchDirs`
already computed at `runtime_validation.cppm:676`, applying §5. This needs no
knowledge of SYCL, CUDA or Unified Runtime, which is what keeps it on the right
side of `test_runtime_contract`'s prohibition on branching in mcpp's source on a
provider's vocabulary. Cost is a few dozen ELF headers.

Severity is **advisory, not fatal**. A farm legitimately holds host-driver links
that dangle, and the check must not turn a CPU-only machine's correct
configuration into a failed build.

Publish the count of members examined into `resolution.json` alongside the
findings. A check whose "nothing found" and whose "nothing looked" read the same
is the failure mode this repository has named more than once.

*Criterion.* A unit test in `tests/unit/test_elf_runtime.cpp` over a synthetic
directory holding three members — one whose `DT_NEEDED` resolves, one naming an
absent SONAME, one that is a dangling link — asserting three distinct outcomes
and the member count. Field assertions on the record, not substring matches on
the message. A second check builds the SYCL example against an unrepaired farm and
asserts the warning names `libnvidia-ml.so.1`; it does not need a GPU, which is
the point.

*Ordering.* R4 lands **after** R1 and R2 reach the index. Landed first, it warns
correctly and loudly about a released package that no user can repair, and the
first thing it reports is our own fixture.

*The check is configuration-sensitive by construction, which is the point.* Run
against this host's long-lived `~/.mcpp`, it reported the OpenCL gap and NOT the
NVML one -- because that registry's SubOS library view carries
`libnvidia-ml.so.1` through `xim:nvidia-gl-host-link`, so on that configuration
the adapter genuinely resolves. The reporter's machine had no such view. A check
that answered the same on both would be answering from a table rather than from
the search path, which is the failure it exists to remove.

### R5 — `mcpp`: the duplicate-symbol warning states the real consequence

When the duplicated set intersects the `_Unwind_*` family, the existing warning
should say that exception handling across the boundary will not work, rather
than that the library's own copy will not be called. Text only; no behaviour
change.

*Criterion.* The SYCL example's build output contains the unwinder-specific
sentence, and a build whose duplicates are ordinary symbols does not. Two checks,
because a message that always appears carries no information.

### R6 — `mcpp`: one unwinder per process, and no exported seam

The invariant is that a process has exactly one unwinder. This artifact has two,
split by which archive members were referenced.

The repair is **both halves of V2**, and the §1 matrix says why neither alone is
the answer:

* `--unwindlib=libgcc`, dropping `libunwind.a` from the unit's link line, for an
  artifact whose link line already names libstdc++. libc++abi and libstdc++ then
  call libgcc's entry points on libgcc's contexts. Measured: the island's
  handler runs, the program prints its diagnosis and exits 1. This is the half
  that fixes the reported silence.
* `-Wl,--exclude-libs,ALL`, which drops the exported set from 58 to 0 and
  removes §4.1's constructor-without-destructor hazard along with it.

**`-Wl,--exclude-libs` alone is not a repair, it is a regression** — this was
written here as a prediction from the mechanism and then measured as V3: exit
139 instead of 134. Hiding the executable's `_Unwind_*` makes libstdc++ bind to
libgcc, but the executable's own libc++abi still calls its statically linked
libunwind internally, so an exception raised there and unwound through a
libstdc++-compiled frame reproduces the mismatch in the opposite direction. The
flag looks exactly right, its run with a device is clean, and it makes the failure
harder to read. Nobody should have to rediscover that.

*Predicate.* The change applies to an artifact whose link line names libstdc++.
It is decidable where the link line is assembled, and it is narrow: an artifact
that does not load libstdc++ keeps today's hermetic `libunwind.a` and gains no
`DT_NEEDED` on `libgcc_s.so.1`. V1 and V2 both acquire that entry; it is
satisfied from the payload farm already on the search path, which is why both
variants run.

*Criterion.* Four cells, all measured on this host and all four required:

| | when it fails | with a device |
|---|---|---|
| before | exit 134, no text | `12 24 36 48` |
| after | `sycl: no usable device: …`, exit 1 | `12 24 36 48` |

plus two structural assertions: mcpp's own duplicate-symbol warning does not
appear for this example after the change (it is the cheapest denominator
available — it counts the seam), and an artifact with no libstdc++ on its link
line is byte-for-byte unchanged.

*Implemented as V4, which is V2 with the archives named.* `--exclude-libs,ALL`
was the measured variant; the shipped one lists `libc++.a` and `libc++abi.a`,
matching the spelling `hide_static_cxx_runtime` already used for shared
libraries, so a user's own static library linked into an artifact keeps its
exports. Measured to be equivalent for this purpose: 0 exported symbols, 0
overlapping, exit 1 with the diagnosis when it fails, `12 24 36 48` with a
device.

*The predicate lives where the mechanism table already is.* `MechanismInput`
gains `foreignCxxRuntime`, set in `flags.cppm` from `bc.ldflags` containing
`stdc++` while the toolchain's library is libc++; `distribution.cppm` reads it
in the libc++ ELF branch. The same field widens `hide_static_cxx_runtime` from
shared libraries to executables, correcting the comment quoted in §4.

*One documented claim was refuted along the way.* The SYCL example states that a
missing device image is the one failure its island cannot turn into a return
code, "because it throws from inside the scheduler in neither of those two
paths". Measured by compiling the same example for `sm_90` and running it on an
sm_89 device: with two unwinders, `terminate called after throwing an instance
of 'ur_result_t'`, exit 134; with one, the island's own handler prints the
build log and `main` prints `device unavailable`, exit 1. It was not outside the
catches; no catch worked. The example's comment and README are corrected.

*Status.* Measured and implemented. What is **not** measured is the scope
question in §7: `--exclude-libs` on a shared-library artifact is unchanged
behaviour, but an executable that deliberately re-exports an interface from a
static archive would now hide it -- no such artifact exists in this tree, and
the flag is emitted only for the libc++-plus-libstdc++ combination.

### R7 — decide `libOpenCL.so.1`

Either declare the install-time edge to `compat.opencl` and farm the ICD loader,
or record that the SYCL lane deliberately offers no OpenCL back end. The present
state — neither carried nor stated — is what let the reporter's machine select a
CPU device silently.

*Criterion.* R2's second check applied to `libur_adapter_opencl.so.0`, plus the
one that made the reversal safe: with the edge declared, removing NVML from the
farm must still fail R3.

*Decided: SERVED, after one round of deciding the opposite.* The history is
worth keeping, because the wrong decision was made from a real measurement.

The edge was written -- `deps = { ["compat.opencl"] = "2026.05.29" }` -- and the
farm test then passed with every member loading. It also passed with
`libnvidia-ml.so.1` REMOVED from the farm. `compat:opencl` depends on
`compat:opencl-runtime`, whose farm mirrors the host's NVIDIA OpenCL family and
therefore carries NVML: the new dependency was satisfying the need R2 exists to
satisfy, and R2's criterion could no longer fail. On that reading the edge was
withdrawn and the adapter was recorded as unserved.

**That reason expired the moment R3 was hardened.** The masking was a property
of a criterion that measured the PROCESS, and R3 was rewritten -- because of
this very measurement -- to read each member's DT_NEEDED against the farm
alone. Once nothing on the search path can answer for the farm, the only
objection left was the size of the surface, and "the payload ships an adapter
that can never load" is not something a runtime adapter should leave standing.

So the edge is declared. Measured with it in place: the farm test passes, and
with NVML removed it still FAILS naming `libur_adapter_cuda.so.0` -- the
criterion is no longer maskable, which is what made the decision reversible.

`libOpenCL.so.1` stays named in the test, with its meaning changed: not "this
package does not serve it" but "a declared dependency of this package provides
it". Everything not on that short list must be in the farm, and the list is
what keeps the self-sufficiency assertion exact without reopening the process
to answer for it.

*And it found three defects in R4*, all of them false positives, and all of
them invisible until a project with a shared dependency was measured:

1. **`$ORIGIN` is not in `runtime_search_dirs`.** Every artifact carries it
   first in its DT_RPATH and a shared dependency is deployed BESIDE the
   executable, so the library resolves at run time and read as missing here.
   `runtime_search_dirs` cannot carry it: `$ORIGIN` is a property of each
   artifact, not of the plan. The artifacts' own directories are added in
   `check_dlopen_surface`.
2. **A plan that produces no program has no surface to judge.** The adapter
   package is `kind = "lib"`, and reporting a consumer's surface against an
   archive's non-existent search path named a library the consumer resolves.
3. **A SONAME is not a filename.** mcpp links `bin/libopencl.so` whose SONAME
   is `libOpenCL.so.1`, and the alias under the SONAME appears later; `mcpp
   test` calls the check twice and only the second call saw it. The SONAMEs
   this build produces are read from the objects and passed in.

After all three, the same project reports the same four findings on both calls,
and every one of them is real -- they are the `compat:vulkan-runtime` class
already filed as mcpp-index#376, reached this time through
`compat:opencl-runtime`.

## 7. Open

* **The OpenCL disagreement (§2.3).** The adapter loads on the reporter's host
  and not here, from the same pinned payload. R7 now serves the adapter through
  `compat:opencl`, so the practical consequence is gone; what is still
  unexplained is why their machine supplied `libOpenCL.so.1` without it.
* **What R6 does to an executable that is itself a plugin host.** Hiding
  `libc++.a` and `libc++abi.a` means a library `dlopen`ed later cannot resolve
  the C++ standard library from the executable. That is the intended direction
  — it is how the second runtime stops leaking — but a libc++-built plugin
  loaded into such a process would now find no provider at all. The
  configuration is narrow (it requires libstdc++ already on the line, which is
  the broken state this repairs) and no artifact in this tree is one, so it is
  recorded rather than handled.
* **Whether other farms have packaging gaps — answered, by R4 itself.** The
  earlier survey here used a deliberately partial search path and its findings
  were about the survey. R4 run per project answers it properly, and the first
  thing it caught is this ecosystem's own `compat:vulkan-runtime`: 4 of 55
  libraries on a host with an NVIDIA driver.

  | library | needs | on this host |
  |---|---|---|
  | `libnvidia-encode.so.1` | `libnvcuvid.so.1` | present in `/usr/lib`, absent from the farm |
  | `libnvidia-opticalflow.so.1` | `libnvcuvid.so.1` | same |
  | `libnvidia-pkcs11-openssl3.so.550.144.03` | `libcrypto.so.3` | same |
  | `libnvidia-pkcs11.so.550.144.03` | `libcrypto.so.1.1` | same |

  All four are real and all four are the same shape as mcpp#596 -- a farm that
  mirrors a driver family and stops one library short. They are NOT repaired
  here: a check whose first catch is its author's own package should report it,
  not quietly absorb it, and the repair belongs to `compat:vulkan-runtime` with
  its own criterion, filed as mcpplibs/mcpp-index#376. On a runner with no NVIDIA driver the farm is nearly empty
  and the check is silent, so this does not appear in CI.

## 8. Order

R1 → R2 (R2 reads what R1 publishes) → R3 (asserts what R2 produced) → R4
(would otherwise report a released package no user can repair). R5 and R6 are
independent of that chain and of each other; R6's second half is gated on the
artifact-kind question in §7, its first half is not. R7 landed with R2.

R5 and R6 are worth stating as one sentence, because they are the same finding
seen from two sides: R6 removes the seam, and R5 is what should have been said
about it while it was still there. Neither is a consequence of #596 — the seam
predates it, the example's README announces it, and the example runs correctly
across it every time nothing throws. #596 is simply the first time something
threw.

## 8.5 What serving the OpenCL adapter uncovered

R7's reversal put a shared library into every SYCL project's plan for the first
time, and two latent defects became active the moment it did. Both are repaired
here; neither is caused by #596, and neither would have been found by reading.

**A build program's objects reached a dependency's image.** `role = "object"`
with no named target attaches to "every linked image", and that was reading as
"every link unit in this plan" -- which includes the shared library a
dependency contributes. Measured: `compat:opencl`'s ICD loader, a C library,
came out of the link with `saxpy_device` and thirty-seven `sycl::`
instantiations in it, 193 dynamic symbols where its own API is 154, and the
process held two copies of the island. `LinkUnit::dependencyOwned` now says
which images are this package's, and the rule reads "every image THIS PACKAGE
produces".

**The symbol-provision check could not tell a weak definition from a strong
one.** `DynamicSymbol` recorded the type and not the binding, so vague-linkage
definitions -- template instantiations, inline functions, vtables, which the
C++ ABI emits into every image and expects the loader to unify -- were counted
as a second provider. `hide_static_cxx_runtime`'s own comment had already
written the rule ("must not ... unifying those across the process is the
intended C++ ABI behaviour"); nothing enforced it one layer up. The binding is
recorded now, weak definitions are counted rather than reported, and the count
is printed so "clean" cannot read as "did not look".

Both were invisible before because the same artifact had 68 real findings
sitting on top of them. A check whose noise is repaired shows what the noise
was covering, which is the third time this issue has produced that shape.

## 9. What this touches, across the three repositories

The review that asks the other question: not "is each repair right" but "what
else moves when they land".

| Change | Who reads it | What happens to them |
|---|---|---|
| `xim:libcuda-host-link` 0.0.2 | `ollama` pins 0.0.1 | unchanged; 0.0.1 is kept and still resolves |
| | `compat.cuda-runtime` pins 0.0.1, frozen | untouched, deliberately: its header says it receives no new versions |
| | `compat.cuda-driver`, `compat.sycl-runtime` | move to 0.0.2, which is the only place that decides which sonames they get |
| `compat.*` 2026.09.10 | consumers pinning 2026.09.05/07 | still resolve; a machine that already holds the directory keeps the old farm until something asks for the new key |
| | `examples/09-heterogeneous/sycl` | pin moved, in the mcpp change |
| R4 (the surface walk) | every Linux build with a dependency `runtime.library_dirs` | a warning where there is a real gap. Measured: 0 for a project with no such dependency, 1 for the SYCL example (the declared-unserved OpenCL adapter), 4 for the Vulkan example on a host with an NVIDIA driver (real, filed as mcpp-index#376) |
| | a non-hermetic binding, or `allow_host_libs` | silent, for the reason the artifact verdict is |
| R6 (one unwinder) | a libc++ link line naming libstdc++ | `--unwindlib=libgcc` and hidden archives |
| | every other link | byte-for-byte unchanged, asserted in `test_distribution.cpp` |
| R7 (`compat:opencl` declared) | every SYCL project | the OpenCL back end loads; one shared library and one symlink farm added to the graph |
| | the two defects in §8.5 | latent before, active from the moment a shared library entered a SYCL plan |
| `compat.opencl` on Windows | a Windows OpenCL consumer | a loader to link, where there was none; no adapter, because the system loader needs no help |

The one regression this could cause is in §7's last item: an executable that
deliberately re-exports the C++ standard library to a plugin it `dlopen`s. It
requires libstdc++ already on the line, which is the broken state R6 repairs,
and no artifact in this ecosystem is one.
