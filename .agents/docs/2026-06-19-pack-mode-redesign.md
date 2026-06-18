# Pack Mode Redesign — Two-Axis Model, Clearer Names, `system` Mode

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `mcpp pack`'s distribution modes legible and complete: rename the modes to idiomatic, self-documenting names (keeping old names as permanent aliases), add a `system` mode that depends entirely on the OS, and freeze the tarball-name wire format so renaming cannot break download URLs.

**Architecture:** Distribution has two orthogonal axes — **libc/static** (a *build-target* property: `…-linux-gnu` vs `…-linux-musl`) and **bundling depth** (a *pack* property: how much of the shared-lib closure travels with the artifact). `--mode` owns only the bundling-depth axis (`system` → `vendored` → `self-contained`), plus `static` as a first-class convenience for the musl single-file case. Names change at the CLI surface; the enum, behavior, and tarball suffixes change only where intended, with the wire-format suffix split out and frozen.

**Tech Stack:** C++23 modules (`src/pack/pack.cppm`, `src/pack/pipeline.cppm`), CLI (`src/cli/cmd_publish.cppm`, `src/cli.cppm`), gtest (`tests/unit/`), shell e2e (`tests/e2e/30_pack_modes.sh`), GitHub release artifacts (`install.sh` consumes tarball names).

---

## Background — current design and the smell

Today `Mode { Static, BundleProject, BundleAll }` (`pack.cppm:30`) is a hand-picked diagonal through a 2-D space, and the names mix two frames:

- **Axis A (libc):** glibc (host/bundled) vs musl (static). `Static` names *this* axis.
- **Axis B (bundling depth):** nothing / third-party only / everything. `BundleProject`/`BundleAll` name *this* axis.

Two concrete problems:

1. **Inconsistent vocabulary** — a user cannot tell from the names that `Static` also bundles everything, or that `BundleAll` is glibc-specific.
2. **A real gap:** there is no "bundle *nothing*, depend on the OS for every `.so` including third-party" mode. `BundleProject` always bundles third-party libs; it only degenerates to "depend on OS" when the project happens to have zero third-party deps. So `.deb`/`.rpm` packaging and same-distro fleet deployment (let the package manager declare deps) have no matching mode.

**Verified wire-format hazard:** `mode_name()` (`pack.cppm:85-92`) is dual-purpose — `default_tarball_name()` (`pack.cppm:115-120`) builds the tarball suffix from it (e.g. `myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz`). `install.sh` / download URLs depend on these names, and `tests/e2e/30_pack_modes.sh:110` asserts the exact string. **Renaming `mode_name` naively would rename release artifacts and break consumers.** The fix splits display name from the frozen tarball suffix.

## Design

### The two axes (mental model — goes in docs)

| Build target (libc) | Pack mode (bundling depth) | Result |
|---|---|---|
| `…-linux-gnu` | `system` | depend on OS for all `.so` (incl. third-party) — for distro packages / same-distro fleet |
| `…-linux-gnu` | `vendored` *(default)* | bundle third-party `.so`; host supplies libc/libstdc++ — manylinux-style |
| `…-linux-gnu` | `self-contained` | bundle whole closure + loader `run.sh` — cross-distro/glibc |
| `…-linux-musl` | `static` | single fully-static file — runs anywhere, same arch/kernel |

`--mode` only selects the bundling-depth axis; libc is selected by `--target`. `static` stays a first-class `--mode` value (most-recognized term in the ecosystem; do not hide it behind `--target musl --mode self-contained`). `pipeline.cppm:25-58` already couples `…-musl` → `Static` and rejects `static` on non-musl targets — keep that.

### Names + permanent aliases

| Canonical (new, shown in `--help`) | Enum | Aliases still accepted | Tarball suffix (FROZEN) |
|---|---|---|---|
| `system` | `Mode::None` *(new)* | — | `system` |
| `vendored` | `Mode::BundleProject` | `bundle-project` | *(none — default)* |
| `self-contained` | `Mode::BundleAll` | `bundle-all` | `bundle-all` |
| `static` | `Mode::Static` | — | `static` |

Why these words (idiom over forced frame-uniformity): each is the single most-recognized term for its level — `system` (`-Bsystem`, distro packages), `vendored` (manylinux / pip / Go vendoring), `self-contained` (`.NET --self-contained`, Deno, PyInstaller), `static` (Go/Rust/Docker). Consistent responsibility-based naming per the repo convention (no grab-bag names).

