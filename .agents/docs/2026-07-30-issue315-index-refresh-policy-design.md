# 索引刷新策略收敛：从「时间驱动」到「解析驱动」— 设计

日期：2026-07-30
状态：**已实施**（2026.7.30.3）。§10 的两条探针已跑，结果与实现修正记在那一节
关联：#315（`mcpp build/test/run` 手动刷新包索引而不是自动刷新）
建议目标版本：**2026.7.31.1**

---

## 1. 摘要

issue #315 报的症状真实：网络差时 `mcpp build` 会停在 `Updating package index (auto-refresh)`。

但这不是「功能缺失」。offline-first / miss-triggered 的刷新策略**已经在仓库里了**，而且写得比
issue 建议的更完整（`src/xlings.cppm:1400-1431`，含 120s 抖动抑制），其注释甚至点名了这个
issue 描述的症状（Termux first-run / build hang）。真正的缺陷是：

| 编号 | 缺陷 | 层级 |
|---|---|---|
| **D1** | 同一个决策（要不要刷索引）在 **5 处各推导一遍**，其中 `prepare.cppm:1300-1311` 推出的策略与 `xlings.cppm:1405` 写死的策略**互相矛盾** | 策略 |
| **D2** | 刷新判据是 **marker mtime（时间轴）**，而「索引能否回答问题」是内容属性。**而且索引自带的内容 revision（`.xlings-index-version`）躺在磁盘上，mcpp 零引用** | 状态/身份 |
| **D3** | `is_index_dir_fresh` 对**未来时间戳**判为「永远新鲜」（`age < 0 < ttl`）；时钟回拨 / tar 保留 mtime / CI cache 恢复都能触发 | 稳定性 |
| **D4** | 无 `--offline` / `MCPP_OFFLINE` 逃生舱；`MCPP_NO_AUTO_INSTALL` 只管**工具链**，不管索引也不管包 | UX |
| **D5** | 刷与不刷的**原因**从不可见；只有一行没有主语的 `Updating package index` | UX |
| **D6** | `mcpp update` 是**空操作**：删 mcpp.lock 条目后叫用户去 `mcpp build`，而 lock 从来没被 build 读过 → 行为影响为零 | 语义 |

D1/D2 是根。本设计做 D1–D6 的收敛，**不做** 「让 `mcpp.lock` 成为构建期解析输入」（§9，必须单独立项）。

---

## 2. 现状机制（逐处核过）

### 2.1 五个刷新站点，四种策略

| # | 站点 | 判据 | 评价 |
|---|---|---|---|
| 1 | `mcpp add`<br>`pm/commands.cppm:160-175` | 先 `lookup_descriptor` 查本地 → **确凿未命中** + 涉及内置 registry + TTL 过期 → 刷一次 → 重查 | 正确样板 |
| 2 | xim 安装前置门<br>`xlings.cppm:1400` | **纯 miss**：包文件本地缺失才刷；120s 内刚刷过则不重刷 | 最完整 |
| 3 | 安装失败重试<br>`package_fetcher.cppm:1060-1075` | **版本 miss**：install 失败 → 刷一次 → 重试 | 补 #2 的漏（文件在、版本旧） |
| 4 | **`mcpp build/run/test`**<br>`prepare.cppm:1300-1311` | **纯 TTL**：根 manifest 有任一依赖路由到内置索引 + marker 超 `searchTtlSeconds`（默认 3600s） | ← 本 issue |
| 5 | `mcpp search`<br>`index_management.cppm:29` | TTL | 合理，search 语义即查上游 |

站点 4 已有两道门（`!m->dependencies.empty()` 与 `usesBuiltinIndex(spec)`），所以「无条件刷新」
的说法不成立；但第三道判据是纯时间，只要有**一个** registry 依赖，门就形同不设。

### 2.2 为什么这次网络往返在稳态下是纯浪费

**所有解析输入都是本地文件**：

