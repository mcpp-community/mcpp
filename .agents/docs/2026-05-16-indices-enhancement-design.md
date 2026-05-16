# `[indices]` 功能增强设计方案

> 2026-05-16 — 让 mcpp.toml 可配置自定义包索引仓库
> 基于 2026-05-08 设计稿精简，聚焦可落地的最小方案
> v2: 修正 namespace 语义 + 项目级隔离方案

## 0. 一句话

在 `mcpp.toml` 中新增 `[indices]` 段，key = index 名（对应 xlings repo name），包描述文件内部可声明 namespace，未声明则回退 index 名。mcpp 自动处理 xlings 项目级隔离，用户零 xlings 知识。

## 1. 现状问题

```cpp
// config.cppm:429-455 — 硬编码
cfg.defaultIndex = "mcpplibs";
add_default("mcpplibs", "https://github.com/mcpp-community/mcpp-index.git");

// xlings.cppm:92 — 强制禁用项目级 index
"env -u XLINGS_PROJECT_DIR ..."
```

- 用户不能添加自己的包索引
- 企业私有包无法发布/使用
- `index_spec.cppm` 是空壳 placeholder
- 所有 index 都是全局的，不同项目会互相污染

## 2. 核心语义

### 2.1 Key ≠ namespace，Key = index 名

`[indices]` 的 key 是 **index 仓库的名字**（对应 xlings 的 `{name, url}`），**不是** namespace。

namespace 的来源（优先级从高到低）：
1. 包描述文件（xpkg.lua）内部声明的 `namespace` 字段
2. 若未声明 → 回退为该包所在的 **index 名**

```toml
# mcpp.toml
[indices]
acme = "git@gitlab.example.com:platform/mcpp-index.git"
```

```
acme index 仓内：
  pkgs/f/foo.lua  → 内部 namespace = "myteam"  → ns = "myteam"
  pkgs/b/bar.lua  → 内部无 namespace 字段      → ns = "acme"（回退 index 名）
```

### 2.2 依赖声明

```toml
[dependencies]
# 不指定 ns → 查 default index（mcpplibs），和现在一样
gtest = "1.15.2"

# 指定 ns → 按 ns 查找（可能来自任何 index）
[dependencies.myteam]
foo = "1.0.0"           # foo.lua 在 acme index 内，但声明了 ns = "myteam"

[dependencies.acme]
bar = "2.0.0"           # bar.lua 在 acme index 内，未声明 ns，回退 "acme"
```

### 2.3 查找行为

与现有逻辑一致：
- 有 ns → 按 ns 查对应包（ns 可能来自 index 内的声明，也可能回退 index 名）
- 无 ns → 查 default index（mcpplibs）

## 3. 用户接口

### 3.1 `mcpp.toml` 的 `[indices]` 段

```toml
[indices]
# 短形式：value = git URL，key = index 名
acme = "git@gitlab.example.com:platform/mcpp-index.git"

# 长形式：指定 git 精确版本
acme-stable = { url = "git@gitlab.example.com:...", tag = "v2.0" }

# 本地路径：开发/测试用
local-dev = { path = "/home/user/my-packages" }

# 锁定官方索引到特定 commit（可选）
mcpplibs = { url = "https://github.com/mcpp-community/mcpp-index.git", rev = "abc123" }
```

**规则**：
- key = index 名（对应 xlings 的 repo name + url）
- `mcpplibs` 内置默认，不写则自动存在，走全局路径
- 项目 `mcpp.toml` > 全局 `~/.mcpp/config.toml` > 内置默认

### 3.2 Index 仓库目录结构

**与官方 mcpp-index 完全一致**：

```
my-index/
├── pkgs/
│   ├── c/
│   │   └── cmdline.lua        ← 标准 xpkg.lua 格式
│   ├── m/
│   │   └── mylib.lua          ← 内部可选 namespace = "xxx"
│   └── ...
└── README.md                  ← 可选
```

不引入新格式，不破坏 xlings 生态。

### 3.3 用户体验

