# Read-only IDE Snapshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a versioned, one-shot `mcpp ide snapshot --format json` command that inspects mcpp workspaces without installing, building, executing project code, or writing artifacts.

**Architecture:** Define an IDE-owned public DTO, populate it through a new read-only workspace inspector, and serialize it through a deterministic JSON adapter. Keep CLI parsing in `mcpp.cli.cmd_ide`; do not import `mcpp.build.prepare`, toolchain resolution, xlings, config initialization, or build execution anywhere below the IDE command.

**Tech Stack:** C++23 named modules, `std::expected`/`std::filesystem`, existing `mcpp.manifest` and `mcpp.project` read paths, vendored nlohmann JSON module, GoogleTest, portable Bash E2E, self-hosted `mcpp build`/fresh-binary `mcpp test`.

---

## File Map

| File | Responsibility |
| --- | --- |
| `src/ide/model.cppm` | Stable IDE DTOs and wire-enum name helpers; no build-plan dependency |
| `src/ide/inspect.cppm` | Read-only root/workspace/member discovery, manifest diagnostics, selection, and CDB existence probes |
| `src/ide/snapshot.cppm` | DTO-to-JSON adapter and deterministic snapshot ID |
| `src/cli/cmd_ide.cppm` | CLI selector parsing, one-document stdout, format/error exit mapping |
| `src/cli.cppm` | Import/register `ide snapshot`, help entry, known-command list |
| `tests/unit/test_ide_snapshot.cpp` | DTO, inspection, selector, diagnostic, artifact, serialization, and identity tests |
| `tests/fixtures/ide/snapshot-v1.json` | Schema-1 golden fixture consumed by unit tests |
| `tests/e2e/196_ide_snapshot.sh` | Fresh-binary CLI contract and workspace/error cases |
| `tests/e2e/197_ide_snapshot_read_only.sh` | Before/after project and isolated-home zero-side-effect proof |
| `docs/superpowers/specs/2026-08-05-mcpp-ide-snapshot-design.md` | Approved protocol and side-effect contract |

### Task 1: Define the Public IDE DTO and Protocol Fixture

**Files:**
- Create: `src/ide/model.cppm`
- Create: `tests/fixtures/ide/snapshot-v1.json`
- Create: `tests/unit/test_ide_snapshot.cpp`

- [ ] **Step 1: Add a failing DTO/wire-name test**

Create `tests/unit/test_ide_snapshot.cpp` with the fixture helper and initial enum contract:

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.ide.model;

TEST(IdeSnapshotModel, WireNamesAreStable) {
    using namespace mcpp::ide;
    EXPECT_EQ(wire_name(SnapshotState::Partial), "partial");
    EXPECT_EQ(wire_name(SnapshotState::Stale), "stale");
    EXPECT_EQ(wire_name(SnapshotState::Unavailable), "unavailable");
    EXPECT_EQ(wire_name(SnapshotState::Ready), "ready");
    EXPECT_EQ(wire_name(ArtifactState::Missing), "missing");
    EXPECT_EQ(wire_name(ArtifactState::Stale), "stale");
    EXPECT_EQ(wire_name(Severity::Warning), "warning");
    EXPECT_EQ(wire_name(Severity::Error), "error");
}
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
mcpp test ide_snapshot
```

Expected: build failure because module `mcpp.ide.model` does not exist.

- [ ] **Step 3: Implement the DTO module**

Create `src/ide/model.cppm` with these exported types and no imports outside `std`:

```cpp
export module mcpp.ide.model;

import std;