- `resolve_semver`（`pm/resolver.cppm:96`）经 `IndexRoute::read()` 读 xpkg.lua，从描述符解析可用
  版本集 —— 纯本地文件解析，本身不联网。
- 包已装时 `fallback::is_install_complete(verdir)` 直接早退（`package_fetcher.cppm:990`）。

即「本地能否回答」是**不需要网络、确定性、可单测**的判定。站点 4 却用一个与之无关的信号
（marker 有多老）决定是否付网络代价。

### 2.3 代价被两个细节放大

- `is_index_fresh` 查的是 **default（mcpplibs）索引**的 marker，但 `update_index` 执行的是
  `xlings update` —— **同步所有索引仓库**。一个 marker 过期换一次全量同步。
- `update_index` 有 **3 次尝试 + 2s/4s 线性退避**（`xlings.cppm:1365-1378`）。网络差时不是等
  一次超时，是三次。

### 2.4 分层体检：真正的缺陷不全在策略层

| 层 | 事实 | 结论 |
|---|---|---|
| **L1 传输** | `update_index` = `xlings update`，刷**全部**索引仓库，mcpp 侧无 per-index / per-package 能力 | 粒度不可选 |
| L1 | **`mcpp index update <name>` 的 `name` 是空承诺**：全局段无条件全量刷（`index_management.cppm:121-124` 硬编码 `"all index repos"`），filterName 只在项目自定义索引循环里过滤，而循环体 `break` + `ensure_project_index_dir` 一次处理全部 | CLI help 承诺了实现做不到的事 |
| L1 | **索引刷新无任何并发锁**。`bmi_cache` 有 flock，`platform/fs.cppm` 已提供跨平台原语（flock / LockFileEx），索引一个都没用 | 并行 CI job 共享 MCPP_HOME、多终端并行构建 → 并发同步 + 并发写 marker |
| **L2 状态** | 唯一新鲜度信号是 `.mcpp-index-updated` 的 **mtime** | 信号选错轴（D2） |
| L2 | **索引自带内容 revision**：`<index>/.xlings-index-version`（实测 `mcpplibs`→`8d67478`，`xim-pkgindex`→`ebf4020`），**mcpp 全仓库零引用** | 内容轴的载体现成，见 §3 INV-2 |
| L2 | `LockedIndex.rev` 只是把用户在 `[indices]` 写的 pin 抄进 lock（`prepare.cppm:3841`），**不是观测值**；内置索引连 rev 概念都没有 | 无内容寻址的索引身份 |
| L2 | **派生缓存烙绝对路径**：`.xlings-index-cache.json` 每条都带 `"path":"<绝对路径>"`。现场证据：本机 `~/.mcpp` 那份的 path 全部指向另一个早先会话的临时 MCPP_HOME | 目录一搬（CI cache 恢复 / MCPP_HOME 迁移 / AUR `/opt` 布局）缓存即坏 |
| L2 | **为上一条写的检测器没接线**：`official_index_cache_matches_package_file` 只被 `is_official_package_index_fresh` 调用，而后者在 production 代码里**零调用**，只有 `tests/unit/test_xlings.cpp` 在用；跑在构建路径上的 `ensure_official_package_index_fresh` 只查 `exists(pkg)` | 检测器写了、测了，没装到产线上 |
| L2 | `mark_known_indexes_refreshed`（`xlings.cppm:444`）在 `env.projectDir` 非空时整段早退 | 项目 env 的刷新永不打标 |
| **L3 策略** | D1 | 见 §3 INV-1 |
| **L4 表达** | `index update/status/pin/unpin` 齐全，`status` 是真 offline（设计正确） | 骨架对 |
| L4 | **`mcpp update` 空操作**（D6，`pm/commands.cppm:403-430`） | 唯一语义为「更新依赖」的命令什么都不做 |

