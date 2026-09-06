# Ascend, through the same island

`op_kernel/` beside `op_host/` is how CANN's own operator libraries are already
laid out — every operator in `ops-math` splits that way on disk. The island is
therefore not a shape mcpp imposes on Ascend; it is the shape Ascend already
has, and this example writes it in mcpp's vocabulary.

`.asc` is that seam made checkable, exactly as `.sycl` is in the SYCL example:
the file's content is C++ and nothing in it would tell a reader otherwise. What
makes it a device translation unit is that it goes to BiSheng — a compiler with
a device back end, and one that does not accept C++20 modules.

## This example does not build yet

Two pieces do not exist:

| Missing | What it is |
|---|---|
| `mcpp.rules.ascendc` | the rule package that drives BiSheng, the sibling of `rules-cuda` / `rules-spirv` |
| `xim:cann-toolkit` | an index package carrying the toolkit |

Nothing else is missing, and that is why the manifest is written out rather than
described. It is listed in `.github/tools/build_examples.sh` as skipped, with
that reason.

## What was established about the toolkit

Measured 2026-09-07 and recorded in
`.agents/docs/2026-09-07-general-build-infrastructure-gaps-design.md` section 10:

* **One payload, not two.** BiSheng and the simulator live in the same toolkit:
  `compiler/ccec_compiler/bin/bisheng` and `*/simulator/<SoC>/lib`.
* **It can be obtained anonymously.** The official distribution is a container
  image, `swr.cn-south-1.myhuaweicloud.com/ascendhub/cann`, and its registry
  issues a pull token without credentials. Fetching from the vendor's own
  registry is the tier this ecosystem's invariant already permits — linked
  where it is, never copied into a release of ours.
* **A device is not required to verify a device build.** Ascend C has three run
  modes, and `sim` needs no hardware. It is not a substitute for `cpu` mode:
  `cpu` links `tikicpulib` and compiles the same kernel source with the HOST
  compiler, so that graph contains no island at all and passing in it would
  prove the kernel's arithmetic rather than the build. Every `RUN_MODE` test in
  asc-devkit is `STREQUAL "cpu"` — there is no `sim` branch — so `sim` takes the
  same path as `npu` and BiSheng is invoked.

## Why `accelerator = "none"` for the fallback

`not(accelerator = "ascend")` would work today and rot tomorrow: `accelerator`
is an open vocabulary, so a fallback written by enumerating what it is not
changes meaning every time the ecosystem gains a backend. Ascend is itself an
instance of that growth, which is the neatest possible argument for the
spelling.
