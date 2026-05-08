# 2026-05-08 — 包索引仓库配置 (Package-Index Repo Configuration)

> **状态**:设计稿 (待实现)
> **依赖**:命名空间支持(PR-A,issue/PR 待合)
> **目标读者**:mcpp 维护者 + 早期采用者

## 1. 背景与动机

mcpp 当前**硬编码**单一包索引仓库 `mcpp-community/mcpp-index.git`,在
`fetcher` 启动时按固定 URL 拉取,且不带版本锁定。这带来三个问题:

1. **不可复现**——索引仓的 `main` 分支随时变;同一份 `mcpp.toml` 在不
   同时间 `mcpp build` 可能拿到不同版本的 `gtest@1.15.2`(如果索引里
   修改了 sha256 / 重写了 url)。
2. **不能私有化**——企业内部要发布私有包,只能 fork 整个 mcpp 然后改
   常量,迁移成本高。
3. **不能多源**——命名空间已在 PR-A 引入,但每个 namespace 仍只能落
   在同一个官方仓里。`mcpp.toml` 里写 `[dependencies.acme] foo = ".."`
   时,mcpp 不知道去哪找 `acme` 的索引。

xim/xlings 多年前就解决过类似问题——`xim-pkgindex-*` 多仓 + 每仓自带
namespace。**mcpp 这次的方案要尽量复用 xpkg 模型**(参见
2026-05-08 的 namespace 设计),同时在工程描述层面给用户暴露一个
极简的 TOML 配置入口。

## 2. 用户接口

### 2.1 `mcpp.toml` 的 `[indices]` 段

```toml
[indices]
# 1. 默认官方索引(隐式存在,显式声明可锁定 commit)。
mcpp = { url = "https://github.com/mcpp-community/mcpp-index.git", rev = "abc123def" }

# 2. 第二方索引(开源生态)。
mcpplibs = { url = "https://github.com/mcpplibs/mcpp-index.git", tag = "v0.3.0" }

# 3. 私有索引(企业内网)。短形式 = 跟踪默认分支(等价 branch = "main")。
acme = "git@gitlab.example.com:platform/mcpp-index.git"

# 4. 跟踪特定分支(适合开发期,生产请改 tag/rev)。
acme-edge = { url = "git@gitlab.example.com:platform/mcpp-index.git", branch = "edge" }
```

**键 = 命名空间名**。表内字段:

| 字段 | 类型 | 说明 |
|---|---|---|
| `url` | string | git URL(必填,除非整个表用短形式) |
| `rev` | string | 完整 commit SHA(40 字符)或唯一前缀。最强锁定。 |
| `tag` | string | 标签名。等同 `rev=tag^{}`。 |
| `branch` | string | 分支名。**追踪式**——`mcpp index update` 会拉新。 |
| `path` | string | 本地路径(可选,绕过 git,适合开发本地索引)。 |

**精确性等级**(从强到弱):`rev > tag > branch`。`rev` 哈希提供完
全可复现;`tag` 默认假设不可变(警告但允许 force-pushed tag);
`branch` 写入 `mcpp.lock` 时会快照实际 sha,即"声明上跟踪 branch,但
本次构建用的是 sha X"。

短形式 `acme = "<url>"` ≡ `acme = { url = "<url>", branch = "main" }`(以
仓库默认分支为准)。

### 2.2 默认索引(`[indices]` 缺失时)

```
mcpp = "https://github.com/mcpp-community/mcpp-index.git" (默认分支)
```

显式声明 `[indices.mcpp]` 后,该项**完全替换**默认值——这意味着
锁定官方索引到固定 commit 的写法就是:

```toml
[indices]
mcpp = { rev = "abc123def" }    # url 省略 → 用默认 URL
```

`url` 字段在 mcpp 内置默认值后允许省略,这样大多数用户只需要写一行
即可锁版本。

### 2.3 全局配置兜底(`~/.mcpp/config.toml`)

为了避免每个项目都重复写企业内网索引,mcpp 同时读 `~/.mcpp/config.toml`
里的 `[indices]`(已存在,扩展):