**一处过时论断的更正**：先前评估称「`xlings update` 是 git-only，Windows no-git 死锁（PR #114）
每小时埋进构建关键路径」。**已过时** —— `IndexSpec` 已有 `source = "auto" | "artifact" | "git"`
与 `artifact` base URL（`config.cppm:40-41`，#269 / xlings ≥ 0.4.68），索引以 **artifact 分发**
为主；本机 `mcpplibs/` 与 `xim-pkgindex/` **都没有 `.git` 目录**，证实了这一点。风险仍在
（`auto` 会回落 git），但性质从「必经」降为「可能」，Windows 论点相应下调。

---

## 3. 架构判断（四条不变量）

### INV-1 刷新策略只有一个真源

D1 是典型的「同一决策两处推导」——本仓库已为这种债付过两次代价（feature 请求集解析×激活两处
不自洽 #242；`build_program` 重推 musl→static 未复用 `prepare`）。加语义时它必然变成构建失败。
故新增单一策略模块，5 个站点全部改为调用它，**任何站点不得再自行判断 TTL**。

### INV-2 gate 用内容，且直接采用已有的 `.xlings-index-version`

刷新的**必要性**由「解析器能否用本地索引回答」决定；索引的**身份**取 `.xlings-index-version`
（7 位 rev），marker mtime 降级为纯 debounce 计时器。

这条同时消解一个既有故障模式：**marker 假新鲜导致上游有新版本却拉不到**（llvm 22.1.8 那次）。
假新鲜在新模型下不再能压制一次必要的刷新，只能压制一句提示。方向与 #311/#317 的教训一致：
**mtime 不可靠，判等走内容/键。**

关键是**不需要新建机制** —— rev 文件已存在，读一个 7 字节文件即可。它一次性支撑四件事：
新鲜度显示、`mcpp index status` 的 rev 列、未来 lock 记录**观测到的** index rev、
「为什么解析结果变了」的诊断。（前置条件见 §10 探针 P-1。）

### INV-3 不可证伪就不刷

`IndexRoute::authoritative_for()`（PR#307）已定义「miss 何时能当作不存在的证明」：lazy git 索引
未克隆、第三方 namespace、**xim 描述符不写 `namespace` 故 `(xim, x)` 永不匹配身份门** —— 这三类
miss 不可证伪。

**这是本方案最容易踩的坑**：若把「查不到」直接当「要刷」，则任何带 xim 依赖的项目会**每次构建都
判 miss、每次都刷**，比现状更糟。必须复用 `authoritative_for`：不可证伪 → fall-through，**不刷**。

### INV-4 策略是环境属性，不是项目属性

刷新策略归**全局 config**（`~/.mcpp/config.toml`），不进 `mcpp.toml`。同一项目在公司内网、家里、
CI 上该允许不同策略；写进 manifest 会让项目不可移植，还会无谓拓宽 lock/指纹面。

---

## 4. 目标设计

### 4.1 新模块 `src/pm/index_refresh.cppm`（module `mcpp.pm.index_refresh`）

依赖方向：`index_refresh` → {`index_route`, `resolver`, `xlings`, `config`}。`resolver` 已 import
`index_route`，无环。

```cpp
export namespace mcpp::pm {

enum class RefreshReason {
    None,                   // 本地可解析 —— 零网络
    IndexAbsent,            // 索引 pkgs 目录不存在（冷启动）
    DescriptorMiss,         // 描述符本地缺失，且该 ns 可证伪
    VersionMiss,            // 描述符在，但 SemVer 约束在本地版本集内无解
    SuppressedDebounce,     // 刚刷过（<debounce），再刷无意义
    SuppressedOffline,      // --offline / MCPP_OFFLINE
    SuppressedInconclusive, // 不可证伪 → INV-3
};

struct RefreshPolicy {              // flag > env > config，一次解析好往下传
    bool         offline = false;
    std::int64_t debounceSeconds = 120;   // 复用 xim 门的既有语义
};

struct RefreshDecision {
    bool          shouldRefresh = false;
    RefreshReason reason        = RefreshReason::None;
    std::string   subject;      // "mcpplibs:fmt@^1.3" —— 进日志与错误文案
};

// 纯函数、零副作用、零网络：可单测，判据表就是它的契约。
RefreshDecision decide_for_dependency(const IndexRoute&           route,
                                      const DependencyCoordinate& coord,
                                      std::string_view            versionReq,
                                      const mcpp::xlings::Env&    env,
                                      const RefreshPolicy&        policy);

// 执行 + 可解释输出 + 打标；进程内保证一次构建最多刷一次。
std::expected<void, std::string> apply(const RefreshDecision&, const mcpp::xlings::Env&);

// 索引身份（`.xlings-index-version`），缺失返回 nullopt（本地 path 索引没有它）。
std::optional<std::string> index_revision(const std::filesystem::path& indexDir);

// advisory：仅在解析失败与 `index status` 使用，不参与 gate。
struct Staleness { std::optional<std::int64_t> days; std::optional<std::string> rev; };
Staleness staleness_hint(const mcpp::xlings::Env&);

} // namespace mcpp::pm
```