```bash
# 用户只需要做这些：
$ cat mcpp.toml
[indices]
acme = "git@gitlab.example.com:platform/mcpp-index.git"

[dependencies.acme]
internal-lib = "2.0.0"

$ mcpp build
# mcpp 自动处理一切：
#   1. 检测 [indices] 有自定义 index
#   2. 创建 .mcpp/ 目录（项目级 xlings 环境）
#   3. 生成 .mcpp/.xlings.json
#   4. 调用 xlings 时设置 XLINGS_PROJECT_DIR=.mcpp/
#   5. xlings clone index + 安装包到 .mcpp/ 下
#   6. 全局 mcpplibs 仍走全局路径，不受影响
```

用户**零 xlings 知识**，只写 `mcpp.toml`。

## 4. 项目级隔离方案

### 4.1 核心机制

利用 xlings 已有的 `XLINGS_PROJECT_DIR` 概念：

```
全局（内置 index，所有项目共享）：
  XLINGS_HOME = ~/.mcpp/registry/
  .xlings.json → index_repos: [{name: "mcpplibs", url: "..."}]
  data/mcpplibs-pkgindex/pkgs/...    ← index clone
  data/xpkgs/mcpplibs-x-gtest/...   ← 包 payload

项目级（自定义 index，仅当前项目可见）：
  XLINGS_PROJECT_DIR = <project>/.mcpp/
  .xlings.json → index_repos: [{name: "acme", url: "..."}]
  data/acme-pkgindex/pkgs/...        ← index clone（在 .mcpp/ 下）
  data/xpkgs/acme-x-internal-lib/... ← 包 payload（在 .mcpp/ 下）
```

### 4.2 调用 xlings 的两种模式

```cpp
// 场景 1：访问全局 index（mcpplibs 等内置 index）
// 与现有行为一致
"env -u XLINGS_PROJECT_DIR XLINGS_HOME='~/.mcpp/registry' xlings ..."

// 场景 2：访问项目级 index（mcpp.toml [indices] 声明的自定义 index）
// 新增：设置 XLINGS_PROJECT_DIR
"XLINGS_HOME='~/.mcpp/registry' XLINGS_PROJECT_DIR='<project>/.mcpp' xlings ..."
```

### 4.3 项目目录结构

```
my-project/
├── mcpp.toml               ← 用户写的
├── mcpp.lock                ← 自动生成
├── .mcpp/                   ← 自动生成（应加入 .gitignore）
│   ├── .xlings.json         ← mcpp 自动生成，用户不碰
│   └── data/                ← xlings 管理的 clone 数据 + 包 payload
│       ├── acme-pkgindex/   ← index clone
│       │   └── pkgs/...
│       └── xpkgs/           ← 包安装目录
│           └── acme-x-internal-lib/
├── src/
└── ...
```

### 4.4 mcpp 自动处理流程

```
mcpp build
  │
  ├── 1. parse mcpp.toml → manifest.indices
  │
  ├── 2. 分类 indices：
  │       内置 index（mcpplibs）→ 走全局路径，不变
  │       自定义 index           → 走项目级隔离
  │
  ├── 3. 若有自定义 index：
  │       a. 创建 <project>/.mcpp/ 目录
  │       b. 生成/更新 .mcpp/.xlings.json（只含自定义 index 条目）
  │       c. 后续对这些 index 的操作带 XLINGS_PROJECT_DIR
  │
  ├── 4. for each dep (ns, name, version)：
  │       a. 确定 dep 属于哪个 index
  │       b. 内置 index → 全局 xlings 环境
  │       c. 自定义 index → 项目级 xlings 环境
  │       d. resolve + install（xlings 自动处理 clone/cache）
  │
  └── 5. 构建（不变）
```

### 4.5 隔离性保证

| 场景 | 行为 |
|---|---|
| 项目 A: `acme = "git@.../team-a/index.git"` | `.mcpp/` 在项目 A 目录下，只 A 可见 |
| 项目 B: `acme = "git@.../team-b/index.git"` | `.mcpp/` 在项目 B 目录下，只 B 可见 |
| 两个项目同时 build | 完全隔离，互不干扰 |
| `mcpplibs` 依赖 | 走全局 `~/.mcpp/registry/`，所有项目共享 |

