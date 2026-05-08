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
| `mcpp index update [<ns>...]` | 拉远端最新 sha,**写回 `mcpp.lock`**。无参 = 更新所有索引;`rev`/`tag` 锁定的索引被显式跳过(no-op + 提示)。 |
| `mcpp index pin <ns> [<rev>]` | 把当前索引解析到的 commit 写回 `mcpp.toml` 作为 `rev`(从"软锁"升级为"硬锁",任何机器都能复现)。空 `<rev>` = 用 lock 里现行 sha。 |
| `mcpp index unpin <ns>` | 反向操作:从 `mcpp.toml` 删除 `rev` 字段(lock 仍锁,但下次 `index update` 又能动)。 |
| `mcpp update [<pkg>]` | 重新解析包版本约束(等同 cargo update)。**会先自动刷新涉及到的索引**,再重选最高匹配版本,写回 lock。无参 = 全部 dep。 |

> 简记:`build / run / test / pack` **永远不改** `mcpp.lock`;
> `mcpp update` / `mcpp index update` / `mcpp index pin` 是**仅有的三个**
> 改写 lock 的命令。

### 2.5 升级与刷新流程的四个典型场景

> 对应"`mcpp.lock` 锁住后,具体怎么动"的四个典型问题。

#### 场景 A:首次构建,无 lock,无 `[indices]`

```
mcpp build
  → 内置默认 mcpp index URL,拉远端默认分支 HEAD
  → 写入 mcpp.lock 的 [indices.mcpp].rev = <sha>
  → 解析依赖,写入 lock 的 [[package]] 段
```

之后所有 `mcpp build` 都用这个 lock,完全离线复现。

#### 场景 B:索引上游有了新版本(包括官方 mcpp-index 加了新包 / 新版本)

**默认行为**:`mcpp build` **看不到**——它只读 lock 里的旧 sha,旧索引
里不包含新版本,**不会报错也不会自动升级**。

这是**特性,不是 bug**:CI / 团队成员 / 老分支构建出来的产物完全可复现,
不会因为上游某天合了一个 PR 就改变行为。

要看到新版本,**必须主动**:

```bash
mcpp index update mcpp        # 拉新 sha,写回 lock
                              #   ↳ build 现在能看到新版本号了
mcpp build                    # 但还是用 lock 里旧的 dep 版本
                              #   (因为 mcpp.toml 约束没变,旧版本仍满足)
```

如果还想**升级到新版本**:

```bash
mcpp update                    # 重新解析所有 dep 的版本约束
                               # 这条会自动先刷索引,再选最高匹配版本
```

或单独升级一个包:

```bash
mcpp update gtest              # 只重选 gtest 的版本
mcpp update mcpplibs:cmdline   # 命名空间形式同样支持
```

#### 场景 C:lock 锁住的 sha 被上游删了 / force-push 没了

例如官方 index 仓被 force-push,本地 lock 里那个 sha 在远端找不到了。

- **本地 `~/.mcpp/index/mcpp/<sha>/` 已经 checkout 过**:`mcpp build`
  仍然离线工作——sha 在本地缓存里没事。
- **从未拉过 / 缓存被 gc 了**:`mcpp build` 报错:
  ```
  error: cannot fetch index 'mcpp' at sha abc123de...:
         remote no longer has this object.
         try: mcpp index update mcpp   # to advance to a current sha
  ```
  **不静默漂移**——必须用户决定怎么办。

#### 场景 D:把"软锁"升级为"硬锁"

`mcpp.toml` 没写 `rev` 字段时,lock 里有 sha,但 `mcpp.toml` 本身没有
不可变锁定信息——别的机器 clone 项目时,如果删了 lock 重 build,会拿到
**当前最新**的 sha,而不是当时构建时的 sha。

如果想让 `mcpp.toml` 本身也带锁定:

```bash
mcpp index pin mcpp            # 把 lock 里的 sha 写到 mcpp.toml 的 [indices.mcpp].rev
git add mcpp.toml mcpp.lock
git commit -m "Pin mcpp index to <sha>"
```

之后任何机器、任何时刻 clone 这个仓库都拿到完全一样的索引。

反向(允许 `mcpp index update` 推动 sha 前进):

```bash
mcpp index unpin mcpp          # 从 mcpp.toml 删 rev,lock 仍保留 sha
```

> 这两条是 cargo 没有的——cargo 的 registry 是"追加式不可变",所以不
> 需要在 `Cargo.toml` 里固定 registry 版本。mcpp 的索引是普通 git 仓
> 库,有 force-push / 删 commit 的可能,所以多了"硬锁到 toml"的维度。

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

`mcpp build` 流程(**lock 永远锁 sha**):

1. 读 `mcpp.toml` 与 `mcpp.lock`。
2. 对每个有效索引(显式 `[indices.<ns>]` 段 **或** 内置默认 `mcpp`):
   - `mcpp.lock` 已有 `[indices.<ns>]` 段 → 直接用锁文件里的 sha,
     **不联网**。
   - `mcpp.lock` 没有 → 按规格解析(`rev` / `tag` / `branch` /
     默认分支)得到一个具体 sha,**写回 lock**;后续构建走第一条。