判据表（`decide_for_dependency` 的全部行为，按序短路）：

| 条件 | 结果 |
|---|---|
| `policy.offline` | `SuppressedOffline` → 不刷 |
| `!route.authoritative_for(ns)` | `SuppressedInconclusive` → 不刷（INV-3） |
| `index_pkgs_dir` 不存在 | `IndexAbsent` → 刷 |
| `route.read(coord) == nullopt` | `DescriptorMiss` → 刷 |
| `is_version_constraint(versionReq)` 且 `resolve_semver(...)` 失败 | `VersionMiss` → 刷 |
| 上面命中 miss 但距上次刷新 < `debounceSeconds` | `SuppressedDebounce` → 不刷 |
| 其余 | `None` → 不刷 |

两条**简洁性**决定，让这张表比初稿更小也更对：

- **VersionMiss 只对 SemVer 约束成立**。精确版本（`is_version_constraint == false`）不走
  `resolve_semver`，其 miss 由安装层已有的 miss 驱动兜住（`is_install_complete` 早退 → 失败则
  刷一次重试，站点 3）。这同时避开一个陷阱：**版本表是 per-OS 的**，"本机没有" ≠ "不存在"
  （`pm/commands.cppm:46-48` 已记录该判据），在此处硬判会造成每次构建的误触发。
- **VersionMiss 直接复用 `resolve_semver` 的失败**，不复制约束求解逻辑 —— 否则又是一处 D1。

### 4.2 站点改造

| 站点 | 改法 |
|---|---|
| `prepare.cppm:1300-1311` | 删掉 `usesBuiltinIndex` + `is_index_fresh` 整段；对根 manifest 每个依赖调 `decide_for_dependency`，任一 `shouldRefresh` 则 `apply` 一次（进程内 once） |
| `pm/commands.cppm:167`（add） | 语义已对，改为调用 `decide_*` 以消除重复推导 |
| `xlings.cppm:1400`（xim 门） | 保留（服务 xim 索引、判据同构），debounce 常量改由 `RefreshPolicy` 提供，不再各写一个 120 |
| `package_fetcher.cppm:1060`（失败重试） | 保留，reason 归一为 `VersionMiss`，走统一输出 |
| `index_management.cppm:29`（search） | 保留 TTL —— 代码里**显式注明**这是唯一允许时间驱动的站点及理由 |

### 4.3 D6：让 `mcpp update` 真的做事

`mcpp update` 是唯一语义为「我要更新依赖」的命令，今天却是空操作。改为：

1. **强制刷索引**（显式意图 → 不看 TTL、不看 debounce；`offline` 下拒绝并说明）；
2. 清除 lock 条目（保留现有行为）；
3. 输出实际发生了什么：`Updated  mcpplibs 8d67478 → a1b2c3d`（无变化时说"already at …"）。

这样「我知道上游有新版」有了名正言顺的入口，`build` 侧不必再开洞（§5）。

### 4.4 稳定性修复

- **S1（D3）** `is_index_dir_fresh`：`age.count() < 0` → 视为 **stale**。未来时间戳来自时钟回拨、
  容器时间、tar 解包保留 mtime、CI cache 恢复；当前实现让索引「永远新鲜」直到墙钟追上。