export namespace mcpp::ide {

enum class SnapshotState { Partial, Stale, Unavailable, Ready };
enum class ArtifactState { Missing, Stale };
enum class Severity { Warning, Error };

std::string_view wire_name(SnapshotState value);
std::string_view wire_name(ArtifactState value);
std::string_view wire_name(Severity value);

struct Position {
    std::size_t line = 0;
    std::size_t column = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Diagnostic {
    std::string code;
    Severity severity = Severity::Error;
    std::string message;
    std::filesystem::path path;
    std::optional<Range> range;
};

struct Selectors {
    std::optional<std::string> package;
    bool workspace = false;
    std::optional<std::string> profile;
    std::optional<std::string> target;
    std::vector<std::string> features;
    std::vector<std::string> capabilities;
    bool includeDevDependencies = false;
};

struct InspectRequest {
    std::filesystem::path start;
    Selectors selectors;
};

struct DeclaredTarget {
    std::string name;
    std::string kind;
    std::optional<std::string> main;
};

struct WorkspaceMember {
    std::string name;
    std::string version;
    std::string workspacePath;
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::vector<DeclaredTarget> targets;
};

struct CompileCommandsArtifact {
    std::string member;
    std::filesystem::path path;
    ArtifactState state = ArtifactState::Missing;
};

struct WorkspaceInspection {
    SnapshotState state = SnapshotState::Unavailable;
    InspectRequest request;
    std::filesystem::path workspaceRoot;
    std::filesystem::path workspaceManifest;
    std::vector<WorkspaceMember> members;
    std::vector<std::string> selectedMembers;
    std::vector<CompileCommandsArtifact> compileCommands;
    std::vector<Diagnostic> diagnostics;
};

} // namespace mcpp::ide
```

Define each `wire_name` as an exhaustive switch returning the strings pinned by
the test. No default branch is needed; return an empty string after the switch to
satisfy compilers that do not treat an exhaustive enum switch as terminating.

- [ ] **Step 4: Add the schema-1 golden fixture**

Create `tests/fixtures/ide/snapshot-v1.json` as a complete, valid example. Use
fixed `/workspace/app` paths, `snapshotId` `fnv1a64:0000000000000000`, one binary
target, one missing CDB, state `partial`, and one `MCPP_IDE_ARTIFACTS_MISSING`
warning. Include every required top-level object from the design spec.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run `mcpp test ide_snapshot`.

Expected: `IdeSnapshotModel.WireNamesAreStable` passes.

- [ ] **Step 6: Commit the model and fixture**

```bash
git add src/ide/model.cppm tests/unit/test_ide_snapshot.cpp tests/fixtures/ide/snapshot-v1.json
git commit -m "feat(ide): define snapshot protocol model"
```

### Task 2: Implement Read-only Workspace Inspection

**Files:**
- Create: `src/ide/inspect.cppm`
- Modify: `tests/unit/test_ide_snapshot.cpp`

- [ ] **Step 1: Add a temporary-project test helper**

Add a local RAII helper to `tests/unit/test_ide_snapshot.cpp`:

```cpp
namespace {
struct TempProject {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        std::format("mcpp-ide-{}", std::chrono::steady_clock::now()
            .time_since_epoch().count());
    TempProject() { std::filesystem::create_directories(root); }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void write(std::filesystem::path relative, std::string_view content) {
        auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << content;
    }
};
} // namespace
```

- [ ] **Step 2: Add failing inspection tests**

Import `mcpp.ide.inspect` and add tests with these exact assertions:

```cpp
TEST(IdeSnapshotInspect, SinglePackageIsPartialWithoutCdb) {
    TempProject p;
    p.write("mcpp.toml", "[package]\nname=\"app\"\nversion=\"1.2.3\"\n");
    auto result = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(result.state, mcpp::ide::SnapshotState::Partial);
    ASSERT_EQ(result.members.size(), 1u);
    EXPECT_EQ(result.members[0].name, "app");
    EXPECT_EQ(result.selectedMembers, std::vector<std::string>{"app"});
    ASSERT_EQ(result.compileCommands.size(), 1u);
    EXPECT_EQ(result.compileCommands[0].state, mcpp::ide::ArtifactState::Missing);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "MCPP_IDE_ARTIFACTS_MISSING");
}

TEST(IdeSnapshotInspect, ExistingCdbIsStaleNeverReady) {
    TempProject p;
    p.write("mcpp.toml", "[package]\nname=\"app\"\nversion=\"1.0.0\"\n");
    p.write("compile_commands.json", "[]\n");
    auto result = mcpp::ide::inspect_workspace({.start = p.root});
    EXPECT_EQ(result.state, mcpp::ide::SnapshotState::Stale);
    EXPECT_EQ(result.compileCommands[0].state, mcpp::ide::ArtifactState::Stale);
    EXPECT_EQ(result.diagnostics[0].code, "MCPP_IDE_ARTIFACTS_UNVERIFIED");
}
```

Also add tests for:

- no ancestor `mcpp.toml` -> unavailable + `MCPP_IDE_MANIFEST_NOT_FOUND`;
- malformed root manifest -> unavailable + parser path/range;
- virtual workspace no selector -> all valid members selected;
- rooted workspace no selector -> root selected;
- invocation from member directory -> current member selected;
- `selectors.package` matches basename and relative workspace path;
- unknown package -> unavailable + `MCPP_IDE_WORKSPACE_MEMBER_NOT_FOUND`;
- missing/invalid unselected member -> diagnostic but valid selected member survives;
- missing/invalid selected member -> unavailable.

- [ ] **Step 3: Run focused tests and verify RED**

Run `mcpp test ide_snapshot`.

Expected: build failure because `mcpp.ide.inspect` and `inspect_workspace` do not
exist.

- [ ] **Step 4: Implement `mcpp.ide.inspect`**

Create `src/ide/inspect.cppm`:

```cpp
export module mcpp.ide.inspect;

