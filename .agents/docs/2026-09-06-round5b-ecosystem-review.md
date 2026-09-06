# Ecosystem review — round 5b

Round 5 shipped the framework tier and recorded four items as open, each with a
stated reason for being open. This round closed three of them, released the
engine change and its consumer together, and found five defects. Four of the
five were in CHECKS rather than in features, and that is the finding worth
keeping.

| repository | change | released as |
|---|---|---|
| mcpp | `cxx_stdlib`, the curriculum in CI, device names, two machine-dependent criteria | 2026.9.6.3 |
| mcpp | naming the std-module-precompile refusal | on main, `a8f5b359` |
| mcpp-plugins | `mcpp.rules.sycl` names the C library | 0.2.1 |
| llama.cpp-m | `backend-vulkan` refuses a libc++ toolchain by name | b10069.2 |
| mcpp-index | plugins 0.2.1 (#360); llamacpp b10069.1 and .2 with the CI pin (#361) | — |
| xim-pkgindex | mcpp latest -> 2026.9.6.2, then 2026.9.6.3 | #771, #773 |

## 1. An answer the engine had and never handed over

`mcpp::cxx_stdlib()` returns which C++ standard library resolved. The engine
had that value in the cache key, the ABI tag, the toolchain fingerprint and
`resolution.json`; it was never given to the layer that has to decide on it.

`compiler()` cannot substitute. clang links libc++ on one machine and libstdc++
on another and answers `clang` in both cases, and the two differ in what they
accept. llama.cpp-m had the case waiting: its Vulkan backend does not compile
under libc++, because upstream destroys a `unique_ptr` to an incomplete type,
and b10069.1 documented that while explicitly declining to refuse it, on the
ground that refusing by compiler name would also refuse the configuration that
works.

The accessor and its consumer shipped in the same round on purpose. An accessor
without a consumer is a recorded field with no reader, which is the shape this
ecosystem keeps finding; a consumer without the accessor is what b10069.1 was.

## 2. Four defects in checks, one in a feature

| # | where | what |
|---|---|---|
| 1 | mcpp e2e 206 | asserted `inconclusive` on the assumption that the private loader cannot reach a host `libtinfo`. `xim:ncurses` is an ordinary ecosystem package, and a sub-OS that has it puts `libtinfo.so.6` on the artifact's RPATH, where the closure genuinely closes |
| 2 | mcpp e2e 168 | selected its musl payload with `ls \| head -1`, which is lexicographic order, hence the OLDEST of three installed versions -- one predating the `std` module |
| 3 | mcpp refusal taxonomy | `Code::StdModulePrecompile` was declared, named, and had ZERO writers, so every std-module refusal reported `other` |
| 4 | llama.cpp-m CI | the new refusal check ran under `bash -e` and aborted before any assertion; a trailing `grep -q ... && {...}` would then have failed it in the other direction; and its "before anything compiles" test searched for a filename the REFUSAL ITSELF NAMES |
| 5 | mcpp examples | all four device islands printed the same four numbers as their CPU fallback, so a silent fallback was indistinguishable from a device run |

1 and 2 were green on every CI runner, and for one reason: **a runner installs
exactly one of anything**. Neither criterion was wrong about the engine; both
were wrong about the machine, and only a machine with several versions of the
same payload could say so.

3 is the same family seen from the other side. The taxonomy comment says an
unnamed branch reports `other`, "a visible admission rather than a silent merge
into a neighbouring reason", and `scan.sh` prints those admissions on every run.
The admission was visible for as long as the code existed and nothing forced
anyone to read it. `expected.tsv` carried exactly one `other` row out of 176.

4 is three faults in one twenty-line step, all mine, and the third is the
instructive one: the criterion's object contained the thing that produces it.
The refusal message names `ggml-vulkan.cpp` because naming it is how the message
explains the cause, and the check read that explanation as evidence that the
compiler had run.

## 3. What now enforces what

* **e2e 617** compares `mcpp::cxx_stdlib()` against `resolution.json` and against
  the compiler family. Verified by removing the wiring and rebuilding: red.
* **`.github/tools/build_examples.sh`** enumerates the example ROOTS from the
  tree and compares them against a build list and a skip table. A root in
  neither fails the job, so adding an example forces a decision. Six of fifteen
  build; every skip names its reason and where the coverage is.
* **`.github/tools/check_matrix_reasons.sh`** refuses an `other` or `mismatch`
  row in the target matrix, and refuses a reason that `refusal.cppm` does not
  define. Both directions verified by breaking them.
* **The Vulkan example is built AND RUN** on the lavapipe payload in CI, and the
  assertion is the device name, not the four numbers.
* **llama.cpp-m's refusal check** asserts the STAGE (`build.mcpp exited with 2
  (build aborted)`) and the absence of a `Compiling` progress line, which is
  independent of message wording. Replayed against the real failing output plus
  four controls.

## 4. What the sandbox could and could not decide

Run in `xlings subos use verify-963 --sandbox` with the CN mirror configured,
addressing mcpp by its store path, against the PUBLISHED 2026.9.6.3:

* **A** identity and mirror: `mcpp 2026.9.6.3`, mirror CN, index snapshot.
* **B** `compat:spirv-headers` delivers the Khronos layout and definitions.
* **G** `mcpp::cxx_stdlib()` answers `libstdc++` and matches that binary's own
  `resolution.json`. This section exists because the `mcpp` module a build
  program imports is EMBEDDED IN THE BINARY: a release whose module source did
  not travel would compile every project in this repository, which uses a
  checkout, and fail for the first user who wrote the new call.
* **C, D, E** listed as NOT RUN, each with the reason. A sandbox has no source
  checkout and no model.
* **F** red until the index carries the version, which is what its own header
  predicts: the release order is the reverse of the dependency order.

## 5. The index has two levers, and they are not interchangeable

`ggml-org:llamacpp@b10069.1` failed three workspace jobs with
`'toolchain_sysroot' is not a member of 'mcpp'`, because `validate.yml` pinned
an mcpp about ten releases old.

`index.toml min_mcpp` governs descriptor GRAMMAR -- the oldest mcpp able to
resolve every descriptor. `validate.yml MCPP_VERSION` governs which mcpp the
index builds its members with. A build program's API belongs to the second.
Raising the floor would refuse the WHOLE index to a client on it, over one
package's build-program call that client may never reach.

The cost is paid once per move and it is large: the members' caches key on
`MCPP_VERSION`, so the first run after it changes rebuilds every member on every
platform. That is why `mcpp:plugins` 0.2.1 went as its own PR -- it needed no
pin change, so `select` picked nothing for it and its CI was eight checks -- and
why `b10069.1` and `b10069.2` landed together in one pin move rather than two.

## 6. Method

Every criterion this round was checked by breaking it. That found three checks
that would have passed while measuring nothing, and one control of mine that
silently mutated a comment line because I reused a line number after adding
eleven lines above it. The rule the round produced:

**A check written against a failing command must be run under the shell that
will run it, with the success path and every failure path exercised.** Local
`bash script.sh` has no `-e`; the runner's does, and the difference is invisible
until the step aborts before its first assertion.
