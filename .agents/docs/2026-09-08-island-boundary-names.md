---
subject: heterogeneous
status: active
---

# The island boundary's names: one rule for both lanes, and the check that makes it true

A project that puts device code in several directories and several files gets no
help from the names it reaches that code through. `mcpp.tools.island` writes a
module and puts every entry point at global scope, while the shader lane writes
a module whose namespaces mirror the payload tree. This record says why the two
differ today, what the single rule should be, and which check has to exist
before that rule is honest.

Measured 2026-09-08 against `mcpp` `origin/main` at `9a71c9a6` (engine
2026.9.8.1) and `mcpp:plugins` `origin/main` (0.4.0).

---

## 1. What was measured

### 1.1 The rule the documentation already states

`docs/42-heterogeneous-builds.md:225` states it for both lanes at once:

> The module and the namespace are one identifier path, derived from names the
> project already wrote.

The data lane obeys it. `rules/spirv.cppm` derives a base directory from the
payload set (`common_base_dir`, :457), takes the path from that base to each
payload as namespace segments (`namespace_of`, :477), and emits **one** module
whose namespaces nest inside it. `shaders/post/tonemap.frag` under
`myapp.shaders` is reached as `myapp::shaders::post::tonemap_frag()`.

The island lane does not. `tools/island.cppm` emits `export using ::name;` at
global scope, so a consumer that writes `import app.kernels;` then calls
`saxpy_device(...)` unqualified. The module name buys nothing but a file name.

A second point the same measurement settles: **the data lane does not put
directories in the module name either.** There is one module per group. A design
that gave islands `import pkg.dir1.dir2` would not be aligning with the shader
lane, it would be inventing a third convention.

### 1.2 Why the island lane has no namespaces today

Not the C ABI, which both lanes have. The difference is who owns the symbol.

The data lane's accessors are `extern "C"` as well, but their linker names are
composed by the generator -- `accessor_base` (`src/plugins.cppm:309`) builds
`mcpp_embed_<module segments>_<namespace segments>_<identifier>` -- and the C++
namespace holds an inline forwarder onto that name. The path is IN the symbol,
so two payloads sharing a stem are two symbols.

An island's symbol is written by the author, in the island's own source, and the
generator only reads it (`entry_name` takes the identifier before the `(`). The
path cannot enter the symbol without rewriting the island's definition, which
needs a C parser and a second compiler's cooperation.

### 1.3 Three measurements on what a namespace over a flat symbol does

Run with `clang++` (DPC++ 7.1.0), `-std=c++23`, on 2026-09-08.

**A namespaced re-export of a C-linkage entity is well-formed.**
`export namespace app::dir1 { using ::compute; }` compiles, exports and links.

**It provides no isolation.** Two modules, `app.dir1` and `app.dir2`, each
re-exporting `::compute` into its own namespace, linked against one definition:

```
dir1=6 dir2=6 same=1
```

where `same` is `&app::dir1::compute == &app::dir2::compute`. Two namespaces,
one entity.

**A declaration split across two groups is not caught anywhere.** A module whose
header declared `int compute(float, unsigned)` against a definition of
`int compute(double, unsigned)`, exactly one of them in the link:

```
LINK: clean
0 (expected 6)
```

No compile error, no link error, no diagnostic. This is the defect
`mcpp.tools.island` exists to prevent, and it returns the moment the entry
points of one package are scanned in more than one call.

---

## 2. The rule

> The module name is the root. Each directory below the group's base directory
> extends the **namespace**. The leaf identifier is decided by the lane: the data
> lane derives it from the file name, because a payload has no name of its own;
> the island lane takes the entry point's name, because the author wrote one.

| | written | reached as |
|---|---|---|
| data lane | `myapp.shaders` + `shaders/post/tonemap.frag` | `myapp::shaders::post::tonemap_frag()` |
| island lane | `app.kernels` + `img_blur` in `src/kernels/image/blur.cu` | `app::kernels::image::img_blur(...)` |

One `import` per group on both lanes. One module, one header, namespaces nested
inside.

This is not a new rule. It is `docs/42:225` applied to the lane that does not
follow it, and the change to the island lane is one emission site.

## 3. What a developer sees

```
roots: src/backends/cuda, src/backends/cpu

src/backends/cuda/image/blur.cu     img_blur, img_sharpen, img_unsharp
src/backends/cuda/image/resize.cu   img_resize, img_crop
src/backends/cuda/audio/fft.cu      aud_fft, aud_ifft
src/backends/cuda/saxpy.cu          saxpy
src/backends/cpu/image/ops.cpp      the CPU implementations of the img_* entries
src/backends/cpu/audio/ops.cpp      the CPU implementations of the aud_* entries
src/backends/cpu/saxpy.cpp          the CPU implementation of saxpy
```