import std;
import mcpp.ide.model;
import mcpp.manifest;
import mcpp.project;

export namespace mcpp::ide {
WorkspaceInspection inspect_workspace(InspectRequest request);
}
```

Implementation order:

1. Normalize `request.start` to an absolute path without creating it.
2. Call `project::find_manifest_root`; on miss, return unavailable with
   `MCPP_IDE_MANIFEST_NOT_FOUND`.
3. Load the discovered manifest. Convert `ManifestError` into a diagnostic that
   preserves `file`, `line`, and `column`.
4. If the discovered package is a member, use `find_workspace_root` to locate and
   load its workspace manifest while retaining the member as the implicit
   selection.
5. Add the rooted workspace package as `workspacePath = "."` when present, then
   load each declared member manifest in declaration order.
6. Convert manifest targets using stable target-kind strings and absolute paths.
7. Apply package/workspace/implicit selection rules from the design spec.
8. Probe only `<member-root>/compile_commands.json` with `exists` and
   `is_regular_file`; never open or validate the CDB in M0.
9. Emit one aggregate missing or unverified artifact warning and derive top-level
   partial/stale/unavailable state conservatively.

Use `std::error_code` filesystem overloads where available so permission errors
become diagnostics instead of exceptions. Do not call `prepare_build`, config,
xlings, toolchain, dependency, scanner, or build modules.

- [ ] **Step 5: Run focused tests and verify GREEN**

Run `mcpp test ide_snapshot`.

Expected: every `IdeSnapshotInspect.*` test passes.

- [ ] **Step 6: Commit the inspector**

```bash
git add src/ide/inspect.cppm tests/unit/test_ide_snapshot.cpp
git commit -m "feat(ide): inspect workspaces without side effects"
```

### Task 3: Serialize the Versioned Snapshot

**Files:**
- Create: `src/ide/snapshot.cppm`
- Modify: `tests/unit/test_ide_snapshot.cpp`
- Modify: `tests/fixtures/ide/snapshot-v1.json`

- [ ] **Step 1: Add failing serializer tests**

Import `mcpp.ide.snapshot` and `mcpp.libs.json`. Add tests that:

- call `snapshot_json(inspection)` and parse the result as exactly one JSON value;
- assert `schemaVersion == 1`, `kind == "mcpp.ide.snapshot"`, version/protocol,
  capability strings, request selectors, members, selected members, artifacts,
  diagnostics, and one-based range;
- call twice and assert byte-identical output and identical `snapshotId`;
- modify a selected member and assert the ID changes;
- load `tests/fixtures/ide/snapshot-v1.json`, replace its sentinel snapshot ID
  with the computed ID, and compare the parsed JSON values.

Use a direct fixture path rooted at `std::filesystem::current_path()` because
fresh-binary `mcpp test` runs tests from the project root.

- [ ] **Step 2: Run focused tests and verify RED**

Run `mcpp test ide_snapshot`.

Expected: build failure because module `mcpp.ide.snapshot` does not exist.

- [ ] **Step 3: Implement JSON serialization and identity**

Create `src/ide/snapshot.cppm` with:

```cpp
export module mcpp.ide.snapshot;

import std;
import mcpp.ide.model;
import mcpp.libs.json;
import mcpp.version;

