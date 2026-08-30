# Project build hooks as owned intervals (#496)

## The question this design answers

The first shape of `[hooks]` gave `mcpp build` three commands — `build_start`,
`build_finished`, `build_failed` — each run to completion with a timeout, each
judged by its exit code.

The request that followed was a background command: music that plays *for the
duration of the build* and stops when it ends, optionally restarting when the
player exits. Written as a knob it looks like

```toml
build_start = { cmd = "play bgm.mp3", loop = true }
```

but `loop` is not the new thing. The new thing is that this command's life is
**longer than the moment that started it**. Adding `background = true` to
`build_start` would give that key two incompatible meanings and would silently
change what `timeout_seconds` and `side_effect` mean for one of them — a table
whose keys mean different things depending on a sibling key is the shape this
design exists to avoid.

## The model

> A hook is a command mcpp **owns for an interval**. The event names the
> interval. mcpp starts the command when the interval opens and ends it when
> the interval closes.

| Event | The interval opens | The interval closes |
|---|---|---|
| `build_start` | after preparation, before the build | when the command exits |
| `build_finished` | after a build that succeeded | when the command exits |
| `build_failed` | after a build that failed | when the command exits |
| `during_build` | after preparation, before the build | after the build, before the terminal hook |

The first three have **self-closing** intervals. "Synchronous" is not a
separate mode in this model — it is what an interval closed by the command
itself looks like. `during_build` is the one interval closed by something else,
and everything that reads as a special case for it falls out of that single
difference rather than being declared:

- **`timeout_seconds` bounds one run of the command.** For a self-closing
  interval that is the whole hook. For `during_build` the build already bounds
  it, so the key is *rejected* there rather than accepted and reinterpreted —
  a per-run cap would only be enforceable when `loop` is on (nothing is
  polling otherwise), and a key that works under one sibling setting and not
  another is the hole this design is trying not to dig.
- **`loop` restarts a command that exits before its interval closes.** A
  self-closing interval ends *when the command exits*, so `loop` can never fire
  there. It is rejected on those events rather than accepted and ignored, with
  a message naming `during_build`.
- **`side_effect` is unchanged**: does a hook failure fail the build. For
  `during_build`, "failure" means *failed to start*, or *failed to stay up*
  (below). Being stopped because the interval closed is not a failure.

Ordering inside the lifecycle:

```text
during_build opens
build_start
    ├─ build succeeds → during_build closes → build_finished
    └─ build fails    → during_build closes → build_failed
```

`during_build` closes **before** the terminal hook, not after. A "build
finished" sound playing over the background music it was supposed to replace is
the whole reason the order is fixed rather than incidental.

## Schema

Every event value is a string or a table; the string is sugar.

```toml
[hooks]
build_start    = "echo start"                              # = { cmd = "echo start" }
build_finished = "notify-send 'build finished'"
during_build   = { cmd = "mcpp-hooks-audioplayer bgm", loop = true }

# Table-level, unchanged.
timeout_seconds = 10
enabled         = true
side_effect     = true
```

Per-event table keys: `cmd` (required, non-empty), `timeout_seconds`
(overrides the table default for this event), `loop` (`during_build` only).

The string-or-table pattern is already how `[dependencies]` and
`[resources].version-info` are spelled, so it introduces no new parsing
semantics — Appendix A of docs/05 is satisfied for the same reason the first
shape satisfied it: fixed keys, open values, and no key that duplicates an
answer another section already gives.

## What the feature actually costs

The schema is the small half. A process whose life spans the build needs three
things mcpp does not have.

### 1. A process group, because SIGKILL on a pid is not enough

`modules/platform/src/unix/bounded_process.cppm:216` kills the direct child:

```cpp
::kill(pid, SIGKILL);
```

For `sh -c "sleep 5"` that is sufficient — the shell execs the command. For
`sh -c "while :; do play a.mp3; done"` it is not: the shell dies and `play`
survives. **Music that cannot be stopped, from a process the user cannot name,
is the worst failure this feature can have**, and it is the default outcome
without process groups.

