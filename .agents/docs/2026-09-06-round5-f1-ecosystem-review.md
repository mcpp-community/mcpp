# Ecosystem review — round 5, F1 (llama.cpp on Vulkan)

Four repositories, one lane carried from an engine primitive to generated text
on a machine with no GPU. Written against the measurements, not the intent.

| repository | change | state |
|---|---|---|
| mcpp-index | `compat:spirv-headers@1.4.357.0` | merged (#358), CN mirror byte-identical |
| mcpp | two engine fixes, the example regrouping, two new checks | PR #576 |
| llama.cpp-m | `backend-vulkan`, released as `b10069.1` | PR pending mcpp's release |
| mcpp-plugins | none needed | 0.2.0 already carries the glslc route |

## What was actually verified

Not "it built". The criteria, and what each can say no to:

* **The device computed the right thing.** The device decode and the host
  decode of one prompt under greedy sampling produce the SAME token (471), and
  the offload is asserted from llama.cpp's own log FIRST -- without that, two
  host decodes agree trivially. A token merely inside the vocabulary is
  produced by a backend that computed nonsense.
* **The shaders are graph edges.** 136 declared action rules for 134 vendored
  shaders plus the generator and the header; 134 generated sources joined the
  compile set; `ar t libllama.a` carries 134 shader objects. A build program
  that looped would leave the same artifacts and no edges.
* **The feature costs a CPU consumer nothing.** With the lock removed, a
  CPU-only build writes no `mcpp.lock` at all: it resolves nothing.
* **The artifact's host surface.** `ldd` outside the mcpp registry: the
  kernel's vDSO and the Khronos loader this ecosystem builds from source.
* **The published form.** V5 section F builds a consumer that declares the
  dependency and the feature and nothing else. Red until the index entry lands,
  by construction.

## The defects this round produced

Seven, none reported by a pre-existing test; six found reviewing work written
in this same round.

| where | defect | what surfaced it |
|---|---|---|
| mcpp engine | `[feature-xlings]` tool installed and then invisible to `xpkg_dir` | building the backend |
| mcpp engine | `[feature-deps]` shared library compiled and then left off the link line | the same build, at link |
| mcpp manifest | the version warning predicted a failure it cannot see | V5's own output |
| llama.cpp-m | the manifest claimed a platform refusal the code did not make | reading the comment against the code |
| llama.cpp-m | a probe that failed for any other reason reported "supported" | reading upstream's rule |
| llama.cpp-m | a warning quoting a diagnostic truncated at the first newline | the directive channel is line-based |
| llama.cpp-m | `-static` not portable between the compilers; the clang variant that links SEGFAULTS | building under `--toolchain llvm@22.1.8` |

**The two engine defects are one shape**: an answer resolved and never wired to
the decision that reads it. Neither had a diagnostic, and the second was quiet
because of its SHAPE -- a library package's `mcpp build` produces an archive,
and an archive resolves no symbols.

## Two of my own criteria were pointed at the wrong object

Both would have shipped as green.

* **e2e 615, first draft.** With a binary root it passed on both the defective
  and the fixed engine -- measured. A fixture built around `mcpp run` was a test
  that could not fail. The defect needs a library root whose test binary links.
* **V5 section C, first draft.** It grepped `resolution.json`, which records
  the machine environment's runtime binding and names `mesa-lavapipe` whether or
  not the project asked for it. `mcpp.lock` records what the project resolved.

## Stated rather than fixed

* **`backend-vulkan` needs libstdc++ on this checkpoint** -- upstream destroys
  a `std::unique_ptr<vk_memory_logger>` where the class is forward-declared.
  Deliberately not refused: the distinction a refusal needs (which standard
  library the project resolved) is not something mcpp hands a build program,
  and the compiler's name would refuse clang with libstdc++, which works. That
  gap should land WITH its consumer, not as a field nobody reads.
* **ggml excludes a software Vulkan device on TYPE** -- lavapipe advertises
  every required feature and is dropped for being `eCpu`. Upstream's policy,
  upstream's selector.
* **Two local e2e failures are machine state.** Both reproduce on the binary
  predating every change here, and `ci-linux-e2e` is green on the base commit.

## Verdict

The lane is complete and the evidence is behavioural at every layer. Each
engine fix has a criterion that exits 1 on the previous binary and 0 on this
one, measured with the two binaries side by side.

Remaining risk is entirely the release chain, whose order is forced: mcpp must
be installable before llama.cpp-m's CI can be green, and llama.cpp-m must be
tagged before the index can name it.