3. 对每个 dep 按 `(namespace, name)` 路由到对应索引(实际打开
     `~/.mcpp/index/<ns>/<sha>/pkgs/...`)。

**`mcpp update` 与 `mcpp index update` 是唯一两条让 lock 里 sha 改变的命令**:
- `mcpp index update [<ns>...]`:重新解析索引规格,写入新 sha。
- `mcpp update [<pkg>]`:重新解析包版本约束(并顺带调用上一条更新所
  涉及的索引)。

其它任何命令(`build` / `run` / `test` / `pack`)**只读**索引 sha,
绝不静默改写。

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

## 4.5 不可变性的两层保证

**用户最在意的不是"今天的 gtest@1.15.2 跟明天的 gtest@1.15.2 是不是
同一个文件"——这一层 mcpp 后期的发布政策会保证不可变。真正担心的
是:索引仓库里那个 `gtest.lua` 描述符**今天写的 url + sha256,明天会
不会被悄悄改成别的东西**。两层是分开的:

| 层 | 保护对象 | 提供方式 | 失败时的影响 |
|---|---|---|---|
| **L1:包发布不可变性**(policy) | 已发布的 `<ns>:<name>@<ver>` 对应的 url + sha256 + 名字空间永不改写;只能新增版本,不能修改也不能删除已发布版本。 | mcpp-index 仓库的 CI gate(已发布版本字段被改 → reject PR);`mcpp publish` 客户端拒绝覆盖。 | 一旦违反,使用旧描述符的 build 会拿到不一致的二进制,极难追溯。 |
| **L2:索引快照锁定**(mechanism) | 当前项目"看到"的索引 commit sha 在 `mcpp.lock` 里固定。 | `mcpp build` / `run` / `test` / `pack` 默认离线读 lock 的 sha,不联网。 | 即使 L1 政策被违反或上游 force-push,**已锁住的项目继续用旧索引,完全不受影响**——只有显式 `mcpp index update` 才会拉到新内容。 |

L1 是承诺,L2 是兜底。两层的关系类似 HTTPS 里"CA 不签发流氓证书"
(policy)+ "客户端 pin 住已知证书"(mechanism)——任何一层成立,
build 就稳定;两层都失效才会出问题。

### L1 — 后期 mcpp-index 仓的发布政策

设计文档不强制实现 L1 检查(那是 mcpp-index 仓 CI 的事),但设计层
面记下假设,给 L2 兜底逻辑做参考:

- **新增**(`pkgs/.../<name>.lua` 中追加 `add_versions("0.0.3", ...)`)
  → 允许,合法 PR。
- **修订已发布版本**(改 `0.0.2` 那行的 sha256、url、build flags)
  → CI gate 必须 reject。如果 sha256 真的算错了,**只能发** `0.0.2-fix`
  之类的新版本,不能就地改 `0.0.2`。
- **删除已发布版本** → reject。极端情况(法律 / 安全)走人工 yank 流
  程,版本仍保留但标 yanked,新解析跳过它。
- **整个包改名 / 改 namespace** → 等同删除 + 新增,要求作者发明确新版
  本号 + 旧版本保留不删。

> mcpp 后期可以再发一个独立 spec(类似 `xpkg-publish-policy`)详细
> 列举可修字段集 / 不可修字段集,本 PR 不展开。

### L2 — `mcpp.lock` 的索引 sha 锁

L2 不依赖 L1 是否真被遵守:**任何时刻,build 看到的索引描述符 = lock
里那个 commit 的 checkout**。所以:

- L1 完美遵守 → L2 锁的 sha 解析出来的描述符 = 远端最新一致(锁多余但
  无害)。
- L1 偶发违反(典型场景:误 force-push、维护者手抖) → L2 仍指向旧
  sha 的 checkout,旧描述符内容不变,build 完全不感知。
- L1 故意违反(攻击) → 同上,L2 给一段缓冲时间;同时 mcpp 可在
  `mcpp index update` 时检测"远端历史被改写"并报警。

**对用户问题的直接回答**:

> "如果包索引仓的包描述发生变化了呢?"

`mcpp build` 不受影响——lock 锁的 commit sha 还指向旧 checkout,
旧 checkout 里的描述符内容不变(git object 是 content-addressed,不会
被远端改写)。除非用户主动 `mcpp index update` 拉到新 sha,才会看到
新内容;即便看到了,L1 政策要求新内容只是"新增版本",不会改写已选
的那个版本——所以已选 dep 仍稳定。

> "对于具体的包发布,后期 mcpp 也是固定不可回退的"

正是 L1 的承诺。L2 是物理层兜底,即使 L1 (人为 / 流程)出错,build
也能稳定到下一次显式 update。

## 5. 兼容性 / 迁移

### 5.1 现有项目(无 `[indices]`)— 隐式默认 + 强制 lock pinning

