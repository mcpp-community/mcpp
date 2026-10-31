# Build Actions Reference

Interface names and declarations for mcpp 2026.10.31.3.

## Commands and options

| Interface | Form | Description |
| --- | --- | --- |
| `mcpp stage` | command | Copy one file into place: create the destination's parent, write only when the content differs (invoked by build.ninja and by ${mcpp.self} actions) |
| `mcpp stage --list` | value | Place every `<source>\t<destination>` pair of FILE in one process (#734 E4); several lines naming one destination are its sources |
| `mcpp stage --output` (`-o`) | value | Destination path; its parent directory is created |
| `mcpp stage --verify` | value | How an existing destination is judged up to date: content (default) \| size |

## Source

- [src/cli.cppm](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/src/cli.cppm)