export namespace mcpp::ide {
std::string snapshot_json(const WorkspaceInspection& inspection);
}
```

Build a `nlohmann::ordered_json` object in protocol field order. Represent absent
selectors as JSON null and preserve selector list order. Omit `path`/`range` from
diagnostics when unavailable. Serialize once without `snapshotId`, hash its
compact `dump()` bytes with local FNV-1a 64, insert
`std::format("fnv1a64:{:016x}", hash)`, and return `dump(2) + "\n"`.

The serializer must not read the filesystem or environment. It consumes only the
DTO and `mcpp::MCPP_VERSION`.

- [ ] **Step 4: Run focused tests and verify GREEN**

Run `mcpp test ide_snapshot`.

Expected: fixture, deterministic identity, and full field tests pass.

- [ ] **Step 5: Commit the serializer**

```bash
git add src/ide/snapshot.cppm tests/unit/test_ide_snapshot.cpp tests/fixtures/ide/snapshot-v1.json
git commit -m "feat(ide): serialize versioned workspace snapshots"
```

### Task 4: Add `mcpp ide snapshot` CLI Routing

**Files:**
- Create: `src/cli/cmd_ide.cppm`
- Modify: `src/cli.cppm`
- Modify: `tests/e2e/01_help_and_version.sh`
- Create: `tests/e2e/196_ide_snapshot.sh`

- [ ] **Step 1: Add failing help and CLI E2E assertions**

Update `tests/e2e/01_help_and_version.sh` to require `mcpp ide snapshot` in help.

Create `tests/e2e/196_ide_snapshot.sh` with `# requires:` on line 2. It must:

1. Create a minimal package in `mktemp -d` without calling `mcpp new`.
2. Run `"$MCPP" ide snapshot --format json`, capture stdout and stderr
   separately, and require exit 0.
3. Parse stdout with Python when present and assert one object with schema 1,
   kind, partial state, member `app`, selected member `app`, and missing CDB.
4. Require empty stderr and reject ANSI escape bytes in stdout.
5. Add `compile_commands.json`, rerun, and assert stale rather than ready.
6. Build a virtual two-member workspace and assert default all-member selection,
   `-p one` selection, and `--workspace` selection.
7. Assert unknown `-p`, invalid manifest, and `--format yaml` each return 3 and
   one JSON document containing the expected diagnostic code.

- [ ] **Step 2: Build and run the focused E2E to verify RED**

Run:

```bash
mcpp build
FRESH_MCPP=$(find "$PWD/target" -type f -path '*/bin/mcpp' -perm -111 | head -1)
MCPP="$FRESH_MCPP" bash tests/e2e/196_ide_snapshot.sh
```

Expected: unknown command `ide` or missing help entry.

- [ ] **Step 3: Implement the CLI command module**

Create `src/cli/cmd_ide.cppm`:

```cpp
module;
#include <cstdio>

export module mcpp.cli.cmd_ide;

import std;
import mcpplibs.cmdline;
import mcpp.ide.inspect;
import mcpp.ide.model;
import mcpp.ide.snapshot;

export namespace mcpp::cli {
int cmd_ide_snapshot(const mcpplibs::cmdline::ParsedArgs& parsed);
}
```

Parse CSV selector values with a local helper that drops empty items but preserves
order. Default `start` to `current_path()`. If `--format` is absent or `json`, run
the inspector; otherwise construct an unavailable inspection with
`MCPP_IDE_UNSUPPORTED_FORMAT`. Print exactly `snapshot_json(result)` to stdout
with `std::print`. Return 3 only for `Unavailable`, otherwise 0.

- [ ] **Step 4: Register the nested command**

In `src/cli.cppm`:

- import `mcpp.cli.cmd_ide`;
- add `mcpp ide snapshot` to canonical help;
- add an `ide` app with nested `snapshot` and all selectors from the design;
- route through existing `dispatch_sub("ide", ..., {{"snapshot",
  cmd_ide_snapshot}})`;
- add `ide` to the known-command array and update the array extent.

Do not add CLI business logic to `src/cli.cppm`.

- [ ] **Step 5: Rebuild and run focused tests/E2E to verify GREEN**

Run:

```bash
mcpp build
FRESH_MCPP=$(find "$PWD/target" -type f -path '*/bin/mcpp' -perm -111 | head -1)
"$FRESH_MCPP" test ide_snapshot
MCPP="$FRESH_MCPP" bash tests/e2e/01_help_and_version.sh
MCPP="$FRESH_MCPP" bash tests/e2e/196_ide_snapshot.sh
```

Expected: focused unit tests and both E2E scripts pass.

- [ ] **Step 6: Commit the CLI**

```bash
git add src/cli/cmd_ide.cppm src/cli.cppm tests/e2e/01_help_and_version.sh tests/e2e/196_ide_snapshot.sh
git commit -m "feat(cli): add read-only ide snapshot command"
```

