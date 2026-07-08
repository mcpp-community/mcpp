# Descriptor & index evolution — 0.0.85 release train roadmap (cross-repo)

Master plan executing, in one pass, the full necessity-ordered set from the two
sister designs:

- `2026-07-08-index-version-semantics-and-descriptor-grammar-design.md`
  (D1 long brackets, D2 single-source lint, D3 index floor)
- `2026-07-08-scanner-backend-abstraction-design.md`
  (scan_overrides, L3 plan-vs-ddi reconciliation; p1689 backend deferred)

Scope decision: user base is small — the one-time compatibility break is
absorbed NOW, all five items ship in **mcpp 0.0.85**, and mcpp-index adopts
immediately after. The p1689 per-package backend is explicitly OUT of this
train (seam documented, deferred until demanded).

---

## 1. Work breakdown — repo `mcpp` (release 0.0.85)

| # | Item | Design ref | Depends on |
|---|------|-----------|------------|
| W1 | **D1 long brackets**: `LuaCursor::read_long_bracket()` (`[[`/`[=[…]=]`, first-newline strip), `read_string()` branch, `strip_lua_comments_and_strings()` long strings + `--[[ ]]` block comments; lua5.4-parity unit fixtures (level ≥ 1, embedded `]]`, CRLF, comment containing `mcpp = {`) | grammar design D1 | — |
| W2 | **D2 CLI**: `mcpp xpkg parse <file> [--json] [--strict]` wrapping `synthesize_from_xpkg_lua` + identity gate; `--strict` warns on unknown mcpp-segment keys (parser collects skipped keys instead of dropping them silently); e2e good/malformed fixtures | grammar design D2 | W1 (validates new grammar) |
| W3 | **scan_overrides**: parse in mcpp segment + project `mcpp.toml`; override-matched files bypass the text scan, declared `(provides, imports)` enter the graph; shape validation (glob must match ≥1 source, module names well-formed) | scanner design §3-pre | — |
| W4 | **L3 reconciliation**: `dyndep.cppm` diffs planned `(provides, requires)` vs compiler `.ddi` per TU; **mandatory for override units**, `MCPP_VERIFY_MODGRAPH=1`-gated for the rest (default-on in a later release once quiet); E-code with file + delta; watch logical-name sanitization parity (`bmi_basename`) | scanner design §3d | W3 (override units are the must-verify set) |
| W5 | **D3 index floor**: `index.toml` reader; check at the index-open choke point (`package_fetcher` index-dir layer); refresh becomes staged-unpack → floor check → atomic swap (keep-old + warn on incompatible); `MCPP_INDEX_FLOOR=ignore`; E-code + `mcpp explain`; version compare via `version_req.cppm` | grammar design D3 | — |
| W6 | **Release**: docs 04 (grammar: long brackets, scan_overrides key, unknown-key strictness) + 05 (`[build]`/override surface, `index.toml`); release notes carry the one-time-break notice for pre-0.0.85 clients | — | W1–W5 |

Parallelism: W1 ∥ W3 ∥ W5 are independent tracks; W2 follows W1; W4 follows W3.

Known implementation caveats:
- **Two refresh paths**: staged+atomic swap applies to the artifact-download
  path; the xlings-git path can't stage inside mcpp — there the open-time
  floor check is the guard (acceptable: git path is maintainer/dev-facing).
- Reconciliation false-positive risk (ddi logical-name formatting vs planned
  names) is why non-override units start env-gated.

## 2. Work breakdown — repo `mcpp-index` (after 0.0.85 is released + mirrored)

Ordered PRs; details in mcpp-index
`.agents/docs/2026-07-08-index-side-adoption-plan.md`:

| # | PR | Content | Gate |
|---|----|---------|------|
| P0 | (pre-train) merge PR #63 as-is | fmt via `generated_files`, works on 0.0.81 today; don't block it on the train | tested 2026-07-08 |
| P1 | **floor PR (atomic)** | `index.toml` (`min_mcpp = "0.0.85"`); CI `MCPP_VERSION` 0.0.81 → 0.0.85; lint job gains pinned-mcpp download + `xpkg parse --strict` loop; `publish_mcpp_index.sh` packs `index.toml` (one `cp`) | 0.0.85 released |
| P2 | fmt → `scan_overrides` | descriptor drops the 3.4 KB `generated_files` copy; upstream `src/fmt.cc` verbatim + `cxxflags -DFMT_IMPORT_STD` + declared `(provides={fmt}, imports={std})` | P1 merged |
| P3 | long-bracket migrations | `nlohmann.json`, `compat.eigen` `generated_files` → `[==[…]==]`; mechanical, parity-oracle-checked, one PR each | P1 merged |
| P4 | docs | `repository-and-schema.md`: replace "不支持 `[[…]]`" with "requires mcpp ≥ 0.0.85 (lint enforces)"; document `scan_overrides` as the pattern for guarded-import module units; add floor explanation + case-index rows | P1–P3 |

Invariant enforced mechanically, not by policy: **no descriptor may use new
grammar/keys before P1** — while lint pins 0.0.81 the `xpkg parse` step doesn't
exist and old-lint passes would be meaningless; after P1, lint parses with
0.0.85, so pre-P1 usage simply cannot merge.

## 3. Verification plan

- mcpp unit: lua5.4-parity grammar fixtures; override shape validation.
- mcpp e2e: fixture index with `min_mcpp = "9.9.9"` → build fails with upgrade
  message, `MCPP_INDEX_FLOOR=ignore` passes; staged refresh against an
  incompatible snapshot keeps the old one; override with a deliberately wrong
  `imports` → reconciliation error names file + delta; `xpkg parse --strict`
  flags an unknown key.
- Integration bed: mcpp-index workspace CI (`mcpp test --workspace`, 3 OS) —
  P2's fmt-via-override member is the live proof; note it is also the first
  `import_std = true` package build on macOS/Windows in the matrix.

## 4. Compatibility ledger (explicit)

- Pre-0.0.85 clients + post-P2/P3 index: descriptor parse errors with no
  guidance — **accepted one-time break**, announced in 0.0.85 release notes;
  floor protects every client ≥ 0.0.85 from all future breaks (staged refresh
  keeps last compatible snapshot).
- `format_version` of artifact/pointer: unchanged. Pointer stays dumb.
- xim/xlings side keeps executing descriptors as real Lua — long brackets and
  new keys are valid Lua; no impact.
