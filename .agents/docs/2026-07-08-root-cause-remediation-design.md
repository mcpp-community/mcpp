# Root-cause remediation for the 0.0.85 rollout incidents (Design)

The 0.0.85 train shipped green, but four incident classes were handled with
workarounds. This doc specifies the ROOT fixes — each with the exact code
anchor of the defect, the design, and the exit criterion that retires the
corresponding workaround. Companion to the roadmap's ops-incident log
(`2026-07-08-descriptor-index-evolution-roadmap.md` §0).

Shared diagnosis, worth stating once: every one of these is the same abstract
defect — **an implicit cross-boundary assumption about who provides something**
(a symbol, a directory, an asset, a version). The fixes all take the same
shape: make the provider explicit, verify at the boundary, fail loudly at the
moment of divergence.

---

## A1 — macOS: test binaries must link the toolchain's own libc++ (repo `mcpp`)

**Defect anchor**: `src/build/flags.cppm:36-42` — distributable targets already
link the static LLVM libc++ (correct, hermetic), but TestBinary targets
deliberately take the SYSTEM `-lc++`. That exception is itself a workaround:
static libc++ SIGABRTs during static destruction unless the entry point guards
with `_Exit` (mcpp/xlings do; gtest's main does not). So the real chain is:

    static-libc++ teardown crash → workaround: tests use system -lc++
    → latent header/dylib split (toolchain headers vs Apple dylib)
    → libc++ 22 moves string hashing out of line (__hash_memory)
    → undefined symbol in every gtest link (incident A1)

**Root design**: tests are host-only by definition, so an rpath into the
toolchain registry is acceptable FOR TESTS in a way it isn't for
distributables. TestBinary on macOS (clang-with-cfg toolchains) links the
toolchain's own libc++ **dynamically**:

    ldStdlibTest = "-L<llvmRoot>/lib -lc++ -Wl,-rpath,<llvmRoot>/lib"

- Same-version headers and dylib → no symbol-surface gambling, ever. Future
  libc++ out-of-lining is self-consistent by construction.
- Dynamic teardown avoids the static-destruction SIGABRT that motivated the
  system-lib exception — the exception's own root cause dissolves.
- Distributables keep the static LLVM libc++ (unchanged, already hermetic).
- `llvmRootForStdlib` is already threaded through `compute_flags`
  (`flags.cppm:173`); the change is confined to how `ldStdlibTest` is formed.

**Verification**: e2e on macOS+llvm — build a gtest test using
`std::unordered_map<std::string,...>` (forces `__hash_memory`-class symbols);
assert `otool -L` on the test binary references `<llvmRoot>/lib/libc++.1.dylib`
and NOT `/usr/lib/libc++`. Add the inverse for a distributable (no libc++
dylib reference at all).

**Exit criteria**: remove the `llvm@20.1.7` pin in `ci-macos.yml` (restore
`xlings install llvm -y` first); delete the "TestBinary → system -lc++"
comment block. LLVM 22.1.8 becomes usable on macOS.

### A1 addendum — evidence-driven revision (CI forensics, rounds 1–7)

The dynamic-dylib design above was falsified by crash-report forensics:
this llvm distribution's libc++abi/libunwind dylibs upward-link
/usr/lib/libc++, so the SYSTEM libc++ always loads beside the toolchain's
(.ips image list), and gtest's initializers constructed a stringstream in
one copy and destroyed it in the other
(BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED at locale::~locale).

Process invariant: exactly one ACTIVE libc++ state per process. Options:
system -lc++ (the original workaround — falsified by __hash_memory);
toolchain dylib + rpath (falsified above); **static -load_hidden archives
— adopted**: the already-proven distributable model, now uniform across
ALL mcpp-built binaries (tests were the only exception, and the exception
was the landmine); fixing the distribution itself — see below.

**Ecosystem root fix (new item, xim llvm packaging)**: rewrite the
packaged lib/*.dylib install names/deps at packaging time (@rpath
inter-references, no /usr/lib/libc++ upward link) so the dylib set is
self-contained. Owned by the xlings-res/llvm builder; the C.1 consumer
smoke gate keeps non-self-contained packages out of the index. Until
then, dynamic toolchain-libc++ linking stays unsupported on macOS.

Also part of the exit criteria: mcpp.toml's own macos toolchain pin moves
with the current llvm (20.1.7 → 22.1.8) so mcpp builds itself on what
users get.

## A2 — GCC 15 module instantiation: raise the cross-toolchain floor (repos `xlings-res` packaging + `mcpp` CI)

**Defect anchor**: `src/manifest/types.cppm` `force_template_instantiations()`
— an anchor that recreates, deliberately, what the old single-file module
provided by accident. It works but taxes every FUTURE module: any
module-attached struct whose member instantiations aren't odr-used inside its
own TU re-triggers the GCC 15 bug with an error that points nowhere.

**Root design**: the bug is fixed in GCC 16 (proven: native x86 gcc 16.1.0
links the same code). The root fix is a toolchain floor, not code:

1. Package `xim-x-aarch64-linux-musl-gcc@16.1.0` (same GCC that the native
   builds already use) into xlings-res + xim-pkgindex — mirroring how the
   15.1.0 cross package was produced.
2. Bump the cross toolchain in `cross-build-test.yml` and the release cross
   job to 16.1.0.
3. Delete `force_template_instantiations()` and its declaration; the deletion
   commit is the regression test (cross CI goes red if the floor didn't take).

**Interim guard** (until the package exists): keep the anchor, but move the
knowledge out of oral tradition — a paragraph in `docs/04-build-from-source.md`
naming the constraint and its exit criterion, so the next person who hits
"undefined reference to std::map<...>::map()" on cross finds it by grep.

## A3 — xlings: staged + atomic index acquisition (repo `xlings`)

**Defect anchor**: `xlings src/core/xim/downloader.cppm:53-73` — "already
cloned → pull; pull failed → **remove and re-clone**". Two windows where the
index dir is absent or partial (between remove and clone; after a failed
clone), and the failure surfaces much later as
`failed to build catalog: pkgs/ directory not found` — observed twice on
2026-07-08 (macOS CI, xim-pkgindex linux-install-test). This is the concrete
evidence for the staged-refresh half of the D3 design that mcpp couldn't
implement from its side.

**Root design** (in xlings' downloader):

1. **Acquire into staging, never in place**: clone/pull into
   `<data>/.staging/<index>-<pid>/`; the live dir is not touched during
   acquisition.
2. **Validate before swap**: `pkgs/` exists and is non-empty; if the tree
   carries `index.toml`, it parses. (This is also where xlings can honor
   `min_mcpp`-style contracts for its own consumers later.)
3. **Atomic swap**: `rename(live, live.old); rename(staging, live);
   remove_all(live.old)`. Any failure → discard staging, keep the previous
   live tree, and error AT FETCH TIME with one retry round — not at catalog
   time with a hint telling the user to run `xlings update` themselves.
4. **Catalog self-heal**: if catalog build still finds a broken index dir
   (legacy states), trigger one refetch instead of erroring into the void.

**Verification**: unit-style test in xlings — kill the clone mid-flight
(cancellable path already exists, `downloader.cppm:101`), assert the live dir
still serves the previous catalog; a full fetch then swaps cleanly.

**Exit criterion**: the `pkgs/ directory not found` class disappears from CI;
the "try running: xlings update" hint becomes dead code and is removed.

## A4 — release pipeline: verified, gated mirroring (repo `mcpp`, `.github/`)

**Defect anchors**: `release.yml` publish-ecosystem job has no
`timeout-minutes` (hung >1h; 6h ceiling); `mirror_res.sh` GH section trusts
`gh release upload` ("gh --clobber, reliable") with no per-file verification —
exactly the protection its own GitCode section already has. One stuck asset →
silent incomplete mirror → downstream 404s (incidents 3 and 4 shared this
root).

**Root design** — the mirror is done only when PROVEN done, on both hosts:

1. `publish-ecosystem`: `timeout-minutes: 20`.
2. `mirror_res.sh`: wrap every upload (`gh` AND `gtc`) in `timeout 300`; after
   each GH upload, verify the asset's download URL answers 200/302 (same
   curl-check the GitCode section does), retry ≤5 with delete-and-reupload
   (the observed bad state cleared on re-upload).
3. **Completeness gate**: after both hosts, diff actual asset lists against
   the expected list; any gap → exit 1. Drop "best-effort/non-blocking" for
   the mirror: both hosts are user-serving (GLOBAL and CN install paths), so
   an incomplete mirror must fail the job visibly instead of leaving a 404
   for the first user (or CI) to find.
4. **Downstream race**: `ci-fresh-install` (workflow_run after release) races
   the index bump merge by design. Give it a bounded wait: poll
   `xlings search mcpp` / the index artifact until the released version
   appears (≤15 min) before installing, converting the race into a wait.

**Exit criterion**: a re-run of the 0.0.85-style failure (kill an upload
mid-flight) fails the job with the missing asset named, instead of
succeeding silently.

## C — toolchain channel version contract (repos `xim-pkgindex` + `mcpp`; the meta-fix)

The llvm 20.1.7→22.1.8 default bump propagated ungated and broke three things
in one day. This is the same disease the index floor just cured for
descriptors: **a channel whose evolution has no compatibility contract with
its consumers**. Two complementary gates:

1. **Publisher-side (xim-pkgindex CI)**: toolchain-package bump PRs gain a
   consumer smoke — install the bumped toolchain, then `mcpp new + build + test`
   a program exercising std (string hashing included) on each platform. The
   llvm 22.1.8 bump would have failed this gate in the PR, not in production.
2. **Consumer-side (mcpp)**: per-platform known-good ranges for toolchains it
   auto-selects (`toolchain/detect.cppm` defaults) — e.g. macOS llvm
   `>=20,<22` until A1 lands, then relaxed. Explicit user pins always win;
   the range only governs what mcpp picks silently.

A1's root fix removes the specific macOS ceiling; the contract remains as
defense-in-depth for the next regression of any kind.

## Implementation plan (live, split from the order below)

| # | Task | Repo / PR | Status |
|---|------|-----------|--------|
| R1 (A4) | pipeline hardening | ✅ PR #201 merged (6/6); positive+negative tested against live 0.0.85 mirror | ✅ |
| R2 (A1) | TestBinary stdlib unification | ✅ PR #202 merged (6/6, 8 forensics-driven rounds): static -load_hidden for tests = distributable model; llvm 22.1.8 green; dynamic route falsified (dylib set upward-links system libc++) | ✅ |
| R3 (A3) | staged+atomic git index acquisition + catalog self-heal | ✅ xlings PR #353 merged; artifact path already atomic since 0.4.62 — incidents were v0.4.44 pins, bumped in #343 | ✅ |
| R4 (C.1) | toolchain-bump consumer smoke + xlings pin v0.4.62 | ✅ xim-pkgindex PR #343 merged | ✅ |
| R5 (A2) | cross gcc 16 packaging → floor bump | ✅ packaged (build-cross/build-native workflows added, release 16.1.0 4 assets, smoke-tested); xim entry merged (#344); mcpp cross pin → 16.1.0. **R5b remains**: anchor stays until xim ships x86_64 musl gcc 16 (the x86_64-linux-musl release build is still on musl-gcc 15.1.0) | 🔄 |
| R6 | mcpp 0.0.86 release + ecosystem closure (mirror / index bump / fresh-install) | all | pending |

## Order of execution

| # | Fix | Cost | Unblocks |
|---|---|---|---|
| 1 | A4 (script + timeouts + gate) | ~1h, shell only | stops silent-404 class immediately |
| 2 | A1 (ldStdlibTest → toolchain libc++) | ~0.5 day + macOS CI cycle | unpins llvm on macOS; users on 22.1.8 |
| 3 | C.1 (xim-pkgindex consumer smoke) | ~0.5 day CI work | gates all future toolchain bumps |
| 4 | A3 (xlings staged fetch) | ~1 day in xlings | kills the fetch-flake class |
| 5 | A2 (gcc 16 cross package + floor) | packaging effort | deletes the instantiation anchor |
| 6 | C.2 (consumer ranges) | small | optional once 1–5 land |