- **S2** 刷新失败**不再直接失败构建**。`apply()` 出错时：本地仍能解析 → 降级 warning 继续；
  只有「本地无答案 + 刷新也失败」才 hard error，并合并 advisory + `mcpp index update` 建议。
  （现状不统一：TTL 刷新失败被静默吞掉，project index clone 失败直接 hard error。）
- **S3** 索引刷新加**跨进程互斥**：复用 `platform/fs.cppm` 的 flock / LockFileEx（`bmi_cache`
  的既有用法），锁文件置于 index data 根。拿不到锁 → 不排队、不报错，直接跳过这次刷新
  （别人正在刷；本次继续用本地解析，失败再按 S2 处理）。这条同时消掉并发写 marker 的竞态。
- **S4** `mark_known_indexes_refreshed` 在 `projectDir` 非空时早退 → 补打标，仅供 advisory，
  否则 `mcpp index status` 对自定义索引永远 unknown。
- **S5** 3×retry 的 2s/4s 退避只在真需要刷新时才可能付费；`offline` 下完全不进 `update_index`。

### 4.5 跨平台

| 平台 | 影响 |
|---|---|
| **Windows** | 主要收益是**少跑一个多仓库同步**：进程创建 + Defender 扫描让每次 sync 显著更贵。另有残余风险 —— `source = "auto"` 可能回落 git，而 no-git + xvm git shim + `XLINGS_HOME` 重定向曾构成死锁（PR #114）；miss 驱动把这条路径从常态挪到例外。**S3 的锁必须用 `platform/fs.cppm` 的封装**，别写 POSIX-only 的 flock |
| **macOS** | 企业网络受限 + 首启 Gatekeeper 场景同理；大包冷路径不受影响（那是 install 轴） |
| **Termux / musl** | musl-static 的 DNS 短板已知（读不到 `$PREFIX/etc/resolv.conf`）。日常构建完全不碰 DNS 是该平台的可用性底线 |
| **mtime 语义** | `file_time_type` 的 epoch 跨平台不同（#317 踩过）。本设计把 mtime 缩到 debounce（秒级容差）+ advisory，并显式处理负 age（S1）；**身份判定改用 rev 文件，与 mtime 无关** |
| **本地 `path` 索引** | 没有 `.xlings-index-version`，也没有"刷新"概念 → `index_revision` 返回 nullopt，`authoritative_for` 判 local 可证伪但 `apply` 对它是 no-op。e2e 正是靠这条离线化 |
| **CI** | 隔离 `MCPP_HOME` + 工程内 `path` 索引即可离线验证全部分支（PR#307 已建立该模式），不需要真网络 |

### 4.6 UX 与表达面（刻意收窄）

**不加 `--refresh-index`**，理由见 §5。表达面只有三个旋钮：

| 面 | 形态 | 语义 |
|---|---|---|
| flag | `--offline` | **本次调用不发起任何网络**：不刷索引、不下载包、不自动装工具链。与 cargo 的 `--offline` 对齐 |
| env | `MCPP_OFFLINE=1` | 同上，作用于整个会话/CI job |
| config | `[index] auto_refresh = true \| false` | 机器级持久开关；`false` = 索引轴永不自动刷（仍允许包下载）。默认 `true` |

三条简化决定：

- **不保留 `always`（旧 TTL 行为）模式**。为模拟一个正被删除的策略而留第二条代码路径，正是 D1
  这笔债的来源。要「每次都最新」的用户写 shell alias 或 `mcpp index update && mcpp build`。
- **`MCPP_NO_AUTO_INSTALL` 被 `--offline` 吸收**：它今天只管工具链（`prepare.cppm:1027`），
  是同一概念的第三个名字。保留为**兼容别名**（等价于 offline 的工具链子集），文档标注 deprecated。
  一个概念替三个。