```cpp
import app.kernels;

app::kernels::image::img_blur(...);     // blur.cu, and blur's CPU implementation
app::kernels::image::img_resize(...);   // resize.cu, same directory, same namespace
app::kernels::audio::aud_fft(...);
app::kernels::saxpy(...);               // directly under a root, no segment
```

The file name contributes nothing, on either lane and for two independent
reasons. On the data lane it is already spent: it becomes the identifier. On the
island lane a file holds zero, one or many marked entry points and each carries
its own name, so there is nothing for a file name to name. A directory is also
the coarser unit: moving a function between two files in one directory is an
edit developers make freely, and it must not rename anything a consumer wrote.
`ops.cpp` above holds five entry points and names none of them.

What each implementation of one entry point shares is its **directory** within
its root -- `image/` under `cuda/` and `image/` under `cpu/` -- and not its file
name.

## 4. The check that makes the namespace true

A namespace over a flat symbol is a lookup alias (§1.3). It becomes honest only
if a name cannot appear in two of them. So the rule above is admissible only
together with:

**Uniqueness within a root.** Two entry points with the same name anywhere in
one root are refused, naming both files. With that check a name exists in
exactly one namespace, and the namespace never lies about what a call resolves
to. (§5 says what the several-roots case means and why it is not this one.)

**Agreement across roots, kept.** The same name in another root is the same
entry point implemented elsewhere. The two declarations must match verbatim, or
the build is refused naming both files -- the check `scan` performs today.

These two are conflated today. `scan` merges by name and treats textually
identical declarations as agreement, so "the seam's two halves" and "two
different functions that collided" produce the same reading. Separating them is
the substantive repair in this design; the namespaces are what makes the repair
visible.

## 5. The model: parallel roots, and no implementation is second class

A first draft of this design named one tree "primary" and called every other
implementation a "variant" with no location of its own. That is wrong on the
face of it: a CPU implementation is a backend, not an annex to one, and the
model forced it out of a `backends/` directory to satisfy the generator. The
model is roots instead.

**A project declares one or more roots. A name that appears in several roots is
one entry point implemented several times, and an entry point's namespace is the
path it occupies in the root that supplies the shape (below).**

Two layouts follow, and both are ordinary:

*Substitution* -- one entry point, several implementations, exactly one in any
link. Each implementation tree is a root, and one entry point's namespace is the
path it occupies inside each of them:

```
roots: src/backends/cuda, src/backends/cpu
  src/backends/cuda/image/blur.cu   img_blur   -> app::kernels::image::img_blur
  src/backends/cpu/ops.cpp          img_blur   -> the same entry point
```

The CPU tree is flat here and the CUDA tree is organised by subject, which is
allowed and changes nothing: the shape comes from one of them, and the other
only has to define the names.

*Additive* -- several backends with distinct names, all in one link, chosen at
run time as `examples/09-heterogeneous/multi-backend` does. One root holds them
and the backend directory becomes the namespace:

```
root: src/backends
  src/backends/cuda/saxpy.cu    opkit_cuda_saxpy   -> app::kernels::cuda::opkit_cuda_saxpy
  src/backends/vulkan/host.cpp  opkit_vulkan_saxpy -> app::kernels::vulkan::opkit_vulkan_saxpy
  src/backends/cpu/saxpy.cpp    opkit_cpu_saxpy    -> app::kernels::cpu::opkit_cpu_saxpy
```

The CPU implementation stays under `backends/` in both. Which layout a project
has is decided by the names its entry points carry -- one name means
substitution, distinct names mean addition -- and the roots it declares say
which of the two it means. Nothing in the model ranks one implementation above
another.

One root also supplies the **shape**, and this is a naming role rather than a
rank: every root compiles, links and is equally a backend, and exactly one of
them additionally answers where entry points live. Every other root provides implementations and its directory
structure is never read for naming. `opt.layout_root` names it and defaults to
the first root.

```cpp
opt.roots       = { "src/backends/cuda", "src/backends/cpu" };
opt.layout_root = "src/backends/cuda";   // the default is roots[0]
```

`cuda/image/blur.cu` puts `img_blur` in `app::kernels::image`, and it stays
there however `cpu/` is organised -- one flat file, six directories, or
reorganised next week. An entry point the layout root does not declare takes its
path from the first root that does, which is what a backend-only kernel needs.

