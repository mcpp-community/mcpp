# Source Baseline and Verification

The skills were checked against mcpp 2026.10.31.3,
commit `ce4dd4b001161f6635bec2a901b18381d6c6548c`.

## Source checks

The review followed command parsing to execution, manifest parsing to consumers,
and public build-program APIs to directive handling. `catalog.json` records
interface ownership and source locations. Its extracted declarations are a
navigation inventory, not proof that every platform and configuration was run.

## Executed checks

| Check | Result |
| --- | --- |
| Skill resources and source declarations | Passed |
| Suite checker tests, including detached copies | 16 passed |
| Official skill metadata validation | 34 entrypoints passed |
| TOML example syntax | 20 parsed; not mcpp semantic validation |
| Installed Windows mcpp CLI smoke checks | 10 passed |
| Test discovery with explicit globs, exclusions and disabled discovery | 3 scenarios passed |
| Isolated-home offline compile | Stopped before compile: provider not initialized |

The runtime checks used installed mcpp 2026.10.31.3, not a fresh source build.
The ten smoke checks covered protocol discovery, environment/cache envelopes,
error statuses, project creation, default test discovery and refusal to
overwrite an existing project. Three additional `test --list --strict` checks
covered `tests/test_*.cpp`, recursive discovery with support/vendor exclusions,
and `discover = []`. They checked discovery without compiling or running tests.

Fresh-source mcpp builds and their C++ and E2E suites were not run.
Linux/macOS builds, hardware devices, signing and accelerator execution were
not tested. Metadata, syntax and source checks do not establish runtime coverage.

## Local checks

From the suite root (`mcpp-api-skills/`):

```sh
python tools/check_skills.py
python tools/check_skills.py --mcpp-root /path/to/mcpp
python -m unittest discover -s tests -p 'test_*.py'
```

Runtime smoke checks use temporary projects and a selected binary:

```sh
python tools/validate_runtime.py --mcpp /absolute/path/to/mcpp --output /tmp/mcpp-skills-results.json
```

For offline compilation, also pass `--build --build-home` with a separately
prepared isolated home containing an initialized provider and toolchain.