- `cache.search_ttl_seconds` **保留不动**（向后兼容），语义收窄为 `search` 站点的门 + advisory
  阈值，不再是构建路径的 gate。

**输出可解释**（D5）：

```
# miss 触发（复用 xim 门已有的句式）
Refreshing  package index — `mcpplibs:fmt@^1.3` not satisfiable locally (one-time)
Updated     mcpplibs 8d67478 → a1b2c3d

# 稳态：默认静默，-v 可见
[index] skip refresh: 4/4 deps resolvable locally (mcpplibs 8d67478, 3d old)

# offline + miss：结构化错误
error: `mcpplibs:fmt@^1.3` not found in the local package index
       index mcpplibs is at 8d67478, last refreshed 12 days ago
       run `mcpp index update` (or drop --offline) to fetch it
```

**陈旧提示只出现在两处**：解析失败时，以及 `mcpp index status` / `mcpp why deps`（后者加 rev 列）。
**不在每次构建唠叨** —— 否则只是把网络噪音换成文字噪音。

**必须写进 `docs/` 的语义变化**：`^1.2` 将稳定解析到「本地索引已知的最新版」，上游新出的 1.3 需
`mcpp index update` 或 `mcpp update` 才可见。这是 offline-first 的应有代价，也正是那两个命令存在
的意义 —— 不写进文档就会变成下一个「为什么拉不到新版本」的 issue。

---

## 5. 为什么不选其他方案

| 方案 | 否决理由 |
|---|---|
| **`build/run/test --refresh-index`**（issue 原文与评论均建议） | ①`mcpp index update && mcpp build` 完全等价，加它等于给 3 个命令各加一份表达面，而它对构建产物**零影响**（不进指纹、不改产物、只有网络副作用）——挂在 `build` 上层次是错的；②cargo 的先例正是一对反例：**有** `build --offline`，**没有** `build --refresh-index`，刷新入口是 `cargo update`；③本设计的目的就是让「何时刷」不需要用户判断，留强制开关等于承认判据不可信。真正的需求（"我知道上游有新版"）语义属于 `mcpp update` → 改为 §4.3 |
| **延长默认 TTL（7 天）** | 只把 D2 的信号错误挪远，没修；且**放大** marker 假新鲜导致拉不到新版本的既有故障模式 |
| **只加 `--no-refresh`** | 默认仍然错，成本转嫁给每个用户每次输入；违反 INV-2 |
| **读 `mcpp.lock` 比对 manifest 后跳过刷新**（评论方案核心） | 前提不成立：`prepare.cppm` 对 lock **只写不读**（3829-3883），构建路径上 lock 不是解析输入。见 §9 |
| **直接把「查不到就刷」接上** | 违反 INV-3：xim 依赖永远匹配不上身份门 → 每次构建都刷，比现状更糟 |
| **把策略放进 `mcpp.toml`** | 违反 INV-4：网络环境属性写进项目清单，项目不可移植 |
| **新增 `mcpp refreshpkg` 子命令**（issue 原文） | `mcpp index update` 已在（`cli.cppm:423`）；新子命令只增认知负担 |

---

## 6. 采纳 / 不采纳 issue 反馈一览

| issue / 评论诉求 | 处置 |
|---|---|
| 日常构建不该等网络 | **采纳**，且做成 INV-2 而非加 flag |
| 仅在「首次运行 / 依赖更新」刷新 | **采纳其意图，改写其判据**：不是"首次/变更"，而是"本地解析不出来"（更准，且不需要 lock） |
| `--refresh-pkg` / `mcpp refreshpkg` | **不采纳**（§5），改为修 `mcpp update`（D6） |
| `--refresh-index`（评论方案 2） | **不采纳**（§5） |
| 读 lock 比对 manifest（评论方案 1） | **不采纳**（前提不成立），拆为独立 issue（§9） |
| 不新增 `refreshpkg`、沿用 `index update`/`update`/`add`（评论方案 3） | **采纳** |
| `MCPP_OFFLINE=1`（评论方案 4） | **采纳**，并扩为完整 offline 语义、吸收 `MCPP_NO_AUTO_INSTALL` |
| `[index] auto_refresh`（评论方案 4） | **采纳但收窄为布尔**，不做三值（不保留旧 TTL 模式） |
| 延长 TTL（评论方案 D） | **不采纳**（§5） |