A rejected alternative is worth recording, because it is the obvious one. Let
every root contribute, take the DEEPEST relative directory among an entry
point's implementations, and require the others to be prefixes of it. It accepts
the same layouts as the rule above, and it has a failure this one does not:
moving `cpu/ops.cpp` to `cpu/image/detail/ops.cpp` deepens the winner and
renames `app::kernels::image::img_blur` to `app::kernels::image::detail::img_blur`.
A refactor of a tree nobody consumes would rename what everybody consumes. The
shape has to come from one stated place.

Two rules then make the namespaces true, and neither constrains a layout:

* **A name appears at most once per root.** Two files in one root declaring one
  name are one symbol, and the build is refused naming both. The message says
  what to do: if they are two implementations of one entry point, they belong in
  two roots.
* **Declarations of one name across roots must match verbatim**, which is the
  check `scan` performs today and the one nothing else in the toolchain can
  perform (§1.3).

## 6. The API

```cpp
island::options opt;
opt.module_name = "app.kernels";          // the module root; directories extend it

// one root, additive backends: it is the layout root by default
opt.roots = { root + "/src/backends" };

// or several roots, one entry point implemented in each
opt.roots       = { root + "/src/backends/cuda", root + "/src/backends/cpu" };
opt.layout_root = root + "/src/backends/cuda";      // default: roots[0]

const auto all = island::scan(opt);         // reads every root
const auto out = island::emit(*all, opt);   // one .cppm, one flat header
```

* `scan` takes roots, not a file list. A root is where a namespace path starts,
  so it is declared rather than inferred, and the call says what the developer
  means: the device code is under here.
* A root may also be a single file, for a project that keeps one implementation
  beside another in one directory. The file is then its own root and contributes
  no namespace segment.
* It registers `mcpp::rerun_if_changed` for every file it reads and
  `mcpp::rerun_if_changed_glob` for the tree, so **adding** a file re-runs the
  build program. A file list cannot express that, and the current examples
  register the two paths they name and nothing else.
* Files are selected by extension: the device table plus `.c`, `.cc`, `.cpp`,
  `.cxx`, overridable through `opt.extensions`. Headers are excluded, and not
  only for cost: a project that declares its entry points in a `.cuh` and
  defines them in a `.cu` would otherwise hand the uniqueness check two files
  for one name and be refused for a layout that is correct.
* The walk is ordered -- files sorted by path, entries within a file in source
  order -- so the generated header and module are a function of the tree and not
  of the filesystem's enumeration. Without it `write_if_different` sees a
  different file on a run that changed nothing, the force-included header's
  timestamp moves, and every island translation unit rebuilds.
* The header stays one file, flat, with no namespaces. It is read by a C or a
  device compiler, and those have no namespaces to read. The module is a view
  onto it.
* Overlapping roots are refused. A file reachable from two roots would have two
  namespace paths, and which one it got would depend on the order of the list.
* A `scan` that finds no marked entry point is an error, not an empty
  module. A tree named explicitly and yielding nothing is a misspelled path or a
  marker that never arrived, and both of those fail later and less clearly --
  the consumer's import resolves to a module that exports nothing.
* A project that wants one module per subtree calls `emit` several times against
  one `scan_tree` result, filtered by subtree, with an explicit module name for
  each. One scan, so §4 still holds; extra `.cppm` files, no extra headers. Not
  the default, because the data lane's default is one module per group.

### 6.1 The short name, beside the authored one

An island's symbol is global to the whole program, so an entry point carries a
package prefix whether or not it sits in a namespace -- and the namespace then
repeats what the prefix already said:
`opkit::kernels::image::opkit_blur(...)`.

`opt.strip_prefix` emits a second spelling beside the first:

```cpp
export namespace opkit::kernels::image {
using ::opkit_blur;                        // the authored name; this is the symbol
inline constexpr auto blur = opkit_blur;   // the short name, for the call site
}
```

Measured 2026-09-08 with `clang++` (DPC++ 7.1.0), `-std=c++23`: both spellings
call one entity (`app::kernels::image::blur == &app::kernels::image::opkit_blur`
is true), and the same declarations compile under GCC 13 outside a module.

* It needs only the identifier, which is why it fits a generator that does not
  parse C. A function-pointer constant is callable, and `constexpr` makes the
  indirection disappear.
* Empty by default. A project that wants one spelling gets one.
* The authored name is always emitted and stays canonical. It is what `nm`, a
  link error, a profiler, a backtrace and `dlsym` show, and a reader who greps
  for the symbol finds the definition. The short name is a convenience at the
  call site and nowhere else -- which is the difference between this and the
  symbol decoration §10 refuses.
