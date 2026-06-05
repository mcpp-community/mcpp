# Workspace

A workspace lets you organize and manage multiple related mcpp packages (libraries or applications) within a single repository. Member packages share a unified set of dependency versions and toolchain settings while each keeping its own `mcpp.toml` project file.

## 1. Overview

Workspaces address the following problems:

- **Unified dependency-version management** — multiple sub-packages use the same versions of third-party dependencies, avoiding duplicate declarations and version drift.
- **Shared toolchain configuration** — declare the toolchain once at the workspace root; members inherit it or override it as needed.
- **Multi-package co-development** — libraries and applications are developed in the same repository and reference one another through `path` dependencies.

A workspace does not change how dependencies are declared. Members reference one another through the existing `path = "..."` mechanism, exactly as in a non-workspace project.

## 2. Project File Structure

### 2.1 The Workspace Root

Declare `[workspace]` in the `mcpp.toml` at the repository root:

```toml
[workspace]
members = [
    "libs/core",
    "libs/http",
    "apps/server",
]
```

`members` lists the relative path of each member package; every such path must contain its own `mcpp.toml`.

The optional `exclude` field excludes specific paths:

```toml
[workspace]
members = ["libs/*"]
exclude = ["libs/experimental"]
```

### 2.2 Virtual Workspaces vs. Root-Package Workspaces

**Virtual workspace**: the root `mcpp.toml` contains only `[workspace]` and no `[package]`. The root produces no build artifacts and serves purely as a management node.

```toml
# Virtual workspace — [workspace] only
[workspace]
members = ["libs/core", "apps/server"]
```

**Root-package workspace**: the root `mcpp.toml` contains both `[package]` and `[workspace]`. The root itself is also a buildable package.

```toml
[workspace]
members = ["libs/core"]

[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
core = { path = "libs/core" }
```

### 2.3 Member Project Files

Each member maintains its own `mcpp.toml`, structured just like a regular project:

```toml
# libs/core/mcpp.toml
[package]
namespace = "myproject"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
```

Members reference one another through `path` dependencies:

```toml
# libs/http/mcpp.toml
[package]
namespace = "myproject"
name      = "http"
version   = "0.1.0"

[dependencies]
core = { path = "../core" }

[dependencies.compat]
mbedtls.workspace = true
```

## 3. Inheriting Dependency Versions

Declare dependency versions centrally under `[workspace.dependencies]`; members inherit them with `.workspace = true`:

```toml
# root mcpp.toml
[workspace.dependencies]
cmdline = "0.0.2"
capi.lua = "0.0.3"       # dotted selector: mcpplibs.capi/lua, then capi/lua

[workspace.dependencies.compat]
mbedtls = "3.6.1"
gtest   = "1.15.2"
```

```toml
# member mcpp.toml
[dependencies.compat]
mbedtls.workspace = true    # inherits version → "3.6.1"

[dev-dependencies.compat]
gtest.workspace = true      # inherits version → "1.15.2"
```

A member can override an inherited version:

```toml
[dependencies.compat]
mbedtls = "4.0.0"          # override; does not use the workspace version
```

## 4. Inheriting Toolchain and Build Configuration

The workspace root's `[toolchain]` and `[target.<triple>]` settings are automatically inherited by all members. A member can override them in its own project file.

Configuration precedence (highest to lowest):

1. Command-line arguments (`--target`, `--static`)
2. Declarations in the member `mcpp.toml`
3. Declarations in the workspace-root `mcpp.toml`
4. Global configuration (`~/.mcpp/config.toml`)
5. Built-in defaults

```toml
# workspace root
[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
```

```toml
# a member overrides the toolchain
[toolchain]
default = "clang@19.0"
```

## 5. Build Commands

### 5.1 Building from the Workspace Root

```bash
mcpp build                  # build the default target (auto-selects the member with a binary target)
mcpp build -p server        # build a specific member and its dependencies
mcpp build -p core          # build a specific library member
```

### 5.2 Building from a Member Subdirectory

```bash
cd libs/http
mcpp build                  # auto-detects the workspace and builds the current member
```

mcpp searches upward from the current directory; if it finds an `mcpp.toml` containing `[workspace]` and the current directory is listed in `members`, it automatically enters workspace mode and inherits the workspace configuration.

### 5.3 The `-p, --package` Option

`-p` works with `build`, `test`, `run`, and other commands to select the target member. Its value is either the last path segment of a member's directory name or the full relative path:

```bash
mcpp build -p server        # matches apps/server
mcpp test -p core           # matches libs/core
mcpp run -p server -- --port 8080
```

## 6. Directory Layout

The recommended directory layout for a workspace:

```
myproject/
├── mcpp.toml               # [workspace] declaration
├── libs/
│   ├── core/
│   │   ├── mcpp.toml       # [package] namespace="myproject" name="core"
│   │   └── src/
│   │       └── core.cppm   # export module myproject.core;
│   └── http/
│       ├── mcpp.toml
│       └── src/
│           └── http.cppm   # export module myproject.http;
└── apps/
    └── server/
        ├── mcpp.toml
        └── src/
            └── main.cpp    # import myproject.http;
```

Each member's build artifacts live under its own `target/` subdirectory.

## 7. Relationship to C++ Modules

Workspaces work in concert with the C++23 module mechanism:

- **Interface visibility is controlled by the language** — `export module` and `import` statements determine a module's public interface; the workspace imposes no additional visibility restrictions.
- **Module names are chosen by the library author** — the workspace does not require module names to match the package name or namespace.
- **Partitions are for internal organization** — a partition imported via `import :internal;` (without `export`) is invisible to consumers, with no build-tool involvement required.

## 8. Complete Example

See [`examples/04-workspace/`](../examples/04-workspace/) for a complete, runnable example of a three-member workspace.