```toml
# ~/.mcpp/config.toml
[indices]
acme = { url = "git@gitlab.example.com:platform/mcpp-index.git", branch = "main" }
```

合并规则:**项目 `mcpp.toml` > 全局 `config.toml` > 内置默认**。命名
空间冲突以最近一层为准。

### 2.4 CLI 命令

| 命令 | 行为 |
|---|---|
| `mcpp index list` | 列出所有索引(标注来源:project / global / built-in)。 |
| `mcpp index update [<ns>...]` | 拉取索引最新提交。`branch` 跟踪式按 `git pull --ff-only`;`rev`/`tag` no-op。 |
| `mcpp index pin <ns> [<rev>]` | 把当前索引解析到的 commit 写回 `mcpp.toml` 作为 `rev`。空 `<rev>` = 用当前 HEAD。 |
| `mcpp index unpin <ns>` | 反向操作:删除 `rev`,改回 `branch="main"`。 |

## 3. 内部模型

### 3.1 数据结构

```cpp
// src/manifest.cppm
struct IndexSpec {
    std::string namespace_;     // 表 key
    std::string url;             // 解析时填补默认 URL
    std::string rev;             // 完整 sha(若有)
    std::string tag;             // 若 rev 为空但 tag 非空
    std::string branch;          // 若 rev/tag 都空
    std::filesystem::path path;  // 本地索引(测试用)
};
struct Manifest {
    ...
    std::map<std::string, IndexSpec> indices;   // ns → spec
};
```

### 3.2 索引存储

```
~/.mcpp/index/
├── mcpp/                                   # ns dir
│   ├── abc123def.../                       # commit-pinned checkout
│   └── HEAD -> ./abc123def.../             # symlink to active checkout
├── mcpplibs/
│   └── ...
└── acme/
    └── ...
```

每个 `<ns>/<sha>/` 是一次 `git clone --no-checkout` + `git checkout <sha>`
的结果。同一 ns 多个 sha 共存,旧的可被 `mcpp index gc` 回收。

`HEAD` symlink 指向当前 `mcpp.toml` 解析到的 commit——fetcher 直接
读 `~/.mcpp/index/<ns>/HEAD/pkgs/...`,不感知具体 sha。

### 3.3 锁定到 mcpp.lock

`mcpp.lock` 现状只锁包版本,不锁索引 commit。新增 `[indices.<ns>]` 段:

```toml
# mcpp.lock
version = 2

[indices.mcpp]
url = "https://github.com/mcpp-community/mcpp-index.git"
rev = "abc123def0123456789abcdef0123456789abcd"     # 全 40 字符

[indices.mcpplibs]
url = "https://github.com/mcpplibs/mcpp-index.git"
rev = "0123456789abcdef0123456789abcdef01234567"

[[package]]
name      = "gtest"
namespace = "mcpp"
version   = "1.15.2"
source    = "index+mcpp@abc123def..."
```

`source` 字段从 `mcpp-index+<url>` 升级为 `index+<ns>@<short-sha>`,显
式记录是哪个索引、哪个 commit 给的版本。

`mcpp build` 流程:
1. 读 `mcpp.toml` 与 `mcpp.lock`。
2. 对每个 `[indices.<ns>]`:
   - `mcpp.lock` 已锁 → 直接用锁文件里的 sha,**不联网**。
   - `mcpp.lock` 未锁 → 按 `mcpp.toml` 里的 spec 解析(rev/tag/branch),
     拉取 / 复用本地缓存,写回 lock。
3. 对每个 dep 按 `(namespace, name)` 路由到对应索引。

`mcpp update` 显式忽略 lock 中的索引 sha,重新解析,然后写回新 sha——
这是**唯一**会让索引 sha 漂移的命令。

### 3.4 索引内部布局(对 mcpp-index 仓自身的要求)

每个索引仓的根布局(本 PR 内不强制,但作为推荐):