* Two entries stripping to one short name are refused, naming both. The short
  name goes through the same `identifier()` sanitiser as every other generated
  name (§8), so a prefix that leaves `default` or `2d` behind is handled once.
* An entry that does not carry the prefix gets no short name and no message. It
  is visible at the first call site that tries the short spelling.

## 7. The rule that keeps a name stable across builds

**The roots must not depend on the accelerator.** `mcpp::device_sources()` is
narrowed by `accel`: under `--no-accel` it is empty. Roots taken from it would
lose the device tree in a CPU-only build, so an entry point would be found only
in the CPU root and a consumer's qualified name would differ between
`mcpp build` and `mcpp build --no-accel`.

`opt.roots` names directories on disk and is therefore accel-independent by
construction. `accel` continues to decide which files are compiled and linked,
which is a different question and stays where it is (the manifest).

## 8. Shared derivation, so the lanes cannot drift

`common_base_dir`, `namespace_of` and the identifier sanitiser (`identifier`,
`src/plugins.cppm`, which handles non-identifier characters, a leading digit and
C++ keywords) move to the `mcpp.plugins` lib root, and both `rules/spirv.cppm`
and `tools/island.cppm` call them. The alignment is then structural: a directory
named `2d` or `default` gets the same answer on both lanes because it is the
same function, not because two documents agree.

## 9. What changes, and where

**`mcpp:plugins` 0.5.0.** `tools/island.cppm`: `options::roots`,
`options::layout_root`, `options::extensions`, `options::strip_prefix`, namespaced emission, the
per-root uniqueness check, the prefix-directory check. `src/plugins.cppm`: the three derivations move
in. `rules/spirv.cppm`: calls them instead of defining them.

**Breaking.** A consumer writes `app::kernels::saxpy_device(...)` where it wrote
`saxpy_device(...)`. Four call sites exist, all in repositories we control:
`examples/09-heterogeneous/{boundary,cuda,sycl}` and
`mcpp-plugins/tests/island-interface`. `tools-island` shipped 2026.9.7.1, so
adoption outside is effectively zero. No compatibility flag: a flag that keeps
the global spelling alive makes the namespace decorative, which is the outcome
this design exists to avoid.

**`mcpp` documentation.** The island row joins the table in `docs/42` §"The name
a payload arrives under", and the rule in §2 above replaces the two separate
statements. The `mcpp.tools.island` section moves from
`docs/31-authoring-a-rule-package.md` (whose reader is a rule author; no shipped
rule calls the generator, and `tools/island.cppm:7-8` says a project calls it) to
`docs/42`, and `docs/README.md`'s index row moves with it. Both languages.

## 10. Considered and not done

**Symbol decoration.** Making `MCPP_EXPORT_C` rewrite the linker name -- through
a `#define` or a GCC/Clang `__asm__` label -- is the only way to make the
namespace true at the symbol level rather than by a check. It is refused: MSVC
has no asm labels, a renamed symbol breaks `dlsym`, a hand-written header and
any non-mcpp C consumer of the same boundary, and the name in the object stops
being the name in the source. §4 buys the same guarantee for the cost of a
comparison.

**Directories in the module name.** `import pkg.dir1.dir2` is not what the data
lane does (§1.1), multiplies modules with the tree, and -- if it also split the
scan -- would restore the defect measured in §1.3.

**File names as namespace segments.** A function moved between two files in one
directory would change a consumer's spelling with no ABI change.

**Generating the boundary from inside a rule.** `mcpp.rules.cuda` could take an
`island_module` option and call the generator itself, which would give the
common case a project with no `scan` call of its own. It is deferred rather
than refused: the module name is a project decision and the rule is the wrong
owner for it, and the shape above has to be used before it is wrapped.

## 11. Criteria

Each has a denominator or a construction that distinguishes it from its
negation.