## 5. 数据结构

### 5.1 IndexSpec（填充 `index_spec.cppm`）

```cpp
// src/pm/index_spec.cppm
export module mcpp.pm.index_spec;
import std;

export namespace mcpp::pm {

struct IndexSpec {
    std::string name;                   // index 名（[indices] 的 key）
    std::string url;                    // git URL（短形式直接填这里）
    std::string rev;                    // 完整 commit sha（最强锁）
    std::string tag;                    // git tag
    std::string branch;                 // git branch
    std::filesystem::path path;         // 本地路径（优先于 url）

    bool is_local() const { return !path.empty(); }
    bool is_pinned() const { return !rev.empty(); }
    bool is_builtin() const { return name == "mcpplibs"; }
};

// 解析 mcpp.toml [indices] 段
std::map<std::string, IndexSpec>
parse_indices(const toml::Table& doc);

// 合并：project > global > built-in default
std::map<std::string, IndexSpec>
merge_indices(const std::map<std::string, IndexSpec>& project,
              const std::map<std::string, IndexSpec>& global);

} // namespace mcpp::pm
```

### 5.2 Manifest 扩展

```cpp
// src/manifest.cppm — Manifest 新增字段
struct Manifest {
    ...existing...
    std::map<std::string, mcpp::pm::IndexSpec> indices;  // index-name → spec
};
```

### 5.3 GlobalConfig 扩展

```cpp
// src/config.cppm
struct GlobalConfig {
    ...existing...
    std::string defaultIndex = "mcpplibs";
    std::map<std::string, mcpp::pm::IndexSpec> indices;  // 全局级
};
```

## 6. xlings 集成改造

### 6.1 现有代码

```cpp
// xlings.cppm:92 — 当前硬编码禁用项目级
"env -u XLINGS_PROJECT_DIR ..."
```

### 6.2 改造方案

```cpp
// xlings.cppm — 新增项目级环境构建

struct Env {
    std::filesystem::path binary;
    std::filesystem::path home;                   // 全局 XLINGS_HOME
    std::filesystem::path projectDir;             // 项目级（可选，空 = 不用）
};

std::string build_command_prefix(const Env& env) {
    std::string cmd = std::format(
        "cd '{}' && env PATH=...:'$PATH' XLINGS_HOME='{}'",
        env.home.string(), env.home.string());

    if (env.projectDir.empty()) {
        cmd += " -u XLINGS_PROJECT_DIR";          // 全局模式（现有行为）
    } else {
        cmd += std::format(" XLINGS_PROJECT_DIR='{}'",
                           env.projectDir.string()); // 项目级模式（新增）
    }
    cmd += std::format(" '{}'", env.binary.string());
    return cmd;
}
```

### 6.3 .mcpp/.xlings.json 自动生成

```cpp
// 新增：src/pm/project_index.cppm 或在 config.cppm 中

// 根据 mcpp.toml [indices] 生成项目级 .xlings.json
void ensure_project_xlings_json(
    const std::filesystem::path& projectDir,          // 项目根目录
    const std::map<std::string, IndexSpec>& indices)   // 自定义 indices
{
    auto dotMcpp = projectDir / ".mcpp";
    std::filesystem::create_directories(dotMcpp);

    // 只写入非内置的自定义 index
    std::vector<IndexRepo> customRepos;
    for (auto& [name, spec] : indices) {
        if (spec.is_builtin()) continue;
        if (spec.is_local()) continue;  // 本地 path 不需要 xlings clone
        customRepos.push_back({name, spec.url});
    }

    if (customRepos.empty()) return;  // 无自定义 index，不创建 .mcpp/

    write_xlings_json(dotMcpp / ".xlings.json", customRepos);
}
```

## 7. Fetcher 改造

### 7.1 双环境调用

