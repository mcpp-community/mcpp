---
subject: targets
status: landed
---

# A verified Web run that asked the host for node

**Status:** implemented in mcpp 2026.9.12.2 (2026.9.12.1 was not published) and openxlings/xim-pkgindex#823.

## What was measured

The sandbox verification of the published mcpp 2026.9.11.4 ran 27 checks in an
`xlings subos use <name> --sandbox` environment: a fresh `$HOME`, a fresh
`/tmp`, and no `mcpp` or `node` on PATH. Twenty-six held. The one that did not
was the Web run:

```
mcpp run --target wasm32-emscripten
  Compiling w v0.1.0 (.)
  Finished dev [unoptimized + debuginfo] in 0.84s
  Running `target/wasm32-emscripten/.../bin/w`
/usr/bin/env: 'node': No such file or directory
```

The build was correct. The run depended on the host.

## Why the development host could not see it

An Emscripten link produces a JavaScript launcher whose first line is
`#!/usr/bin/env node`. When neither the project nor its dependency graph
declares a runner, `mcpp run` executes the artefact, and the kernel resolves the
interpreter from the PATH mcpp inherited. `choose_device_action` consults the
manifest and the graph and nothing else, and the spawned artefact receives no
toolchain directory on its PATH.

The ecosystem had already installed the right program. `xim:emsdk` declares
`xim:node` as a runtime dependency and writes that payload's `bin/node` into its
own `.emscripten`, deliberately so that linking does not depend on PATH. Running
the result was the one step that still did.

The row's `verified` record states how it was measured: `node bin/<name>`, with
a `node` the measuring machine had. The development host has one on PATH through
its default xlings environment. So every measurement on that host, including the
dry run of the verification script itself, read the host's `node` and passed.
Only an environment without one could distinguish "the ecosystem runs this" from
"this machine runs this".

## Where the answer belongs

Three placements were considered and rejected.

- **The engine reads `NODE_JS` from emsdk's `.emscripten`.** This would put an
  emsdk layout fact into the engine, which is the coupling the payload
  descriptor was introduced to remove (2026-09-11 record, item C).
- **The recipe places a `node` link inside the emsdk payload's `bin/`.** Recipe
  hooks in this ecosystem have no reliable symlink or permission primitive, and
  a link would restate a fact the recipe already records as a path.
- **Projects declare `[target.wasm32-emscripten] runner` and `xim:node`.** A
  `verified` row that needs two lines of vocabulary to run is not the row the
  2026.9.11.3 release described, where changing a flag was the whole cost.

The payload knows which program runs what its compiler produces, so the payload
states it. `.mcpp-toolchain.json` gains a fourth key.

## The contract

```json
{
  "schema": 1,
  "frontend": "emscripten/em++",
  "runner": "<store>/xim-x-node/26.7.0/bin/node"
}
```

- **One program, no arguments.** The artefact path is appended. The key cannot
  carry flags, which keeps the descriptor what it was: a set of answers to
  questions the engine already asks, and not a flag channel.
- **Relative or absolute.** A relative runner obeys the rules `frontend` obeys
  and resolves against the directory the descriptor was read from. An absolute
  runner exists because the program is usually a dependency's, and it is
  honoured only inside the package store that holds the payload
  (`<store>/<package>/<version>`), compared on canonical paths so that a store
  reached through a symbolic link is still that store.
- **Outside the store is ignored, malformed is refused.** Where a store lives is
  a property of the machine, so a runner outside it is not honoured and the run
  proceeds as it did before the key existed. A structurally wrong value (not a
  string, empty, a backslash, a `.` or `..` component) is refused by name at
  read time, like every other key.
- **Last in precedence, run slot only.** A project's `[target.<triple>] runner`
  and a dependency's `mcpp::runner(...)` are statements about this program and
  outrank a statement about everything a compiler produces. `--no-runner` still
  executes the artefact directly. `flash`, `monitor` and `debug` name actions a
  board package owns, and a compiler has no opinion about them.

## Compatibility

Both directions hold without coordination. An mcpp that predates the key ignores
it, so the recipe ships first. A payload installed before the recipe wrote the
descriptor has none, and its artefact runs through its own shebang exactly as
before; reinstalling the payload adds the descriptor.

## Criteria

| claim | criterion | measured |
|---|---|---|
| a runner in the payload's store is the default for its compiler | unit test with the emsdk shape: compiler at `emscripten/em++`, runner a sibling payload's `bin/node` | yes |
| a runner outside the store is not honoured | unit test with `/usr/bin/node`; and the same test with the store rule removed | yes, and the probe turned it red |
| malformed runners are refused by name and by key | seven shapes, each refusal naming the file and `"runner"` | yes |
| the wiring reaches `mcpp run` | in the sandbox, no `node` on PATH, with a descriptor in the sandbox's own emsdk payload | `1-2-3` |
| the runner came from the descriptor and not from somewhere else | the same run with the descriptor removed | fails at `/usr/bin/env: 'node'` again |
| CI holds the claim against the published recipe | `scan (linux-x86_64)` runs a Web program with a fresh `MCPP_HOME`, so the emsdk payload is the one the index publishes rather than one restored from cache, and with a decoy `node` first on PATH that exits 97 with a marker; the step fails if the payload has no descriptor, if the decoy ran, or if `1-2-3` is not a line of the output | on this PR, after openxlings/xim-pkgindex#823 was published |

The first attempt at the sandbox reading measured nothing. The development binary
was dynamically linked, and its loader lives in the host's `~/.mcpp`, which the
sandbox replaces with its own; both readings were "cannot execute: required file
not found" and said nothing about runners. The reading above used a
`x86_64-linux-musl` static build, the form a release ships.

## What remains

An emsdk payload already installed on a machine keeps running through PATH until
it is reinstalled. The published-artefact check is the sandbox verification
script's Web section, run again after this release.