| criterion | measured |
|---|---|
| the namespace mirrors the tree | fixtures with 0, 1 and 2 directory levels; the emitted `.cppm` carries 0, 1 and 2 namespace segments |
| one module, one header, whatever the tree | `ls <out_dir>/island/*.h \| wc -l` is 1 for the 2-level fixture |
| a name cannot appear in two namespaces | negative fixture: two files in ONE root declare one name; the build fails and the message names both files |
| the layout root alone decides a namespace | move the fallback under a deeper directory; the generated `.cppm` is byte-identical |
| the qualified name does not depend on the build | `examples/09-heterogeneous/cuda` built with and without `--accel`; the generated `.cppm` is byte-identical |
| adding a file re-runs the generator | append a `.cu` to the tree, touch nothing else, rebuild; the new entry point is in the module |
| the two lanes share one derivation | `grep -c "common_base_dir\|namespace_of" rules/spirv.cppm` counts calls, not definitions |
| the short name and the authored name are one entity | `blur == &opkit_blur` in a fixture, and the artifact holds one symbol (`nm \| grep -c blur` is 1) |
| a prefix that collides is refused | negative fixture: two entries strip to one name; the message names both |
| a flatter fallback tree is accepted | fixture: `cuda/image/blur.cu` with `cpu/ops.cpp`; the namespace is `image` |
| a backend-only entry point still has a place | fixture: a name only the second root declares; its namespace is that root's path |
| the disagreement check still fires | the existing `island-interface` negative fixture, unchanged in behaviour |
| overlapping roots are refused | negative fixture: `src` named beside `src/kernels`; the message names both roots |
| the derivation runs on every host | the fixture builds and its namespaces are asserted on macOS and Windows, not only Linux |
| the output is a function of the tree, not of the walk | the same tree scanned twice produces byte-identical files, and a no-op rebuild touches neither |
| an empty root set is an error | a fixture whose roots hold sources but no marker; the build fails naming the root |
| two files in ONE root, one name | refused, and the message says the two belong in two roots |
| a CPU implementation keeps its place under `backends/` | the additive fixture's three backends land in three namespaces, none of them moved |

## 12. What shipped

`mcpp:plugins` **0.5.0** (2026-09-08) carries §2 through §8 and §6.1. **0.5.1**
carries the refusal of overlapping roots that §6 states and 0.5.0 omitted. That
is the shape of defect where a requirement folded into a larger change
disappears when that change ships: §6 stated it in prose and §11 gave it no
criterion of its own, so nothing was red when it was absent. It has one now.
0.5.1 also stops a single-file root from registering a re-run glob over the
directory that file happens to sit in.

**0.5.2** carries a defect the cross-platform run found. `mcpp.tools.island`
compiled on all three hosts and had been exercised on one; the first Windows
run of the fixture failed to compile with `no member named 'image' in namespace
'island_interface::kernels'`. `namespace_of` trimmed the base directory off a
file's directory as a STRING, and a root stated with forward slashes against a
directory iterator appending with the preferred separator left `\image` rather
than `image` -- so the root directory became a segment, sanitised to `_`. It
compares components now.

The shader lane shares that function and had the defect latent: its Windows
fixture keeps every payload in one directory, so a segment was never derived
there. The leg that surfaced it is not the lane it lives on, and a step now
asserts the derivation exists once so the island fixture's cross-platform run
keeps protecting both.

All three are indexed, and `mcpp` pins 0.5.2 across the example tree. The
published package was verified in an xlings sandbox against the index rather
than a working tree: a project written inside the sandbox resolved
`registry/data/xpkgs/mcpp-x-plugins`, printed `6 12 18 24`, and its generated
module carried `export namespace sandbox::kernels` and
`export namespace sandbox::kernels::vec`. The published generator also refused
overlapping roots there, naming both.

One path no CI covers was measured by hand: `examples/09-heterogeneous/cuda`
with its device leg, on an RTX 4080 (`sm_89`, the architecture the example
names). It printed `12 24 36 48` and `device: NVIDIA GeForce RTX 4080` -- an
island compiled by a device compiler, reached through the namespaced boundary,
with the seam calling `kernels::saxpy_device` from inside `namespace app`.

## 13. Decided in review

1. **No compatibility flag** (§9). The four call sites are ours and are updated
   with the release; a flag that kept the global spelling alive would leave the
   namespace decorative, which is the outcome this design exists to avoid.
2. **`variants` is gone.** The primary/variant model ranked a CPU implementation
   below a device one and forced it out of a `backends/` directory. Roots
   replace it (§5), and a CPU backend is a backend.
3. **The short name ships with this design, not after it** (§6.1). It is purely
   additive, so shipping it late would have been safe -- but the stutter it
   removes is visible in the first example anyone reads, and the design is being
   adopted by its examples in the same batch.
4. **The shape comes from one stated root, and no rule constrains a layout**
   (§5). Two drafts were discarded: requiring every implementation to sit at the
   same relative directory, which refused an ordinary flat fallback file; and
   taking the deepest directory among them, which let a refactor of any tree
   rename what every consumer writes.