### `system` (`Mode::None`) behavior

Same code path as `vendored` but with the bundle set forced empty: bundle no `.so`, clear the dev-sandbox RUNPATH, and repoint `PT_INTERP` to the LSB-standard `/lib64/ld-linux-x86-64.so.2` (so the binary uses the host loader). Distinguished from `vendored` only when the project *has* third-party deps — `system` leaves them for the OS. Inherits `vendored`'s existing limitation (LSB interp assumes a glibc host; not for musl-only hosts like Alpine) — document it.

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/pack/pack.cppm` | mode enum, name/parse/suffix helpers, bundling behavior | Modify |
| `tests/unit/test_pack_modes.cpp` | parse aliases + canonical names + `system` | Create |
| `src/cli/cmd_publish.cppm` | `--mode` parse + error message | Modify (`:36-40`) |
| `src/cli.cppm` | `mcpp pack` help line | Modify (`:62`) |
| `tests/e2e/30_pack_modes.sh` | add `system` mode coverage | Modify |
| `docs/02-pack-and-release.md` | two-axis model + grid | Modify |

---

### Task 1: Split `mode_name` → display name + frozen tarball suffix

**Files:**
- Modify: `src/pack/pack.cppm:77-92` (declarations + impl) and `:115-120` (`default_tarball_name`)

- [ ] **Step 1: Replace `mode_name` with two purpose-specific helpers**

In `src/pack/pack.cppm`, replace the declaration (`:78`) `std::string_view mode_name(Mode m);` with:

```cpp
// Canonical name shown in `--help`/diagnostics (renamed for legibility).
std::string_view mode_cli_name(Mode m);
// FROZEN wire-format suffix for tarball filenames. Never rename these —
// install.sh / download URLs depend on them. "" means "no suffix" (default).
std::string_view mode_tarball_suffix(Mode m);
```

Replace the impl (`:85-92`) with:

```cpp
std::string_view mode_cli_name(Mode m) {
    switch (m) {
        case Mode::None:          return "system";
        case Mode::Static:        return "static";
        case Mode::BundleProject: return "vendored";
        case Mode::BundleAll:     return "self-contained";
    }
    return "?";
}