### Task 5: Prove the Zero-side-effect Contract

**Files:**
- Create: `tests/e2e/197_ide_snapshot_read_only.sh`
- Modify: `tests/unit/test_ide_snapshot.cpp`

- [ ] **Step 1: Add the failing side-effect E2E**

Create `tests/e2e/197_ide_snapshot_read_only.sh` with `# requires:` on line 2. The
script must create a project containing:

- a remote-looking version dependency;
- `[generated_files]` output that would be written by a build;
- `build.mcpp` that writes a sentinel if executed;
- a toolchain declaration whose payload is absent from the isolated home.

Set `MCPP_HOME` to an empty sibling directory. Record sorted file paths and SHA-256
hashes for both project and home before invocation. Run snapshot in default mode
and again with target/profile/features/cap/include-dev selectors. Record the same
inventories afterward and require byte equality.

Explicitly assert absence of `.mcpp`, `.xlings.json`, `mcpp.lock`, `target`,
`compile_commands.json`, generated output, `build.mcpp` sentinel, and every file
under isolated `MCPP_HOME`.

- [ ] **Step 2: Run the E2E before any correction**

Run with the current fresh binary:

```bash
FRESH_MCPP=$(find "$PWD/target" -type f -path '*/bin/mcpp' -perm -111 | head -1)
MCPP="$FRESH_MCPP" bash tests/e2e/197_ide_snapshot_read_only.sh
```

Expected: pass if the architecture boundary is intact. If it fails, treat any new
file, network/install output, or build sentinel as a release-blocking defect and
remove the offending dependency/import/call from the IDE path.

- [ ] **Step 3: Add an in-process no-write unit assertion**

Add a test that snapshots a recursive inventory of the temporary project before
and after `inspect_workspace()` and compares paths, file sizes, and modification
times. This gives a fast local guard even when the E2E hash utility varies by OS.

- [ ] **Step 4: Run focused unit and side-effect E2E**

Run `mcpp test ide_snapshot`, then the fresh-binary E2E command above.

Expected: both pass; no project/home additions or modifications.

- [ ] **Step 5: Commit the side-effect proof**

```bash
git add tests/unit/test_ide_snapshot.cpp tests/e2e/197_ide_snapshot_read_only.sh
git commit -m "test(ide): prove snapshot is read only"
```

### Task 6: Full Verification and Contribution Handoff

**Files:**
- Modify only files required by failures attributable to this branch

- [ ] **Step 1: Check formatting and scope**

Run:

```bash
git diff origin/main --check
git status --short
git diff origin/main --stat
```

Expected: no whitespace errors and only the files in this plan.

- [ ] **Step 2: Self-host a fresh binary**

Run `mcpp build`.

Expected: successful release-profile self-host build. Locate the binary without
hardcoding the host triple:

```bash
find "$PWD/target" -type f -path '*/bin/mcpp' -perm -111 -print
```

- [ ] **Step 3: Run the full unit/integration suite with the fresh binary**

```bash
FRESH_MCPP=$(find "$PWD/target" -type f -path '*/bin/mcpp' -perm -111 | head -1)
"$FRESH_MCPP" test
```

Expected: all test binaries pass with zero failures.

- [ ] **Step 4: Run focused E2E with the fresh binary**

```bash
MCPP="$FRESH_MCPP" bash tests/e2e/01_help_and_version.sh
MCPP="$FRESH_MCPP" bash tests/e2e/196_ide_snapshot.sh
MCPP="$FRESH_MCPP" bash tests/e2e/197_ide_snapshot_read_only.sh
```

Expected: all three scripts print `OK` and exit 0.

- [ ] **Step 5: Inspect the protocol manually**

Run:

```bash
"$FRESH_MCPP" ide snapshot --format json
```

Expected: one JSON object, schema 1, state `stale` for this already-built
worktree, and no human status text on stdout.

- [ ] **Step 6: Review commits and working tree**

```bash
git log --oneline origin/main..HEAD
git status --short --branch
```

Expected: focused design/model/inspection/serialization/CLI/test commits and a
clean worktree.

- [ ] **Step 7: Prepare the external contribution handoff**

The repository contribution guide requires a GitHub Issue and PR, but publishing
external state is outside the current local-only authorization. Report the local
branch, commit list, verification evidence, and the exact remaining Issue/PR
actions. Do not push or create remote objects without explicit authorization.
