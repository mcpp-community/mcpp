---
subject: build-program
status: active
---

# Four upstream asks from a UI framework: what each one is under mcpp's design, and the combined plan

**Status:** proposed and reviewed (2026-09-13). Nothing here is implemented.
The first ask was analysed in an earlier draft of this record (the action
buffer, §2); this revision reads all four against the rules the 2026-09-11
and 2026-09-12 records state, and replaces that draft. The review accepted
the four conclusions and added four things, recorded in §8 and folded into
the sections they touch. Engine facts were read at `baa89d2b` (mcpp
2026.9.12.4), mcpp-plugins at `db16e27` (0.8.0). `mcpp::deploy` and
`mcpp::min_platform_version()` are landed engine facts (2026.9.12.3,
protocol 11), not pending ones; an earlier draft read them from a branch.

## 0. The asks, as HuxerUI states them

| # | repository | ask | blocks | HuxerUI's workaround |
|---|---|---|---|---|
| 1 | mcpp | `mcpp::action`'s six fixed `char[]` become heap buffers | stage 0, resources via `mcpp::deploy` | one bridging action per payload file, each declaration under the bound |
| 2 | mcpp-plugins | `dist-apk`: a project manifest template with tokens; `java_sources` becomes an array, one `javac`, one `d8` | the whole Android stage | overwrite the member's manifest after `plan_for()`; merge Java into one directory at prepare |
| 3 | mcpp-plugins | `dist-web`: the per-file `cp` becomes a portable copy | nothing (Windows hosts pack Web elsewhere) | pack on Linux or macOS |
| ½ | xim-pkgindex | `cubism-sdk-native`, `cubism-sdk-web` at tier 2 | nothing (a downstream library, not HuxerUI) | Lib-Live2D's fetch script |

Web and iOS need nothing upstream; the wasm row is measured configuring.
Android is blocked by 2 alone. The framework's stage 0 is blocked by 1
alone.

Each ask is read against the same questions: which rule of the design it
touches, whether the precedent already exists in the collection, what the
member or engine alone knows and therefore must own, and what the criterion
is in both directions (2026-09-12 record, rules 7 and 8). The readings
change two of the four: ask 3's answer is an engine token plus a member
edit, not a member edit; ask 2's manifest check is a construction rule, not
an XML parse.

## 1. The rules the four are read against

From the 2026-09-12 record (§1), the ones that bind here:

- **Rule 1, per project / per target / per shape.** A per-shape knob is a
  member option; a per-project value is a manifest key; a per-target value is
  the engine's.
- **Rule 3, declare unconditionally, submit conditionally.**
- **Rule 4, a name is cache-safe, a path is not.**
- **Rule 7, each item has its own criterion.** A requirement folded into a
  neighbour's fix disappears when the neighbour ships.
- **Rule 8, the negative direction is part of the criterion** wherever a
  check can pass while measuring nothing.
- **Rule 9, the package, not the host path.**

From the 2026-09-11 record (§6) and the mcpp-plugins README: a dist member
exposes a plan/submit pair so a project edit is an edit and not a
reimplementation; it names its inputs rather than harvesting a directory; a
member drives what the ecosystem resolved and reads nothing from the host;
a member that runs a program says where the program comes from.

