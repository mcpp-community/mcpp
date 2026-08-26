# 11 — Machine-Readable Output

mcpp writes for two audiences. This chapter is the contract for the second one:
programs. Editor extensions, CI scripts, and anything else that parses mcpp's
output may rely on what is stated here.

Design and the measurements behind it:
`.agents/docs/2026-08-08-machine-readable-output-protocol-design.md`.

## 1. Primary rule

> **Detect the protocol by parsing stdout. Never by exit code, and never by
> "the command did not fail".**

Read stdout, try to parse it as JSON, and require `schemaVersion` and `kind`
to be present. If either is missing, this mcpp does not speak the protocol the caller
asked for.

This is not a stylistic preference. `mcpp --protocol-version` looks like it
should be the entry point, and on a version that has it, it is a useful
shortcut. But on **every mcpp released before it existed**, that command is
itself an unknown option — and an unknown option used to print human text to
*stdout* with exit code 1 and an empty stderr. Success and failure arrived on
the same channel. Spelling it `--json` instead changes nothing; both hit the
same path.

So positive detection is the only rule that works across versions. Everything
below is designed around it.

## 2. The envelope

Every enveloped response has this shape:

```jsonc
{
  "schemaVersion": 1,          // the ENVELOPE's version
  "kind": "mcpp.env",          // which document this is
  "kindVersion": 1,            // this kind's own data version
  "effects": [],               // what running the command did — see §4
  "mcpp": {
    "version": "2026.8.8.3",
    "protocol": { "min": 1, "max": 1 }
  },
  "data": { /* specific to `kind` */ },
  "diagnostics": []
}
```

`schemaVersion` and `kindVersion` are separate on purpose. One global number
would mean that adding a field to `mcpp.env` moves the version a client reads
for `mcpp.xpkg`, with no way to tell which actually changed.

`effects` is always present. An empty array means "nothing"; an absent array
would mean "unknown", which is a different claim.

### Diagnostics

```jsonc
{
  "code": "MCPP_MANIFEST_UNKNOWN_KEY",
  "severity": "error" | "warning" | "note",
  "source": "mcpp",
  "message": "unknown key 'standrad'",
  "path": "mcpp.toml",                       // omitted when there is none
  "range": { "start": {"line": 3, "column": 1},
             "end":   {"line": 3, "column": 9} }   // omitted when there is none
}
```

Positions are 1-based. `column` counts UTF-8 **bytes**, so it indexes the same
file mcpp read.

A diagnostic without a location omits `path` and `range` rather than sending
zeros — `line: 0` would point at a position that does not exist.

`code` is always present. Parse `code`; never parse `message`.

## 3. Asking for machine output

```
mcpp <command> --format json
```

`json` is the only supported value today. `ndjson` is reserved for a future
streaming case and is **not** accepted — asking for it is an error, not a
silent fallback.

### Unsupported values and unknown options

Both go to **stderr** with **exit code 2**, and write nothing to stdout:

```
$ mcpp self env --format yaml
error: unsupported --format 'yaml'; expected: json      # stderr
$ echo $?
2
```

A request that does not yet know what it will be given must not write into the
channel the protocol owns. Combined with §1, a client's rule is complete: no
JSON on stdout means "not supported", whatever the reason.

Exit codes:

| code | meaning |
|---|---|
| 0 | success |
| 2 | usage error — unknown option, unsupported value |
| 70 | internal error (uncaught exception) |
| 127 | unknown command |

## 4. Effects — what a command does before it prints

An IDE with an untrusted-workspace gate has to decide **before** running.
By the time an envelope arrives, whatever it describes has already happened.
So the same information is available statically:

```
mcpp --protocol-version
```

```jsonc
{
  "schemaVersion": 1,
  "kind": "mcpp.protocol",
  "envelope": { "min": 1, "max": 1 },
  "kinds":    { "mcpp.env": 1, "mcpp.xpkg": 1, "mcpp.cache": 1 },
  "commands": {
    "self env":   { "effects": ["init-mcpp-home"] },
    "xpkg parse": { "effects": [] },
    "cache list": { "effects": [] }
  }
}
```