```
<index-root>/
├── pkgs/
│   ├── <name-letter>/<name>.lua      # 用 name 首字母分桶,跟当前一致
│   └── ...
├── mcpp-index.toml                    # 索引元数据(可选)
└── README.md
```

`mcpp-index.toml`(未来扩展点):

```toml
spec_version = 1
namespace    = "mcpplibs"             # 仓内默认 namespace,描述符没写时兜底
description  = "mcpplibs C++ modular packages"
maintainers  = ["..."]
```

每个 lua 描述符里的 `namespace` 字段(xpkg 标准)优先;缺失则用仓的
默认 namespace;再缺失则用查询时的 namespace。

## 4. 解析流程图

```
mcpp build
   │
   ├── 1. parse mcpp.toml → Manifest (含 indices, dependencies)
   ├── 2. parse mcpp.lock → 已锁 sha 表
   ├── 3. for each ns in deps.namespaces:
   │       a. 找对应 [indices.<ns>] 的 IndexSpec
   │       b. 若 lock 有 sha → ensure_checkout(ns, sha)
   │       c. 若 lock 无 sha → resolve_to_sha(spec) → ensure_checkout
   │       d. 写回 lock
   ├── 4. for each (ns, name) in deps:
   │       a. read ~/.mcpp/index/<ns>/HEAD/pkgs/<l>/<name>.lua
   │       b. semver-resolve version
   │       c. fetch source, build, ...
   └── 5. emit ninja, link
```

## 5. 兼容性 / 迁移

### 5.1 现有项目(无 `[indices]`)

`mcpp.toml` 不含 `[indices]` → mcpp 自动注入内置默认:

```cpp
m.indices["mcpp"] = IndexSpec {
    .namespace_ = "mcpp",
    .url        = "https://github.com/mcpp-community/mcpp-index.git",
    .branch     = "",   // 等同当前行为:用仓的默认分支 + 不锁定
};
```

行为与今天**完全一致**(包括非确定性问题,但这是已知现状)。

### 5.2 `mcpp.lock` schema bump

`version = 1` → `version = 2`。读到 v1 时:
- 把所有包的 `source = "mcpp-index+<url>"` 当作"无索引锁",触发一次
  在线解析后写回 v2 形式。
- 不删除现有 v1 包条目。

### 5.3 与命名空间 PR (PR-A) 的关系

PR-A 已经引入 `(namespace, name)`。本 PR 把 namespace 跟"索引来源"挂
钩——`namespace == ns of an [indices.<ns>] entry`。

### 5.4 与 xim 的关系

`xlings install` 仍然管全局工具链(gcc / mcpp 自身),不参与 mcpp-index
的拉取——mcpp 直接用 git。这避免了"用 xim-pkgindex 装 mcpp-index"这
种循环依赖。

## 6. 错误处理

| 场景 | 行为 |
|---|---|
| 引用未声明的 namespace(如 `acme:foo` 但没 `[indices.acme]`) | 报错并提示"add `[indices.acme]` to mcpp.toml or `~/.mcpp/config.toml`" |
| 索引 git URL 网络不通(首次拉取) | 报错;若 `~/.mcpp/index/<ns>/<sha>/` 存在则降级到离线缓存 |
| `rev` / `tag` 在远端不存在 | 报错;不要静默切换到默认分支 |
| `mcpp.lock` 锁的 sha 在本地缓存被人为删了 | 触发一次拉取重建 |
| 同一 namespace 在 project + global config 都声明 | 用 project 的(无 warning,符合 cargo 行为) |

## 7. 落地步骤

按 PR 拆分,每一步独立可测试:

### PR-1:Manifest schema + 解析

- `Manifest::indices` 字段
- `[indices]` 段 toml 解析(短形式 + inline 表 + ns key)
- 单元测试覆盖 5+ 种写法(短/长/rev/tag/branch/path)
- 不接 fetcher,不影响构建(默认值兜底)

### PR-2:Index 存储与解析

- `~/.mcpp/index/<ns>/<sha>/` 布局 + 拉取
- `resolve_to_sha`(rev/tag/branch → 实际 sha)
- `ensure_checkout`(幂等)
- 单元 + e2e:本地 path 索引(避免 e2e 依赖网络)