So the POSIX side gains `posix_spawnattr_setpgroup` + `killpg`. Windows already
has the right shape: a Job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`
(`windows/bounded_process.cppm:205`), which takes the whole tree — and takes it
even if mcpp dies, because the handle closes with the process.

This is worth doing on its own terms: the same gap is why
`mcpp test --timeout` and `[build] build_program_timeout` can leave
grandchildren behind today.

### 2. A signal handler, because a process group stops receiving Ctrl-C

The two requirements fight each other. A child in its own process group no
longer receives the terminal's SIGINT — which is what makes `killpg` possible
and *also* what makes Ctrl-C leave the music playing. Both halves are needed:
its own group, plus a SIGINT/SIGTERM handler in mcpp that stops the group
before re-raising.

mcpp installs no signal handlers today. The one added here is the minimum that
is async-signal-safe: a `volatile sig_atomic_t` holding the process-group id, a
handler that calls `killpg` (which is async-signal-safe) and re-raises the
default action. It is installed only while a spanning hook is running.

Windows needs no equivalent for correctness — the job object already covers
process death — but `SetConsoleCtrlHandler` is installed for a clean stop.

### 3. A restart floor, because `loop` on a typo is a fork bomb

`loop = true` with `play /nonexistant` restarts thousands of times per second
for the length of the build. Two bounds, both fixed in v1 rather than
configurable, because a knob whose wrong value is a spin is not a knob:

- **250 ms between runs.**
- **Five consecutive runs that exited non-zero in under a second** stops the
  loop and reports a hook failure: *"during_build command failed to stay up".*
  Whether that fails the build is `side_effect`, as everywhere else.

A supervisor thread exists **only when `loop = true`**. Without it, a spanning
hook is a spawn and a stop, and no thread is created.

## Stopping

POSIX: `SIGTERM` to the group, a 2 s grace period, then `SIGKILL`. A player
asked to stop should get to close its audio device.

Windows: the job object is closed, which terminates the tree at once. There is
no graceful equivalent that does not require enumerating the job's processes
and posting window messages; the asymmetry is stated rather than hidden.

⚠️ The obvious symmetry — "ask with `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
pid)` first" — was written, and it is wrong in a way no local run shows. That
call addresses a process GROUP attached to the caller's console, not a process.
When the id does not name a live group of ours (and it does not, once the child
has exited — `start /b`-style commands exit immediately) the event reaches
everything sharing the console. Measured on the Windows e2e runner: the entire
suite died eleven seconds into the hooks test with exit code `-1073741510`
(`0xC000013A`, `STATUS_CONTROL_C_EXIT`) and printed no summary, because mcpp had
sent Ctrl-Break to its own console. The design said Windows has no graceful stop;
the first implementation did not believe it. The code now matches the design.

## Output

A self-closing hook inherits stdio, which is safe because nothing else is
writing at that moment. A spanning hook writes **concurrently with ninja** and
would interleave into the middle of a compiler diagnostic. Its output is
therefore discarded by default, and inherited under `--verbose` — no new schema
key, and the answer to "why is there no music" is one flag away.

## Criteria

The assertions this design has to earn are about *state*, not about log lines —
"we called stop" is not evidence that anything stopped.

1. **It runs during the build.** The command appends a heartbeat line every
   200 ms; the file is non-empty when the build ends.
2. **It is stopped.** Record the heartbeat file's size after `mcpp build`
   returns, wait one second, read it again: unchanged. A log line saying
   "stopped" would pass whether or not the process died.
3. **`loop` restarts it.** A command that exits immediately produces a heartbeat
   count that grows across the build; without `loop` it produces exactly one.
4. **The whole tree dies, not just the shell.** The command is
   `sh -c '... & wait'`, so the writer is a grandchild. This is the case
   `kill(pid)` misses and `killpg` catches, and it is the only assertion that
   distinguishes the fix from the bug.
5. **The failure cap trips.** A command that fails instantly stops after five
   attempts and reports; the heartbeat count is bounded, not "large".
6. **Ctrl-C leaves nothing behind.** POSIX only: `mcpp build` in the
   background, `kill -INT`, then criterion 2. Sending Ctrl-C to another process
   on Windows needs a helper this suite does not have; the gap is declared
   rather than papered over with a test that passes vacuously.
7. **A self-closing event rejects `loop`.** Otherwise the key is accepted,
   does nothing, and the user concludes the feature does not work.