```cpp
class Fetcher {
    // 全局环境（内置 index）
    xlings::Env globalEnv_;

    // 项目级环境（自定义 index），惰性初始化
    std::optional<xlings::Env> projectEnv_;

    // 根据 index 名选择环境
    xlings::Env& env_for(const IndexSpec& spec) {
        if (spec.is_builtin()) return globalEnv_;
        if (!projectEnv_) {
            projectEnv_ = xlings::Env{
                globalEnv_.binary,
                globalEnv_.home,
                projectDir_ / ".mcpp"
            };
        }
        return *projectEnv_;
    }
};
```

### 7.2 read_xpkg_lua 路由

对于自定义 index，查找路径变为：

```
项目级：<project>/.mcpp/data/<index-name>-pkgindex/pkgs/<letter>/<name>.lua
全局级：~/.mcpp/registry/data/<index-name>-pkgindex/pkgs/<letter>/<name>.lua
```

根据 dep 的 ns 确定 index，再确定查找路径。

### 7.3 包安装路由

同理，自定义 index 的包 payload 安装到：

```
<project>/.mcpp/data/xpkgs/<index>-x-<name>/<version>/
```

而非全局的 `~/.mcpp/registry/data/xpkgs/`。

## 8. Lockfile v2

### 8.1 Schema

```toml
version = 2

[indices.mcpplibs]
url = "https://github.com/mcpp-community/mcpp-index.git"
rev = "abc123def0123456789abcdef0123456789abcd"

[indices.acme]
url = "git@gitlab.example.com:platform/mcpp-index.git"
rev = "0123456789abcdef0123456789abcdef01234567"

[package."gtest"]
namespace = "mcpplibs"
version = "1.15.2"
source = "index+mcpplibs@abc123def..."
hash = "sha256:..."

[package."acme.internal-lib"]
namespace = "acme"
version = "2.0.0"
source = "index+acme@0123456789..."
hash = "sha256:..."
```

### 8.2 v1 → v2 迁移

读到 `version = 1` 时：
- 所有包的 source 视为 `mcpplibs` namespace
- 下次 build 自动补充 `[indices.mcpplibs]` 段（触发一次 git resolve）
- 写回 `version = 2`

### 8.3 锁定语义

- `mcpp build/run/test/pack`：只读 lock 的 sha，**不联网**
- `mcpp index update [<ns>]`：重新 resolve → 写入新 sha
- `mcpp update [<pkg>]`：重新解析版本约束（自动先 index update）
- `mcpp index pin <ns>`：把 lock 里的 sha 写回 mcpp.toml 的 `[indices.<ns>].rev`
- `mcpp index unpin <ns>`：从 mcpp.toml 删除 rev 字段

## 9. CLI 命令

```bash
mcpp index list                    # 列出所有索引（标注来源：project/global/built-in）
mcpp index update [<ns>...]        # 拉远端最新 sha，写回 lock
mcpp index pin <ns> [<rev>]        # 硬锁到 mcpp.toml
mcpp index unpin <ns>              # 从 mcpp.toml 删除 rev
```

## 10. 待验证事项

以下 xlings `XLINGS_PROJECT_DIR` 的细节行为需要实际验证：

| 问题 | 预期 | 需验证 |
|---|---|---|
| `XLINGS_PROJECT_DIR` 下的 `data/` 是否自动创建 | 是 | ✅ |
| 项目级和全局级的 index 是否可以并存 | 项目级叠加在全局之上 | ⚠️ 需确认是叠加还是覆盖 |
| 全局已安装的包在项目级环境中是否可见 | 应该可见 | ⚠️ 关键：影响 mcpplibs 包的可见性 |
| xlings install 在项目级环境中写到哪里 | `<project>/.mcpp/data/xpkgs/` | ⚠️ 需确认 |
| 本地 path index 是否需要 xlings 管理 | 不需要，mcpp 直接读 pkgs/ | 设计决策 |
| xlings interface 命令是否支持 `XLINGS_PROJECT_DIR` | 应该支持 | ⚠️ 需确认 |

**建议**：PR-1 先做 TOML 解析（不接 xlings），PR-2 时做 xlings 行为验证。

## 11. 落地步骤（5 个 PR）

### PR-1：IndexSpec 数据结构 + TOML 解析