### PR-3:fetcher 改造为按 namespace 路由

- `fetcher::open(namespace, name)` 替代当前的全局 `pkgs/<l>/<name>.lua` 查询
- 兼容 layer:`namespace == "mcpp"` 时仍能读老式平铺索引
- e2e:多 namespace 多索引混合工程

### PR-4:Lockfile schema v2

- 读写 `[indices.<ns>]` 段
- v1 → v2 自动迁移
- `mcpp.lock` 里 dep 条目带 `namespace` 字段

### PR-5:CLI

- `mcpp index list/update/pin/unpin`
- 错误信息友好化

### PR-6:文档

- `docs/40-package-index-config.md`
- `mcpp-index` README 更新(声明 `mcpp-index.toml` 期望)

## 8. 设计决策的取舍

| 决策 | 选 | 弃 | 原因 |
|---|---|---|---|
| 锁定粒度 | commit sha | 索引仓的"语义版本" | 索引仓不打 release,sha 是唯一稳定标识 |
| 短形式 vs inline 表 | 都支持 | 强制 inline 表 | 短形式覆盖 80% 常见情况 |
| 默认分支跟踪 | 允许但不推荐 | 全部强制 rev | 开发期需要追上游;生产用 `mcpp index pin` |
| 索引存储位置 | `~/.mcpp/index/` | 跟项目 target 走 | 多项目共享缓存,git clone 一次即可 |
| 索引仓元数据文件 | `mcpp-index.toml`(未来) | 现在不要求 | 优先收敛 PR-1~PR-3,元数据是后话 |
| 多 namespace 同 url | 允许(各自独立 checkout) | 强制去重 | 极少见,实现复杂 |
| 全局 config.toml | 与项目 toml 合并 | 仅项目 toml | 企业内网索引复用 |

## 9. 未决问题(PR 实现前需确认)

1. **`mcpp index pin` 是否要强制全部 namespace 一起 pin**?
   倾向:默认只 pin 当前 ns;`--all` 一并 pin。

2. **跨 namespace 的 deps 重复名怎么处理**?例 `acme:cmdline` 和
   `mcpplibs:cmdline` 在同一项目共存——理论支持,但实际 BMI / link
   名字会冲突。倾向:link 时注入 namespace 前缀(`libacme-cmdline.a`)。
   本 PR 不处理,留给"namespace 隔离链接"独立 PR。

3. **是否允许把 `[dependencies]` 里的 dep 显式绑定到非默认 ns 的索
   引**?例如:

   ```toml
   [dependencies.acme]
   foo = { version = "1.0.0", index = "acme-edge" }
   ```

   倾向:**不支持**,保持 namespace 与索引一一对应。需要切换索引就
   换 namespace。

## 10. 不在范围内

- 索引内部权限控制(读写 ACL)——交给 git server。
- 索引仓的镜像 / CDN——交给 git host。
- 索引签名校验 / supply-chain 防护——独立大 PR,后续设计。
- xpkg 描述符的 `namespace` 字段强制 schema 验证——等命名空间 PR-B
  整理 mcpp-index 时一并处理。

---

**附录 A**:与 cargo `[source]` / npm `registry` 的对比简记

| 维度 | mcpp `[indices]` | cargo `[source]` | npm `.npmrc` |
|---|---|---|---|
| 锁定粒度 | git commit sha | crates.io 时间戳 | tarball sha512 |
| 多源 | namespace 路由 | replace-with 链 | scoped registry |
| 私有 | git URL + ssh | sparse-index + token | scoped registry + token |
| 离线 | 本地 path 索引 + 缓存 | `--offline` + 缓存 | `--offline` + 缓存 |

mcpp 的优势:**git 原生**——不需要发明索引格式 / 索引服务,clone 一
个仓就是一个索引;劣势:无 fast-path 增量(每次拉全仓)。规模大到这
个成为瓶颈时再考虑 sparse-index / 索引镜像。
