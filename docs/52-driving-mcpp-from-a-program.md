# 52 — Driving mcpp from a Program or an Agent

**Reader:** the author of an agent's instructions, an editor's backend or a CI
job that runs mcpp in a shell and acts on what it finds.

**The question this chapter answers:** which commands, outputs and statuses let
a program build, run and inspect an mcpp project with no terminal and no IDE.

**Not here:** the shape of each document, which is
[50 — Machine-Readable Output](50-machine-output.md); the exit-code contract,
which is [SPEC-003](specs/exit-codes.md); and the editor integrations
themselves, which build on what this chapter describes.

Every chain that builds a project ends in a run of mcpp: a person at a
terminal, an agent with a shell, an editor's build on save, a CI job. mcpp
therefore writes what one chain needs to know about another's build to disk,
and states its outcome in a status a shell can branch on. A program needs
nothing beyond a shell to use any of it.

## 1. Build, and read the result

```sh
mcpp build --message-format json > build.ndjson
```

Each line of standard output is one JSON record. The last is the `mcpp.build`
envelope, whose `data` is the result record: `status` (`ok`, `failed`),
`artifacts` (with how to start each runnable one) and `diagnostics` (each with
`path` and `range`). The lines before it are events (`plan`, `waiting`,
`progress`, `diagnostic`, `artifact`) for a reader that reports progress. The
narration — what a person would read — is on standard error.

A program that did not start the build reads the same record from disk:

```sh
cat target/.last-result.json                 # names the record and its status
cat "$(jq -r .data.result target/.last-result.json)"
```

A build in a terminal and a build in an editor write the same record, so each
can see the other's result.

## 2. Run, or start the program yourself

`mcpp run` builds and then replaces itself with the program, which owns
standard output; its status is the program's (`101` when the build failed).
A debugger or a harness that starts the program itself asks for the launch:

```sh
mcpp run app --print-launch
```

The `mcpp.launch` envelope holds `argv`, `cwd` and `env` — only the variables
mcpp sets for the program — exactly as `mcpp run` would use them.

## 3. Statuses

| status | meaning | the program's response |
|---|---|---|
| `0` | done | read the record |
| `1` | the project or the environment failed | read `diagnostics`, fix, run again |
| `2` | usage error | correct the command line |
| `5` | the run stopped before an effect the caller denied | ask the caller for approval, then run again without the denial |
| `101` | `mcpp run` could not build the program | as `1` |
| `130`, `143` | ended by SIGINT, SIGTERM (POSIX) | the record says `cancelled`; the next build resumes |

## 4. Limit what a run may do

An agent's host, or an editor in an untrusted workspace, states which effects a
run may not have:

```sh
mcpp --deny network,exec-build-script build
MCPP_DENY_EFFECTS=network mcpp build          # the same, for every command of a session
```

The effects are `init-mcpp-home`, `write-project`, `write-global-cache`,
`network` and `exec-build-script`. A run that needs a denied effect stops
before it, names it on standard error and exits `5`; nothing was half done. A
run that does not need it is not affected: a project whose plan is recorded
builds under `--deny exec-build-script`. `mcpp --protocol-version` lists the
effects each command may have, before anything runs.

## 5. Several runs at once

Two builds of one configuration do not corrupt each other: the second waits for
the first and says whose run it waits for
(`Waiting for the build directory, held by pid 1234 (mcpp build, from my-agent, 8s)`).
Setting `MCPP_CLIENT` names the caller in that line and in the result record:

```sh
MCPP_CLIENT=my-agent mcpp build
```

A program started by `mcpp run` holds no lock, so rebuilding while it runs is
safe; a linked file is replaced whole. Queries never wait. The contract is
[SPEC-011](specs/concurrency.md).

## 6. Questions that need no build

| question | command |
|---|---|
| the packages the indexes offer | `mcpp search [keyword] --format json` |
| the configured registries | `mcpp index list --format json` |
| the package formats `mcpp pack` accepts | `mcpp pack --list-formats` |
| the compile commands of every unit | `mcpp emit build-database` |
| the installed toolchains | `mcpp toolchain list --format json` |
| the locations mcpp uses | `mcpp self env --format json` |
| one run against another home | `mcpp --home <dir> <command>` |

## 7. Reading the human output

When a program has only the narration — a CI log, a terminal transcript — the
shape is fixed ([SPEC-012](specs/human-output.md)): a status line starts with a
right-aligned word (`Compiling`, `Finished`); an mcpp message starts with
`error:`, `warning:` or `note:`; a message about a file starts with
`<path>:<line>:<col>:`; and a next step is on its own line starting `help:`.
Colour is off in a pipe unless `CLICOLOR_FORCE` or `FORCE_COLOR` asks for it.
The words themselves carry no compatibility promise; the records above do.