**核心原则:lock 文件里一定锁住具体 commit sha——即使 `mcpp.toml`
完全没写 `[indices]`**。空 / 未指定**只表达"我没显式选源",绝不表
达"不可复现地浮动跟随 main"**。

`mcpp.toml` 没写 `[indices]` 时,mcpp 内存里注入一个内置默认:

```cpp
m.indices["mcpp"] = IndexSpec {
    .namespace_ = "mcpp",
    .url        = "https://github.com/mcpp-community/mcpp-index.git",
    // rev / tag / branch 全空 = "我没显式选,跟着远端默认分支拿一次"
};
```

**首次** `mcpp build` 的行为:

1. 读 `mcpp.toml`(没 `[indices]`) + `mcpp.lock`(没 `[indices.mcpp]`)。
2. 对默认 `mcpp` namespace 的索引规格走 resolve:
   - `rev` / `tag` / `branch` 都空 → 拉远端默认分支(`main`)的当前
     `HEAD` sha。
   - 立刻把这个 sha **写回 `mcpp.lock`**:
     ```toml
     [indices.mcpp]
     url = "https://github.com/mcpp-community/mcpp-index.git"
     rev = "abc123def0123456789abcdef0123456789abcd"
     ```
3. 之后所有 `mcpp build` 看到 `mcpp.lock` 已锁,**离线复用同一个 sha**,
   不再联网。

**升级索引的唯一入口是 `mcpp index update`**——它显式忽略 lock,重新
解析,写入新 sha;用户必须主动触发,不会"自动漂移"。

> 跟 cargo 一致:`Cargo.toml` 不写 `[source]`,但 `Cargo.lock` 必锁
> 每个包的 registry 来源 + 精确版本 + checksum。

**与今天行为的差异**:

| 维度 | 今天(无 lock pinning) | 本设计(强制 lock) |
|---|---|---|
| 第一次 `mcpp build` | 拉 main HEAD,无记录 | 拉 main HEAD,**写入 lock** |
| 第二次 `mcpp build` | 再次拉 main HEAD(可能已变) | **离线读 lock 的 sha**,完全复现 |
| 切到不同机器 / CI | 可能拿到不同 commit | 锁文件保证 commit 一致 |
| 想升级索引 | 没 API,只能 git pull 自己的索引镜像 | `mcpp index update` |

**注:本节解决了今天那个"两次构建可能不一样"的非确定性问题**,
而不是把它原样保留。

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
| 锁定粒度 | git commit sha + 包 checksum | 包版本 + 包 checksum | tarball sha512 |
| 多源 | namespace 路由 | replace-with 链 | scoped registry |
| 私有 | git URL + ssh | sparse-index + token | scoped registry + token |
| 离线 | 本地 path 索引 + 缓存 | `--offline` + 缓存 | `--offline` + 缓存 |
| 刷新触发 | `mcpp index update` / `mcpp update`(显式) | 每次 `cargo build` 自动 fetch index(可 `--frozen` 关) | 每次 `npm install` 自动查 registry(可 `--offline` 关) |
| 已发布不可变 (L1 policy) | mcpp-index 仓 CI gate + `mcpp publish` 拒绝覆盖(后期承诺) | crates.io 服务端强制 | npm registry 服务端强制(标 deprecated 不算改写) |
| 索引快照锁 (L2 mechanism) | `mcpp.lock [indices.<ns>].rev = <sha>` | 无(L1 已足够,registry 服务承诺 immutable) | 无(同上) |
| 配置层硬锁 | `mcpp.toml [indices.<ns>].rev = "<sha>"` 把 lock 抬到 toml | 无(不需要) | 无(不需要) |

**核心差异**:cargo 由 crates.io 这一个**受控服务**单独承担"已发布
不可变"承诺;mcpp 选了**双保险**——L1 后期由 mcpp-index 仓的发布政
策 + CI gate 承担(类似 crates.io),L2 在客户端 lock 文件里再锁一层
索引 commit sha。

为什么多一层 L2?因为 mcpp 的索引是普通 git 仓库——任何人 fork 一个
就是新索引,官方仓也理论上可能 force-push;不像 crates.io 那样有服务
端硬约束。L2 给两类用户兜底:

1. 用第三方 / 私有索引的项目(`[indices.acme] = ...`)——第三方维护
   方未必有 L1 级别的 CI gate,L2 让 lock 锁住自己当时 build 用的索引
   sha。
2. 即使是官方索引,L1 真的偶发出错时(误 force-push、签错 sha256
   patch),已经在 CI / 用户机器上锁住的项目不受影响,只有显式 update
   才会拉到新内容。

代价:用户多了一个心智负担——"什么时候该跑 `mcpp index update`"。文
档里要有明显的 onboarding 提示;典型答案是"开发期跟着自己节奏更新,
合并到 main 之前 lock 跟着 PR 一起进 git,CI 必走 lock"。

mcpp 的工程优势:**git 原生**——不需要发明索引格式 / 索引服务,clone
一个仓就是一个索引;劣势:无 fast-path 增量(每次拉全仓)。规模大到
这个成为瓶颈时再考虑 sparse-index / 索引镜像。
