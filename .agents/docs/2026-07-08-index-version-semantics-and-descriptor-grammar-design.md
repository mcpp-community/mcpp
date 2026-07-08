# Index version semantics + descriptor grammar v2 (long brackets) + single-source-of-truth lint (Design)

Three coupled deliverables toward "描述符可读、lint 与客户端文法一致、index 对客户端
有版本契约", targeting the **0.0.85** release window while the user base is small
enough to absorb a one-time grammar extension:

- **Phase 1 (repo `mcpp`)** — (D1) teach the descriptor mini-reader Lua long-bracket
  strings `[[…]]` / `[=[…]=]` and block comments; (D2) add `mcpp xpkg parse <file>`
  so index CI validates with the *same* parser users build with; (D3) read an
  index-level `index.toml` version contract (`min_mcpp` / `latest_mcpp`) and
  error/hint accordingly.
- **Phase 2 (repo `mcpp-index`)** — add `index.toml`, switch the lint job to
  `mcpp xpkg parse`, then migrate `generated_files` descriptors to long-bracket
  form (fmt PR #63 first candidate).

Ordering rule that makes the rollout safe: **floor first, migration after** —
no descriptor may use the new grammar until the index declares
`min_mcpp = "0.0.85"` and CI's pinned `MCPP_VERSION` is ≥ 0.0.85.

---

## 1. Problem (grounded in code + experiments, 2026-07-08)

Three defects observed while testing mcpp-index PR #63 (`fmtlib.fmt`):

1. **Validator and consumer speak different grammars.** Index lint validates
   descriptors with real Lua (`lua5.4 loadfile`), but mcpp parses the `mcpp = {}`
   segment with a hand-written subset reader (`LuaCursor` /
   `synthesize_from_xpkg_lua`, `src/manifest.cppm:1458+`) whose `read_string()`
   accepts only `"`/`'` quotes. Experiment: rewriting the fmt descriptor's
   `generated_files` value as `[==[…]==]` is byte-identical at the Lua level
   (loadfile OK, 3425 bytes equal) yet mcpp 0.0.81 fails at *user build time*
   with `malformed mcpp segment near key 'Formatting'` (the reader falls out of
   sync at `[` and reads string content as keys). Lint green, user red — the
   worst failure geometry.

2. **Escaped one-line `generated_files` are unreviewable.** fmt's module wrapper
   is a 3.4 KB single-line `\n`-escaped string; verifying "only 2 edits vs
   upstream fmt.cc" required scripted extraction. nlohmann.json and compat.eigen
   have the same shape. This is a direct consequence of the grammar gap above —
   long brackets are the natural fix and are already valid Lua for the xim side.

3. **No index→client version contract.** Index repos (`[indices]`,
   `src/pm/index_spec.cppm`; default `mcpplibs` fetched/refreshed via xlings,
   `src/xlings.cppm:271-295`) carry no "minimum mcpp that can read me". Any
   grammar/schema evolution therefore breaks old clients *at descriptor parse
   time* with a confusing error and no upgrade guidance. There is also no
   channel to tell users a newer mcpp exists (`mcpp self` has
   version/init/config only — no update machinery, `src/cli/cmd_self.cppm`).

---

## 2. Design

### D1 — descriptor grammar v2: long brackets + block comments (repo `mcpp`)

Extend the `LuaCursor` data-literal subset in `src/manifest.cppm`:

- `read_string()`: on `[` followed by `=`* then `[`, read a long-bracket literal
  of that exact level; content is verbatim (no escape processing); strip one
  leading newline immediately after the opening bracket (Lua semantics).
- `strip_lua_comments_and_strings()`: recognize `--[[…]]` / `--[=[…]=]` block
  comments and long-bracket strings; blank content (preserve newlines) while
  keeping the bracket structure, mirroring today's quote handling.
- `skip_table()` / `read_table_body()`: they dispatch through the same
  string-aware paths — verify they inherit the new branch; add cases if not.

Explicitly **out of scope** (grammar stays a data-literal subset, documented in
`docs/04-schema-xpkg-extension.md`): `..` concatenation, function calls,
variables, computed keys. The reader remains "typed fields out of a literal
table", not a Lua VM. (Rejected alternative: embed real Lua — loses the
"mcpp never executes descriptors" security property, breaks static auditability
of urls/mirrors, adds a C dependency; the schema is ~12 closed keys and does not
justify it.)

Parity oracle for tests: for every fixture, content extracted via
`lua5.4 loadfile` must equal content extracted via `LuaCursor` — this pins the
subtle rules (level matching, first-newline strip, `]]` inside content requiring
a higher level, CRLF passthrough).

