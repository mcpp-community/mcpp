---
subject: design
status: active
---

# Build output, revision 3: every package that does work is named, the live display is one line drawn in one write, and a repeated warning is stated once per file

- Status: active (a proposal for review; draft 3, after review rounds 1
  and 2 of section 5). It refines `2026-09-29-build-progress-display-design.md`
  (revision 2, landed in #742, released as 2026.9.29.5) and replaces the
  parts listed in section 6.
- Date: 2026-09-30
- Origin: a report on `mcpp build` in the xlings repository with mcpp
  2026.9.29.5. It raised three questions: why the build prints warnings;
  why the first line flickers before the output appears; and why the
  dependencies, which the previous release listed, are no longer visible.
  The report summarised the result as a display that shakes and conveys
  less than before.

## 0. Scope

The human output of the commands that build (`build`, `run`, `test`, `pack`),
from the first line to `Finished`. Machine output (`--message-format json`)
is unchanged, except that the identity warnings of section 8 are grouped.
The cost of planning is outside the scope. Every edit of a source file takes
the planned path by the rule of mcpp#225, and planning took 3.06 s of the
reported 33.63 s. Section 11 records this.

## 1. The report

The reported output, with eight of its nine warnings elided (section 3, F9):

```
   Workspace building member 'xlings'
warning: 'mcpplibs.xlings@path' declares the dependency 'cancellation', which names mcpplibs.cancellation; the manifest '/home/…/xlings/modules/cancellation/mcpp.toml' declares xlings.cancellation, and that identity is used.
  hint: write 'xlings.cancellation' in 'mcpplibs.xlings@path' to state the identity the manifest declares.
   … eight more warnings of the same form …
   Resolving toolchain
    Resolved gcc@16.1.0 → @mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++
      Target x86_64-linux-gnu → x86_64-unknown-linux-gnu
    Inferred sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]
  build.mcpp 1 dependency      ran 0.64s
   Compiling xlings (.)        done 29.65s
   Compiling 21 dependencies   done 8.10s · 13 cached

    Finished dev [unoptimized + debuginfo] in 33.63s · plan 3.06s · programs 0.64s · build 29.94s · longest xlings: obj/xlings/src/core/xself/doctor.o 8.38s
```

## 2. Method

- The xlings tree at c4b6cef (`git archive`, so the report's working tree was
  not touched) was built on a pseudo-terminal of 120 × 40 under
  `strace -f -y --trace-fds=1,2 -e trace=write,writev`. The trace gives every
  write to the terminal, with its time and thread.
- Each recording was replayed through the terminal emulator of e2e 843. After
  every write the replay records whether the status line is on the screen.
- Three builds were recorded:
  - **B1**: the first build of the tree;
  - **B2**: a build after touching one source file of the root package;
  - **B3**: a build after editing a module of the path dependency
    `modules/platform`.
- 2026.9.29.4 built the same tree for comparison.
- The recorder and the replay are in the session's scratch directory.
  Section 13 proposes keeping the replay as a test helper.

## 3. Findings

**F1. A frame reaches the terminal in two or three writes.** B1 made 440
writes to the terminal. 202 of them began a frame by erasing the region:

- In 184 of those 202, the status line followed in a second write. The cause
  is the stdio buffer: `mcpp.platform.terminal::write` uses `std::fwrite`
  (`terminal.cppm:207`), stdout on a terminal is line-buffered, and a frame
  whose last row, the status line, has no newline is therefore flushed in two
  parts.
- The other 18 were lines written above the region. `emit_locked`
  (`ui.cppm:489`) erases the region, writes the line and redraws, and flushes
  after each of the three.

Each frame erases first (`\r ESC[nA ESC[J`) and draws afterwards. Between
the writes the screen shows the region blank, for 0.1 ms at the median and
1.0 ms at most. A local terminal usually receives both writes in one read.
The blank state becomes visible when they arrive separately: in a remote
session, through ConPTY on Windows, or in a terminal that is busy.

**F2. The status line is drawn before anything is known, and every early
line moves it.** `progress::open` opens the region at once, and the status
line `Resolving · 0:00` was on the screen 0.6 ms after the command started.
The first warning arrived 0.37 s later. It took the status line's row, and
the status line was drawn again below it. Each of the 18 lines written during
planning moved the status line down: by one row per line, and by three rows
for a warning that wraps. This is the first-line flicker of the report.

**F3. The region changes height.** It is two rows (a blank row and the status
line) during planning. During the build it is four: two live package rows, a
blank row and the status line. It shrinks again as packages settle. In B1 the
region was erased at heights 2, 4 and 3, 37, 164 and 1 times. Each change of
height moves the status line.

**F4. The dependencies are folded.** Revision 2 (R4) lists the requested
packages and folds every dependency into one line:
`Compiling 21 dependencies done 8.10s · 13 cached`. The line does not say
which dependencies compiled, or which the cache supplied.

2026.9.29.4 printed a line for the root and for each of its 12 direct
dependencies, as `Compiling` or as `Cached … (N units)`. It printed these
lines whether or not the dependency compiled, and it never named transitive
dependencies.

**F5. The dependency line is late, out of order, and states a fact that
belongs to the plan.** A dependency served by the global cache receives its
units through `stage_file` steps. These run in a ninja pass of their own
(`ninja_backend.cppm:4214`), which runs with `--quiet` and without the
progress model, so the steps are never counted. Under revision 2's
completion rule (§3.2), a package is complete only when all its steps have
run. A cache-served dependency therefore never completes, and the folded line
waits for ninja to exit. The consequences in the three builds:

- B1: the dependencies' live row, `Compiling 9 dependencies 97 steps`,
  stopped changing at 28.1 s and stayed on the screen until 55.1 s. The
  root's final line was written before the dependencies' line.
- The report: `Compiling xlings (.)` precedes `Compiling 21 dependencies`,
  although the root cannot finish before its dependencies.
- B2: no dependency did anything, yet `Compiling 13 dependencies cached` was
  written. This is a fact of the plan, stated as an event of the build, which
  contradicts R7.

**F6. The phase is wrong after the build programs.** `programs_done()` does
not return the phase to planning. In B1 the status line read
`Running build programs · 0:04` through `0:17`, 13 s after the only program
finished, while the plan continued.

**F7. The subjects are inconsistent.**

- The root reads `xlings (.)` without a version, because a workspace member's
  subject is its directory. A single package reads `bad v0.1.0 (.)`.
- A path dependency is named by the consumer's key: `cancellation (path)`,
  for the package that declares itself `xlings.cancellation` and lives in
  `modules/cancellation`.
- `build.mcpp 1 dependency ran 0.64s` reads as if `1 dependency` were a
  package.

**F8. `Finished` is long, names an object file, and differs between the two
paths.**

- The reported line is 156 columns, so it wraps at 120.
- `longest` names `obj/xlings/src/core/xself/doctor.o`. The reader edits
  `src/core/xself/doctor.cpp`.
- The fast path writes `Finished dev in 0.04s`, without the descriptor that
  the planned path writes: `Finished dev [unoptimized + debuginfo] in 9.59s`.

**F9. Nine warnings state one fact nine times.** Each warning takes two lines.
The first line is about 256 columns, because it carries the absolute manifest
path, so the nine take about 36 rows at 120 columns before the build starts.
The consumer is named `mcpplibs.xlings@path`, an identity that nobody wrote.
The hint, "write 'xlings.cancellation' in 'mcpplibs.xlings@path'", names
neither the file nor the table. Section 4 explains why the warnings appear.

**F10. Four configuration lines on every planned build.**
`Resolving toolchain`, `Resolved gcc@16.1.0 → <path>`,
`Target x86_64-linux-gnu → x86_64-unknown-linux-gnu` and
`Inferred sources [...]` are printed each time the plan runs, which is after
every edit (section 0). None of them states a change.

**F11. An adjacent defect, found during the measurement: the lock file is
rewritten.** Since 2026.9.29.1, a planned build rewrites xlings's
`mcpp.lock`:

- the entries of `[dependencies.mcpplibs]` (`cmdline`, `xpkg`) are recorded
  under their bare keys, with a different hash;
- the committed `mcpplibs.cmdline` and `mcpplibs.xpkg` entries are kept.

The lock therefore holds two entries for one identity, and the working tree
is always modified. 2026.9.28.3 leaves the file unchanged; 2026.9.29.1,
2026.9.29.4 and 2026.9.29.5 rewrite it.

## 4. Why the warnings appear

A dependency key without a namespace names the default namespace `mcpplibs`
(`docs/specs/package-identity.md` §4.2). xlings writes its path dependencies
as bare keys in three manifests:

- `mcpp.toml` lists `cancellation`, `i18n`, `json`, `platform`, `sha256`,
  `theme` and `xhttp`;
- `modules/i18n/mcpp.toml` lists `platform`;
- `modules/platform/mcpp.toml` lists `cancellation`.

The manifests at those paths declare `namespace = "xlings"`.

Since #634 (A2), the identity a path or git manifest declares is the
package's identity, whatever key reached it. mcpp therefore builds
`xlings.cancellation` and reports that the key named another identity. Since
2026.9.27.1 (#719) it reports this once per declaring edge. The build is
correct. The warning asks for the key to state the identity it reaches.

The correction in xlings is a namespace table in each of the three manifests:

```toml
[dependencies.xlings]
cancellation = { path = "modules/cancellation" }
platform     = { path = "modules/platform" }
# ... and the other five in mcpp.toml
```

It removes the warnings with every mcpp release since #634. It needs no mcpp
change and is proposed as a separate xlings pull request (section 15).

## 5. Review round 1 and the options

### 5.1 The review

The first review returned four points:

1. A path dependency inside the project should be exempt from the identity
   warning, or its identity should be settled without one.
2. The terminal writer of section 9 is accepted.
3. A line, once written, should not move or change.
4. The line format of 2026.9.29.4 was clear. Its one defect is `(path)`,
   which should state the directory relative to the project.

Point 2 is settled. Points 1, 3 and 4 admit several designs, listed below
with what each costs. Sections 6 to 16 describe the recommended
combination.

### 5.2 The identity warning (point 1)

Adoption does not depend on the warning. The resolver already records the
identity each source declares, and builds that identity whatever key reached
it (#634 A2). #634 chose adoption with a warning rather than refusal, because
nine working builds wrote such keys. The warning therefore only asks for a
spelling to change. Three designs follow from this:

- **W-a. Exempt a path dependency inside the project.** A bare key whose
  `path` lies inside the project adopts the declared identity without a
  warning. "Inside the project" means inside the root of the workspace, or of
  the package when there is no workspace. A path outside the project and a
  git dependency keep the warning, grouped as in section 8.
- **W-b. A bare key of a path or git dependency states only the short name.**
  A bare key never names a namespace for a `path` or `git` source: the source
  fixes the package, and the manifest there names its namespace. Adoption is
  then not a correction, and nothing is reported. A key that writes a
  namespace (`[dependencies.foo]`, `foo.cancellation`) that the manifest
  contradicts still warns: the author stated an identity that is false.
  Section 4.2 of `package-identity.md` gains one clause: "bare means
  `mcpplibs`" applies to selectors that an index resolves, not to a source
  the manifest line fixes. The code already compares only the short name in
  the git-member search, for the same reason.
- **W-c. Rewrite the key in the manifest automatically.** A build that edits
  the user's manifest changes files the user did not ask it to change, and
  the rewrite would be a second writer of `mcpp.toml`. Not proposed.

The recommendation is W-b. The reason W-a gives for a path inside the
project, that the source rather than the key determines the package, holds
for every path and git source. W-b states that reason as a rule and has no
boundary to define. A git dependency whose upstream renames its namespace
changes identity under W-b without a word. The lock file records the new
identity, so the change appears in the lock's diff.

### 5.3 The package lines (points 3 and 4)

All three designs use the line format of 2026.9.29.4, with two corrections.
The origin `(path)` becomes the directory relative to the project root, and a
path dependency states its version. Round 3 (section 5.8) settled the name:
the short name inside the project, the full identity outside it.

```
   Compiling xlings v2026.9.29.1 (.)
   Compiling cancellation v0.1.0 (modules/cancellation)
      Cached compat.ftxui v6.1.9 (73 units)
```

The designs differ in when a line is written and which packages have one.

- **A. The plan's list, as in 2026.9.29.4.** When the plan ends, a line is
  written for the root and each of its direct dependencies (for a workspace,
  each member's), in manifest order. The verb is `Cached` when the cache
  serves the package, and `Compiling` otherwise.
- **B. The plan's list, reduced to the packages ninja will run.** Before
  ninja starts, `ninja -n` lists the steps this build runs. This takes 10 ms
  on xlings's graph of 1,200 steps; the cache pass has no dyndep file, so its
  dry run always completes. A line is written, in graph order, for every
  package with steps in either list, including transitive dependencies. Two
  cases fall outside the dry run:
  - On a first build the dry run stops at the first dyndep file that does not
    exist yet (section 13, A). In a directory without a ninja log, every
    package with steps that the cache does not serve is listed; they all
    compile.
  - A package the dry run did not foresee (a module file no build has scanned
    yet) receives its line when its first step finishes.
- **C. A line when the work happens.** A package's line is written when its
  first step in the main pass finishes, or when its first `check` or
  `prepare` action starts. A `Cached` line is written when the cache pass
  ends, for each package whose units that pass placed. Packages with nothing
  to do have no line; `-v` names them `Fresh`.

| | A | B | C |
|---|---|---|---|
| Resemblance to 2026.9.29.4 | identical | same lines, and the whole list at the start | same lines, written during the first seconds of ninja |
| Every line true (R7) | no: an incremental build lists packages with nothing to do as `Compiling`, and `Cached` is stated when nothing was placed | nearly: every compile rule has `restat = 1`, so a dependent that the dry run lists is skipped when an edit leaves a module interface's BMI unchanged | yes, by construction |
| Transitive dependencies named | no | yes | yes |
| Lines after an edit of one root source (B2) | all 13 direct packages | the root | the root |
| Lines on a first build (B1) | 13 | 21, at the start | 21, as their first steps finish; in B1 all appeared within 1.5 s of ninja starting |
| Order | manifest order | graph order | the order in which work finishes |
| Work added | restore the old announcement, change the subject | a dry run, its parser (descriptions carry `$out`), the first-build rule, the late additions | an announcement at the first step, the cache pass reported |
| Cost per build | none | one more ninja invocation (a graph load) | none |

The recommendation is C. It is the only design in which every line is true,
and it is the simplest. On a first build it lists what A lists and the
transitive dependencies besides. After an edit it names what the build
actually compiled, which is cargo's convention. B is the choice if the whole
list must be on the screen before the first compile ends. Its residual error
is bounded (it may name a dependent that `restat` skips, never omit one), and
it needs the most machinery. A is listed because it restores the reviewed
output exactly. Revision 2 replaced A because of its first row in the table.

### 5.4 The live display (point 3)

Under every design, a line, once written, is never rewritten, recoloured or
moved. It leaves the screen only by scrolling. Two designs remain for what
changes while the build runs:

- **1. Nothing changes.** On a terminal, as in a log, a status line is
  appended after 60 s of silence, naming the phase, the counts, the clock and
  the longest-running step. Nothing on the screen is ever redrawn. The
  terminal shows no activity until the first heartbeat; that silence was the
  defect #742 set out to fix, although the heartbeat bounds it.
- **2. One status line below the output.** This is the design of sections 7
  and 9. The status line is the only row that changes. It is redrawn in place
  in one write, and it first appears after 0.5 s. When a line is written
  above it, the status line moves down one row, as the screen scrolls. With
  design A or B no line is written while ninja runs, so the status line stays
  where it is.

The recommendation is 2. It shows that the build is progressing within half
a second, and apart from the status line it keeps the screen as static as
design 1.

### 5.5 The recommended combination

W-b, C and 2. Sections 6 to 16 describe this combination; section 8 then
covers only a key that states a namespace which the declaration
contradicts. Choosing B instead of C changes section 7.1 (when a line is
written) and section 10 (a dry run before the main pass). Choosing W-a
instead of W-b keeps section 8 for paths outside the project and for git
dependencies.

### 5.6 Review round 2: the status line

The second review accepted W-b, C and 2, and asked whether the status line
could be more engaging while staying useful, uncluttered and simple.

**Constraints the answer keeps.**

- The line stays one row.
- Every element states something mcpp knows (R7), or is recognisably
  decoration that claims nothing.
- A frame is one write.
- Nothing above the line changes.
- A log (CI) receives none of it: the heartbeat line there stays plain text.

**Candidate elements.**

| Element | What it shows | Useful | Engaging | Cost | Verdict |
|---|---|---|---|---|---|
| A bar of the phase's fraction, `━━━━━╸━━━━`, the filled part coloured | `f/t` while building; programs finished / scheduled while the build programs run | yes: the fraction at a glance | yes | small | recommended |
| An indeterminate sweep: a short bright segment moving along the bar's track | the phase has no total yet (planning) | yes: planning takes 3 to 17 s in the recordings and has no count | yes: calm motion | small | recommended |
| A spinner that turns once per finished step, at most once a frame | the pace of the build: it spins fast during a burst of compiles and stops while the build waits on one long step | yes: a stopped spinner beside a ticking action clock says "one long step" | yes: the animation carries information | a counter | recommended |
| The cache's share of the bar, in a second tone | the steps the cache pass staged, beside those compiled | yes: what the cache saved | yes | small | recommended |
| Red on failure | a step failed while ninja finishes the running ones (`Stopping`) | yes | | none | recommended |
| Progress in the terminal's tab and taskbar (OSC 9;4) | the bar's fraction, red on failure, cleared at exit | yes: visible from another window | yes | small | recommended, for an allow-list of terminals |
| A sparkline of steps per second over the last 12 s, `▁▂▅▇▆▃` | the rhythm: bursts, stalls, the link at the end | partly | yes | small | optional (the "pulse" style) |
| Running jobs, `●●●●●○○○` (ninja's `%r` against the job count) | how parallel the build is at this moment | partly: shows why the tail of a build is slow | yes | `%r` added to the status format | optional (the "pulse" style) |
| An estimate of the time left | a guess: step times range from 0.01 s to 12 min in the validation project | misleading | | | not proposed (R7) |
| The terminal's window title | progress in the title | duplicates OSC 9;4; the previous title cannot be restored reliably | | | not proposed |
| A bell when a long build ends | attention | intrusive by default | | | not proposed |
| Emoji, colour cycling, scrolling text | nothing | no | | | not proposed |

**Three styles, in the demonstration.** The script
`statusline_demo.py` (kept with the recordings) plays a simulated build in
each style, on the terminal it runs in. Flags: `--fail`, `--ascii`, `--osc`,
`--block`, `--speed`.

    quiet  Building 612/707 · 0:35 · gpp.gui: CMAKE ElaWidgetTools 0:04
    bar    ⠹ Building ━━━━━━━━━━━━━━━━━━━━╸━━━ 612/707 · 0:35 · gpp.gui: CMAKE ElaWidgetTools 0:04
    pulse  ⠹ Building ━━━━━━━━━━━━━━━━━━━━╸━━━ 612/707 · 0:35 · ▁▁▂▅▇▆▅▃▂▁▁▁ 38/s · ●○○○○○○○ · gpp.gui: CMAKE …

The recommendation is **bar**, with the tab and taskbar progress. Each
element in it states a fact, and together they fit in about 60 columns
before the running step's name. The motion (the sweep, and a spinner whose
pace is the build's) shows that the build is alive and how fast it moves. The
line does not become a dashboard. Pulse adds two readings that are
interesting at first and redundant with the counts afterwards, and it
doubles the line's length, which truncates the running step's name, the
element a long build most needs.

**Details of bar.**

- The bar is 24 columns. Its head has half-cell resolution (`╸`). When
  dyndep adds steps and `t` grows, the bar shortens by that fraction. This
  states the new total and is not smoothed away.
- The spinner has ten braille frames, `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`. While mcpp itself works
  (planning, build programs) it turns once a frame. While ninja builds it
  turns once per finished step, and never more than once a frame.
- Glyphs and colours: the filled part is `━` in bright cyan, and the
  cache's share `━` in cyan. The track is `─` in dark grey, so that fill and
  track differ by weight without colour, and on a theme whose dark grey is
  the background colour (Solarized dark). After a failure the filled part
  turns red and the phase yellow.
- **Fallbacks.** Under `NO_COLOR` the glyphs alone separate fill and
  track. Where the glyphs are unavailable, the ASCII forms apply: `|/-\`
  and `[=====>    ]`. Section 5.7 states where.
- **Width.** The running step's name is truncated first, then the bar is
  dropped (below 60 columns). Phase, counts and clock are always kept.
- **Tab and taskbar progress.** The sequence is
  `ESC ] 9 ; 4 ; state ; percent ESC \`: 1 while building, 2 after a
  failure, 3 while planning (indeterminate), 0 to clear.
  - It is sent only to terminals that support it: Windows Terminal
    (`WT_SESSION`), ConEmu (`ConEmuANSI=ON`), WezTerm and Ghostty
    (`TERM_PROGRAM`). iTerm2 and kitty read `OSC 9` as a desktop
    notification, so an unconditional sequence would post notifications
    there.
  - cargo emits the same sequence for the terminals it recognises. The
    exact list is to be checked against cargo's source before
    implementation.
  - The sequence is cleared when the region closes, on success, on failure
    and on interruption through the existing signal guard.
- **One setting.** `MCPP_PROGRESS` takes three values:
  - `bar`, the default on a capable terminal;
  - `plain`, the quiet style, for a screen reader or by preference;
  - `off`, no status line: the heartbeat lines of a log, on the terminal
    too.
- **Frame rate.** At most ten frames a second, as before. A frame is written
  only when it differs from the previous one: while nothing finishes, the
  spinner stops and only the clock changes, so one frame a second is written.

### 5.7 Compatibility of the bar style

The bar style is drawn only where the landed status line is drawn: stdout
is a terminal that can move the cursor. Everywhere else (a CI log, a pipe, a
redirected file, `TERM=dumb`, Emacs' `M-x compile`, a Windows console
without virtual-terminal processing, a native program under MSYS2 mintty
without ConPTY), the output is plain lines with the heartbeat, as today.

| Terminal | Glyphs | Ambiguous-width characters | OSC 9;4 |
|---|---|---|---|
| Linux: GNOME Terminal and other VTE terminals, Konsole, xterm, Alacritty, foot | Unicode; fontconfig supplies missing glyphs | narrow by default; wide when so configured | not sent |
| WezTerm, Ghostty | Unicode | narrow by default | sent |
| kitty | Unicode | narrow | not sent: kitty reads `OSC 9` as a notification |
| macOS Terminal.app | Unicode; the system supplies missing glyphs | narrow | not sent |
| iTerm2 | Unicode | narrow by default; an option makes them wide | not sent: `OSC 9` posts a notification |
| Windows Terminal | Unicode; DirectWrite supplies missing glyphs | narrow | sent |
| Windows console host (conhost), Windows 10 1511 and later | ASCII: conhost has no glyph fallback, and Consolas, its long-standing default font, lacks the braille block | wide under code pages 932, 936, 949 and 950 | not sent |
| VS Code, JetBrains terminals | Unicode | narrow | not sent (not on the allow-list) |
| tmux, GNU screen | as the outer terminal | as the outer terminal | not sent: the multiplexer drops it, so `TMUX` or `STY` disables it even when `WT_SESSION` is inherited |
| SSH | as the local terminal | as the local terminal | as the local terminal |

**The one real hazard is the width of ambiguous characters.** `━`, `─`,
`·` and `…` are East Asian Ambiguous. `╸` and the braille spinner are
narrow. A terminal that renders ambiguous characters wide is common among
Chinese, Japanese and Korean users. conhost does so under a CJK code page,
and GNOME Terminal, iTerm2, mintty and PuTTY do so on request. In such a
terminal the 24-cell bar takes 48 columns, the row overflows, and it wraps.
`\r` then returns only to the start of the wrapped part, so every frame
leaves a row behind.

The demonstration was replayed through a terminal emulator 60 columns wide
that renders ambiguous characters wide. With autowrap on, 69 rows of stale
status fragments remained at the end. The landed status line has the same
latent defect through `·`, although it overflows only near the right
margin. Three measures follow:

1. **Autowrap is off while the status row is drawn.** Each frame writes
   `ESC[?7l`, the row, `ESC[K` and `ESC[?7h` in one write, so a row wider
   than the terminal is clipped at the margin and never wraps. In the same
   replay no stale row remained. Lines written above the status row wrap as
   usual. DECAWM is part of VT100; Windows Terminal and conhost share the VT
   parser that implements it (to be confirmed on a Windows machine).
2. **The width is budgeted conservatively under a CJK locale.** When
   `LC_ALL`, `LC_CTYPE` or `LANG` names `zh`, `ja` or `ko`, or the Windows
   console's output code page is 932, 936, 949 or 950, `fit` counts
   ambiguous characters as two columns. The row then fits whichever way the
   terminal renders them; in a terminal that renders them narrow it ends a
   little short of the margin.
3. **conhost receives ASCII.** It has no glyph fallback, and Consolas lacks
   the braille block. The glyphs are chosen from the environment: Unicode
   where `WT_SESSION`, `TERM_PROGRAM` or a UTF-8 locale on POSIX says so,
   ASCII otherwise.

The other hazards are small:

- A colour theme can hide the track. The fill and the track differ by glyph
  weight, so the bar reads without colour.
- A missing glyph renders as a box. `MCPP_PROGRESS=plain` removes every
  glyph beyond the landed line.
- The tab and taskbar sequence is sent only on the allow-list, never under a
  multiplexer, and is cleared when the region closes. A process killed with
  `SIGKILL` cannot clear it; the terminal clears it when the next program
  sets its own progress, or when the tab closes.

### 5.8 Review round 3: names, alignment, and what the bar can say

The third review asked for three things:

1. The project's own packages should be told apart by colour, and named
   without the `xlings.` prefix.
2. The status row should align with the lines above it rather than start in
   the first column.
3. A design that is inventive, compatible, and states real information.

**Names and colour.**

- A package inside the project has two properties that locate it: its
  directory, which is always shown, and the project colour. It is named by
  its short name (`platform v0.1.0 (modules/platform)`).
- Every other package keeps its full identity (`compat.ftxui v6.1.9`,
  `mcpplibs.xpkg v0.0.59`), because nothing else on its line says where it
  comes from.
- "Inside the project" is the root, the workspace members, and the path
  dependencies whose directory lies under the project root: the boundary
  that section 5.2 calls W-a.
- Two packages inside one project with one short name are told apart by
  their directories.

The colours:

- the verb keeps its colour (bold green for `Compiling` and `Cached`);
- the name of a package inside the project is cyan, the colour of the
  status row's phase;
- the name of any other package is in the default colour;
- the version, the origin and a unit count are dim.

A line therefore carries at most three styles. Under `NO_COLOR` the two
forms still differ in text: a short name followed by a directory, or a full
identity. The same subject is used wherever a package is named: in the
package lines, the build-program lines, the error line, the `-v` summary,
and the label of a running action (`gui: CMAKE ElaWidgetTools`).

```
      Cached compat.ftxui v6.1.9 (73 units)
   Compiling cancellation v0.1.0 (modules/cancellation)
   Compiling mcpplibs.xpkg v0.0.59
   Compiling xlings v2026.9.29.1 (.)
```

**Alignment.** The phase becomes a verb in the 12-column verb field, right
aligned like `Compiling`. Every phase is at most eight letters: `Planning`,
`Running`, `Building`, `Stopping`, `Checking`. In the style with a spinner,
the spinner stands in the gutter at column 2, where no verb reaches:

```
   Compiling platform v0.1.0 (modules/platform)
  ⠹ Building ━━━━━━━━━━━━━━━━━━━━╸─── 612/707 · 0:35
```

**Three tests for an element.** An element earns its place when it passes
all three:

- it states a fact (R7);
- it survives the terminals of section 5.7, or degrades to text;
- it says something the rest of the row does not.

Two elements pass that the previous rounds did not have.

- **The tail clause, `last N running`.**
  - ninja reports `%u`, the steps not yet started. Measured with ninja
    1.12.1 through a pipe (six steps of 0.1 s to 1.5 s at `-j3`), `%u`
    reached 0 when the last step started, and from then on the status lines
    counted down `t − f` running steps exactly.
  - Once `%u` is 0, the row states `last 7 running`, followed by the name
    of a known action if one is among them:
    `Building ⣿⣿…⣷ 706/707 · 0:10 · last 1 running · gui: CMAKE ElaWidgetTools 3:12`.
  - The clause answers the question a long build raises most often ("why
    is it still running at 99 %"). It needs one more placeholder in the
    status format.
- **The LED bar.**
  - The bar is made of braille cells: `⣿` for a filled cell, and `⣀` (the
    bottom row of dots) for the track, which reads as a dotted rule. The
    head cell fills dot by dot, `⣀⣄⣆⣇⣧⣷⣿`, six steps a cell.
  - At 24 cells the bar has 144 positions. A build of 707 steps therefore
    moves it every five steps, and the motion is the progress itself, so no
    spinner is needed.
  - Every braille cell is East Asian narrow, so the LED bar is immune to the
    ambiguous-width hazard of section 5.7. Only the separator `·` remains
    ambiguous, and the conservative budget covers it.
  - The cache's share is the first run of cells, in cyan; failure turns the
    filled cells red; planning shows three lit cells sweeping along the
    track.
  - Where braille is unavailable (conhost), the ASCII bar applies.

```
   Compiling i18n v0.1.0 (modules/i18n)
    Building ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆⣀⣀⣀ 600/707 · 0:06
```

Elements considered and not taken:

- **A heat trail**: each filled cell coloured by the pace at which it was
  filled, so that the finished bar shows where the build was slow. It is
  accurate, but sixteen colours give it two or three tones, too few to read.
- **One cell per package**: package completion is not known (section 13, A).
- **The last finished file**: it states the past as if it were the present
  (section 13, E).

**The two candidates** are in the demonstration (`--style bar`,
`--style led`):

| | bar | led |
|---|---|---|
| Look | a line, as in pip and rich | a dotted LED strip, as in btop |
| Motion | spinner in the gutter, turning once per finished step | the head fills dot by dot |
| Resolution | 48 positions (half cells) | 144 positions |
| Ambiguous-width terminals | needs autowrap off and the conservative budget | immune, apart from `·` |
| conhost | ASCII | ASCII |

The recommendation is **led**. It is the more compatible of the two where
compatibility is weakest, namely CJK terminals, which are a large share of
mcpp's users. Its motion carries information without a separate element,
and its look is distinctive without extra elements. The tail clause and the
tab and taskbar progress apply to either candidate.

### 5.9 Review round 4: what the dots can carry

The fourth review asked three questions about the LED bar:

- whether each dot can be controlled;
- whether the dots can be coloured by time spent or by library;
- whether the dots can carry simple animation.

**What can be controlled.**

- A braille cell is a matrix of 2 × 4 dots, each on or off (U+2800 plus
  eight bits, 256 patterns), so every dot is addressable. At 24 cells the bar
  is 48 columns of four dots.
- Colour applies to the whole cell: the eight dots of a cell share one
  foreground colour. A background colour fills the cell's rectangle rather
  than its dots, so it would draw blocks and is not used.
- The finest unit of colour is therefore a cell, two columns wide. The
  finest unit of shape is a dot.

**Colour by library.** One hue per package was considered and rejected:

- a build has twenty or more packages;
- the sixteen-colour palette offers about four usable hues once red and
  yellow are reserved for failure and warning, and blue is too dark on a
  dark background;
- 256 colours are not available everywhere (conhost, many remote sessions);
- a reader would need a legend;
- colour-blind readers lose the distinction.

What the palette can carry is three kinds, the same three the package lines
use:

- the cache (grey);
- a dependency (the default colour);
- the project (cyan).

Each filled cell takes the kind of the steps that filled it. The bar then
says how much of this build was the project's own code, how much the
dependencies', and how much the cache spared. The names above the bar
already carry the same colours, so they serve as its legend.

**Time spent.** Colour cannot carry both kind and cost, since a cell has one
colour, but the height of a column can carry cost. In the `cost` form, a
filled column is as tall as its slowest step: two dots under 0.5 s, three
under 5 s, four otherwise. The measured facts are the durations that
ninja's log already gives the model.

- The staging of 480 units from the cache lies flat, heavy units stand up,
  and the long action at the end is a full column.
- The heights read without colour, so under `NO_COLOR` and for colour-blind
  readers too.
- The axis is steps, not time. A 12-minute action occupies one column of
  48; the height marks it, and its duration is stated by the tail clause.
- The filled part is no longer solid, so the fraction is read at the head
  and at the colour change to the grey track.

**Animation.** Every motion is driven by an event, or states that no total
is known:

| Motion | Driven by | Verdict |
|---|---|---|
| The head fills dot by dot (six steps a cell) | finished steps | kept |
| Sand: in each frame in which steps finished, one dot drops from the top row of the head cell and falls a row per frame onto the fill | finished steps: a steady fall means a busy build, none means a wait | proposed |
| Three lit cells sweep along the track | planning, which has no total | kept |
| The cell where the first failure landed turns red and stays red while the build stops | the failure | proposed |
| A shimmer running along the bar | nothing | not proposed: it claims activity that may not exist |
| A flourish when the bar is full | nothing, and `Finished` would have to wait for it | not proposed |

**The four forms in the demonstration.** `dots_demo.py` plays each form,
and `--fail` adds a failure. Flags: `--style solid|kind|cost|sand`,
`--fail`, `--speed`.

    solid  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆⣀⣀⣀   round 3: one colour, cache share in a second tone
    kind   ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆⣀⣀⣀   each cell coloured cache / dependency / project
    cost   ⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣶⣶⣷⣷⣷⣾⣿⣆   height by the slowest step, colour by kind
    sand   ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣇⣁⣀⣀   solid, with a grain falling into the head

The recommendation is kind, with sand and the failure mark. It stays one
calm strip. Its colours repeat the package lines, so it needs no legend.
Its only motion beyond the head is one grain in one cell, and that motion
is the build's pace. Cost is the alternative if the shape of the build's
cost is wanted on the bar. It reads without colour, at the price of a
textured fill.

All forms use braille cells only, which are East Asian narrow, and four
colours of the sixteen-colour palette: grey (90), default (39), cyan (96)
and red (31). conhost receives the ASCII bar. The data they need is already
in the model: the kind of each step (the step record's package, and the
cache pass), and its duration (ninja's log).

### 5.10 Review round 5: colour by source, and the dots as a display

The fifth review asked for two changes:

1. The name's colour should state where the package comes from: the
   official index, a path, a git repository, or another index. A package
   inside the project should be plain.
2. The dot area need not be a bar filling from left to right. Since the
   counts state the progress, the area could play simple pixel animations:
   a few built in, one chosen at random.

**Colour by source.** This replaces the project-cyan rule of section 5.8.

| Source | Name | Origin in parentheses | Colour of the name |
|---|---|---|---|
| inside the project (root, members, path dependencies under the root) | short name | the directory: `(modules/platform)`, `(.)` | the default colour |
| the official index (the default index: `mcpplibs.*`, `compat.*`) | full identity | none | cyan |
| another index (a project `[indices]` entry) | full identity | `(index acme)` | magenta |
| a git repository | full identity | `(git tag v0.3.0)`, `(git rev 1a2b3c4d5e6f)` | blue |
| a path outside the project | full identity | `(../util)` | the default colour |

- The version and the origin stay dim, and the verb keeps its colour.
- Every source is also distinct in text, by its origin, so the scheme reads
  under `NO_COLOR`.
- Four colours are used: default, cyan, magenta and blue (the bright blue,
  94, which reads on dark and light backgrounds). Red and yellow stay
  reserved for failure and warning.
- `compat.*` and `mcpplibs.*` share the official colour, because both come
  from the official index. A fifth colour for upstream ports was considered
  and not proposed: it would separate two namespaces of one source.
- The index a package came from is known to the plan: the index route that
  resolved its namespace.

**The dots as a display.** The display is 48 × 4 dots. Four dots of height
rule out text: a 3 × 4 font is unreadable. It fits small sprites, a trace,
or a cellular automaton. The forms considered:

| Form | Fits 48 × 4 | Verdict |
|---|---|---|
| Pac-Man eating a row of pellets | yes: 4 × 4 sprites | built in |
| An ECG trace | yes: a trace is one dot per column | built in |
| Pong, self-playing | yes: 2-dot paddles, a 1-dot ball | built in |
| Conway's Game of Life on a 48 × 4 torus | yes: a glider needs 3 × 3 | built in |
| Snake | yes | built in |
| Breakout, Dino runner, Tetris lying on its side | yes, with more rules to write | not in the first set |
| Space Invaders | no: its sprites need 8 rows | not proposed |
| Elementary automata (Rule 30, Rule 110) | a column of four cells has 16 states, so it cycles within seconds | not proposed |
| Scrolling text | no: unreadable at four dots | not proposed |

**One rule keeps the animations honest.** Time moves an animation slowly,
which shows that mcpp is alive. Finished steps move it further, which shows
that the build is busy. A failure changes it. A playful form therefore still
carries the build's pulse, and a wait looks like a wait: the counts and the
tail clause then say why.

| Animation | Time moves | Finished steps move | A failure |
|---|---|---|---|
| pacman | a slow chomp | a chomp; Pac-Man's position is the fraction, and the pellets ahead of him are the work left | the ghost catches him; he turns red |
| heartbeat | the trace scrolls one column a frame; flat without work | a beat when steps finished since the last one; the busier the build, the faster the heart (up to one beat a second), and a large batch beats tall | the trace turns red |
| pong | the ball moves slowly | the ball speeds up with the pace | the ball stops, red |
| life | a generation every 0.6 s | a generation a frame; the world is seeded from the project's name and reseeded when it stills or cycles | it freezes, red |
| snake | it moves slowly | it moves faster; it grows with the fraction | it stops, red |
| bar | (the LED bar of round 4, each cell coloured by source) | fills | the failure's cell turns red |

**Choosing an animation.**

- One animation is chosen at random per command and kept until
  `Finished`, since switching during a build would be noise.
- `MCPP_PROGRESS` names one (`pacman`, `heartbeat`, `pong`, `life`,
  `snake`), or chooses `bar`, `plain` (text only), or `off` (no status row;
  the heartbeat lines of a log).
- A default chosen per day instead of per command was considered: it is
  less surprising, and less playful.

**Structure.**

- A dot canvas (48 × 4, one colour per lit dot) is rendered to braille with
  one colour per cell: a sprite's colour where the cell holds one, the
  background colour otherwise.
- An animation implements `update(elapsed, finished, fraction, failed)` and
  `draw(canvas)`. Each is 30 to 60 lines, in a small module beside `mcpp.ui`,
  with no dependency.
- Given a seed and a sequence of events, an animation is deterministic. Its
  frames can therefore be compared with recorded ones in a unit test, and
  the random choice is seeded in tests.
- The status row's budget is unchanged: 24 cells, one write per frame, at
  most ten frames a second.

**Compatibility.** The display uses braille cells (East Asian narrow) and
the sixteen-colour palette only. Under `NO_COLOR` the animations keep their
shapes. Where braille is unavailable (conhost) the display is replaced by
the ASCII bar, and in a log there is no display.

**The demonstration.** `anim_demo.py` plays each form after a build's
package lines coloured by source (`--anim NAME`, `--random`, `--fail`,
`--speed`).

### 5.11 Review rounds 6 and 7: a sign, the classics, and a mascot

The sixth and seventh reviews proposed three things:

- the display could spell `MC++`, or the project's name, lighting up from
  left to right;
- the display could play animations that people recognise at a glance, even
  blurred;
- mcpp could have a mascot, as Chrome has its runner, and the mascot could
  be the progress animation.

The review also judged the first animations: the chomper was liked but left
the left side empty; the snake needed work; the rally should become an
emitter that deposits the progress; and Tetris lying on its side was asked
for.

**The sign.**

- A four-dot font (letters, digits, `+` and `-`; `M`, `N` and `W` five dots
  wide, the others three) draws `MC++` at double width in 34 of the 48
  columns.
- The part the build has reached is lit. The rest shows as a dim outline,
  so the whole word is visible from the start and fills in.
- A lit column at the frontier, the print head, is steady while steps
  finish and blinks while the build waits.
- The project's name is used instead when it fits: at double width (up to
  about six letters: `XLINGS` takes exactly 48 columns), otherwise at single
  width (up to about ten). Otherwise, and for a name outside the font, the
  sign falls back to `MC++`.

**The classics.** Ranked by how well they survive four dots:

- the chomper and its ghosts;
- a light sweeping to and fro with a trail (the scanner of a 1980s
  television car);
- an ECG trace;
- three blocks gliding along a track (a well-known operating system's boot
  screen);
- the snake;
- the rally;
- Tetris on its side;
- a runner jumping cacti.

Game of Life reads only to those who know it. In the product the
animations carry generic names (`chomp`, `scanner`, `pulse`, `glide`,
`snake`, `rally`, `stack`, `runner`, `life`), since several of the
originals are trademarks.

**The revised four.**

- **chomp**
  - The chomper stays at column 16 and the maze scrolls past it, so both
    sides of the display are occupied.
  - Three ghosts chase from behind.
  - The pellets ahead are the work left: the stream is three widths long and
    ends at completion, so its end comes into view in the last fifth of the
    build.
  - A package's first step drops a power pellet ahead; eating it turns the
    ghosts blue for two seconds.
  - A failure: the ghosts close in.
- **snake**
  - The snake follows the shortest free path (breadth-first search on a
    48 × 4 grid that wraps horizontally) to its food, instead of wandering.
  - A package's first step drops a golden food.
  - The snake grows with the fraction. Its head is bright, and the last
    third of the body dims.
- **ions** (the rally, reworked)
  - An emitter on the right edge scans the four rows and fires one to three
    ions for each frame in which steps finished.
  - The ions fly left and vanish at the deposit.
  - The deposit is the progress bar. Its columns fill from the bottom, and
    each dot is coloured by the source whose work it records: the cache
    grey, the official index cyan, the project white. The deposit follows
    the fraction exactly; the ions only show the pace.
- **stack** (Tetris on its side)
  - Pieces fall leftward and rest against the stack. Each piece is placed,
    over its rotations and rows, where it leaves the fewest holes.
  - At most three pieces fly at once, and a burst of progress (the cache
    pass) settles its pieces at once, so the stack's area follows the
    fraction within four pieces.

**A mascot.** A mascot works at this size when its silhouette survives four
dots, it tells the tool's story, and it is distinct from its neighbours.

| Candidate | Story | At four dots | Neighbours |
|---|---|---|---|
| beaver | the builder: carries logs (modules) and builds a dam (the build) | the flat tail makes the silhouette | DBeaver, a database client; no build tool |
| ants | the parallel workers: the number of ants can be the number of running steps | a column of small dots in motion | Apache Ant, a Java build tool |
| weaver bird | weaves pieces into a nest, as linking joins units | a bird in flight | none known |
| pangolin | a body of scales (modules) that curls into one ball; native to China, where much of the community is | a scaled arc | none known |

The prototype is the beaver:

- the dam on the left is the progress, drawn as logs laid in courses;
- the pile on the right is the work left, and shrinks;
- a pond lies behind the dam;
- the beaver carries a log from the pile to the dam for each stretch of
  finished steps, faster when the build is busy, and slaps its tail on the
  water when a package starts;
- a failure opens a red breach in the dam.

The sign and the mascot combine: the beaver's dam can be laid in the shape
of `MC++`, so the finished build shows the logo built of logs. A mascot
also needs a full-size drawing for the README, the documentation and an
icon. That is design work of its own, and the four-dot sprite is its
smallest rendition.

**Demonstrations.**

- `logo_demo.py`: the sign, the project's name, the scanner, the glide, the
  runner and the first stack.
- `play_demo.py`: the revised chomp, snake, ions and stack, and the beaver.
  Flags: `--anim NAME`, `--fail`, `--speed`.

### 5.12 Review round 8: the first set, a playable stack, and the sign withdrawn

The eighth review settled or asked about five things:

- the revised chomp is harder to read than the first;
- the snake is liked, and its food and body should carry the colours of real
  modules;
- the stack is liked, and the review asked whether it could be a real game
  steered with the arrow keys;
- the runner is liked;
- the `MC++` sign reads oddly.

**Chomp** returns to the first design, in which the chomper's position is
the fraction, pellets lie ahead and one ghost follows. It gains one addition:
a faint corridor along the top and bottom rows, so the part already eaten is
not blank (`--anim chomp1`).

**Snake.**

- A package's first step drops a food in the colour of the package's source
  (section 5.10), and the snake takes the foods in the order the packages
  started.
- The segments that grow after a meal keep that meal's colour, so the body
  is a coloured record of the packages the build reached. The head stays
  bright.
- The grey food between meals keeps the snake moving at the build's pace.

**A playable stack.** It can be built, and it would be the first input mcpp
reads while a build runs:

- the ticker thread reads keys in a non-canonical terminal mode (POSIX
  `termios` without `ICANON` and `ECHO`, `ISIG` kept so that Ctrl-C still
  stops the build; the console input mode on Windows);
- up and down move the piece between the four rows, left drops it, space
  turns it; a filled column clears, as a filled row does in the original;
- the score is stated after `Finished`.

Its costs decide its place:

- **Type-ahead.** Keys typed ahead while a build runs, usually the next
  command, would be taken by the game. No build tool reads its terminal
  during a build, for this reason.
- **The terminal mode.** It must be restored on every exit: success,
  failure, Ctrl-C, termination. A process killed outright leaves the shell
  without echo until `stty sane`.
- **Competing readers.** A child that reads the terminal (a prepare action's
  installer) would compete with the game for keys.
- **What the stack means.** In play the stack no longer shows progress; the
  counts do.

It is therefore proposed only as an explicit opt-in (`MCPP_PROGRESS=play`),
never a default, and after the display itself has shipped.

**The sign is withdrawn.** A status row is one text row, and braille gives
it four dots of height, the most any character offers. The smallest legible
pixel fonts need five rows (3 × 5), and at four `M`, `C` and `+` become
ambiguous. No arrangement of the letters fixes that. A mark for mcpp at this
size is better carried by a mascot's silhouette (section 5.11) than by
letters.

**The first set,** from the reviews: chomp (the first design, with the
corridor), snake (coloured by source), stack (automatic), runner, and the
beaver if the mascot is adopted, chosen at random per command. The LED bar
and a plain text row remain available through `MCPP_PROGRESS`.

### 5.13 Review round 9: the four animations, and where they live

The ninth review fixed the set:

- the chomper exactly as first designed (its position is the fraction, the
  pellets ahead are the work left, one ghost follows);
- the snake coloured by source;
- the stack (Tetris on its side);
- the ion emitter.

The command chooses one at random. The runner and the LED bar are not built
in.

The screen and its animations form one module in a directory of their own,
`src/ui/dots_screen/`:

- `mcpp.ui.dots_screen:core` holds the 48 × 4 screen (`Screen`), its colours,
  the sources, the input an animation receives, and the `Animation`
  interface;
- one partition per animation (`:chomp`, `:snake`, `:stack`, `:ions`);
- the primary unit `mcpp.ui.dots_screen` knows them by name.

The progress model imports only the primary unit. An animation is pure, so
its frames are compared in unit tests from a seed and a sequence of inputs.

### 5.14 Review round 9: `--play-game`

The review asked whether the snake, the stack and the runner could be
steered with the arrow keys, turned on by a `--play-game` option, and run at
a game's own speed.

They can. The option removes the objection of section 5.12: a user who asks
for a game has chosen to have the keys read, so keys typed ahead are no
longer taken by surprise. The design:

**Games.**

| Game | Keys | Rules on 48 × 4 |
|---|---|---|
| snake | arrows steer | the food of section 5.12, coloured by the source of each package that starts; hitting the body ends the round, and a new one starts at once |
| stack | up and down move the piece between the four rows, left drops it, space turns it | a filled column clears; a stack that reaches the right edge ends the round |
| runner | space or up jumps | cacti come from the right; a collision ends the round |

- `--play-game` chooses one of the three at random; `--play-game=snake`
  names one.
- The option is accepted by `build`, `run` and `test`.
- A game runs at a fixed speed of its own (the snake eight cells a second,
  the stack's pieces two cells a second, faster as columns clear), not at the
  build's pace. The counts and the clock beside the screen state the build.

**The row.** The status row reads, for example,
`    Building ⣿…⣀ 612/707 · 0:35 · snake 12`: the score follows the clock. When
the build ends the game ends, and a line after `Finished` states the round's
best score.

**The terminal.** Keys are read by the region's thread from the controlling
terminal:

- **POSIX**: the terminal's mode loses `ICANON` and `ECHO` and keeps `ISIG`,
  so Ctrl-C still stops the build. Reads do not block (`VMIN` 0, `VTIME` 0).
  The arrows arrive as `ESC [ A` to `ESC [ D` (or `ESC O A` in application
  mode).
- **Windows**: the console input mode loses `ENABLE_LINE_INPUT` and
  `ENABLE_ECHO_INPUT`, and `ReadConsoleInputW` yields key events (`VK_UP`
  and the others).
- **Restoring the mode**: it is restored when the region closes, at normal
  exit, on failure, and in the handler of SIGINT, SIGTERM and SIGHUP, which
  then re-raises. A process killed outright cannot restore it; the option's
  help says so, and names `stty sane`.
- **No competing readers**: ninja already gives the commands it runs
  `/dev/null` for input; the build programs mcpp runs receive a null input
  while a game is on.

**Where it is off.** The game is off when standard input or standard output
is not a terminal, under `--quiet`, and where the screen is off
(`MCPP_PROGRESS=plain` or `off`, or no braille). A note says why.

**Tests.**

- A game is pure given its seed, its key sequence and its clock, so rounds
  are replayed in unit tests.
- An e2e through a pseudo-terminal writes arrow keys to the terminal's
  master side and checks that the snake turned, that the terminal's mode
  after the command equals its mode before, and that Ctrl-C during a game
  leaves the mode restored.

## 6. Requirements, revised

| Revision 2 | Revision 3 |
|---|---|
| R1 each step's line carries its state, updated in place on a terminal | **R1'** A package is named once, when it does work in this build: `Compiling` when the first of its steps finishes, `Cached … (N units)` when the cache pass places its units (design C of section 5.3). A line, once written, is never rewritten, recoloured or moved; it leaves the screen only by scrolling. |
| R2 one status line, separated from the step lines by a blank line | **R2'** The live display is one status line, directly below the output, redrawn in place (design 2 of section 5.4). There are no live rows and no blank row. Download bars remain above it while they run. |
| R3 `Finished` after a blank line, with the total and its parts | kept. The parts are stated from 60 s, and `longest` names the source file (section 7.3). |
| R4 dependencies folded into one line | withdrawn: R1' names every package that does work. |
| R5 `-v` shows everything | kept, and extended by one summary line per package at the end (section 7.4). |
| R6 `done`, `ran`, `cached`, `waiting`, `failed`, `fresh` | `ran`, `cached` and `failed` state a build program's outcome. `waiting`, `compiling` and `running` appear in the status line. `fresh` appears in `-v`. |
| R7 no line states something mcpp does not know | kept. F5 violates it, and R1' is exact by construction. |
| | **R8** A frame reaches the terminal in one write, and no screen between two frames shows the region partly drawn. |
| | **R9** A bare key of a `path` or `git` dependency states only the short name, and its adoption of the declared identity is not reported (W-b of section 5.2). A key that states a namespace the declaration contradicts is reported once per file, with the file named relative to the project and a hint written in the file's syntax. |

R1' follows the convention of cargo: a unit is named when it is compiled, and
units with nothing to do are not named. It needs no knowledge of which steps
ninja will run. Revision 2 needed that knowledge and could not obtain it
(its §3.1).

## 7. The output

### 7.1 Lines

| Verb | Subject | Written when | Example |
|---|---|---|---|
| `Compiling` | the package | its first step in the main ninja pass finishes, or its first `check` or `prepare` action starts | `   Compiling platform v0.1.0 (modules/platform)` |
| `Cached` | the package, with the number of units placed | the cache pass ends, for each package whose units it placed; sorted by name | `      Cached compat.ftxui v6.1.9 (73 units)` |
| `build.mcpp` | the package | the program ran or failed | `  build.mcpp mcpplibs.xpkg v0.0.59   ran 0.71s` |
| `Downloading` | unchanged | unchanged | unchanged |

- **The subject** has two forms (section 5.8).
  - A package inside the project is named by its short name, its version and
    its directory relative to the project root, in the default colour:
    `platform v0.1.0 (modules/platform)`, `xlings v2026.9.29.1 (.)`.
    Inside the project are the root, the workspace members, and the path
    dependencies whose directory lies under the project root.
  - Every other package is named by its full identity and version, and its
    name is coloured by its source (section 5.10). A package from another
    index adds the index, a git dependency its reference, and a path outside
    the project its relative directory: `compat.ftxui v6.1.9`,
    `acme.fmt v11.0.2 (index acme)`, `acme.fw v1.2.0 (git tag v1.2.0)`,
    `util v0.2.0 (../util)`.
  - The version, the origin and a unit count are dim. This replaces the
    three forms of F7.
- **The standard library module** is named `Compiling std` when it compiles.
  It is not named when it is staged, because every first build of every
  project would then name it.
- **A build program** whose result is reused (`cached`) did no work and is
  listed only with `-v`. The programs block keeps its state column, since
  mcpp knows each program's outcome exactly.
- **A failed step** is followed by `error: build failed in <subject>` and the
  step's diagnostic. The prefix `error: build failed` is kept.

### 7.2 The status line

    <phase, right-aligned in 12 columns> <bar> f/t · <elapsed>[ · last N running][ · <current>]

The phase is aligned with the verbs above it. The bar, its fallbacks, the
tail clause `last N running`, the tab and taskbar progress and the setting
`MCPP_PROGRESS` are those of section 5.8. The quiet style is this line
without the bar.

- The phases are verbs of at most eight letters: `Planning`, `Running`
  (the build programs; the bar counts programs), `Building`, `Stopping` and
  `Checking`. `Planning` replaces `Resolving`, which also opened the line
  `Resolving toolchain`, and matches the `plan` part of `Finished`. The
  phase returns to `Planning` when the build programs end (F6).
- `<current>` is the longest-running step that mcpp knows to be running: a
  build program with its state (`mcpplibs.xpkg compiling 0:02`), or a `check`
  or `prepare` action (`gpp.gui: CMAKE ElaWidgetTools 6:10`). It is omitted
  when no such step runs. The package of the step that finished last is not
  shown (section 13, E).
- The line is first drawn when the command has run for 0.5 s. A command that
  ends sooner never shows it, and the lines written in its first half-second
  do not move it (F2).
- The cache pass counts in `Building f/t` like the main pass: its stage
  steps are part of the build (F5).

### 7.3 `Finished`

- Below 60 s: `Finished <profile> [<descriptor>] in <total>`.
- From 60 s: the parts and `longest`, as in revision 2. For a compile step,
  `longest` names the step's source file (`xlings: src/core/xself/doctor.cpp`),
  which the step record now carries (section 10).
- The fast path states the descriptor. The step record's header carries it,
  and the fast path reads only that line (F8).

### 7.4 Verbosity

`-v` adds four things:

- `Fresh <subject>` for each package with nothing to do;
- the reused build programs (`cached`);
- each ninja step as `[f/t] <command>`;
- before `Finished`, one line per package that did work:
  `    Compiled xlings v2026.9.29.1 (.) · 610 steps · 37.09s`.

The span in the last line runs from the package's first step to its last,
read from ninja's log. This is the `done <span>` of revision 2, stated when it
is exact: when ninja has exited.

### 7.5 Examples

The examples use the recordings' names and times. The order of the
`Compiling` lines is the order in which each package's first step finished,
which varies between runs.

The first build of the xlings tree (B1), under W-b (so the xlings manifests
need no correction) and with F10 applied:

```
   Workspace building member 'xlings'
  build.mcpp mcpplibs.xpkg v0.0.59   ran 0.71s
      Cached compat.bzip2 v1.0.8 (7 units)
      Cached compat.ftxui v6.1.9 (73 units)
      Cached compat.libarchive v3.8.7 (127 units)
      Cached compat.lua v5.4.7 (32 units)
      Cached compat.lz4 v1.10.0 (5 units)
      Cached compat.mbedtls v3.6.1 (108 units)
      Cached compat.xz v5.8.3 (74 units)
      Cached compat.zlib v1.3.2 (15 units)
      Cached compat.zstd v1.5.7 (26 units)
      Cached mcpplibs.capi.lua v0.0.3 (2 units)
      Cached mcpplibs.cmdline v0.0.2 (3 units)
      Cached mcpplibs.tinyhttps v0.2.9 (8 units)
   Compiling cancellation v0.1.0 (modules/cancellation)
   Compiling json v0.1.0 (modules/json)
   Compiling sha256 v0.1.0 (modules/sha256)
   Compiling theme v0.1.0 (modules/theme)
   Compiling platform v0.1.0 (modules/platform)
   Compiling i18n v0.1.0 (modules/i18n)
   Compiling xhttp v0.1.0 (modules/tinyhttps)
   Compiling mcpplibs.xpkg v0.0.59
   Compiling xlings v2026.9.29.1 (.)

    Finished dev [unoptimized + debuginfo] in 55.38s
```

On a terminal during that build, the status line is the last row:

```
   Compiling platform v0.1.0 (modules/platform)
   Compiling i18n v0.1.0 (modules/i18n)
    Building ⣿⣿⣿⣿⣿⣿⣿⣿⣷⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀ 247/707 · 0:19
```

After an edit of one source file of the root (B2):

```
   Workspace building member 'xlings'
   Compiling xlings v2026.9.29.1 (.)

    Finished dev [unoptimized + debuginfo] in 9.59s
```

After an edit of a module of `modules/platform` (B3):

```
   Workspace building member 'xlings'
   Compiling platform v0.1.0 (modules/platform)
   Compiling xlings v2026.9.29.1 (.)

    Finished dev [unoptimized + debuginfo] in 18.94s
```

A failed build:

```
   Compiling bad v0.1.0 (.)
error: build failed in bad v0.1.0 (.)
src/main.cpp:1:2: error: #error SV_MARKER
```

A long build (the validation project's cross-verification of #742):

```
    Finished … in 19m07s · plan 1m48s · programs 29.45s · build 16m49s · longest gpp.gui: CMAKE ElaWidgetTools 12m46s
```

## 8. The identity warning

Under W-b (section 5.2) the xlings manifests of the report produce no
warning: every key there is bare. What remains reported is a key that
states a namespace which the manifest at its source contradicts. For
example, `[dependencies.acme] cancellation = { path = "modules/cancellation" }`
reaches a manifest that declares `xlings`. One warning is written per
consumer manifest. It names the file relative to the project root and every
key of the file that the rule concerns, and its hint is written in TOML:

```
warning: mcpp.toml names 2 dependencies in namespace acme, and the manifests they reach declare xlings; the declared identity is used: cancellation, platform
  hint: write them in a [dependencies.xlings] table, for example `cancellation = { path = "modules/cancellation" }`
```

Under W-a the same form also applies to bare keys whose path lies outside the
project, and to bare keys of git dependencies. The xlings manifests produce
no warning under W-a either, since all their paths lie inside the project.

- The resolver collects the adoptions while it walks the graph and reports
  them once, after resolution. It groups them by consumer manifest and by
  declared namespace. A consumer with keys in two declared namespaces
  receives two warnings.
- The terminal rendering and the machine record are one record per group.
  The record's `what` names the keys, so a machine reader still receives
  every key.
- The once-per-process rule of `mcpp.diag` still applies, so a `--workspace`
  build states each group once.

## 9. The terminal writer

### 9.1 One write per frame (R8)

The renderer composes the whole byte sequence of a frame before it writes
anything. A frame consists of the lines leaving above the region and the
region's new rows. The renderer flushes stdio, then writes the sequence with
one call: `write(2)`, repeated only on a partial write or `EINTR`, on a
POSIX terminal, and `WriteConsoleW` on a Windows console. Nothing in the
frame passes through the line-buffered `stdout`.

### 9.2 Overwrite instead of erase

A frame first moves the cursor to the region's first row (`\r`, then
`ESC[{n-1}A` when the region has more than one row). It then writes each
leaving line and each new row, each followed by `ESC[K`, with rows separated
by `\n`. It ends with `ESC[J`, which clears the rows of a larger previous
region. No row is erased before it is written, so no screen between two
frames shows the region blank, even when the terminal paints in the middle
of a write.

### 9.3 Standard error

When standard error is the same terminal as standard output, a line written
to standard error is part of the stdout frame. On POSIX this is decided by
`isatty` on both descriptors and equal `st_rdev`; on Windows, by both handles
being console handles. Otherwise the line is written to standard error alone
and the region is not touched: the line does not reach the screen that holds
the region.

### 9.4 Fewer frames

A frame is written only when its bytes differ from the previous frame's. The
rate stays at ten frames a second at most. The clock changes once a second,
so a build that makes no progress writes one frame a second, and that frame
rewrites one row.

### 9.5 What is removed

The live package rows, the `… N more` row, the blank separator row, and the
height calculation `max_live_lines` are removed. The region holds the
download bars and the status line.

### 9.6 Platforms

The sequences used are `\r`, `\n`, `ESC[nA`, `ESC[K` and `ESC[J`. xterm,
VTE, iTerm2, Terminal.app and Windows Terminal support them, as does the
Windows console once virtual-terminal processing is enabled. Where
`can_move_cursor` answers no (`TERM=dumb`, a console without
virtual-terminal processing, a pipe), the log medium applies and is
unchanged. It writes lines as they become final, and a heartbeat after 60 s
of silence.

## 10. The progress model

- **Removed**: the folded lines (`dependencies_line`,
  `folded_programs_line`), the completion rule as the trigger of a line
  (`complete`, `dependencies_complete`, the commit loop of `settle`),
  `package_column`, and the frame's live rows.
- **Added**:
  - A package is announced at its first finished step. This replaces
    `settle`, and is one set lookup per finished step.
  - The cache pass runs through `run_ninja_reporting` as the build's first
    pass (`pass_begin`/`pass_end` already sum passes). Its per-package counts
    give the `Cached` lines when the pass ends.
  - `programs_done()` returns the phase to `Planning`.
- **The step record becomes version 2.** Its header carries the profile
  descriptor. An `S` line gives the source file of each compile step. A
  version 1 record makes the fast path decline once, so the first build after
  the upgrade plans and writes a version 2 record. Without this, the fast
  path would name no packages until the next plan.
- **Unchanged**: the log reader and its recompaction handling, the
  attribution in the generator, the action-start file, and the heartbeat.

## 11. Compatibility

- **Human output.** The package lines, the status line and `Finished` change
  as stated. The prefix `error: build failed` is kept. Three kinds of
  consumer read these lines:
  - the e2e tests that read the lines of revision 2: 842, 843 and those
    listed in the #742 commit;
  - mcpp-plugins' `.github/scripts`;
  - the validation project's workflow.

  Each is checked before the pull request is opened.
- **Machine output.** Unchanged, except that the identity warnings are
  grouped (section 8).
- **Upgrade.** A build directory with a version 1 record is planned once
  (section 10). No configuration is involved.
- **Downgrade.** An older mcpp reads a version 2 record as absent: its fast
  path names no packages until its next plan.

## 12. Not addressed here

- **Planning after every edit.** Every source edit takes the planned path by
  the rule of mcpp#225. This cost 3.06 s of the reported 33.63 s, and more in
  a larger workspace. It deserves its own design.
- **The duplicated lock entries (F11).** The fix belongs in the resolver's
  lock writer: one entry per identity, keyed by the qualified name. It is
  proposed as a separate commit of the same pull request, with an e2e in
  which a second planned build leaves `mcpp.lock` byte-identical.
- **A terminal resized during a build.** Rows drawn at the old width may
  wrap. The next frame's `ESC[J` clears what lies below the cursor, but not
  a wrapped row above it. cargo shares this limitation. It is recorded, not
  solved.

## 13. Alternatives considered

- **A. Keep the outcome on each package line, and learn completion from a
  dry run.** `ninja -n` takes 10 ms on xlings's graph of 1,200 steps and
  lists the steps a build will run. On an empty build directory, however, it
  stops at the first dyndep file that does not yet exist (measured: at step
  730 of 1,200 with
  `loading 'obj/xlings_xhttp/src/tinyhttps.cppm.ddi.dd': No such file or
  directory`). Every first build is such a case, and the first build is the
  one a user watches. Any fallback would state completion early or late
  (R7). Not taken; the exact spans move to `-v` (section 7.4).
- **B. Keep the live rows and repair only the writer.** This removes F1 but
  keeps F3 (the region changes height) and F5 (the late dependency line).
- **C. Synchronised output (DEC private mode 2026).** It is unnecessary once
  a frame is one write that overwrites in place (section 9). Terminals that
  do not know the mode ignore it, so it can be added later without risk.
- **D. Keep the fold, and list the names in the folded line.** The line
  would be truncated to the terminal's width, and would still be late (F5).
- **E. Show the package of the step that finished last in the status line.**
  A reader takes it for the package being compiled, which mcpp does not know
  (R7).

## 14. Tests

- **Unit, writer, ambiguous width.** The frames are replayed through the
  emulator with ambiguous characters counted as two columns and a width of 60.
  No stale status row remains, and under a CJK locale `fit` leaves the row
  within the width.
- **Unit, writer.**
  - A capture seam replaces the descriptor write.
  - Each emitted line and each redraw is one write.
  - Replaying the writes through the terminal emulator shows the status line
    after every write between the first draw and `Finished`.
  - No frame contains `ESC[J` before its last row.
  - With an injected clock, nothing is drawn before 0.5 s.
  - An unchanged frame is not written.
- **Unit, model.**
  - A package is announced once, at its first finished step, and never
    again after a recompaction of the log.
  - The cache pass yields `Cached` lines with the counts of steps that ran.
  - The phase returns to `Planning` after `programs_done()`.
  - A record of version 2 round-trips its descriptor and source lines.
  - A version 1 record makes the fast path decline.
- **Unit, identity warning.** A bare key of a path or git dependency is not
  reported. A key that states a contradicted namespace is reported, and the
  formatter groups such keys by manifest and namespace, uses relative paths,
  and writes a hint that names the table.
- **e2e 842 (log medium).** It is revised to check that:
  - every package that does work is named once, including path and index
    dependencies;
  - an incremental build names only the edited package and its dependents;
  - no `Cached` line appears when the cache pass placed nothing;
  - the fast path's `Finished` states the descriptor.
- **e2e 843 (terminal).** It is revised to check that no blank row lies
  above the status line, and that the final screen holds the package lines
  and `Finished`. The replay helper moves to `tests/e2e/_terminal_replay.py`,
  beside the other shared helpers, so that 842, 843 and later tests share it.
- **e2e, new.**
  - Three manifests with bare keys to path dependencies that declare a
    namespace build with no warning, and the graph record names the declared
    identities.
  - Two keys that state a contradicted namespace yield one warning, with the
    relative path and the table named.
  - A second planned build with `[dependencies.mcpplibs]` keys leaves
    `mcpp.lock` byte-identical (F11).
  - e2e 679 (#634 A2) and 713 (a git member) assert the adoption warning
    today. They are revised so that a bare key does not warn and a key that
    states a contradicted namespace does.
- **After release.** The sandbox verification of the released binary
  repeats B1 to B3 on the xlings tree. The validation project's CI accepts
  the release, as for 2026.9.29.5.

## 15. Tasks and order

| # | Task | Repository | Depends on |
|---|---|---|---|
| T1 | The writer: one write, overwrite, standard error, delay, unchanged frames (section 9) | mcpp | |
| T2 | The model: announce at first step, the cache pass reported, the phase fix, removals (section 10) | mcpp | T1 |
| T3 | Subjects and the programs block (section 7.1) | mcpp | T2 |
| T4 | `Finished`: 60 s, the source of `longest`, the descriptor on the fast path; record version 2 | mcpp | T2 |
| T5 | The identity rule of W-b and the grouped warning (section 8); `package-identity.md` §4.2 and docs/05 | mcpp | |
| T6 | Configuration lines under `-v` unless they state a change (F10) | mcpp | |
| T7 | One lock entry per identity (F11) | mcpp | |
| T8 | Tests (section 14); docs/09 "What a build prints" in both languages; CHANGELOG | mcpp | T1–T7 |
| X1 | The lock restored; optionally `[dependencies.xlings]` in three manifests, which removes the warnings for users of earlier mcpp releases | xlings | T7 for the lock only |

T1 to T8 form one mcpp pull request. T5, T6 and T7 are separate commits
within it, so that each can be reviewed alone. X1 does not wait for this
change.

## 16. Decisions for the reviewer

Settled in review round 1: the terminal writer (section 9); `Cached` lines
listed one per package, only when the cache pass placed units in this build;
the line format of 2026.9.29.4 with the directory relative to the project in
place of `(path)`; and no written line moves.

- **D1. The identity warning (section 5.2).** W-a, W-b (recommended) or W-c.
- **D2. When a package line is written (section 5.3).** A, B, or C
  (recommended).
- **D2'. The live display (section 5.4).** 1, or 2 (recommended).

Settled in review round 2: W-b, C and 2.

- **D3'. The status row (sections 5.6 to 5.10).**
  - The display: the LED bar, or the pixel animations chosen at random
    (section 5.10). If the animations, the set to build in.
  - The tail clause, and the tab and taskbar progress.
  - `MCPP_PROGRESS` and its values.
Settled in review round 9: the four animations (section 5.13), the module
`mcpp.ui.dots_screen`, and `--play-game` (section 5.14).

- **D3''. The animations (section 5.11).** The first set to build in, from
  the sign (logo or project name), chomp, snake, ions, stack, scanner,
  pulse, glide, runner and life; whether mcpp adopts a mascot (the beaver is
  the prototype) and whether the mascot builds the sign.
- **D4'. Colour by source (section 5.10).** The five sources and four
  colours as tabled; whether `compat.*` shares the official colour.

Settled in review round 3: a package inside the project is named by its
short name and drawn in the project colour; the phase aligns with the verbs.
- **D3. The configuration lines (F10).** The recommendation is to move
  `Resolving toolchain`, `Resolved`, `Target` and `Inferred` under `-v`,
  except when the toolchain is installed in this command. This changes the
  e2e tests that read those lines. `Workspace building member` is kept,
  because it states a selection.
- **D4. `Finished`.** The recommendation is to state the parts from 60 s
  rather than 10 s, and to name the source file in `longest`.
- **D5. The lock defect (F11).** The recommendation is to fix it in this
  pull request, as its own commit.
