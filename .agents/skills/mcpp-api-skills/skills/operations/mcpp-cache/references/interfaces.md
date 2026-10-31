# Cache Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp cache` | command | Inspect and manage the global build cache |
| `mcpp cache clean` | command | Drop cache entries (default: package entries only) |
| `mcpp cache clean --all` | flag | Drop every kind of entry |
| `mcpp cache clean --deps` | flag | Drop package entries (default) |
| `mcpp cache clean --legacy` | flag | Remove the unused pre-v1 cache at $MCPP_HOME/bmi |
| `mcpp cache clean --std` | flag | Drop std module entries |
| `mcpp cache clean --tools` | flag | Drop host tool entries |
| `mcpp cache dir` | command | Print the cache root (and any pre-v1 cache) |
| `mcpp cache gc` | command | LRU-collect package and host tool entries to a size and/or age budget |
| `mcpp cache gc --max-size` | value | Keep the package cache under this size (e.g. 5GiB) |
| `mcpp cache gc --older-than` | value | Also drop entries unused for longer than this |
| `mcpp cache info` | command | Show details (incl. key inputs) for a cached package |
| `mcpp cache info <pkg>` | required argument | <pkg>@<ver> |
| `mcpp cache list` | command | List cache entries with size + last-use |
| `mcpp cache list --format` | value | Machine-readable output (enveloped; see docs/50-machine-output.md) |
| `mcpp cache list --json` | flag | Emit machine-readable JSON (legacy payload, kept for ever) |
| `mcpp cache prune` | command | Drop entries not used within a threshold |
| `mcpp cache prune --older-than` | value | Age threshold (e.g. 30d) |
| `mcpp cache verify` | command | Check every entry's manifest against the files on disk |
| `mcpp clean` | command | Remove target/, or with --stale only what no configuration still uses: fingerprint directories, generated sources, build-program groups |
| `mcpp clean --bmi-cache` | flag | Also wipe the global build cache (see `mcpp cache clean`) |
| `mcpp clean --dry-run` | flag | List what would be removed and delete nothing (implies --stale) |
| `mcpp clean --older-than` | value | Keep unrecorded directories written more recently than this, e.g. 12h, 3d (default 1d; 0 keeps none; implies --stale) |
| `mcpp clean --report` | flag | Show where target/ holds its bytes, per configuration and area; removes nothing |
| `mcpp clean --stale` | flag | Only remove target/<triple>/<fingerprint>/ directories that no recorded build considers current |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