Effects are named rather than a `destructive: true|false`, because a boolean
cannot separate the harmless from the thing a gate exists for:

| effect | meaning |
|---|---|
| `init-mcpp-home` | may create `$MCPP_HOME` on first use. **Outside the project directory.** |
| `read-project` | reads the manifest and sources |
| `write-project` | writes into the project tree (`target/`, the compile DB) |
| `write-global-cache` | writes the shared build cache |
| `network` | may fetch |
| `exec-build-script` | **runs code from the workspace** (`build.mcpp`) |

Most gates care about `exec-build-script` and `write-project`, and can ignore
`init-mcpp-home` — mcpp setting itself up is not the workspace acting.

## 5. `--json` is not `--format json`

Two commands shipped a `--json` flag before this protocol existed:

```
mcpp xpkg parse <file> --json     ->  {"namespace": …, "name": …, …}
mcpp cache list --json            ->  {"root": …, "entries": [ … ]}
```

Those payloads are **bare** — no envelope — and consumers already read them.
So:

> **`--json` keeps its payload for ever. `--format json` is the enveloped one.**

`--json` is not deprecated, and using it prints no warning: clients parse this
output, and a warning would land in the middle of it.

Both spellings are produced from the same source, so they always describe the
same thing — one answer, two shapes.

## 6. Stability guarantees

For each `kind`, within a `kindVersion`:

- fields are **added**, never removed
- the meaning of a field never changes
- a breaking change bumps the version and, where a window is needed,
  `protocol.min`/`max` overlap so both are readable

That promise is only worth something if it is enforced, so each kind has a
test that fails when a field name changes. A schema nobody can break is not a
schema — `xlings interface --list` declares 20 capabilities whose
`outputSchema` is, for all 20, only `{"exitCode": integer}`, and a client that
sees a version number assumes there is a contract behind it.

## 7. Kinds

### `mcpp.env` — where mcpp keeps things

```
mcpp self env --format json
```

```jsonc
{
  "initialized": false,          // is there a config.toml yet?
  "mcppHome":    "/home/u/.mcpp",
  "registry":    "/home/u/.mcpp/registry",
  "xlingsHome":  "/home/u/.mcpp/registry",
  "xlingsBinary":"/home/u/.mcpp/registry/bin/xlings",
  "config":      "/home/u/.mcpp/config.toml",
  "buildCache":  "/home/u/.mcpp/build-cache/v1",
  "mcppVersion": "2026.8.8.3"
}
```

This path is read-only, deliberately. The human `mcpp self env` initialises
`$MCPP_HOME` if it is missing — someone typing it at a prompt expects that —
but a client asking *where things are* should not be what puts them there. On
a machine that has never run mcpp, the output is the paths it **would** use and
`initialized: false`, and the disk is untouched.

That is why this exists at all: without it a client has to reimplement mcpp's
home resolution, including the part where the `mcpp` on `PATH` may be an
xlings shim rather than the real binary.

### `mcpp.xpkg` — a parsed descriptor

```
mcpp xpkg parse <file.lua> --format json
```

`data` is the same document `--json` prints bare.

### `mcpp.cache` — the global build cache

```
mcpp cache list --format json
```

`data` is `{root, entries[]}`, the same document `--json` prints bare.

### `mcpp.toolchain.list` — what is installed, and which targets this host serves

```
mcpp toolchain list --format json
```

`data` is `{host, toolchains[], targets[]}`. A toolchain is
`{family, version, default}` — plus `source: "system"` for a Visual Studio
installation, which is located on the machine rather than installed by mcpp. A
target row is `{target, note, toolchain, pin, status, default}`, and `status` is
one of `installed` / `available` / `via dependency graph` / `planned`.

⚠️ **`toolchain` and `pin` are not the same field twice.** `toolchain` is what
the row is associated with — the installed payload on an installed row, the
convention on a vocabulary row. `pin` is only ever the target table's
convention, and is empty for a row that has none. `x86_64-linux-gnu` has an
installed gcc and no convention at all, so selecting "rows whose convention is a
gcc" must read `pin`.

