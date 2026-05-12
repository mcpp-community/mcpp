# 工作空间 (Workspace)

工作空间允许在同一个仓库中组织和管理多个相关的 mcpp 包（库或应用程序）。各成员包共享统一的依赖版本配置和工具链设置，同时保持独立的 `mcpp.toml` 工程文件。

## 1. 概述

工作空间解决以下问题：

- **依赖版本统一管理** — 多个子包使用相同版本的第三方依赖，避免重复声明和版本不一致
- **工具链配置共享** — 在工作空间根目录统一声明工具链，各成员继承或覆盖
- **多包协同开发** — 库与应用在同一仓库中开发，通过 `path` 依赖相互引用

工作空间不改变依赖声明方式。成员之间通过已有的 `path = "..."` 机制声明依赖关系，与非工作空间项目的用法完全一致。

## 2. 工程文件结构

### 2.1 工作空间根

在仓库根目录的 `mcpp.toml` 中声明 `[workspace]`：

```toml
[workspace]
members = [
    "libs/core",
    "libs/http",
    "apps/server",
]
```

`members` 列出各成员包的相对路径，每个路径下须包含独立的 `mcpp.toml`。

可选 `exclude` 字段排除特定路径：

```toml
[workspace]
members = ["libs/*"]
exclude = ["libs/experimental"]
```

### 2.2 虚拟工作空间与根包工作空间

**虚拟工作空间**：根 `mcpp.toml` 仅包含 `[workspace]`，不包含 `[package]`。根目录不产出构建产物，仅作为管理节点。

```toml
# 虚拟工作空间 — 只有 [workspace]
[workspace]
members = ["libs/core", "apps/server"]
```

**根包工作空间**：根 `mcpp.toml` 同时包含 `[package]` 和 `[workspace]`。根目录本身也是一个可构建的包。

```toml
[workspace]
members = ["libs/core"]

[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
core = { path = "libs/core" }
```

### 2.3 成员工程文件

各成员维护独立的 `mcpp.toml`，结构与普通项目一致：

```toml
# libs/core/mcpp.toml
[package]
namespace = "myproject"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
```

成员之间通过 `path` 依赖引用：

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

## 3. 依赖版本继承

在 `[workspace.dependencies]` 中集中声明依赖版本，成员通过 `.workspace = true` 继承：

```toml
# 根 mcpp.toml
[workspace.dependencies]
cmdline = "0.0.2"

[workspace.dependencies.compat]
mbedtls = "3.6.1"
gtest   = "1.15.2"
```

```toml
# 成员 mcpp.toml
[dependencies.compat]
mbedtls.workspace = true    # 继承版本 → "3.6.1"

[dev-dependencies.compat]
gtest.workspace = true      # 继承版本 → "1.15.2"
```

成员可以覆盖继承的版本：

```toml
[dependencies.compat]
mbedtls = "4.0.0"          # 覆盖，不使用 workspace 版本
```

## 4. 工具链与构建配置继承

工作空间根的 `[toolchain]` 和 `[target.<triple>]` 配置自动继承到所有成员。成员可在自身的工程文件中覆盖。

配置优先级（从高到低）：

1. 命令行参数（`--target`、`--static`）
2. 成员 `mcpp.toml` 中的声明
3. 工作空间根 `mcpp.toml` 中的声明
4. 全局配置（`~/.mcpp/config.toml`）
5. 内置默认值

```toml
# 工作空间根
[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
```

```toml
# 某成员覆盖工具链
[toolchain]
default = "clang@19.0"
```

## 5. 构建命令

### 5.1 从工作空间根目录构建

```bash
mcpp build                  # 构建默认目标（自动选择含二进制目标的成员）
mcpp build -p server        # 构建指定成员及其依赖
mcpp build -p core          # 构建指定库成员
```

### 5.2 从成员子目录构建

```bash
cd libs/http
mcpp build                  # 自动检测工作空间，构建当前成员
```

mcpp 从当前目录向上搜索，若发现包含 `[workspace]` 的 `mcpp.toml` 且当前目录在 `members` 列表中，则自动进入工作空间模式，继承工作空间配置。

### 5.3 `-p, --package` 选项

`-p` 可用于 `build`、`test`、`run` 等命令，指定构建的目标成员。参数值为成员路径的最后一段目录名或完整相对路径：

```bash
mcpp build -p server        # 匹配 apps/server
mcpp test -p core           # 匹配 libs/core
mcpp run -p server -- --port 8080
```

## 6. 目录布局

工作空间推荐的目录布局：

```
myproject/
├── mcpp.toml               # [workspace] 声明
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

各成员的构建产物位于各自的 `target/` 子目录下。

## 7. 与 C++ 模块的关系

工作空间与 C++23 模块机制协同工作：

- **接口可见性由语言控制** — `export module` 和 `import` 语句决定模块的公开接口，工作空间不做额外的可见性限制
- **模块名由库作者决定** — 工作空间不强制模块名与包名或命名空间一致
- **partition 用于内部组织** — `import :internal;`（不带 `export`）的 partition 对消费者不可见，无需构建工具介入

## 8. 完整示例

参见 [`examples/04-workspace/`](../examples/04-workspace/)，包含一个三成员工作空间的完整可运行示例。
