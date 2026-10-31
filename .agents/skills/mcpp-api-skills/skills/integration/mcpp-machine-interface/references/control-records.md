# Control Record Fields

Baseline: mcpp 2026.10.31.3. Fields below are grounded in `wire::to_json`,
`result::to_json`, `result::render_last` and the result event writer. Paths and
values in examples are illustrative, not captured runtime output.

## Envelope

Ordinary machine documents contain:

| Field | Meaning |
|---|---|
| schemaVersion | Envelope shape version; require a supported version |
| kind | Document identity, such as mcpp.build or mcpp.launch |
| kindVersion | Version of this kind's data |
| mcpp.version | Producing mcpp release |
| mcpp.protocol | Supported envelope min/max |
| mcpp.features | Capabilities available inside existing commands |
| effects | Effects observed/reported for this invocation |
| diagnostics | Envelope-level diagnostics |
| data | Kind-specific data; can be absent on failures with nothing to describe |

The discovery document `mcpp.protocol` has its own top-level envelope/kinds/
commands/features shape, not ordinary command data. An event is also a distinct
stream record, not a full envelope. A parser must branch by kind and version.

Diagnostics can carry code, severity, source, message, optional path and range.
Ranges have start/end line and column; the serializer derives locations from
compiler messages when possible. Do not require a location on a global error.

## Build NDJSON

Read one JSON record per stdout line. Events have `kind = mcpp.event`, an event
name and event-specific fields. Multiple legs may add target context. The final
`mcpp.build` envelope has result data:

| Data field | Meaning |
|---|---|
| command, client, pid | Invoking command, caller label and process |
| startedAt, finishedAt | Epoch milliseconds; finishedAt can be null |
| status | running / ok / failed / cancelled |
| configuration | triple, profile, fingerprint, outputDir, platform, hostPlatform |
| compileDatabase | Database path or null |
| artifacts | name, kind, path; optional launch and entry |
| diagnostics | Build diagnostics with optional path/range |

Envelope diagnostics and data.diagnostics have different origins; preserve
both. A nonzero process status does not mean stdout cannot contain useful JSON.
Missing final output after cancellation/crash must not be labeled successful.

Minimal consumer for a previously saved build stream:

```python
import json

final = None
with open("build.ndjson", encoding="utf-8") as stream:
    for line in stream:
        record = json.loads(line)
        if record.get("schemaVersion") != 1:
            raise ValueError("unsupported envelope/event version")
        if record.get("kind") == "mcpp.event":
            continue  # update progress from this event if needed
        if record.get("kind") == "mcpp.build":
            if record.get("kindVersion") != 1:
                raise ValueError("unsupported build data version")
            final = record
if final is None:
    raise RuntimeError("build did not report a final result")
data = final.get("data")
if not isinstance(data, dict) or data.get("status") != "ok":
    raise RuntimeError({"diagnostics": final.get("diagnostics", []), "data": data})
```

Also check the original process status. Keep stderr separate when capturing the
stream; never feed human narration to this parser. For test JSON use its per-test
records, not this final-build algorithm.

## Launch

Run and single-test launch share the `mcpp.launch` envelope. Its data is:

```json
{
  "argv": ["/absolute/path/to/program", "--example"],
  "cwd": "/absolute/path/to/project",
  "env": {"EXAMPLE_INJECTED_VARIABLE": "value"},
  "via": null
}
```

via is null or the selected runner/run_under path classification. Overlay env
on the caller's environment. Spawn argv directly with cwd; do not join it into
a shell string. Inspecting launch builds first, but does not run the product.

Test launch requires a single selected member, one matched test and one target
leg. --list, --archive, --no-run and --message-format json are incompatible.

## Disk records and cancellation

`target/.last-result.json` is `mcpp.last-result` with data.result pointing to a
configuration result.json, data.status and data.finishedAt. Multiple legs also
list their records under data.legs, with failed/cancelled/ok aggregate status.
Read the pointer instead of guessing a fingerprint directory. The record file
has a `mcpp.result` document whose data follows the result schema above. This
kind differs from the final stdout `mcpp.build` envelope.

POSIX cancellation can write cancelled records. SIGKILL can leave running plus
a dead pid. Check liveness and freshness; do not assume running means a live
build or force-remove the build lock.

## Denied effects

Use the discovery command table before running to inspect possible effects.
An envelope's list is not a preflight approval. Denial usually stops before the
operation with status 5. Denying write-global-cache instead selects local
caching where applicable. Offline denies network, while MCPP_NO_AUTO_INSTALL
only prevents automatic toolchain installation.
