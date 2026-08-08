# 11 — Machine-Readable Output

mcpp writes for two audiences. This chapter is the contract for the second one:
programs. If you are building an editor extension, a CI script, or anything
that parses mcpp's output, this is what you may rely on.

Design and the measurements behind it:
`.agents/docs/2026-08-08-machine-readable-output-protocol-design.md`.

## 1. The rule that matters most

> **Detect the protocol by parsing stdout. Never by exit code, and never by
> "the command did not fail".**

Read stdout, try to parse it as JSON, and require `schemaVersion` and `kind`
to be present. If either is missing, this mcpp does not speak the protocol you
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
| `init-mcpp-home` | may create `$MCPP_HOME` on first use. **Outside your project.** |
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

## 6. What you may rely on, and what changes

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
a machine that has never run mcpp you get the paths it **would** use and
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