std::string_view mode_tarball_suffix(Mode m) {
    switch (m) {
        case Mode::None:          return "system";   // brand-new mode: free to pick
        case Mode::Static:        return "static";   // frozen
        case Mode::BundleProject: return "";         // frozen: default → no suffix
        case Mode::BundleAll:     return "bundle-all"; // frozen
    }
    return "";
}
```

(`Mode::None` is added in Task 3; adding its cases here first keeps the switch exhaustive — it compiles once the enum gains the value. If implementing strictly in order, temporarily omit the `Mode::None` cases and add them in Task 3.)

- [ ] **Step 2: Rebuild the tarball name from the frozen suffix**

Replace `default_tarball_name` (`:115-120`) body with:

```cpp
std::string default_tarball_name(std::string_view name, std::string_view version,
                                 std::string_view triple, Mode mode)
{
    auto sfx = mode_tarball_suffix(mode);
    if (sfx.empty())
        return std::format("{}-{}-{}.tar.gz", name, version, triple);
    return std::format("{}-{}-{}-{}.tar.gz", name, version, triple, sfx);
}
```

- [ ] **Step 3: Fix the remaining `mode_name` reference**

Search and update any other caller: `grep -n "mode_name" src` — replace display/diagnostic uses with `mode_cli_name`. (Expected: only `default_tarball_name` used it for the suffix, now via `mode_tarball_suffix`.)

- [ ] **Step 4: Build + run the wire-format guard**

Run: `mcpp build && ./tests/e2e/30_pack_modes.sh`
Expected: PASS — `30_pack_modes.sh:110` still finds `myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz`; `:31`/`:79` unchanged. Tarball names are byte-identical (the freeze worked).

- [ ] **Step 5: Commit**

```bash
git add src/pack/pack.cppm
git commit -m "refactor(pack): split mode display name from frozen tarball suffix"
```

---

### Task 2: Canonical names + permanent aliases in `parse_mode`

**Files:**
- Modify: `src/pack/pack.cppm:94-99`
- Test: `tests/unit/test_pack_modes.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_pack_modes.cpp`:

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.pack;

using mcpp::pack::Mode;
using mcpp::pack::parse_mode;
using mcpp::pack::mode_cli_name;

TEST(PackModes, CanonicalNamesParse) {
    EXPECT_EQ(parse_mode("system"),         Mode::None);
    EXPECT_EQ(parse_mode("vendored"),       Mode::BundleProject);
    EXPECT_EQ(parse_mode("self-contained"), Mode::BundleAll);
    EXPECT_EQ(parse_mode("static"),         Mode::Static);
}

TEST(PackModes, OldNamesStayAsAliases) {
    EXPECT_EQ(parse_mode("bundle-project"), Mode::BundleProject);
    EXPECT_EQ(parse_mode("bundle-all"),     Mode::BundleAll);
}

TEST(PackModes, UnknownIsNullopt) {
    EXPECT_FALSE(parse_mode("nonsense").has_value());
}

TEST(PackModes, CliNamesAreCanonical) {
    EXPECT_EQ(mode_cli_name(Mode::None),          "system");
    EXPECT_EQ(mode_cli_name(Mode::BundleProject), "vendored");
    EXPECT_EQ(mode_cli_name(Mode::BundleAll),     "self-contained");
    EXPECT_EQ(mode_cli_name(Mode::Static),        "static");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `mcpp test`
Expected: FAIL — `Mode::None` undeclared and `system`/`vendored`/`self-contained` not parsed. (Full green requires Task 3's enum value; this task adds the parse arms.)

- [ ] **Step 3: Extend `parse_mode` with new names + aliases**

Replace `parse_mode` (`:94-99`) with:

```cpp
std::optional<Mode> parse_mode(std::string_view s) {
    // Canonical names.
    if (s == "system")         return Mode::None;
    if (s == "vendored")       return Mode::BundleProject;
    if (s == "self-contained") return Mode::BundleAll;
    if (s == "static")         return Mode::Static;
    // Permanent back-compat aliases (old names — keep forever).
    if (s == "bundle-project") return Mode::BundleProject;
    if (s == "bundle-all")     return Mode::BundleAll;
    return std::nullopt;
}
```

- [ ] **Step 4: Run the tests (after Task 3 lands the enum)**

Run: `mcpp test`
Expected: PASS — all `PackModes` cases green.

- [ ] **Step 5: Commit**

```bash
git add src/pack/pack.cppm tests/unit/test_pack_modes.cpp
git commit -m "feat(pack): canonical mode names + permanent old-name aliases"
```

---

### Task 3: Add `Mode::None` (`system`) — enum + bundling behavior

**Files:**
- Modify: `src/pack/pack.cppm:30` (enum) and `:586-627` (bundling block)

- [ ] **Step 1: Add the enum value**

In `src/pack/pack.cppm`, change (`:30`):

```cpp
enum class Mode { Static, BundleProject, BundleAll };
```
to:
```cpp
enum class Mode { None, Static, BundleProject, BundleAll };
```

(Now the `mode_cli_name` / `mode_tarball_suffix` switches from Task 1 are exhaustive, and `parse_mode("system")` resolves.)

- [ ] **Step 2: Force an empty bundle set for `system`, share the LSB interp repoint**

In the bundling block (entered by `if (plan.opts.mode != Mode::Static)` at `:586`), make the skip loop bundle nothing for `Mode::None`. Replace the per-dep skip loop (`:592-601`) so the classification is gated:

```cpp
        std::vector<ResolvedDep> toBundle;
        for (auto& d : *deps) {
            bool skip = false;
            if (plan.opts.mode == Mode::None) {
                skip = true;  // system: host provides every .so, bundle nothing
            } else if (plan.opts.mode == Mode::BundleProject) {
                if (is_system_lib(d.soname))                          skip = true;
                if (soname_matches(d.soname, plan.alsoSkipLibs))      skip = true;
                if (soname_matches(d.soname, plan.forceBundleLibs))   skip = false;  // override
            }
            // Mode::BundleAll: skip nothing — we want the loader too.
            if (!skip) toBundle.push_back(d);
        }
```

Then extend the interp repoint (`:623`) so `system` also gets the LSB-standard loader (its dev-sandbox absolute interp won't exist on the target):

```cpp
            if (plan.opts.mode == Mode::BundleProject || plan.opts.mode == Mode::None) {
                if (auto r = set_interpreter(bundledBinary,
                        "/lib64/ld-linux-x86-64.so.2", patchelf); !r)
                    return std::unexpected(Error{r.error()});
            }