From the engine: the graph fixes the output set at prepare (docs/30, "You
must name the output files"); an action's command is an argv with no shell
assumed; "the engine is the portable wrapper. It is already on disk on every
platform mcpp runs on" (`cli.cppm:920`, the reason `__action-stamp` exists).

## 2. Ask 1: the action declaration is bounded by a fixed buffer

### 2.1 The bound

The bundled `mcpp` module every build.mcpp imports is a string literal in
`src/build/hostprogram.cppm`, compiled per build.mcpp with `<cstdio>` and
`<cstdlib>` in its global module fragment and no `import std;`. Its
`mcpp::action` collects six list fields into six fixed arrays
(`hostprogram.cppm:252`): `inputs_` 8192, `outputs_` 8192, `command_` 16384,
`provides_` 2048, `imports_` 2048, `targets_` 1024. Capacity is bytes of
serialised JSON, not entries. When a literal does not fit, `add()` drops it
and `submit()` writes `"overflow":true`; the engine refuses with "build.mcpp
declared an action whose arguments did not fit" and recommends a response
file or a directory input (`directives.cppm:1095`).

HuxerUI#130 measured the same 44 files under three unpack prefixes:

| prefix | `inputs_` bytes | result |
|---|---|---|
| 149 chars | 8131 + ~106 | refused |
| 109 chars | 6371 | builds |
| in-tree, relative | 1531 | builds |

The margin is about 45 bytes, and which row a consumer gets is decided by
where their checkout sits. No other limit in the action pipeline has that
property.

### 2.2 Why fixed, and which half of the reason holds

The comment at line 248 says the module "must stay buildable BEFORE a std
BMI exists", so no `std::string`. Two constraints are folded into that:

- The module must not `import std;`. True: it is compiled with the build
  program's base flags and no module references, and the engine stages a std
  BMI only when build.mcpp asks for one. This does not forbid `#include` in
  the global module fragment, which is how `<cstdlib>` is already there.
- The module's exported interface must name no std type. Not written in the
  comment, and the stronger reason: a build.mcpp with `import std;` and
  `import mcpp;` would see `std::string` by two routes, and the repository
  has measured what std types in a widely imported interface do to
  downstream BMIs (`2026-08-11-source-kind-table-and-build-program-timeout.md`
  §7.2). Every public member of `action` is `const char*` or `bool`; that
  stays.

Neither constraint concerns the heap. `std::realloc` and `std::free` come
from the `<cstdlib>` already included, exist on GCC, Clang and MSVC, and
appear in no exported signature. The fixed arrays were the simplest std-free
storage, not the only one, and the sizes were a guess about protoc command
lines.

### 2.3 Why the outputs side has no way around it

Inputs were worked around correctly: the action emits a Make-style
dependency file (`action::depfile`, #587, 2026-09-08) and `inputs` lists the
tool. `mcpp::deploy(from, to)` (#622 A4, landed in 2026.9.12.3, protocol 11) moves the same list to
the other side. Its `from` is a copy edge's input in the ninja graph, so it
must exist at prepare or be a declared action output; an `hrc` product is
the second kind, so every deployed file must be in `outputs_`. Three rules
the engine states and is right about close every exit:

1. Names may not arrive later (`prepare_actions` fixes the set before
   anything runs). There is no depfile for outputs and cannot be one.
2. A directory is not an output (the #622 record, §2.4: dirty on the wrong
   events, members unknown to the graph).
3. A response file is the tool's flag. `hrc --root <dir>` already takes a
   directory; that shortens the command, which was never the field that
   overflowed.

The framework's 44 files with the `out_dir` prefix serialise to about 5 KB.
An application's resources have no upper count.

### 2.4 What bounds an action once the buffer is gone

| stage | bound |
|---|---|
| `submit()` → stdout | none (`std::printf` into the capture pipe) |
| engine capture (`build_program.cppm:757`) | none (`std::getline`) |
| cache record | none; stored verbatim, replayed byte-identical |
| `decode_action` | none |
| `build.ninja` edge lists (`ninja_backend.cppm:2414`) | none; never an argv |
| the action's **command** at run time | OS argv: Linux 128 KiB per argument, Windows 32767 (`CreateProcess`), 8191 (`cmd.exe`) |

The last row is real, is the operating system's, bounds the tool's own argv,
and is answered only by the tool taking a response file or a directory. The
fixed array is the only artificial bound on `inputs` and `outputs`.

### 2.5 Three answers, and the recommendation

**A. A growable buffer inside the module.** A small owning type in the
module's private section over `realloc`, replacing the six arrays. No
exported signature changes, no new directive, no protocol bump; the payload
of every action that fit before is byte-for-byte what it was, so the cache
key `apply()` stores is untouched. `add()` loses its `while (buf[o]) ++o;`
scan (linear per call, quadratic per declaration) and its capacity
parameter. `overflow_` keeps its wire form and now means allocation failure;
the engine's message says so and drops the sentence about fixed buffers.
`action` gains a destructor and deep copies; every use read in the ecosystem
(`mcpp-plugins` rules and dist members, `mcpp-accel` examples, HuxerUI)
declares a local and calls `submit()`, and `action` was never an aggregate.
About thirty lines. Recommended.

**B. A response file for the declaration.** The module writes the lists to
a file under `out_dir`; the engine reads it at parse and on every replay;
the cache record must carry its contents or hash. Three moving parts and one
new way for a cache hit to be wrong. Rejected.

**C. Streaming directives**, one line per entry. Changes the wire shape, the
record, the decoder and the same-bytes invariant, and needs a protocol bump.
Rejected.

The response file the current message recommends is the right answer for
the one row of §2.4 the engine cannot remove and the wrong one for the row
it can.

### 2.6 The workaround, read

One bridging action per payload file, its input the index and its output
that file, is legal under the graph's rules and keeps every declaration
under the bound. It is N edges declared to satisfy a buffer, each an edge
whose command does nothing the producing action did not already do; the
graph carries it, ninja schedules it, and the log names it. It is the
correct interim shape if the release is not waited for, and it is the shape
the engine should make unnecessary.

## 3. Ask 2: `dist-apk` takes a manifest template and several Java roots

### 3.1 What the member does today

`dist-apk` 0.8.0 generates `AndroidManifest.xml` at plan time from four
values (`manifest_xml`, `apk.cppm:314`): the application id, the label, the
SDK levels, and either `android.app.NativeActivity` with `lib_name` (level
0) or `options::activity` (level 1). It also writes `assets/mcpp-run.json`
from the same id and activity, which `adb-run` reads to start the
application without `aapt2` on the running machine. `java_sources` is one
directory; `javac` compiles every `.java` under it, `d8` dexes the result.

What HuxerUI's host needs in the manifest, read from the framework's own
Gradle-side files (`platform/android/huxerui/src/main/AndroidManifest.xml`
and the `local_notification` example): `configChanges` on the activity,
`uses-permission` entries, a `receiver` and `meta-data` for notifications,
`android:icon`, an `activity-alias` for a URL scheme. Gradle merges the
library's manifest into the application's; mcpp has no merger and should
not grow one, for the reason `dist-web` gives for its page: which shape the
host needs is the program's contract, not the member's guess. The Java host
is in two places: 25 files in the framework package and the application's
own `Activity` in the project.

### 3.2 The manifest: a template with tokens, and the precedent on both sides

The collection already has both shapes. `dist-web` renders a project
`template_file` with `{{name}}` and `{{title}}`; `dist-wix` takes a whole
`.wxs` and passes the one value it cannot bake in (`$(Executable)`) through
`-d`. The question is which values are the member's, because those are the
ones a project file cannot spell as literal text:

| token | who knows it | why the project cannot write it |
|---|---|---|
| `{{application_id}}` | member, from `[package]` or `options::application_id` | the run sidecar is written from the same value |
| `{{activity}}` | member, from `options::activity` or the level-0 constant | same |
| `{{lib_name}}` | member, from the `app` target's name | the link output's stem is the engine's |
| `{{min_sdk}}` | member, from `mcpp::min_platform_version()` (A11, landed 2026.9.12.3) | a manifest key the engine resolves, not the member |
| `{{target_sdk}}` | member, from the platform payload's directory name | read back from `xpkg_dir`, so it cannot disagree with `-I android.jar` |
| `{{label}}` | member, from `options::label` | convenience; a project may write the literal |

The rule that follows, and where it differs from the ask: **the tokens that
carry a value the member also writes elsewhere are required, not merely
substituted.** A template without `{{application_id}}`, `{{activity}}`, and
at level 0 `{{lib_name}}`, is refused at plan time; the message names the
missing token and the reader downstream of it (`assets/mcpp-run.json`, which
`adb-run` starts the application from), so the author learns what the value
is for and not only that it is missing. The default template's comments mark
the three required tokens.

What the rule guarantees is stated exactly: **the token appears**. It does
not guarantee that the token lands in the `package=` attribute; an author
who writes `{{application_id}}` into a comment and a different literal into
`package=` has defeated it, and the member cannot see that without reading
the XML. That is a deliberate act and not the member's to prevent. The
ordinary case, an author who forgets, is caught. The ask's alternative,
rendering and then checking
`package=`, the launcher activity and `lib_name` in the result, needs a
second reader of the XML inside the member, and a second parser of one file
is the shape that reports the whole file wrongly
(`second-parser-reports-the-whole-file`); a substring check is the shape
that goes quiet when the spelling changes. Requiring the token is the same
guarantee with no parser. A project that wants a fixed package name sets
`options::application_id` and keeps the token.

An unknown `{{...}}` in the template is refused at plan time, naming it.
`dist-web` does not do this today and leaves an unknown token literal; for a
manifest that literal would reach `aapt2` and fail there with a worse
message, and the refusal belongs where the name is known.

Everything else in the file is the project's, verbatim: permissions,
receivers, meta-data, icon, aliases, `configChanges`. The member adds
nothing to it. The default template is the file `manifest_xml` generates
today, expressed with the same tokens, so level 0 with no template is
byte-identical to 0.8.0's manifest.

The rendered manifest is written at plan time to the side file the link step
already takes as input (`<out_dir>/dist-apk/AndroidManifest.xml`), through
`write_if_different`, so a template edit reaches the graph. The template
itself is declared with `rerun_if_changed`, which is the part the ask's
workaround (overwrite after `plan_for()`) cannot do: it depends on the
member's internal path and leaves the sidecar written from the member's
values while the manifest says something else.

### 3.3 `java_sources` as an array

One `javac` over every root and one `d8` over its output is the shape; the
member compiles what it is given (#622 §3.2) and a second root is more of
the same input, not a second step. `options::java_sources` becomes
`std::vector<std::string>`; a single string stays accepted in a
`build.mcpp` because the member is C++ and a one-element initialiser list
is the same spelling.

The re-run question is the one that needs stating. Today the member
declares `rerun_if_changed_glob("<root>/**/*.java")` so that a file
appearing re-runs the program. `glob_fingerprint` walks the **package
root** (`directives.cppm:805`) and matches paths relative to it; a root
under a dependency's unpack directory is outside that walk, matches
nothing, and the fingerprint is the same as "no files", which is a
criterion whose "no" reads as silence. So:

- a project root (under the manifest directory) is declared with the glob,
  as today;
- a dependency root is not: its file set changes only with the dependency's
  version, which is already in the build's fingerprint, and each file's
  content is already an input of the `javac` action. Declaring the glob for
  it would be a line that measures nothing.

The member decides which of the two a root is by whether it lies under
`mcpp::manifest_dir()`, and says nothing for the other case rather than
warning about a normal one.

One corner is the consumer's and HuxerUI meets it daily: a **path
dependency**. Its three examples declare `huxerui = { path = "../../.." }`,
so during development a `.java` added to the framework changes the file set
under a root the glob does not walk, with no version change to carry it into
the fingerprint. The build program does not re-run, and the new file is not
compiled until something else triggers a re-run. The member's rule is still
right; the answer is the dependency's, and it is the engine's own "declare
instead of discover": the rule package carries a list of its Java sources
(`android/java-sources.txt`, maintained by `huxerui-build-check`, so drift
is red in CI) and declares `rerun_if_changed` on that one file. A file
appearing is then an edit to the list, which is a content change the
fingerprint sees.

### 3.4 What the ask does not list, noted and not folded in

`options::resources` is one directory, and the compile step declares that
directory as its input (`apk.cppm:788`), which is the "dirty on the wrong
events" shape §2.3 refuses for outputs. The framework's `res/` and the
application's are two roots for the same reason the Java is. Rule 7: this
is its own item with its own criterion and is not part of ask 2. Recorded
so it is not lost.

### 3.5 Criteria

- A template that omits `{{application_id}}` is refused at plan time and the
  message names the token; the same template with it renders, and
  `aapt2 dump badging` on the linked `base.apk` reports the id and the
  launcher activity that `assets/mcpp-run.json` carries, compared as two
  values from two files.
- A template with a `uses-permission` and a `receiver` produces an APK whose
  `aapt2 dump xmltree` lists both; the default template on the same project
  produces neither, and the level-0 manifest with no template is
  byte-identical to 0.8.0's.
- Two Java roots, one in the project and one under a dependency, produce one
  `classes.dex` containing classes from both (`dexdump` or `d8`'s own
  listing); a `.java` added to the project root re-runs the build program on
  the next build, and one added to the dependency root does not, which is
  the intended reading, not a gap.

## 4. Ask 3: `dist-web` copies with something that exists on Windows

### 4.1 What the member says and what the engine already has

`dist-web` declares one `cp SRC DST` action per staged file, argv only, and
its header says why it is POSIX-only: neither precedent (`appimagetool`'s
own argument list; `ditto`, macOS only) is a portable multi-file copier, and
"lifting it needs either a `copy`-argv branch on the host OS or a small
copier this member carries itself" (`web.cppm:48-59`).

Both of those are the wrong shape. `cmd /c copy` is a shell, is the 8191
limit, and is the switch-quoting the repository has already been bitten by
twice. A copier carried by the member is a host tool sub-build (#355) for
one `cp`.

The engine has the copier. `mcpp stage --output <dst> <src>` is the
subcommand every `stage_file` edge in `build.ninja` already runs
(`ninja_backend.cppm:841`): it creates the destination's parent, compares
content and writes only on difference, which is what makes `restat = 1`
worth having, and it is on disk wherever mcpp runs. The `check` role is
wrapped with `mcpp __action-stamp` for exactly this argument
(`cli.cppm:920`).

What is missing is the way for an action to **name the engine**. `$mcpp` is
a ninja variable and an action's tokens are ninja-escaped, so a member
cannot spell it. The substitution family an action's argv already has
(`${mcpp.out_dir}`, `${mcpp.bin_dir}`, `${mcpp.compile_db}`,
`${mcpp.stage_dir}`, `${mcpp.target_file:NAME}`, `prepare.cppm:11138`) is
where the answer goes: `${mcpp.self}`, replaced by the engine's own absolute
path, the same `mcpp_exe_path()` the check wrapper bakes in and with the
same caveat (a version change regenerates `build.ninja`; moving the binary
without changing its version leaves a stale path, exactly as for the
compiler). The review confirmed the precedent from an existing artifact: the
`build.ninja` of the wasm probe already carries `mcpp __action-stamp` as the
engine's absolute path, so the token adds a second reader of a value the
graph already holds and no new kind of value.

The name: `self` is the word the engine already uses for itself in the
`mcpp self version` / `mcpp self doctor` family, so a reader knows what it
names. `${mcpp.bin}` collides with `${mcpp.bin_dir}`; `${mcpp.engine}` has no
precedent.

### 4.2 The split

- **mcpp:** the token, one `rep(...)` line and a row in docs/30's
  substitution table (both languages). The path-check skips for tokens
  containing `${mcpp.` (`directives.cppm:1141`) already cover it.
  **`mcpp stage` becomes a contract in the same change.** The subcommand is
  marked internal today ("invoked by ninja", `cli.cppm:844`); the first
  member that names it turns its argument shape into something the engine
  may not change under a published plugin, or the plugin fails on the old
  graph with no message that says why. So `stage --output <dst> <src>`,
  with `--verify content` spelled out, is written into the same docs/30
  table beside `${mcpp.self}`, and the release that carries both is the
  floor the member declares. Spelled out rather than defaulted because the
  help text says the default is `size` and the code's default is content
  (`stage.cppm:60`), and a contract must not depend on which of the two a
  reader believes.
- **mcpp-plugins:** `dist-web`'s two copy steps become
  `{ "${mcpp.self}", "stage", "--verify", "content", "--output", dst, src }`;
  the POSIX-only note
  leaves the header and the README row; the member's floor rises to the
  engine release carrying the token. `dist-web` also stops creating
  destination directories at plan time, since `stage` does.

An older engine leaves `${mcpp.self}` literal and the action fails at run
time with a not-found for a path that reads as a token; rule 6's
"compatibility by ignoring" does not reach argv, which is why the floor is
raised rather than the token guarded.

### 4.3 Criteria

- The e2e that packs the verified Web row on the Windows shard produces the
  same file set the Linux shard does, listed and compared; it runs without
  `# requires:` gating.
- The negative direction: a `${mcpp.self}` in an action under the engine one
  release earlier appears literally in `build.ninja`'s command line, which
  is what the floor exists to prevent; checked once, at merge.
- A second `mcpp pack --format web` with nothing changed copies nothing
  (`ninja -n` lists no `WEB FILE` edge), which `cp` could never give and
  `stage` gives for free.

## 5. Ask ½: the Cubism SDKs at tier 2

The 2026-09-11 record §9.8 states the ladder: redistribute if the licence
permits, otherwise fetch from upstream with no CN mirror, otherwise locate
what the machine has. The #622 record (§4.4, C5) already places Cubism at
tier 2 by `iphoneos-sdk.lua`'s shape: one anonymous upstream URL, `sha256`
pinned, no CN entry, `licenses` recording both the Open Software License and
the Core's proprietary one. Nothing in the ask departs from that.

Two things the recipe author measures rather than assumes, because each has
a silent failure mode:

- The URL is anonymous. Cubism's download page gates on a licence
  acceptance; if the archive URL is not fetchable without it, tier 2 is
  not reachable and the honest recipe is tier 3. Lib-Live2D's
  `fetch_cubism.py` already opens that URL with no credential and checks
  the `sha256`, which is a measurement and belongs in the recipe's comment
  as its evidence; the rule still asks for one install in the sandbox from a
  clean home, because a script that works on the author's machine is the
  reading the sandbox exists to separate from the general case.
- `licenses` is a set and a wrong member is worse than none. Both licences
  are named; the Core's is not "proprietary" as a placeholder but the
  licence's own name.

Native and Web are two packages with two payloads and two PRs, by the index's
one-package-one-identity rule. Neither blocks a HuxerUI row; Lib-Live2D
consumes them when they exist and its fetch script is the interim.

## 6. Order and repositories

| step | repository | what lands | unblocks |
|---|---|---|---|
| 1 | mcpp | §2.5 A, the buffer; §4.1's `${mcpp.self}` and the `stage` contract (§4.2) in the same release | HuxerUI stage 0; `dist-web` on Windows |
| 2 | mcpp-plugins | `dist-apk` template and array (§3), `dist-web` on `stage` (§4.2); one release, floor raised to step 1's engine | the Android stage |
| 3 | xim-pkgindex | the two Cubism recipes | Lib-Live2D |
| 4 | HuxerUI | `hrc:builtin` / `hrc:app` enumerate outputs and emit one `deploy` per file; the rule adapter passes both Java roots, its manifest template and its `java-sources.txt` (§3.3); engine floor raised | its own stage 0 and Android |

Step 2 depends on 1; 3 depends on nothing; 4 depends on 1 and 2. Nothing
waits on an engine branch: `mcpp::deploy` and the platform floor are in
2026.9.12.3. Two of the asks were filed against mcpp-plugins and one of
those (ask 3) is answered in mcpp first; the table is what the issues
should say.

**What HuxerUI does while it waits.** The bridging action of §2.6 is not
built; it stays in this record as the fallback. The parts that depend on
none of the four proceed: closing its PR #6, everything in stage 0 except
the `deploy` step, Web, and the iOS simulator row.

## 7. What this record does not propose

- A manifest merger. The project writes the whole file; the member fills the
  values only it knows.
- A bound on the growable buffer. The engine bounds no other directive.
- Guarding `${mcpp.self}` for older engines. The floor is the mechanism.
- Changing `options::resources` (§3.4), the command-axis OS limits (§2.4), or
  a general "copy" action role. Each would be its own record.

## 8. Review (2026-09-13)

The four conclusions were accepted as written. Three decisions were put to
the reviewer and answered:

1. **Required tokens** (§3.2): accepted over render-then-check, with the
   wording corrected from "by construction" to "the token appears", and the
   refusal message and default template extended as §3.2 now says.
2. **Ask 3 in two steps, the issue moved to mcpp** (§4): accepted, with the
   addition that `mcpp stage`'s argument shape enters the contract and the
   floor with the token, not after it.
3. **The name `${mcpp.self}`**: accepted, for the reason §4.1 now records.

Four additions came from the review and are folded in above:

- The engine's absolute path is already in `build.ninja` (`__action-stamp`),
  checked on the wasm probe's graph; the byte-identity claim of §2.5 was
  checked against existing artifacts and holds (§4.1).
- The path-dependency corner of the Java re-run rule, and HuxerUI's answer
  to it (§3.3).
- Lib-Live2D's fetch script as recorded evidence for the Cubism URL (§5).
- A11 was already landed; the earlier draft's step that waited for it is
  removed (§6).

### 8.1 What implementation changed (mcpp side, 2026-09-13)

- **The count criterion of M5 read the whole file and reported 2N.** A
  source action's outputs appear twice in `build.ninja`: on the action's own
  edge and again as the inputs of the package's ordering phony. The fixture
  now reads only the `build ...: mcpp_action_<k>` line; the first run of the
  fixture is what surfaced the second listing.
- **`add()` now escapes control characters as `esc()` does.** The fixed-array
  revision passed them through, which produced a payload that was not JSON
  and was refused as malformed; so no payload the engine ever accepted
  contained one, and the byte-identity claim of §2.5 holds for every
  accepted payload. Measured: the cache record of a three-entry action is
  byte-identical (1088 bytes) under 2026.9.12.4 and under the change.
- **The reverse legs.** Under 2026.9.12.4, e2e 659 is refused with "arguments
  did not fit" and e2e 660 fails with `${mcpp.self}: not found` from the
  shell ninja handed the literal token to. Both recorded in the PR.
- **`mcpp stage`'s help said the default was `size`; the code's default is
  content.** The help now states the shape the docs/30 row quotes. The
  subcommand stays out of the top-level usage list, since nobody types it;
  the contract is the argument shape, not its place in `--help`.
- **The long-command leg of M5 is skipped on Windows only**, and the reason is
  §2.4's last row: ninja runs every command through `cmd /c` there, whose
  8191-character cap is the operating system's limit on the tool's argv. The
  wide-list leg runs on every shard.

One observation stands as recorded and not acted on: `options::resources`
as a single directory declared as an input (§3.4). It affects HuxerUI
little today, since the framework's Android library carries no `res/` and
only the application has one root, and it is its own item.

## 9. The task list

One pull request per repository carries everything that repository owes; a
split is made only where a release boundary forces it (mcpp-plugins can only
be green against a released engine). Each task names its files, its
criterion in both directions, and what it waits on. "Done" for the whole is
§9.5, not any row.

### 9.1 mcpp, one PR (release 2026.9.13.1)

| id | task | files | criterion | waits on |
|---|---|---|---|---|
| M1 | `mcpp::action`: six arrays become one growable std-free buffer type; `add()` O(1) amortised; `overflow_` means allocation failure; comment at line 248 restated as the two constraints of §2.2 | `src/build/hostprogram.cppm` | e2e M5; byte identity M6 | — |
| M2 | `action_error`'s overflow message rewritten for allocation failure | `modules/buildmcpp/src/directives.cppm` | unit test: the marker still refuses, the message no longer names a buffer size | — |
| M3 | `${mcpp.self}` in the argv substitution family, replaced by `mcpp_exe_path()` | `src/build/prepare.cppm` | e2e M7 | — |
| M4 | `mcpp stage` promoted from internal to contract: CLI help states the argument shape and the content default correctly (`--verify` help text says `size (default)`; the code default is content, `stage.cppm:60`) | `src/cli.cppm` | `mcpp stage --help` names `--verify content` as the default; the docs row of M8 quotes the same shape | — |
| M5 | e2e: an action with 200 outputs and a command past 16 KiB builds; `build.ninja` lists exactly 200 outputs on that edge; all 200 exist; a program-cache replay after an unrelated source is added keeps the edge complete; no `# requires:` gate | `tests/e2e/659_*.sh` | reverse leg: the same fixture at `baa89d2b` is refused with the overflow diagnostic, run once and recorded in the PR | M1 |
| M6 | byte identity: the `mcpp:action=` line of e2e 188's fixture, captured under `baa89d2b` and under M1, compared with `cmp` | PR body | equal | M1 |
| M7 | e2e: an `artifact` action `${mcpp.self} stage --verify content --output <dst> <src>` copies a file on every shard; a second build copies nothing; no `# requires:` gate | `tests/e2e/660_*.sh` | reverse leg: under `baa89d2b` the token stays literal in `build.ninja` | M3, M4 |
| M8 | docs/30 en and zh: the action section states the list fields have no declared limit and the command is bounded by the OS at run time; the substitution table gains `${mcpp.self}` with the `stage` shape beside it; the "did not fit" advice is removed | `docs/30-build-mcpp.md`, `docs/zh/30-build-mcpp.md` | `check_docs_structure.sh`, `check_docs_style.sh` | M1, M3, M4 |
| M9 | CHANGELOG entry; version bump to 2026.9.13.1 in `mcpp.toml` and `modules/versioning/src/version.cppm` (same commit); `check_version_pins.sh` | as named | `01_help_and_version.sh` | M1 to M8 |
| M10 | this record: status to landed, §8 extended with what implementation changed | this file | index regenerated | M9 |

### 9.2 mcpp-plugins, one PR (release 0.9.0)

| id | task | files | criterion | waits on |
|---|---|---|---|---|
| P1 | `dist-apk`: `options::manifest_template`; six tokens; the three required ones refused by name with their downstream reader; unknown token refused; default template is the 0.8.0 manifest in token form with the required tokens marked | `dist/apk.cppm` | §3.5 first two bullets, in `tests/apk-consumer` (level 0 default: byte-identical manifest; a template with a permission and a receiver: `aapt2 dump xmltree` lists both; a template missing `{{application_id}}`: refused, message names the token and `mcpp-run.json`) | — |
| P2 | `dist-apk`: `java_sources` becomes a vector; one `javac`, one `d8`; `rerun_if_changed_glob` only for roots under `manifest_dir()` | `dist/apk.cppm` | §3.5 third bullet: two roots, one dex with classes from both; a `.java` added under the project root re-runs the program | — |
| P3 | `dist-web`: copy steps use `${mcpp.self} stage --verify content --output`; plan-time `create_directories` removed; the POSIX-only note leaves header and README; floor raised | `dist/web.cppm`, `README.md` | `tests/web-consumer/check-web-plan.sh` unchanged and green; second `pack` copies nothing (`ninja -n` lists no `WEB FILE` edge) | M-release |
| P4 | README rows for `dist-apk` and `dist-web`; `mcpp.toml` version 0.9.0; CI `MCPP_VERSION` to 2026.9.13.1 | `README.md`, `mcpp.toml`, `.github/workflows/ci.yml` | CI green on all three runners | M-release |
| P5 | tag `v0.9.0`; source archive to `mcpp-res/mcpp-plugins` on GitCode; sha256 recomputed from both ends and equal | release | GET on both URLs returns the archive; `sha256` equal | P4 merged |
| P6 | mcpp-index: `pkgs/m/mcpp.plugins.lua` gains 0.9.0 and `latest` moves | mcpp-index PR | index artifact published; `mcpp add` resolves 0.9.0 | P5 |

### 9.3 xim-pkgindex, one PR

| id | task | files | criterion | waits on |
|---|---|---|---|---|
| X1 | `cubism-sdk-native` 5-r.5 and `cubism-sdk-web` 5-r.5 at tier 2: the upstream URL, `sha256` from Lib-Live2D's pinned values recomputed after an anonymous download, no CN entry, `licenses` naming both licences by their own names read from the archive | `pkgs/c/cubism-sdk-native.lua`, `pkgs/c/cubism-sdk-web.lua`, `tests/c/` | the index's own recipe tests; an install in the sandbox from a clean home; the header states the tier and why | — |

### 9.4 Verification, in the sandbox

| id | task | criterion |
|---|---|---|
| V1 | `xlings subos new eco-2026-9-13 --sandbox`, CN mirror set for mcpp and xlings; `xim:mcpp@2026.9.13.1` from the index reports its version | the version line, from the store path |
| V2 | `mcpp:plugins@0.9.0` from the index: `tests/web-consumer` packs and the copy steps carry the engine path; `tests/apk-consumer` at level 0 and with a template | listings, not "it ran" |
| V3 | a consumer with 200 declared outputs builds through the released engine | M5's fixture, against the installed engine |
| V4 | `xim add cubism-sdk-native` and `-web` install from the anonymous URL | the extracted tree's `LICENSE` files exist |

### 9.5 Done

The index's `main` names 2026.9.13.1 as `latest` for `xim:mcpp` and 0.9.0 for
`mcpp:plugins`; V1 to V4 pass in the sandbox; this record and the two
repositories' READMEs say what shipped. HuxerUI's step is theirs and is not
in this list.