---

## 7. 实施顺序与规模

| 阶段 | 内容 | 规模 |
|---|---|---|
| **P0（本 issue）** | §10 探针 → §4.1 策略模块（含 `index_revision`）→ §4.2 五站点收敛 → S1/S2/S3 → §4.3 `mcpp update` → §4.6 `--offline` / `MCPP_OFFLINE` / `[index] auto_refresh` + 可解释输出 → e2e | ~350–450 行，单 PR |
| **P1** | advisory 陈旧提示、`mcpp index status`/`why deps` 增加 rev 列、S4、`docs/` 新增「索引刷新策略」章节 | ~150 行 |
| **P2（各自独立 issue）** | ①`.xlings-index-cache.json` 烙绝对路径（主体在 xlings 侧）+ 把已有检测器接进 production；②`mcpp index update <name>` 的 per-index 粒度（需 xlings 支持）；③lock 成为构建期解析输入（§9） | — |

P0 内部顺序：**先落 `decide_for_dependency` + 单测**（判据表是契约），再切站点 4，再切其余四站，
最后加表达面。先测再切，否则 INV-3 的回归无法及早暴露。

---

## 8. 测试矩阵

e2e（全部离线可跑：隔离 `MCPP_HOME` + 工程内 `path` 索引）

| # | 场景 | 断言 |
|---|---|---|
| 1 | 依赖本地可解析 | **无** `Refreshing`/`Updating` 输出，exit 0 |
| 2 | 描述符本地缺失 | 刷一次，且只刷一次 |
| 3 | 本地只有 1.2，依赖 `^1.3` | 触发 `VersionMiss` |
| 4 | 精确版本 `= "1.2.3"` 而本机版本表无此条 | **不**在 decide 层触发（per-OS 版本表陷阱），由安装层兜 |
| 5 | `--offline` / `MCPP_OFFLINE=1` + miss | 结构化错误含 rev + age + `mcpp index update`，且**不发起网络** |
| 6 | **xim / lazy-git / 第三方 ns 依赖** | **不**触发刷新（INV-3 回归闸，最关键的一条） |
| 7 | 一次构建多个 miss | 只刷一次（进程内 once + debounce） |
| 8 | `mcpp update` | 输出 rev 变化（或 already at）；`--offline` 下明确拒绝 |
| 9 | 两个 mcpp 并发构建同一 MCPP_HOME | 只有一个刷新，另一个跳过而**不报错**（S3） |

单测：`decide_for_dependency` 判据表逐行；`is_index_dir_fresh` 未来时间戳 → stale；
`index_revision` 在文件缺失/为空/含尾随换行时的行为。

**测试写法陷阱（本仓库已踩过）**：反向断言不要写 `! $MCPP build | grep -q X` —— 管道左侧在
`errexit` 下被豁免，该断言**永不失败**。用
`out=$($MCPP build 2>&1); echo "$out" | grep -q 'Refreshing' && { echo FAIL; exit 1; }`。

---

## 9. 明确不做：`mcpp.lock` 作为构建期解析输入

现状：`prepare.cppm` 只**写** mcpp.lock（3829-3883）；全仓库读 lock 的只有
`pm/commands.cppm:394,413`（update/remove）与 `index_management.cppm:209`（index pin）。
`LockedIndex.rev` 还只是用户 pin 的副本，不是观测值。

让 lock 成为解析输入是「零网络 + 可重现」的终局，但它牵出：`resolve_semver` 仍绕过
`index_route` 的遗留（PR#307 的尾巴）；lock schema 演进与 index rev 的可信度；
「lock 与 manifest 漂移」的检测与自动修复语义（等价于 cargo 的 lock 更新规则）。