### D2 — single source of truth for lint: `mcpp xpkg parse` (repo `mcpp`)

New CLI: `mcpp xpkg parse <file.lua> [--json]`

- Runs `synthesize_from_xpkg_lua` + `canonical_xpkg_identity_from_lua` (identity
  gate) on the file, for each platform present in `xpm`.
- Success: prints a summary of the synthesized manifest (name, namespace,
  versions, modules, generated_files paths+sizes, targets, features); `--json`
  emits machine-readable form. Failure: the exact error users would see at
  build time, non-zero exit.
- ~30 lines of routing; the parsing functions are already exported.

mcpp-index lint then runs **both** validators, because the descriptor has two
real consumers with two real grammars:

- `lua5.4 loadfile` — guards the xim/xlings side (which executes real Lua);
- `mcpp xpkg parse` (pinned `MCPP_VERSION` binary) — guards the mcpp side.

Lint's verdict and the user's build verdict become identical *by construction*,
for this and every future grammar divergence — this retires the whole bug
class, not just `[[]]`. It is also the enforcement mechanism for the rollout
rule: while CI pins 0.0.84, a long-bracket descriptor simply fails lint.

### D3 — index version contract: `index.toml` (both repos)

Principle: **the contract describes the index content, so it lives in the index
tree and travels with it** (same pattern as Cargo's registry `config.json`).
One file, one check site, one flow change; the distribution chain
(`tools/publish_mcpp_index.sh`: git repo → content-hash artifact → rolling
pointer) is otherwise untouched — in particular the **pointer stays a dumb
pointer** (name/sha/size, no new fields, `format_version` unchanged).

**One file** — `index.toml` at the index tree root (sibling of `pkgs/`),
authored and reviewed in the index repo, packed into the artifact by
`publish_mcpp_index.sh` (today it packs only `pkgs/` + README; one `cp` line).
Every place an index tree exists — git checkout, unpacked artifact snapshot,
CI-restored cache, `[indices] path =` local dir, third-party clone — therefore
carries its own contract, with no derivation or sync logic:

```toml
[index]
spec        = "1"        # index layout spec (pkgs/<x>/<name>.lua)
min_mcpp    = "0.0.85"   # oldest mcpp able to parse every descriptor herein
latest_mcpp = "0.0.85"   # optional: newest known-good mcpp (upgrade hints)
```

**One check site** — the code has a single choke point where an index tree is
opened to read descriptors (the index-dir layer in
`src/pm/package_fetcher.cppm`: `sorted_index_dirs` / `read_xpkg_lua_from_path`).
The floor check lives there and only there: if `own_version < min_mcpp` → hard
error with an E-code and explicit upgrade instructions (`mcpp explain <CODE>`
for the long form). All transports converge on "a directory containing
`pkgs/`", so one check covers them all — including offline use and mcpp-index's
own CI workspace members, which exercise it for free. Escape hatch:
`MCPP_INDEX_FLOOR=ignore`. Version comparison reuses `src/version_req.cppm`.

**One flow change** — index refresh becomes *staged + atomic*: download →
unpack to a staging dir → read the staged tree's `index.toml` → if compatible,
atomically swap into place; if not, **discard staging, keep the current
snapshot, warn once** with the upgrade hint. This yields graceful degradation
on future floor bumps (a 0.0.85 client facing `min_mcpp = "0.0.90"` keeps its
last compatible snapshot instead of bricking itself) using the same file and
the same check code — no pre-download sensing channel needed. The cost is one
wasted tarball download (the index tarball is a few hundred KB of .lua files,
at most once per freshness-TTL window, only while the client is behind the
floor); staged-unpack + atomic swap is good hygiene against partial downloads
anyway.

**Soft hint** — if `own_version < latest_mcpp`, print one rate-limited stderr
line (piggyback the freshness TTL: at most once per refresh window, not per
build).

**Missing `index.toml`** → no constraint (back-compat with existing snapshots
and third-party indices).

#### Architecture rationale: authority / projection / channels

Two contracts with different owners and lifecycles must not be conflated:
the **transport contract** (how to fetch/unpack: pointer JSON structure,
tarball layout — owned by the publishing pipeline, versioned by the existing
`format_version`) and the **content contract** (what capabilities consuming
the tree requires: descriptor grammar/schema — owned by index maintainers,
versioned by `index.toml` `spec`/`min_mcpp`). Putting the content floor *only*
in the pointer would hand content authority to the transport layer — the
wrong owner, and blind for every path that bypasses the pointer (git clone,
`path =`, restored caches).

Three layers, each subsuming the previous one's failure mode; invest per
ecosystem scale:

- **L1 — authority (this design, mandatory)**: contract travels inside the
  tree; single check at the index-open choke point; staged refresh keeps the
  last compatible snapshot. Parse failure → graceful keep-old + warn.
- **L2 — projection (optional, add anytime)**: publish script mechanically
  derives `min_mcpp` into the pointer entry as *catalog metadata*, enabling
  pre-download sensing. Legitimate exactly because L1 exists: projections may
  be redundant, authority may not be ambiguous — on conflict the in-tree file
  wins. Deferred today (saves only a few hundred KB per TTL window), never
  architecturally wrong.
- **L3 — routing (evolution path, not now)**: versioned channels
  (`mcpp-index-v1-latest`, `-v2-latest`, …; Debian dists / Rust channels
  pattern). Incompatibility becomes a *routing decision* instead of an error:
  clients select the channel matching their capability, old channels freeze,
  `min_mcpp` degrades to a backstop assertion. Operationally premature at the
  current user base; the hooks already exist for free (`index.toml` `spec` as
  the future channel key; pointer `format_version` + the `indexes:{mcpp:{…}}`
  shape extends to channel maps; the L1 check survives unchanged as the final
  assertion).

Rejected alternative: per-descriptor `min_mcpp`. Finer-grained, but N places to
maintain, and the failure it prevents (client parses *this* package) is already
covered by one index-wide dial that CI keeps honest. The per-descriptor
`spec = "1"` field stays as-is — the grammar change is representation-level,
not schema-level.

Known limitation, accepted deliberately: the floor only protects clients
≥ 0.0.85 (older binaries don't know to look for `index.toml`). With today's
user base the one-time break is acceptable — that is exactly why this ships
now. Release notes for 0.0.85 carry the notice.

### D4 — rollout order

1. **mcpp 0.0.85**: D1 + D2 + D3 land together (D2 without D1 would lint a
   grammar nobody can use; D1 without D3 would let migrated descriptors brick
   old clients silently).
2. **mcpp-index**, in one PR: add `index.toml` (`min_mcpp = "0.0.85"`), bump CI
   `MCPP_VERSION` to 0.0.85, add the `mcpp xpkg parse` lint step.
3. **mcpp-index**, follow-up PRs: migrate `generated_files` descriptors to
   `[==[…]==]` (fmt → nlohmann.json → compat.eigen), update
   `docs/repository-and-schema.md` (replace "不支持 `[[…]]`" with "requires
   mcpp ≥ 0.0.85; lint enforces"), and require long-bracket form for *new*
   generated_files descriptors going forward.

---

## 3. Implementation sketch

Phase 1 (`mcpp`), roughly in commit order:

1. `manifest.cppm`: `read_long_bracket()` helper + `read_string()` branch +
   `strip_lua_comments_and_strings()` support; unit tests incl. the
   lua5.4-parity fixtures (level ≥ 1, embedded `]]`, first-newline, block
   comment containing the text `mcpp = {`, CRLF).
2. `cli.cppm` + new `cli/cmd_xpkg.cppm`: `xpkg parse` routing; e2e test feeding
   a good and a malformed descriptor.
3. `pm`: `index.toml` reader (tiny TOML via existing manifest TOML machinery);
   floor check at the index-open choke point; refresh flow → staged unpack +
   `index.toml` check + atomic swap (keep-old + warn on incompatible); E-code +
   `explain` entry; e2e: fixture index with `min_mcpp = "9.9.9"` → build fails
   with upgrade message; `MCPP_INDEX_FLOOR=ignore` passes; refresh against an
   incompatible staged snapshot keeps the old one.

Phase 2 (`mcpp-index`): `index.toml`; `tools/publish_mcpp_index.sh` packs it
into the artifact tree (one `cp`; pointer untouched); lint job
gains a pinned-mcpp download step (same snippet the workspace job uses) +
`xpkg parse` loop over changed descriptors; descriptor migrations as separate
reviewable PRs (the diff of escaped-string → long-bracket is mechanical and
the parity oracle makes it safe).

## 4. Risks / open questions

- **`]]` inside generated C++**: writer picks a sufficient level (`[==[`);
  lint (via D2) catches a wrong choice. No auto-selection needed.
- **Parity drift**: the lua5.4-parity test is the guard; any future LuaCursor
  change must keep it green.
- **`latest_mcpp` staleness**: it's advisory only; index CI bumps it alongside
  `MCPP_VERSION`. Do we want the hint at all in v1? (cut-line candidate)
- **Third-party indices** never gaining `index.toml`: fine — absence means no
  contract, same as today.
- **Interaction with `mcpp self update`** (doesn't exist yet): the floor error
  prints manual instructions now; if/when self-update lands, the E-code message
  upgrades to "run `mcpp self update`".