### `mcpp.why.toolchain` — what a build for one pair would resolve to

```
mcpp why toolchain [--target <triple>] [--toolchain <spec>] --format json
```

It resolves and reports; it does not build. `data` is:

| field | |
|---|---|
| `requested` | `{target, toolchain}` — what was asked for |
| `status` | `ok` or `refused` |
| `reason` | a refusal token, or `none` |
| `compiler` | `{family, version, driver, chosenBy}` — the driver that would run, and why |
| `triple` | `{requested, toolchain, llvm}` |
| `cLibrary` | `{mode, path, origin, suppliesTarget}` — `mode` is `sysroot` / `payload-first` / `none`; `origin` is `payload` / `subos` / `host` / `none` |
| `layers[]` | the five target-side layers: `{layer, interface, impl, origin, subset}` |

⭐ **`compiler.chosenBy` answers "why this one".** `{origin, requiredBy,
replaced}` — `origin` is the same phrase the build's status line uses
(`[toolchain] in mcpp.toml`, `your default`, `target default`,
`required by the dependency graph`, `first-run default`). `requiredBy` names the
package when a `requires = ["mcpp:compiler=…"]` decided it, and `replaced` names
the spec that was displaced; both are empty when nothing was.

```jsonc
"compiler": { "family": "clang", "version": "22.1.8", "driver": "…/clang++",
              "chosenBy": { "origin":     "required by the dependency graph",
                            "requiredBy": "openkal-llvm-runtime@0.1.3",
                            "replaced":   "gcc@16.1.0" } }
```

Without it a consumer asking *why* would have to parse the status line — the
substring matching this document exists to remove.

⚠️ **`cLibrary` and `layers[].c-abi` answer two questions, and `suppliesTarget`
says which one governs.** `cLibrary` describes the *payload's* link model — the
search paths a payload-supplied C library would use. `layers[].c-abi` describes
the *build*. When a dependency supplies the C library the two diverge, and
before `suppliesTarget` existed the document reported both with no way to tell
them apart:

```jsonc
"cLibrary": { "origin": "payload", "path": "…/xim-x-glibc/2.44/lib64",
              "suppliesTarget": false },   // ← added; the payload is not in the artifact
"layers":   [ { "layer": "c-abi", "interface": "musl",
                "impl": "openkal-musl@0.3.5", "origin": "graph" } ]
```

A field was added rather than `cLibrary` renamed or `mode` widened, because §6
promises that fields are added and never removed and that a field's meaning
never changes.

⭐ **`reason` is a token, not a sentence.** The refusal's message is still
written for a person and still names the target, the rule and the way out — but
a program classifying the outcome reads `reason`:

| `reason` | |
|---|---|
| `unknown-target` | the spelling names no row, and no `(arch, os)` group either |
| `ambiguous-request` | several rows serve this `(arch, os)` and none is the default |
| `compiler-requirement-conflict` | the graph's required compiler cannot be used here |
| `tier-planned` | the row exists in the vocabulary; nothing is wired yet |
| `host-cannot-serve` | no payload here, and no dependency supplied the system |
| `capability-pin` | the row's toolchain is a capability, not a preference |
| `convention-unreplaced` | the convention was overridden and nothing replaced it |
| `os-mismatch` | the requested and resolved triples name different systems |
| `layer-requirement` | a package requires a layer the resolution did not give it |
| `layer-ordering` | the five layers do not stack |
| `other` | a refusal whose branch has not been given a token yet |

⚠️ **Exit 0 whenever the question was answered, including "refused".** "Would
this build, and if not why" is answered successfully by "no, because the row's
pin is a capability". A non-zero exit means the query itself could not run.

⚠️ **Its effects are broad on purpose.** `--protocol-version` lists `network`,
`write-global-cache` and `exec-build-script` for this command: the answer comes
from the same resolution a build performs, which may fetch packages, install a
payload and run a dependency's build program. A client gates on that table
*before* running anything, so an omission would be a safety claim that is not
true.