任一项都比本 issue 大。**本设计与它正交**：P0 之后即便永远不做 lock-honoring resolve，日常构建
也已经零网络。而 INV-2 采纳的 `.xlings-index-version` 恰好是它未来需要的地基
（把**观测到的** rev 写进 lock）。硬绑在一起只会让 #315 卡在大重构后面。

---

## 10. 探针结果（已跑，2026-07-30）

设计里有两个前提来自磁盘观测而非源码，开工前实测（"写判据要先用探针实测误报"）。

### P-1 `.xlings-index-version` 的更新语义 — 通过，但推翻了一个格式假设

真实 `mcpp index update` 的输出：

```
[xlings] [index] updated from artifact xim-index-ebf4020.tar.gz (ebf4020)
[xlings] [index] updated from artifact mcpp-index-8d67478.tar.gz (8d67478)
[xlings] [index] updated from artifact xim-index-awesome-2026.7.30.1.tar.gz (2026.7.30.1)
```

- ✅ artifact 通道确实写 `.xlings-index-version`，值等于 artifact 名里的 rev；刷新后上游没变则值不变
  ⇒ **它是内容身份，不是时间戳**。marker mtime 同时被更新（17:16 → 21:42），两者职责因此可以分开。
- ⚠️ **值不是稳定的 7 位 sha**：主索引是 `8d67478`，而子索引（awesome / scode / d2x）是**日期版本号**
  `2026.7.30.1`。实现必须按**不透明字符串**处理 —— 只比较相等、只打印，绝不解析长度或格式。
  文件本身**无尾随换行**，但实现仍 trim（Windows 侧写入的 `\r` 会让「同一 rev」每次都判成变化）。
- ❓ git 通道未直接观测（本机全部走 artifact）。按设计的降级预案处理：缺失 → `index_revision`
  返回 nullopt，advisory 只显示 age，**gate 不受影响**（gate 只依赖描述符可读性）。

### P-2 INV-3 误报实测 — 通过

含 `mcpplibs.cmdline` + `xim.nasm` + `somevendor.thing` 的真实工程，`-v` 下的判定：

```
index: mcpplibs:cmdline@0.0.1: resolvable locally
index: xim.nasm@2.16.03: no index can refute this
index: somevendor.thing@1.0.0: no index can refute this
```

xim 与第三方 ns 都落在 `SuppressedInconclusive`，**零刷新**。这正是把 miss 直接当刷新理由会
翻车的那一类：`xim.nasm` 被解析成候选 `(mcpplibs.xim, nasm)`，永远匹配不上身份门。

顺带发现并修正：诊断里回显解析后的候选（`mcpplibs.xim:nasm`）会把读者指向一个他从没写过的
命名空间 —— 主语改为**用户写的 `[dependencies]` 键**。

回归闸：单测 `InconclusiveNamespaceNeverRefreshes` + e2e 173 第 3 步。

---

## 11. 风险登记

| 风险 | 缓解 |
|---|---|
| INV-3 判据接错 → 每次构建都刷（比现状更糟） | 探针 P-2 + e2e #6 双闸；先落单测再切站点 |
| `.xlings-index-version` 语义与假设不符 | 探针 P-1；gate 不依赖 rev，最坏只损失 advisory 精度 |
| 用户感知「mcpp 不再自动更新索引」 | advisory 提示 + docs 明写 + `mcpp update` 变成真入口（D6） |
| `VersionMiss` 与 `resolve_semver` 重复实现约束求解 | 强制调用同一入口，不复制逻辑（否则又一处 D1） |
| per-OS 版本表导致 `VersionMiss` 误触发 | 只对 SemVer 约束判 VersionMiss；精确版本交安装层（e2e #4 锁住） |
| 刷新失败的降级掩盖真实网络故障 | 降级只在「本地有答案」时发生，warning 必须可见 + `-v` 打印底层 rc |
| S3 的锁在 Windows 上行为不一致 | 必须走 `platform/fs.cppm` 封装；拿不到锁=跳过而非阻塞，避免死锁面 |
