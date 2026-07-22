# 契约准入(Contract Admission)设计 —— 索引级 + 包级 floor

> 日期:2026-07-22
> 基线:mcpp **0.0.102**(`d571365`,含 PR #266)
> 状态:**设计文档,不含实现**。本文定义 mcpp 的「索引/描述符 → 客户端版本契约」机制应有的形状,供后续实现批次取用。
> 触发来源:issue **#265**(`mcpp new --template imgui` 三平台失败)。#265 本身**不作为缺陷单独修复**——裁定见 §0.2;其现场分析作为本设计的实证输入,存档于 §6。
> 所有 `file:line` 锚点在 `d571365` 上核实;§6 的行为结论用两个真实二进制实证。
> 架构主线沿用 [2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md](2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md) §5.2「同一决策不许两处推导」,本文推广一格:**同一返回值不许编码两个正交语义**。

---

## 0. 结论速览

### 0.1 机制目标

一个客户端(mcpp 二进制)可能遇到比自己新的索引树或描述符。契约机制回答一个问题:**这份数据我能不能信?** 目标形态是**两级契约**:

| 层 | 载体 | 求值点 | 不满足时 |
|---|---|---|---|
| 索引树 | `index.toml` 的 `min_mcpp`(**已实现**,0.0.85 起) | 索引集合枚举 —— 必须早于解析该树的任何描述符 | 该树移出搜索路径,**继续搜其他索引**;绝不终止进程 |
| 描述符 | `mcpp = { min_mcpp = ... }`(**未实现**,本文设计) | 段体被采信之前(`synthesize_from_xpkg_lua` 入口) | **终止该包的解析,精确报错;绝不回退到另一个候选** |

两级是**合取**关系,顺序被强制,包级不能下调索引级(§2.2)。

### 0.2 关于 #265 的裁定

`mcpp new --template imgui` 在 fresh-install CI 三平台失败,现场根因是 `check_index_floor()` 把「是否违约」与「本次要不要打印」编码进同一个返回值,导致**同一进程内第一次查询被拦、之后静默放行**(§6)。

**裁定:不作为独立缺陷修复**,并入本文的契约机制整体实现。理由:

- 它只在「客户端比索引旧」这一**契约外状态**下可见,而这正是本机制要重新定义的区域;
- 单独打补丁会固化当前的单级形态,与 §2 的两级模型冲突;
- CI 变红的直接原因是版本 skew(§5),与代码修不修无关。

**承认的残留**:在该状态下,依赖解析路径会静默使用客户端读不懂的描述符 —— 正是索引侧抬 floor 想防的。记录在案,随本机制实现一并消除。

### 0.3 本设计的关键结论

| # | 结论 | 依据 |
|---|------|------|
| 1 | 契约求值必须拆成**纯谓词 / 幂等决策 / 唯一有状态 reporter** 三件事 | §1.1;混用一个返回值必然让决策非幂等(§6.2) |
| 2 | 契约是**候选集过滤器**,不是读取路径上的检查 | §1.2;roadmap W5 原文即 open-time |
| 3 | **I7:契约不满足永远不能成为「再换一个候选」的理由** | §1.3;#265 的因果链就是「不满足 → 换候选 → 换到另一个包」 |
| 4 | 被拒索引 = 排除该索引,**不是终止进程** | §1.4;否则一个超前的私有索引就能 brick 掉 `mcpp toolchain install` 与升级路径 |
| 5 | 查找落空要**精确归因**(「它在索引 X 里,X 要求 mcpp >= N」),而不是把用户指向 `mcpp index update` | §1.5 |
| 6 | 包级 floor 的键**必须先于索引启用它进入客户端词表**,否则老客户端静默忽略,机制自废 | §2.3;未知 mcpp 段键今天是「跳过 + 记录 + 警告」(`xpkg.cppm:1519-1528`) |
| 7 | 不让 `schema` 兼任客户端下限 | §2.4;同一决策两处推导 |
| 8 | 索引侧需要 lint:描述符文法 ≤ 自身声明的 `min_mcpp` | §2.5;否则维护者会忘记声明,包级 floor 沦为装饰 |

---

## 1. 索引级契约:准入模型

### 1.1 三职责分离

现状(`src/pm/index_contract.cppm:96-112`)把三件事压进一个 `optional<std::string>`:违约文本、放行判据、以及内部 `static std::set` 的通知去重。通知天然需要状态,决策天然必须无状态 —— 二者共用返回值,状态就必然渗进决策。

目标形状:

```cpp
// 纯:该索引是否违约。无状态、无输出、不读环境,可任意次调用
std::optional<std::string> index_floor_violation(const path& indexRoot);

// 唯一有状态:首次遇到该实体时打印,返回是否打印过。
// 去重键是不透明字符串("index:mcpplibs" / 将来 "pkg:compat.opencv"),
// 不是路径 —— 两级契约共用同一个 reporter。
bool report_blocked_once(std::string_view key, std::string_view violation);

// 决策:本进程可否读取该索引。幂等 —— memo 缓存的是决策,不是通知
bool index_admitted(const path& indexRoot);
```

核心原语仍只有一个:既有的纯谓词 `floor_violation(minMcpp, ownVersion)`(`index_contract.cppm:79-94`)。它天然**粒度无关**,索引级与包级都只是它的适配器。

### 1.2 契约是候选集过滤器

```
数据根 (data/) ──枚举+排序──▶ 候选索引集合
                                  ├── admitted ──▶ 解析层只看得见这些
                                  └── blocked  ──▶ 进程内报告一次(E0006);
                                                   仅在失败路径被回访(§1.5)
```

解析层(描述符读取、身份校验、版本解析、依赖图)**完全不认识 floor**,只消费一个已过滤的集合。今天的实现把契约放在最内层的 `read_identity_verified_xpkg_lua`(`package_fetcher.cppm:582-619`),等于把「这棵树整体不可用」这种全局事实表达成「这次查找没找到」这种局部软失败。

求值点应为(每处一行):

1. `sorted_index_dirs`(`package_fetcher.cppm:623-633`)→ 过滤后成为全局数据根的唯一索引集合来源(消费点 `:652`、`:698`);
2. `read_xpkg_lua_from_path`(`:667-679`)单根打开 —— 准入幂等,所以「每次调用都问」与「开一次问一次」行为等价;
3. 将来的描述符接受点(§2)。

> 印证:`.agents/docs/2026-07-08-descriptor-index-evolution-roadmap.md:27` 的 W5 结论原文即「**open-time check** is the mcpp-side enforcement」。实现当初落在了 read-time。

### 1.3 不变量

| ID | 不变量 |
|---|---|
| **I1** | 契约求值是纯函数:无状态、无输出、不读环境 |
| **I2** | 准入决策幂等:同一进程内同一实体,第 1 次与第 n 次结果恒等 |
| **I3** | 进程内唯一可变状态是「已报告集合」,只影响是否打印,**不影响任何返回值或控制流** |
| **I4** | 契约逻辑只存在于一个模块,且只在**候选集构造点**生效;候选集的消费者一律不得复检 |
| **I5** | 不静默降级:被拒实体不会以任何路径被采信;查找落空时给出精确归因,而非裸 not-found |
| **I6** | 永不 brick:无 `index.toml`、畸形 `min_mcpp`、逃生舱开启 → 一律准入;被拒索引只被排除,**绝不终止进程** |
| **I7** | **契约不满足永远不能成为「再换一个候选」的理由**。只有在身份尚未确定的来源层(索引树 = 可替代的来源),它才表现为候选集过滤;身份一旦确定(某个具体包的描述符),它是该实体的终局事实 |

**I7 是本设计最重要的一条**,它把 #265 的因果链固化为禁令:当年是「契约不满足 → 换个 namespace 再试 → 换到了另一个包」。包级 floor 落地时,如果写成「这个描述符不合格就继续扫下一个索引」,会立刻复刻同一个 bug,只是换了触发点。

### 1.4 为什么是「排除」而不是「终止」

被拒索引若直接 abort 整个进程,一个超前的私有索引就能让用户连 `mcpp toolchain install`(升级自己的路径)都跑不了 —— 把局部问题升级成砖头。`xim-pkgindex` 没有 `index.toml`,永远不会被拒,所以工具链安装、升级、以及不依赖该索引的构建在任何情况下都应照常工作。这是 **I6** 的实质内容。

### 1.5 失败归因:精确回答,而不是泛泛后缀

查找落空后,**只扫被拒的索引**,用同一套身份校验回答一个确定的问题:这个包是不是就在被拒的索引里?

- 今天:`dependency 'X' ... isn't cloned locally yet — run 'mcpp index update' first`(`resolver.cppm:97-102`)—— 把用户指向一个更新解决不了的方向;
- 目标:`dependency 'X': 'X' is present in index 'mcpplibs', but that index requires mcpp >= 0.0.102 [E0006]`。

成本只有失败路径上的一次额外扫描(成功路径零开销);契约知识仍只在准入层,调用点只是拼一句现成的话。两个消费点:`resolver.cppm:97-102`(依赖)、`create.cppm:51-55`(模板)。

### 1.6 逃生舱

`MCPP_INDEX_FLOOR=ignore` 语义保持,位置应在 `index_admitted` —— 它关的是**准入**,不是「打印」。包级 floor 落地时增加粒度无关的别名 `MCPP_FLOOR=ignore`(旧名保留)。

---

## 2. 包级契约:`mcpp = { min_mcpp = ... }`

### 2.1 为什么位置是 `mcpp` 段

Form B 的清单由**索引维护者随时改写、与包版本解耦** —— 「索引改了、客户端没换」的窗口只存在于 Form B。Form A 的清单随 tarball 冻结,不存在该窗口,**不需要**对称机制。

per-OS 免费生效:`mcpp.linux.min_mcpp` 自动工作,因为当前平台的子表在解析循环前已拼进段体(`xpkg.cppm:1508-1512`)。

### 2.2 与索引级的关系:合取 + 强制顺序

索引 floor 保护的是「你能不能正确解析这棵树」。连解析都不可信时,从描述符里读到的包级 floor 同样不可信 —— 所以**索引级必须先于解析该树的任何描述符求值**,包级不能下调索引级。这条要写进模块头注释,防止以后被「优化」成一处。

### 2.3 启用顺序被强制:键先进客户端,索引后使用

今天未知的 mcpp 段键是**跳过 + 记录 + 适配点警告**(`xpkg.cppm:1519-1528`),不是错误。因此:

> 索引一旦启用 `min_mcpp`,**老客户端会静默忽略它** —— 版本标记恰好被它要提醒的那批客户端忽略,机制自废;要么白做,要么又得靠抬索引级 floor 强推,回到我们想摆脱的连坐。

所以实现顺序是:**客户端先支持并发布 → 广泛部署 → 索引侧再启用**,中间至少隔一个传播周期。落地内容:

- `kKnownXpkgKeys` 增加 `min_mcpp`(`xpkg.cppm:134-140`);
- 解析循环在 `schema` 分支旁增加一支(`:1514`),复用纯谓词 `floor_violation`;
- 违规 → `synthesize_from_xpkg_lua` 返回 `ManifestError`(终局失败,不回退 —— **I7**)。

### 2.4 不让 `schema` 兼任

`schema` 是布局标签(`:1514-1517`,"informational only")。让它承担客户端下限,就是两个机制表达同一个决策,触碰「同一决策不许两处推导」。`min_mcpp` 与 `index.toml` 共用同一个词、同一个谓词、同一句可执行提示。

### 2.5 索引侧配套

`mcpp xpkg parse` 增加 lint:**描述符使用了高于自身 `min_mcpp` 的文法即报错**。这是整套机制可靠的前提;否则维护者会忘记声明,包级 floor 变成装饰。

### 2.6 终局效果

#265 的触发是索引侧为了 `compat.opencv` 的 per-OS feature flags 把**整棵树**的 floor 抬到 0.0.101(mcpp-index #107),于是毫不相干的 imgui 模板被连坐。包级 floor 之后,受影响的只有 `compat.opencv`;索引级 floor 退回「整棵树布局/文法演进」才动 —— 这类连坐**从机制上消失**,而不是靠运维排期去躲。

---

## 3. 实现时的变更面(供后续批次取用,本 PR 不做)

| 文件 | 改动 |
|---|---|
| `src/pm/index_contract.cppm` | `check_index_floor` → `index_floor_violation` / `report_blocked_once` / `index_admitted`;模块头写入 I4/I6/I7 与两级顺序约束 |
| `src/pm/package_fetcher.cppm` | `:593` 契约分支删除;索引集合过滤;`read_xpkg_lua_from_path` 单根准入;失败归因入口 |
| `src/pm/resolver.cppm` | `:97-102` 依赖 not-found 前先问归因 |
| `src/scaffold/create.cppm` | §6.3 的冗余第二趟删除;Form B 命中给精确诊断(PR #266 已用 `continue` 覆盖该场景,届时合并为一种写法) |
| `src/manifest/xpkg.cppm` | `min_mcpp` 进词表 + 解析分支 + 终局失败(§2.3) |

**测试(缺口很大,实现时必须补齐)**

- 单测:`index_contract` 今天只覆盖**纯**函数(`floor_violation`、`read_index_contract`);有状态的 `check_index_floor` **零覆盖**。必须加「同一实体连问 3 次结果恒等」——幂等缺陷天生逃逸任何「调一次」的用例。
- e2e:floor 全链**零覆盖**(`grep -rl "min_mcpp\|E0006" tests/` 只命中那一个单测文件);roadmap W5 记的「9.9.9/0.0.84 e2e」在树里不存在。需要:违规索引不得回落到其他候选、E0006 恰好一次、逃生舱、多索引时只排除违规的那个、描述符级违规不得回退。

---

## 4. 显式否决的方案

| # | 方案 | 否决理由 |
|---|---|---|
| **R1** | 只删 `index_contract.cppm:110` 的去重行 | 每次查询都打印(单次 resolve 可达数十次)→ 刷屏;策略仍在机制层,且把「一个返回值两种语义」的类型缺陷原封保留 |
| **R2** | 任何被拒索引 → 全局 abort | 见 §1.4:把局部问题升级成砖头 |
| **R3** | 契约检查下沉到 xlings staged-unpack | 跨仓库;roadmap W5 已裁定 open-time 检查是 mcpp 侧责任;且不解决幂等性缺陷 |
| **R4** | 给每条 not-found 挂泛泛的「可能有索引被排除」后缀 | 信息量低且把契约知识散回所有调用点;已被 §1.5 的精确归因取代 |
| **R5** | 用 `schema` 承载包级 floor | 见 §2.4 |
| **R6** | 只在模板路径规避(PR #266 的形态) | 治标:同样场景仍失败;泄漏在依赖解析路径原样存在。#266 作为错误信息改善已合入(`d571365`),不承担机制职责 |

---

## 5. 运维不变量(与代码无关,先行)

#265 的**直接**触发是版本 skew,不是代码:索引侧 2026-07-21 把 `min_mcpp` 抬到 0.0.101(mcpp-index #107),而 `ci-fresh-install` 经 `xlings install mcpp` 装到的是 0.0.100。

现场证据:`wait-index` 守卫在 22:30:02 报告 `index tracks 0.0.102`,10 秒后同一次运行里的 job 仍装到 `xim:mcpp@0.0.100`。**守卫轮询的是 `raw.githubusercontent.com`,而 runner 解析走的是自己的索引副本(镜像/缓存链路)** —— 两者不是同一个源,所以守卫绿 ≠ 装得到。

不变量:

1. **索引侧抬 `min_mcpp` 之前**,必须先确认发布链路(xim-pkgindex 及其镜像)已能装到满足该 floor 的 mcpp;
2. **fresh-install 必须安装「被测版本」**,而不是「索引里最新的那个」。本 PR 已把 `ci-fresh-install` 的四处安装点改为显式 `mcpp@${MCPP_PIN}`(工作流级 `env`,随发布与 `.xlings.json` 一起 bump)。装不到时以 `version not found` 显式失败,而不是静默降级到旧二进制。
3. `ci-aarch64-fresh-install` 保持不 pin —— 它的既定目的就是「完全模拟新用户 `xlings install mcpp`」,且不与发布事件绑定。

包级 floor 落地后,这条运维约束的触发频率会大幅下降(§2.6),但**不会消失**:整棵树的布局演进仍需索引级 floor。

---

## 6. 附录:#265 现场分析(设计的实证输入)

### 6.1 现象

CI `ci-fresh-install` 三平台同步失败(run [29873685262](https://github.com/mcpp-community/mcpp/actions/runs/29873685262)),步骤 `Template: mcpp new --template imgui`:

```
error: index requires mcpp >= 0.0.101 but this is mcpp 0.0.100 [E0006]
 Downloading compat.imgui v1.92.8
error: package 'imgui@1.92.8' has no mcpp.toml
```

两条 error 相隔 0.4 ms,**属于同一次失败**(issue 正文把第一条判为「无关的 workspace pin 滞后」,是误判)。

| # | 实验 | 结果 |
|---|------|------|
| E-a | `.../xim-x-mcpp/0.0.100/mcpp new abc1 --template imgui`(真实索引,floor=0.0.102) | `E0006` + `package 'imgui@1.92.8' has no mcpp.toml` —— **与 CI 逐字一致** |
| E-b | 同上 + `MCPP_INDEX_FLOOR=ignore` | `Created abc2 (template imgui@0.0.6:window)` —— **同一二进制、同一索引,仅关掉门就正确** |
| E-c | 0.0.102 同命令 | 正常 |

**E-b 是判决性的**:候选顺序、Form A/B、索引内容全部无关,唯一变量是准入门吞掉了第一趟查找。`imgui.lua` 与 `compat.imgui.lua` 在**同一个索引目录**,且同一次查找里 `imgui.lua` 排在前面。

### 6.2 机制

```cpp
// index_contract.cppm:96-112
if (!reported.insert(indexRoot).second) return std::nullopt;  // :110 决策被通知污染
```

唯一调用点(`package_fetcher.cppm:593`)把返回值当放行判据 → **第 1 次查询=拦截,第 2..n 次=静默放行**。

`fetch_template_package()`(`create.cppm:44-50`)恰好查两次:

| 轮次 | 查询 | 该索引内候选顺序 | 实际结果 |
|---|---|---|---|
| 1 | `read_xpkg_lua("", "imgui")` | `imgui.lua` → `compat.imgui.lua` | **被门拦掉**,E0006 打印,`imgui.lua` 从未被看到 |
| 2 | `read_xpkg_lua("compat", "imgui")` | `compat.imgui.lua` → `imgui.lua` | 门已「报过」→ **放行** → 命中 Form B 的 `compat.imgui` |

### 6.3 冗余的第二趟(I7 的直接来源)

设请求短名为 `S`:

| | 候选文件集(`compat.cppm:159-198`) | 接受判据(`xpkg.cppm:682-716`) |
|---|---|---|
| 趟 1 `ns=""` | `{S.lua, compat.S.lua}` | `A1 = (id.name == S)` —— discovery 模式,任何 namespace 都接受(`:700`) |
| 趟 2 `ns="compat"` | `{compat.S.lua, S.lua}` —— **同一集合**,仅顺序不同 | `A2 = (id.name == S && id.ns == "compat")` |

`C2 = C1`、`A2 ⊊ A1` ⇒ 趟 2 能接受的任何描述符,趟 1 必然也接受;而趟 2 只在趟 1 返回 `nullopt` 时执行,趟 1 返回 `nullopt` ⇔ 所有索引里都不存在名字匹配的描述符 ⇒ **趟 2 必然也 `nullopt`**。

**准入正常时趟 2 恒为空转;它唯一能产出结果的情形,就是趟 1 被非语义原因中断。**这就是 I7 的由来:把「解析失败」当成「换个候选」的信号,会把一次报错升级成一次错误解析。

### 6.4 测试为什么没抓到

- 单测只覆盖纯函数,唯一有状态、唯一被生产代码调用的 `check_index_floor` 零覆盖;
- floor 全链零 e2e;
- 幂等性缺陷天生逃逸单次调用的测试 —— **任何「调一次」的用例都会通过**。
