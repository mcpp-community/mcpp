# 索引版本下限(E0006)不应让旧客户端不可用 —— 分析与方案

> 状态：分析定稿，待实施
> 触发：用户报告 mcpp `0.0.109` 撞上 `index requires mcpp >= 2026.8.3.3 [E0006]`，
> 随后整条依赖解析崩成 `package 'compat:compat.libarchive@3.8.7' not found`
> 涉及：`src/pm/index_contract.cppm`、`src/pm/package_fetcher.cppm`、
> `src/pm/index_refresh.cppm`、`src/doctor.cppm`；索引侧 `mcpplibs/mcpp-index`
> 跨仓依赖：[openxlings/xlings#476](https://github.com/openxlings/xlings/issues/476)（枚举 + 按版本同步索引快照）
> 相关设计：`.agents/docs/2026-07-08-index-version-semantics-and-descriptor-grammar-design.md`

---

## 0. 结论摘要

**提问是对的，而且比它看上去更严重。**「旧 mcpp 应该仍然可用，只是提示可以升级」不仅是
合理诉求 —— 它本来就是 2026-07-08 那份设计里 **L1 明文承诺过的行为**：

> **L1 — authority (this design, mandatory)**: contract travels inside the tree;
> single check at the index-open choke point; **staged refresh keeps the last
> compatible snapshot**. Parse failure → graceful keep-old + warn.

「保留上一个兼容快照」**从未被实现**。今天的实现只有那一个 choke point 检查，
刷新路径对下限一无所知。于是 `xlings update` 会把一份能用的索引换成一份用不了的，
**且没有退路** —— 用户在刷新之后比刷新之前更糟。

同时，下限本身正在以设计没有预期的频率上移：它被当作「等于 CI pin」维护，
而 CI pin 跟着最新发布走。结果是**一个一周前的版本，被一次「新增一个包」的提交废掉**。

**方案的骨架是一条「索引历史线」**（§4.0）：索引不再是一个 rolling 指针，而是一条
带下限的版本线；客户端刷新时从最新往回找，取**第一个自己满足下限的快照**，
并只在发生降级时打印一次「最新索引需要 mcpp X，你是 Y，已改用 Y 能用的最新索引 Z，
升级请 `xlings update mcpp`」。**版本下限从终止信号变成路由信号。**

分发侧不需要新建任何东西：F5 证明 313 个历史快照都还在，缺的只是指针里的一份元数据，
而按 F1 的历史，下限拐点至今只有 6 个 —— **一条 6 项的历史线覆盖整个索引生命周期**。

六个发现，四个必须修，两个是治理问题。

---

## 1. 现象：一次失败，三段错误

```
Downloading xim:musl-gcc@… [===================>] 92.0 MB / 92.9 MB
error: index requires mcpp >= 2026.8.3.3 but this is mcpp 0.0.109 [E0006]
  Upgrade:  curl -fsSL .../install.sh | bash
error: xlings install_packages failed (exit 1) for 'compat.libarchive@3.8.7' …
  xlings reported: E_NOT_FOUND: package 'compat:compat.libarchive@3.8.7' not found
  wire address tried: compat:compat.libarchive@3.8.7
```

三段，各自都有问题：

1. **已经下载完 92MB 才发现用不了。** 兼容性判定发生在解包之后、读描述符的时候。
2. E0006 说清楚了原因，但它**不是终止点**。
3. 真正终止构建的是第三段 —— 一条 `not found`，**既不提版本也不提下限**，
   还附了一个"wire address tried"，把读者引向命名/寻址问题。

第 3 段不是巧合，是第 2 段的必然结果，见 F3。

---

## 2. 六个发现（均有证据）

### F1 —— 下限被当作「= CI pin」维护，而不是「能解析这棵树的最老客户端」

`index_contract.cppm` 的定义是清楚的：

```
min_mcpp = "0.0.85"   # oldest mcpp able to parse every descriptor
```

但 `mcpp-index/index.toml` 的维护规则写的是另一件事：

```
# Bump min_mcpp ONLY together with the CI MCPP_VERSION pin — lint parses
# descriptors with the pinned mcpp, which enforces the "floor first, new
# grammar after" rollout rule mechanically.
```

「ONLY together with」本意是**约束**（不许单独抬下限），实际被当成**等价**（pin 动，下限跟着动）。
证据是最近这次 bump，commit `160c389` **"feat: add boost-ext.ut 2.3.1 C++23 module package"**：

| 文件 | 变化 |
|---|---|
| `index.toml` | `min_mcpp` `0.0.109` → `2026.8.3.3` |
| `.github/workflows/validate.yml` | `MCPP_VERSION` `0.0.109` → `2026.8.3.3` |

而该 commit 自己的注释说明了 pin 为什么动：

> `…this floor on macOS, and **min_mcpp/latest_mcpp move with the pin as they** …`
> `2026.8.3.3 is the pin rather than .3.1: .3.2/.3.3 are cross-compilation…`

pin 上移的真实理由是 **mcpp#336(macOS 静态初始化)** 与交叉编译修复 ——
**与描述符语法毫无关系**。新加的 `boost-ext.ut` 描述符用的是 `generated_files`，
一个早就存在的特性；commit 信息本身还写着它是对着 **0.0.109** 开发验证的。

⇒ **下限上移了一百多个版本，没有任何描述符真的需要它。**
一个约一周前的发布(`0.0.109`)因此被废掉。

### F2 —— L1 承诺的「保留上一个兼容快照」从未实现

下限在**整个代码库里只被检查一次**：

```
$ grep -rn "check_index_floor" src/
src/pm/package_fetcher.cppm:619
```

`index_refresh.cppm`（决定「现在该不该联网刷新索引」的唯一真源）**完全不知道下限的存在**。
所以刷新流程从不问「我拉回来的这棵树，我自己能用吗」。

后果正是用户遇到的：**刷新把一份能用的索引换成用不了的，且不可回退**。
设计文档承诺的 "staged refresh keeps the last compatible snapshot" 落空了。

### F3 —— 违反下限降级成了「包不存在」，而不是「索引不可用」

`package_fetcher.cppm:614-622`：

```cpp
// Loud once per index; the resolve then fails as not-found with the cause
// already printed.
if (auto violation = mcpp::pm::check_index_floor(pkgsDir.parent_path())) {
    mcpp::ui::error(*violation);
    return std::nullopt;            // ← 每一次描述符读取都变成 "没找到"
}
```

注释承认了这个设计（"the resolve then fails as not-found"），但它把**两个语义不同的事实
压成了同一个返回值**：

- 「这个包不在索引里」
- 「这个索引我读不了」

于是解析器继续按「没找到」的剧本走：回退到 legacy 派生 wire 地址、尝试其它 repo、
最后报一条 `E_NOT_FOUND … wire address tried: …`。**终止构建的那条错误，与真实原因隔了两层。**

> 这个失败形态在本仓库有先例，且已被写进 `install_pinned_mcpp.sh` 的头注释：
> *"the floor check made EVERY descriptor read return nothing, every dependency fell
> back to its legacy derived address, and the build died on `mcpplibs.cmdline@0.0.1`
> — an error naming neither the version nor the floor."*
> 同一个坑，换了一个触发源，又踩了一次。

### F4 —— `latest_mcpp` 被每个索引写入，但**没有任何消费者**

```
$ grep -rn "latestMcpp\|latest_mcpp" src/ | grep -v index_contract.cppm
(空)
```

它被解析进 `IndexContract::latestMcpp` 就再没被读过。而它恰好就是协商层需要的那个字段。

### F5 —— 分发侧已经保留了全部历史快照，协商今天就做得到

`xlings-res/xim-index` 的 rolling `latest` release 里有 **313 个 artifact**
（`xim-index-<ver>.tar.gz`），历史快照并没有被删。

缺的只是**指针里没有下限信息**：

```json
{ "format_version": 1, "index_version": "20e53c6",
  "artifact": { "name": "xim-index-20e53c6.tar.gz", "sha256": "…", "size": 371707 } }
```

客户端在下载前无从判断这份 artifact 自己能不能用 —— 这正是设计里 **L2（projection）**
被推迟掉的那一层。**「用自己版本当前最新的 index」在分发侧已经可行，只差元数据。**

### F6 —— 下限是**整个索引**一个开关，而需求是**逐描述符**的

设计明确拒绝过逐描述符下限：

> Rejected alternative: per-descriptor `min_mcpp`. Finer-grained, but N places to
> maintain, and the failure it prevents … is already covered by one index-wide dial.

代价是：`compat.zlib` 的描述符几个月没动过，却因为**别人**新加了一个包而变得不可读。
索引越大、贡献越活跃，这个耦合的伤害越大 —— 而索引正在变大。

---

## 3. 根因分层

| 层 | 陈述 | 对应发现 |
|---|---|---|
| **L0 表层** | 违反下限终止构建，且终止在一条误导性的 `not found` 上 | F3 |
| **L1 机制** | 兼容性只在「读描述符」处检查；**刷新与下载都不检查**，所以没有「保留可用快照」这回事 | F2 |
| **L2 分发** | 只有一个 rolling `latest`，指针不带下限 ⇒ 客户端无法选择自己能用的最新快照 | F4 F5 |
| **L3 治理** | 下限的**语义**是「最老可解析客户端」，**维护方式**却是「等于 CI pin」⇒ 它以发布频率上移 | F1 |
| **L4 粒度** | 全索引一个开关，而要求是逐描述符的 ⇒ 一个包的新语法废掉整个目录 | F6 |

**判据一句话**：*版本下限是一个路由信号，不是一个终止信号。*
客户端与索引不匹配时，正确的反应是**选一个匹配的**，选不到才报错 —— 而不是把手上能用的也丢掉。

---

## 4. 方案

### 4.0 核心机制：**索引历史线 + 兼容性优先选择**

一句话：**索引不是一个 rolling 指针，而是一条带版本下限的历史线；
客户端每次刷新时，从最新往回找，取第一个自己满足下限的快照。**

不匹配从「错误」变成「选择」——这是整份方案的骨架，其余各项都是为它服务或为它兜底。

#### 解析算法

```
refresh(index_name, own_version):
    hist = fetch_history(index_name)          # 新 → 旧，含每条的 min_mcpp
    latest = hist[0]

    if own_version >= latest.min_mcpp:
        return use(latest)                    # 常规路径，与今天一致

    # 最新的用不了 —— 不是错误，是一次路由
    notice(latest, own_version)               # 见下方文案
    for snap in hist:                         # 继续往回找
        if own_version >= snap.min_mcpp:
            return use(snap)                  # 「当前版本兼容下的最新 index」

    # 整条线都不兼容（只可能发生在极老的客户端）
    keep_existing_or_fail()                   # → R2 / R3
```

判据是**单调**的：`min_mcpp` 沿历史线只增不减，所以一旦找到第一个满足的就是最优解，
不需要遍历全部 313 条。

#### 用户看到的（正是提问描述的那种）

```
note: the newest index (2026.8.3.3) requires mcpp >= 2026.8.3.3; this is mcpp 0.0.109.
      Using the newest index your version supports: 0.0.109-era (indexed 2026-07-27).
      To pick up newer packages, upgrade mcpp:
          xlings update mcpp        (or: curl -fsSL <install.sh> | bash)
```

三件事一次说清：**为什么没拿最新**、**实际用了哪个**、**怎么升级**。
构建**继续进行**，退出码 0。

它只在「选择发生了降级」时打印一次，常规路径完全静默 —— 否则每次构建刷屏，
用户很快就学会无视它，那这条提示就白写了。

#### 历史线从哪来

F5 已经证明**分发侧不需要新建任何东西**：`xlings-res/xim-index` 的 rolling `latest`
release 里 313 个 artifact 都还在。缺的只是「每条的 `min_mcpp` 是多少」这份元数据。

指针从「一条」变成「一条 + 历史」：

```json
{
  "format_version": 2,
  "index_name": "xim",
  "latest":     { "index_version": "20e53c6", "min_mcpp": "2026.8.3.3",
                  "generated_at": "2026-08-03T10:14:21Z", "artifact": { … } },
  "history": [
    { "index_version": "20e53c6", "min_mcpp": "2026.8.3.3", "generated_at": "…", "artifact": { … } },
    { "index_version": "0adb288", "min_mcpp": "0.0.109",    "generated_at": "…", "artifact": { … } },
    { "index_version": "7fef5ec", "min_mcpp": "0.0.108",    "generated_at": "…", "artifact": { … } }
  ]
}
```

- `min_mcpp` 由发布脚本从**树内 `index.toml`** 机械投影，不手写 ——
  权威仍在树内，指针只是投影，冲突以树内为准（与原设计 L2 的定性一致）。
- `history` 只需保留**下限发生变化的那些拐点**，不必是全部 313 条：
  同一 `min_mcpp` 的连续快照里只有最新那条有意义。按 F1 的历史，
  拐点至今只有 6 个（`0.0.85 / 0.0.87 / 0.0.101 / 0.0.102 / 0.0.108 / 0.0.109 / 2026.8.3.3`）。
  **一条 6 项的历史线就能覆盖整个索引生命周期。**
- **存量 313 个快照没有这份元数据。** 三选一，建议 (b)：
  (a) 一次性回填（逐个解包读 `index.toml`）；
  (b) **只为拐点回填**（6 条，人工可核，成本几分钟）；
  (c) 缺失 ⇒ 视为「未知，试一次」——不推荐，把确定性换成了下载。

#### ⚠️ 谁来执行「回溯选择」：mcpp 今天**做不到**，这是本方案最硬的约束

自然的想法是「这全是客户端的事，mcpp 自己解决就好」。核实下来不成立：

**mcpp 不下载索引。** `index_management.cppm:148` 的刷新就是

```cpp
int rc = mcpp::xlings::update_index(xlEnv);   // → 跑 `xlings update`
```

artifact / git 的同步全部发生在 **xlings** 进程里。mcpp 对索引传输的全部控制权，
是它写进 `.xlings.json` 的 `index_repos` 条目 —— 而那个结构

```cpp
struct SeedRepo {
    std::string name;
    std::string url;
    std::string artifact;   // artifact 源基址
    std::string source;     // "auto" | "artifact" | "git"
};
```

**没有版本/rev 字段**。索引版本串在 mcpp 里还被显式标注为 *OPAQUE BY CONTRACT*
（`xlings.cppm:370`）。也就是说：**mcpp 无法要求「给我索引版本 X」**，
它只能说「去同步」，然后接受 xlings 给的那一份。

⇒ 4.0 的回溯选择需要三选一，按代价排序：

| 路径 | 内容 | 代价 |
|---|---|---|
| **(a) 推荐** | xlings 增加「枚举索引快照 + 按版本同步」的能力，mcpp 在 `.xlings.json` 里下发（`SeedRepo` 加一个 pin 字段）。**已提 [openxlings/xlings#476](https://github.com/openxlings/xlings/issues/476)** | 跨仓协作一次，之后两边都干净 |
| (b) | mcpp 自己接管索引 artifact 的下载与解包 | 与 xlings 职责重叠、镜像/CDN/校验逻辑要重写一遍 |
| (c) | 只对 mcpp 自己的索引（`mcpplibs/mcpp-index`）实现，xim 索引靠 R2 兜底 | 覆盖面不完整，但**不需要任何跨仓改动** |

**这不影响 R2。** R2（保留/回滚到上一个可用快照）**完全在 mcpp 侧、今天就能做**：
刷新后检查下限，违反就把索引目录回滚到刷新前的备份。它不需要选择任何版本，
只需要「不要用坏的覆盖好的」。**「变砖 → 可用 + 提示升级」这一步不依赖任何人。**

#### git 传输的索引怎么办

`mcpp-index` 同时是一个 git 仓库（`[indices] git =`）。历史线在那里是**天然存在**的：
`index.toml` 的每一次 `min_mcpp` 变更就是一个拐点 commit。等价实现是
**按 commit 回溯**取第一个兼容的树，或（更省事、更可审计）由索引仓库为每个拐点打 tag：

```
index/min-mcpp-0.0.109
index/min-mcpp-2026.8.3.3
```

客户端 `git fetch --tags` 后按 tag 选择即可，无需 clone 全史。
本地 `path =` 索引不参与历史线（它就是用户自己的目录），保持现状：违反 ⇒ R1 报错。

#### 离线

历史线的选择结果必须**落盘记录**（哪个 index_version、什么下限、为什么选它）。
离线时不联网、不重新选择，直接用上次选中的快照 —— 与 `index_refresh` 现有的
offline-first 判据（「所有解析输入都在磁盘」）一致，不新增网络依赖。

---

以下各项按「是否为 4.0 所必需」排序。

### R1（必须）· 违反下限必须是「索引不可用」，不能退化成「包不存在」

`package_fetcher` 的读取路径要能区分三种结果，而不是两种：

```cpp
enum class DescriptorLookup { Found, Absent, IndexUnusable };
```

- `IndexUnusable` **不得**触发 legacy 地址回退、不得被其它 repo 的 `Absent` 掩盖、
  不得被 `index_refresh` 读成「本地没有 ⇒ 该刷新了」（否则形成刷新抖动：
  拉回同一份不兼容的树，再失败，再刷新）。
- 解析结束时若有索引处于 `IndexUnusable`，**最终错误就是 E0006 本身**。
  用户看到的第一条与最后一条错误必须是同一件事。

4.0 落地后这条路径应当**几乎不可达**（选择阶段就避开了不兼容的树）——
但它必须存在，因为 `path =` 索引、手工放置的树、以及历史线整条不兼容时仍会走到它。

### R2（必须）· 选择失败时保留上一个可用快照，绝不用不可用的覆盖可用的

补上 L1 承诺过而没实现的那一半（F2）。即使有了 4.0，这一条仍然必须：
历史线获取失败（网络、镜像、artifact 缺失）时，**手上那份能用的索引就是最后的防线**。

1. 下载/解包后、切换生效前，对**暂存的**树跑 `check_index_floor`；
2. 违反 ⇒ 不切换，保留现有索引并 warn；
3. 本机完全没有可用索引时才落到 R3。

xim 索引的下载/解包由 **xlings** 执行，不在 mcpp 进程内。在拿到「解包到暂存目录」
的能力之前，退化实现同样有效：刷新后立即检查，违反则**回滚到刷新前的快照**
（保留一份 `last-compatible` 备份），并在本进程内标记该索引不可再刷新。

> 这一条修掉的是最伤人的性质：**刷新让情况变得更糟，且不可逆。**

### R3（必须）· 无任何可用树时，一次说清楚

冷启动且历史线整条不兼容（或不可达）时，只报 E0006，指明**哪个索引、要求什么、本机是什么**，
并给出升级命令。保留 `MCPP_INDEX_FLOOR=ignore` 作为调试逃生舱。

### R4（应做，治理）· 下限必须与 CI pin 解耦，并由 CI 证明

**优先级说明（修订）**：早先的排序把这条摆得过重。4.0 + R2 落地后，下限乱涨的危害
从「变砖」降到「旧客户端停在老索引、拿不到新包，但构建正常」。所以这条是 **hygiene，
不是阻塞项** —— 它值得做，因为无声地把所有旧客户端钉在老索引上仍然是损失，
而且这个损失**不再有任何报错提醒任何人**；但它不该排在 R1/R2 前面。

1. **改掉 `index.toml` 的维护规则**：从「与 CI pin 一起 bump」改为
   **「只有当某个描述符真的用了旧客户端解析不了、或会静默错解的东西时才抬」**，
   并在 commit 里点名是哪个描述符、哪个特性。
2. **让 CI 证明它**：索引 CI 现在只用 pin 的 mcpp 跑 lint（只能证明「新版本能解析」）。
   应当**再用当前 `min_mcpp` 那个版本跑一遍**：
   - 旧版本能全部解析 ⇒ **下限不许动**；
   - 旧版本解析失败 ⇒ 下限**必须**抬到能解析的最低版本，且 CI 报出是哪个文件。
   这把「floor first, migration after」从**约定**变成**机器判据**。
3. **`latest_mcpp` 与 `min_mcpp` 分开**：前者跟 pin 走（「已知最佳」，也是 4.0 提示文案里
   建议升级到的目标版本 —— F4 指出它目前无人消费，这是它的第一个真实用途），
   后者只在 (2) 判定必须时才动。今天两者恒等，等于把「最佳」当成了「最低」。

### R5（应做）· 逐描述符下限

原设计以「N places to maintain」拒绝过它。这个理由在贡献者变多后不再成立：
维护成本落在**新增该描述符的那个 PR**（一行），
而现状的成本落在**所有旧客户端**（整个目录不可读，F6）。

- 索引级 `min_mcpp` 保留，语义收紧为「解析这棵树的目录结构与通用字段」所需
  （真正的全局语法变更，如 SPEC-001 身份形态迁移）；
- 单个描述符可选声明 `mcpp = ">= X"`，客户端不满足时**只跳过该描述符**并 warn。

这样「新加一个包」在结构上不可能再废掉整个索引 —— F6 的根治。
它与 4.0 是互补的：4.0 让**客户端**总能找到能用的快照，R5 让**索引**不必因为一个包而整体后退。

### R6（可选）· 版本化通道（原设计 L3）

`index.toml` 的 `spec` 作为通道键，`mcpp-index-v1-latest` / `-v2-latest`。
4.0 落地后增量收益不大，**不建议现在做**；但指针形状应留出扩展位（`format_version` 已在）。

## 5. 不采纳

| 方案 | 理由 |
|---|---|
| 直接把下限检查改成 warning、继续解析 | 下限存在的理由是真的：`0.0.101` 那次是 `compat.opencv` 的 per-OS feature flags 被旧客户端**静默忽略**，产出错误的构建。放行 = 用「静默错误产物」换「明确失败」，方向反了。R1/R2 保住的是**旧快照**，不是**用旧客户端读新描述符**。 |
| 让旧客户端忽略看不懂的字段 | 同上：`0.0.101` 的教训正是「忽略 = 静默错」。 |
| 索引不再抬下限 | 语法要演进。问题不在抬，在**抬的理由**（F1）和**抬之后旧客户端没有退路**（F2）。 |
| 只做 R4（分发侧协商），不做 R2 | 冷启动能救，但**已经装好的用户在 `xlings update` 后依然会被换成不可用的索引**——最伤人的那条路径没修。 |
| 只做 R2，不做 R5 | 旧客户端不再变砖，但下限仍以发布频率上移，**所有人都会持续拿不到新包**，只是不再报错。症状消失，问题还在。 |

---

## 6. 实施顺序（修订）

排序的依据是**受众**，不是技术难度。

### 为什么索引侧那一步删不掉

**任何 mcpp 侧修复都救不了已经发出去的 0.0.109。** 修复最早出现在 `2026.8.3.5`，
而现在被卡住的用户手里就是 0.0.109。他们只有两条路：**升级 mcpp**，
或者**索引把下限降回去**。所以：

- **索引侧回退** = 针对**存量用户**的唯一无痛解（且不需要发 mcpp）
- **mcpp 侧修复** = 针对**未来所有版本偏斜**的根治（但只对未来的版本生效）

两者受众不同，不是重复劳动。这也是为什么它排第一 —— 不是因为它更重要，
而是因为它是唯一今天就能让人恢复的动作。

### 顺序

| # | 动作 | 侧 | 受众 / 效果 | 依赖 |
|---|---|---|---|---|
| 1 | `min_mcpp` 回退到真实需要的值（`latest_mcpp` 保留 `2026.8.3.3`） | 索引 | **存量 0.0.109 用户立刻恢复** | 无 |
| 2 | R1 + R3：错误语义（`IndexUnusable` ≠ `Absent`） | mcpp | 可诊断性；4.0 的兜底 | 无 |
| 3 | **R2：刷新闸 + 回滚到上一个可用快照** | mcpp | **「变砖 → 可用 + 提示升级」** | **无** |
| 4 | 4.0 历史线：先走路径 (c)（只覆盖 mcpp 自己的索引） | mcpp | 「拉取当前版本兼容下的最新 index」 | 无 |
| 5 | 4.0 完整覆盖：xlings 加索引版本 pin，`SeedRepo` 下发（[xlings#476](https://github.com/openxlings/xlings/issues/476)） | xlings + mcpp | xim 索引也进入历史线 | 跨仓，**不阻塞 1–4** |
| 6 | R4：索引 CI 双版本 lint + 维护规则改写 | 索引 | hygiene：阻止下限无理由上移 | 无 |
| 7 | R5：逐描述符下限 | mcpp + 索引 | 一个包不再废掉整个目录 | 无 |
| 8 | R6：版本化通道 | — | 视生态规模再议 | — |

**第 3 步是性价比最高的一步**：纯 mcpp 侧、不依赖任何人、不需要任何新协议，
就把最伤人的性质（刷新让情况变得更糟且不可逆）修掉。
第 4 步给出你要的「历史线」语义，第 5 步才把它铺满整个生态。

---

## 7. 验证

- **回归 e2e**：fixture 索引 `min_mcpp = "9.9.9"`。当前断言是「build fails」；
  R2 之后应改为**「保留旧索引、构建成功、打出升级 warning」**，并新增一条
  「本机无任何兼容索引 ⇒ 只报 E0006，且 stderr 中不出现 `not found`」。
- **F3 的判据要写成断言**：违反下限的构建，其**最后一条**错误必须包含 `E0006`。
  这条断言正是这次（以及 `install_pinned_mcpp.sh` 记载的上一次）缺失的那个。
- **R5(2) 的自证**：在索引 CI 里故意把 `min_mcpp` 调低一档，双版本 lint 必须变红。

---

## 8. 附：这次事件的时间线（供复盘）

| 时间 | 事件 |
|---|---|
| 2026-07-08 | 设计确立 L1/L2/L3；L2、L3 判为「operationally premature」推迟 |
| 2026-07-08 | L1 落地，但只实现了 choke-point 检查；**「保留上一个兼容快照」未实现**（F2） |
| 2026-07-21 | 下限 `0.0.87` → `0.0.101`，理由真实（opencv feature flags 被静默忽略） |
| 2026-07-26 | 下限 → `0.0.108`/`0.0.109`，理由真实（SPEC-001 身份迁移） |
| 2026-08-03 | 下限 `0.0.109` → `2026.8.3.3`，**理由不成立**：pin 因 mcpp#336(macOS) 上移，下限机械跟随（F1） |
| 2026-08-03 | 用户 `0.0.109` 变砖，且报错指向 `not found`（F3） |

前两次 bump 是这个机制**正确工作**的样子；第三次是它**语义漂移**的样子。
区别不在于机制，而在于「谁来证明这次 bump 是必要的」—— 目前没有人，也没有机器。