```

The empty `toBundle` makes `rpath = toBundle.empty() ? "" : "$ORIGIN/../lib"` (`:613`) clear the dev RUNPATH automatically. The `BundleAll` wrapper branch (`:630`) is not entered for `None`, and the `else` branch (`:645`) drops the top-level entry-point wrapper as for `BundleProject`/`Static`.

- [ ] **Step 3: Build to verify it compiles**

Run: `mcpp build`
Expected: PASS — all `switch (Mode)` sites are exhaustive; no missing-case warnings.

- [ ] **Step 4: Commit**

```bash
git add src/pack/pack.cppm
git commit -m "feat(pack): add system mode (depend on OS for all shared libs)"
```

---

### Task 4: Update CLI help + error strings

**Files:**
- Modify: `src/cli/cmd_publish.cppm:40`, `src/cli.cppm:62`

- [ ] **Step 1: Update the `--mode` error message**

In `src/cli/cmd_publish.cppm`, change the invalid-mode message (`:40`) to list canonical names and mention aliases:

```cpp
                "invalid --mode '{}'; expected: system | vendored | self-contained | static "
                "(aliases: bundle-project=vendored, bundle-all=self-contained)", *v));
```

- [ ] **Step 2: Update the `mcpp pack` help line**

In `src/cli.cppm` (`:62`), make the summary point at the new vocabulary:

```cpp
    std::println("  mcpp pack [--mode <m>]               Build + bundle a tarball (m: system|vendored|self-contained|static)");
```

- [ ] **Step 3: Build + sanity-check the help/error**

Run: `mcpp build && mcpp pack --mode nonsense 2>&1 | grep -q "system | vendored | self-contained | static" && echo OK`
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add src/cli/cmd_publish.cppm src/cli.cppm
git commit -m "docs(cli): surface canonical pack mode names in help + errors"
```

---

### Task 5: e2e coverage for `system` mode

**Files:**
- Modify: `tests/e2e/30_pack_modes.sh`

- [ ] **Step 1: Add a `system`-mode section**

In `tests/e2e/30_pack_modes.sh`, immediately before the final `echo "OK"`, add:

```bash
# ─── Mode `system` (Mode::None — depend on OS for all .so) ─────────────
"$MCPP" pack --mode system > "$TMP/pack-sys.log" 2>&1 || {
    cat "$TMP/pack-sys.log"; echo "system pack failed"; exit 1; }

tarball_sys="target/dist/myapp-0.1.0-x86_64-linux-gnu-system.tar.gz"
[[ -f "$tarball_sys" ]] || { echo "system tarball missing at $tarball_sys"; exit 1; }

mkdir -p "$TMP/sys"
tar -xzf "$tarball_sys" -C "$TMP/sys"
root_sys="$TMP/sys/myapp-0.1.0-x86_64-linux-gnu-system"
[[ -x "$root_sys/bin/myapp" ]] || { echo "system: missing bin/myapp"; exit 1; }

# system mode bundles NOTHING — lib/ must be empty or absent even though the
# binary still links libc/libstdc++ (those come from the host).
if [[ -d "$root_sys/lib" ]]; then
    n=$(ls "$root_sys/lib" 2>/dev/null | wc -l)
    [[ "$n" -eq 0 ]] || { echo "system: lib/ should be empty, has $n"; ls "$root_sys/lib"; exit 1; }
fi

# PT_INTERP repointed to the LSB-standard loader (host glibc).
file "$root_sys/bin/myapp" | grep -q 'interpreter /lib64/ld-linux-x86-64.so.2' || {
    file "$root_sys/bin/myapp"; echo "system: PT_INTERP not repointed to LSB loader"; exit 1; }

# Runs on this (glibc) host.
"$root_sys/bin/myapp" > "$TMP/sys-out.log" 2>&1 || {
    cat "$TMP/sys-out.log"; echo "system: extracted binary failed to run"; exit 1; }
grep -q 'Hello' "$TMP/sys-out.log" || { echo "system: runtime output missing"; exit 1; }

# Old alias must still parse identically (back-compat).
"$MCPP" pack --mode bundle-project > "$TMP/pack-alias.log" 2>&1 || {
    cat "$TMP/pack-alias.log"; echo "alias bundle-project failed"; exit 1; }
```