**改动文件**：
- `src/pm/index_spec.cppm`：填充 IndexSpec + parse/merge 函数
- `src/manifest.cppm`：Manifest 新增 `indices` 字段 + `[indices]` 解析
- `src/config.cppm`：GlobalConfig 新增 `indices` map + config.toml `[indices]` 解析
- `tests/unit/test_index_spec.cpp`：解析测试（短形式/长形式/path/rev/tag/branch）

**不影响构建**：解析结果暂不接入 fetcher，默认值兜底。

### PR-2：项目级隔离 + xlings 行为验证

**新增文件**：
- `src/pm/project_index.cppm`：`ensure_project_xlings_json()` + `.mcpp/` 管理

**改动文件**：
- `src/xlings.cppm`：`Env` 支持 `projectDir` 字段 + 命令构建双模式
- `src/config.cppm`：初始化时处理 `.mcpp/` 目录

**验证**：手动测试 xlings `XLINGS_PROJECT_DIR` 的叠加/覆盖行为

**E2E 测试**：用本地 path 索引（避免网络依赖）

### PR-3：Fetcher 按 index 路由

**改动文件**：
- `src/pm/package_fetcher.cppm`：双环境调用 + read_xpkg_lua 路由
- `src/cli.cppm`：`prepare_build` 中根据 dep ns 确定 index → 选择环境

**兼容**：无 `[indices]` 的项目走原有路径，行为不变。

### PR-4：Lockfile v2

**改动文件**：
- `src/pm/lock_io.cppm`：新增 `[indices.<ns>]` 读写 + v1→v2 迁移
- `src/lockfile.cppm`：LockedPackage 新增 namespace 字段

### PR-5：CLI 命令 + 文档

**改动文件**：
- `src/cli.cppm`：新增 `mcpp index list/update/pin/unpin` 子命令

## 12. 关键设计决策

| 决策 | 选择 | 原因 |
|---|---|---|
| 包描述格式 | 只 xpkg.lua | 不破坏 xlings 生态 |
| Key 语义 | index 名（非 namespace） | 对应 xlings 的 {name, url}，包内部声明 ns |
| ns 回退 | 包未声明 ns → 回退 index 名 | 直觉一致，简化配置 |
| 隔离方案 | 项目级 `XLINGS_PROJECT_DIR` | 不同项目互不污染，数据在 .mcpp/ 下 |
| 用户感知 | 零 xlings 知识 | mcpp 自动处理 .mcpp/ 和 .xlings.json |
| 全局 index | 不走项目级隔离 | mcpplibs 等内置 index 全局共享，节省磁盘 |
| 本地 path | mcpp 直接读，不经 xlings | 开发场景，无需 clone |
| Lockfile | 升级到 v2 | 记录 index sha + dep namespace |

## 13. 不做的事

- 不引入新的包描述格式（只用 xpkg.lua）
- 不做 HTTP API 后端（只 git + local path）
- 不做索引签名校验（后续独立 PR）
- 不做跨 namespace 同名包冲突处理（后续独立 PR）
- 不改 xlings 核心的包安装/拉取流程

## 14. 与现有代码的映射

| 改动 | 当前代码 | 位置 |
|---|---|---|
| IndexSpec 数据结构 | placeholder 空壳 | `src/pm/index_spec.cppm:1-19` |
| Manifest.indices | 不存在 | `src/manifest.cppm` Manifest struct |
| config.toml [indices] | `[index.repos.NAME]` 只有 url | `src/config.cppm:436-444` |
| GlobalConfig.indexRepos | `vector<IndexRepo>{name,url}` | `src/config.cppm:30-33, 52` |
| 默认 index 注入 | `add_default("mcpplibs", url)` | `src/config.cppm:451-455` |
| xlings 命令构建 | 硬编码 `-u XLINGS_PROJECT_DIR` | `src/xlings.cppm:92` |
| xpkg.lua 查找 | 全局扫描 data/ 下所有 pkgs/ | `src/pm/package_fetcher.cppm:392-407` |
| Lockfile schema | v1，无 indices 段 | `src/pm/lock_io.cppm` |