Note: for this no-dep `myapp`, `system` and `vendored` produce identical `lib/` (both empty); the distinguishing "third-party not bundled" behavior is the same skip-loop code path with the bundle set forced empty (Task 3), exercised by code, and would diverge only for a project with third-party `.so`.

- [ ] **Step 2: Run it**

Run: `./tests/e2e/30_pack_modes.sh`
Expected: ends with `OK`.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/30_pack_modes.sh
git commit -m "test(e2e): cover pack system mode + old-name alias"
```

---

### Task 6: Document the two-axis model

**Files:**
- Modify: `docs/02-pack-and-release.md`

- [ ] **Step 1: Add the model + grid + alias table**

In `docs/02-pack-and-release.md`, add a section:

```markdown
## Two axes: target (libc) × mode (bundling depth)

Distribution is two orthogonal choices:

- **libc / static** — a *build-target* property: `--target …-linux-gnu` (glibc)
  vs `--target …-linux-musl` (musl, static-capable).
- **bundling depth** — a *pack* property: how much of the shared-lib closure
  travels with the artifact. This is what `--mode` selects.

| `--mode` | Host must provide | Use case |
|----------|-------------------|----------|
| `system` | every `.so` (incl. third-party) | `.deb`/`.rpm`, same-distro fleet (pkg manager declares deps) |
| `vendored` *(default)* | libc / libstdc++ / loader | manylinux-style; portable across same-libc hosts |
| `self-contained` | nothing | cross-distro; bundles closure + `run.sh` loader wrapper |
| `static` | nothing (single file) | musl; runs anywhere, same arch/kernel |

`static` is a first-class `--mode` value; `--target …-musl` without `--mode`
implies it. PT_INTERP cannot be `$ORIGIN`-relative, so `self-contained` ships a
`run.sh` that invokes the bundled loader (`ld.so --library-path … "$@"`).

### Mode name compatibility

Canonical names are shown in `--help`. Old names remain permanent aliases:
`bundle-project` = `vendored`, `bundle-all` = `self-contained`. Tarball-name
suffixes are a frozen wire format (consumed by `install.sh`) and do **not**
follow the rename: `vendored` → no suffix, `self-contained` → `-bundle-all`,
`static` → `-static`, `system` → `-system`.
```

- [ ] **Step 2: Commit**

```bash
git add docs/02-pack-and-release.md
git commit -m "docs(pack): document two-axis distribution model + mode aliases"
```

---

## Self-Review

**1. Spec coverage** — rename with aliases (Tasks 1,2,4,6 ✓), `system`/OS-default mode (Tasks 3,5 ✓), wire-format freeze (Task 1, guarded by `30_pack_modes.sh:110` ✓), semantic clarity/docs (Task 6 ✓).

**2. Placeholder scan** — every step has concrete code, exact paths/lines, and expected output. The one judgment note (system≡vendored for no-dep projects) is an explicit design statement, not a deferred TODO.

**3. Type consistency** — `mode_cli_name(Mode)→string_view`, `mode_tarball_suffix(Mode)→string_view`, `parse_mode(string_view)→optional<Mode>`, `Mode{None,Static,BundleProject,BundleAll}` are used identically across declaration (Task 1/3) and all call sites (Tasks 1,2,4,5). Tarball strings asserted in Task 1/Task 5 match `mode_tarball_suffix` exactly (`bundle-all`, `system`, none, `static`).

**Scope boundary (intentional):** this redesign is independent of `2026-06-19-runtime-launch-and-multi-distro-plan.md` (the `mcpp run` loader-env fix). They touch disjoint files and can land in either order. Distinguishing `system` from `vendored` for a project *with* third-party `.so` is asserted by code structure, not by a dedicated dep-fixture e2e — add one only if a regression appears.

## Execution Handoff

Plan complete and saved to `.agents/docs/2026-06-19-pack-mode-redesign.md`. Two execution options:

1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks.
2. **Inline Execution** — execute tasks here with checkpoints.

Note the inter-task dependency: Task 3 (the `Mode::None` enum value) makes Tasks 1–2 compile/pass green, so if executing strictly per-task, land Task 3's enum line early or expect Tasks 1–2 to go green only after Task 3.

Which approach?
