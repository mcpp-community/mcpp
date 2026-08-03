# 索引的可用性不得决定 mcpp 的可用性 —— 纯 mcpp 侧优化方案

> 状态：**已实施**（2026.8.3.5）。实施中的三处偏差记录在 §7。
> 范围：**全部落在 `mcpp-community/mcpp` 内**。零跨仓依赖 —— 不需要 xlings 改动，
> 也不需要索引侧改动。
> 背景分析：`.agents/docs/2026-08-03-index-floor-should-degrade-not-brick.md`
> 跨仓增强（非前置）：[openxlings/xlings#476](https://github.com/openxlings/xlings/issues/476)

---

## 0. 要立的不变量

> **INV —— 索引侧的任何改动，都不得使 mcpp 从「能用」变成「不能用」。**

这条不变量今天不成立：`mcpplibs/mcpp-index` 抬了一次 `min_mcpp`，
所有 `0.0.109` 用户的构建就死了。**一次索引提交，让一个已发布客户端不可用**，
这在包管理器里是不可接受的耦合 —— 索引是**数据**，客户端是**程序**，
数据的更新不该让程序失效。

三条可检验的推论：

| # | 推论 | 今天 |
|---|---|---|
| **INV-1** | **刷新是单调的**：刷新只能维持或改善可用性，绝不能降低 | ❌ 刷新把能用的换成不能用的，且不可逆 |
| **INV-2** | **不可用是有边界的**：一个索引不可用，只影响**它提供的包**；不影响其它索引，也不影响**已解析/已安装**的依赖 | ⚠️ 边界在读取层存在，但在错误语义与刷新决策上泄漏 |
| **INV-3** | **不可用要自报家门**：绝不伪装成「包不存在」 | ❌ `return std::nullopt` ⇒ 全部变成 not-found |

再加一条设计判据，后面几处都从它推出来：

> **下限约束的是「读描述符」，不是「构建」。**
> 一个不需要读任何描述符的构建（锁文件齐全、payload 已装），不该被下限拦住。

---

## 1. 今天为什么违反

### 1.1 刷新路径对下限一无所知（违反 INV-1）

```
$ grep -rn "check_index_floor" src/
src/pm/package_fetcher.cppm:619        ← 全代码库唯一一处
```

`index_management.cppm:148` 的刷新是

```cpp
int rc = mcpp::xlings::update_index(xlEnv);   // 跑 `xlings update`，原地覆盖
```

**刷新前不问「换来的这棵树我能用吗」，刷新后也不问。** 于是一次 `xlings update`
就能把一份能用的索引原地换成用不了的，**没有备份、没有回滚、没有退路**。

用户在刷新之后比刷新之前更糟 —— 这是最伤人的性质，也是本方案第一优先级。

### 1.2 违反下限退化成「包不存在」（违反 INV-3）

`package_fetcher.cppm:614-622`：

```cpp
if (auto violation = mcpp::pm::check_index_floor(pkgsDir.parent_path())) {
    mcpp::ui::error(*violation);
    return std::nullopt;      // 「读不了」与「没有」压成同一个返回值
}
```

后果是终止构建的那条错误与真实原因隔了两层
（`E_NOT_FOUND ... wire address tried: ...`，既不提版本也不提下限）。

还有一个更隐蔽的二次伤害：`index_refresh` 的判据是「解析输入是否都在磁盘」。
一个被下限挡住的读取**看起来就是一次真实的 miss** ⇒ 判定「该刷新了」⇒
再拉一次同样不兼容的树。**不可用状态自己驱动了重复刷新。**

### 1.3 整条下限行为**没有任何 e2e 覆盖**

```
tests/unit/test_index_contract.cpp   ← 只测纯谓词 floor_violation()
tests/e2e/                            ← 零
```

2026-07-08 的设计里写了要有
（*"e2e: fixture index with `min_mcpp = "9.9.9"` → build fails"*），**没有落地**。
所以 1.1 和 1.2 从上线到今天没有任何机制会发现 —— 谓词是对的，
**它周围的行为从没被端到端跑过**。

---

## 2. 方案（M1–M5，全部 mcpp 侧）

### M1（必须）· 刷新单调性：先备份，后验收，不合格就回滚

**落点**：`src/pm/index_management.cppm` 的刷新编排。

```
refresh(repo):
    usable_before = floor_ok(repo.root)        # 刷新前的可用性
    snapshot      = archive(repo.root)         # 见 M2，硬链接复制，≈0 成本
    xlings::update_index(...)                  # 原地覆盖，不在 mcpp 进程内
    if usable_before and not floor_ok(repo.root):
        restore(repo.root, snapshot)           # ← 回滚
        warn_once(downgrade_notice)
        mark_no_refresh_this_process(repo)     # 防抖，见 1.2 的二次伤害
```

要点：

- **判据是「刷新前能用 ⇒ 刷新后必须仍能用」**，不是「刷新后必须能用」。
  本来就不能用的（冷启动撞上高下限），回滚没有意义，交给 M3/M4。
- `xlings` 原地覆盖是既成事实，mcpp 无法要求它写到暂存目录
  （见分析文档 §4.0 的传输约束）。所以是**备份 + 事后验收 + 回滚**，
  而不是设计文档原本设想的「staged unpack + 验收后再切换」。
  **效果等价，且不需要任何跨仓改动。**
- 索引树很小（实测 `mcpplibs` 912K、`xim-pkgindex` 2.2M），
  用硬链接复制（`copy_options::create_hard_links`）近似零成本、零额外空间。

用户看到：

```
warning: the refreshed index requires mcpp >= 2026.8.3.3; this is mcpp 0.0.109.
         Kept the previous index (<date>) — your build continues to work.
         Upgrade to pick up newer packages:  xlings update mcpp
```

**这一条单独就把「变砖」变成「可用 + 提示升级」。**

### M2（必须）· mcpp 自己的本地快照历史

M1 只留一份「上一个」。M2 把它变成一条**本地历史线** —— 也就是
「即使 xlings 没有相关功能，至少还有之前的」这句话的实现。

```
~/.mcpp/registry/data/.index-snapshots/<repo>/<index-version>/
```

- **快照身份**用现成的 `.xlings-index-version`（每个索引根下已经有这个文件；
  它的值按契约是**不透明串**，不要当 sha 解析）。
- 每次验收通过（`floor_ok`）就归档一份；保留最近 N 份（建议 N=5）+ 按体积上限回收，
  复用 `mcpp cache gc` 已有的 LRU 记账思路。
- **需要时从新到旧回溯**，取第一个 `floor_ok` 的快照恢复。

与 [xlings#476](https://github.com/openxlings/xlings/issues/476) 的关系：

| | 覆盖面 | 依赖 |
|---|---|---|
| **M2（本机历史）** | 只覆盖**这台机器见过**的快照 | **无** |
| #476（发布侧历史线） | 覆盖全部已发布快照，含全新安装 | 跨仓 |

**M2 不是 #476 的替代品，是它的本地退化版**，且覆盖了绝大多数真实场景
（一台持续在用的开发机/CI 缓存机，一定见过兼容的快照）。
#476 落地后 M2 依然有价值：它让常见路径不必联网重下。

**诚实的边界**：全新安装 + 最新索引下限过高 ⇒ 本机没有任何历史 ⇒
M2 无能为力，落到 M3/M4。这种情况下正确答案本来就是「升级 mcpp」，
M3 会把这件事说清楚。

### M3（必须）· `IndexUnusable` ≠ `Absent`

**落点**：`src/pm/package_fetcher.cppm` 的描述符读取返回类型。

```cpp
enum class DescriptorLookup { Found, Absent, IndexUnusable };
```

三条硬约束：

1. `IndexUnusable` **不得**触发 legacy 派生地址回退（那是给「这个索引里没有这个包」用的）；
2. `IndexUnusable` **不得**被 `index_refresh` 读成「本地缺东西 ⇒ 该刷新」——
   这正是 1.2 那个重复刷新循环的入口；
3. 解析结束时若存在 `IndexUnusable`，**最终错误就是 E0006 本身**，
   并指明**哪个索引、要求什么、本机是什么、怎么升级**。
   用户看到的第一条与最后一条错误必须是同一件事。

这条同时是 INV-2 的执行者：一个索引不可用**只**让它自己的包不可解析，
不得影响其它索引的 `Absent` 判定。

### M4（应做）· 下限拦的是「读描述符」，不是「构建」

由 §0 那条判据直接推出：**如果本次构建根本不需要读任何描述符
（依赖都在 `mcpp.lock`、payload 都已安装），下限不该让它失败。**

这正是 `index_refresh` 已经在用的那根轴（「所有解析输入都在磁盘」），
M3 落地后它自然成立：`IndexUnusable` 不再被误读成 miss，
于是「所有输入都在磁盘」的判定不受影响，构建照常离线进行。

**这是「mcpp-index 里的包升级不该影响 mcpp 可用性」的直接答案**：
一个已经解析完、装好的工程，与索引后来发生了什么完全无关。

> 边界要说清楚：如果构建**确实**需要装新东西（用户那次就是在装 `musl-gcc`），
> M4 帮不上，那是 M1/M2 的战场。M4 保的是**存量工程不被索引变更波及**。

### M5（必须）· 补上从未存在的 e2e

现在只有纯谓词单测。至少三条端到端断言，**每一条都对应上面一个不变量**：

| e2e | 断言 | 守的是 |
|---|---|---|
| 刷新单调性 | fixture 索引可用 → 构建成功；把它换成 `min_mcpp = "9.9.9"` 再刷新 → **构建仍然成功**，且 stderr 出现降级 warning | INV-1 / M1 |
| 错误自报家门 | 本机无任何兼容快照 → 构建失败，且**最后一条**错误含 `E0006`，stderr **不出现** `not found` | INV-3 / M3 |
| 边界隔离 | 索引 A 不可用、索引 B 正常 → B 的包照常解析成功 | INV-2 / M3 |

第二条那个「最后一条错误必须是 E0006」正是这次（以及
`install_pinned_mcpp.sh` 头注释记载的上一次）缺失的断言。

---

## 3. 不采纳

| 方案 | 理由 |
|---|---|
| 下限降级为纯 warning，照常解析 | 下限存在的理由是真的：`0.0.101` 那次是 `compat.opencv` 的 per-OS feature flags 被旧客户端**静默忽略**，产出错误构建。放行 = 用「静默错误产物」换「明确失败」，方向反了。M1/M2 保的是**旧快照**，不是**用旧客户端读新描述符**。 |
| 等 xlings#476 落地再做 | #476 是增强，不是前置。M1+M2+M3 **今天就能把「变砖」修掉**，且 #476 落地后全部保留价值。把用户可用性挂在别的仓库的排期上，本身就是这次问题的翻版。 |
| mcpp 自己接管索引下载 | 与 xlings 职责重叠，镜像/CDN/校验要重写一遍。M1 的「备份+回滚」用几十行拿到同样的可用性保证。 |
| 只做 M1，不做 M3 | 可用性修好了，但一旦真的走到不可用路径，用户仍然读到一条误导性的 `not found`。两者成本都很低，没有理由只做一半。 |
| 把 `MCPP_INDEX_FLOOR=ignore` 宣传成解法 | 它是调试逃生舱。让用户常态化绕过一个**为防止静默错误构建而存在**的闸，是把一个可用性问题换成一个正确性问题。 |

---

## 4. 实施顺序

| # | 项 | 依赖 | 效果 |
|---|---|---|---|
| 1 | **M3** 错误语义 | 无 | 最小改动；同时堵掉重复刷新循环 |
| 2 | **M1** 备份 + 验收 + 回滚 | M3（复用 `floor_ok`） | **变砖 → 可用 + 提示升级** |
| 3 | **M2** 本地快照历史 | M1（归档即 M1 的备份） | 「至少还有之前的」升级为一条历史线 |
| 4 | **M5** e2e | M1–M3 | 让上面三条永不回归 |
| 5 | M4 复核 | M3 | 多数情况下 M3 落地即自然成立，需实测确认 |

M1–M3 是一个 PR 的量级，建议一起发；M5 同 PR。

**发版说明要写清楚**：本次修复只对**升级到该版本之后**的用户生效。
已经卡住的 `0.0.109` 用户，要么升级 mcpp，要么等索引侧把下限降回去 ——
这也是为什么索引侧那次回退仍然值得单独做（见背景分析文档 §6 第 1 步）。

---

## 5. 验证

- `mcpp test --workspace`（单测）+ 新增三条 e2e。
- **先红后绿**：M5 的三条 e2e 必须先在 `main` 上跑出红，再实施 M1–M3。
  按 §1.3，今天它们一条都不存在，所以「先红」这一步不能省 ——
  否则无法证明它们真的覆盖到了。
- 人工复现：构造一个 fixture 索引，`min_mcpp` 从合法值改成 `9.9.9`，
  跑 `mcpp build` 两次，断言第二次仍然成功且打出降级 warning。

---

## 6. 这次暴露的一条通用教训

设计文档写下的行为（"staged refresh keeps the last compatible snapshot"）
与实现之间没有任何机器化的连接，于是**承诺存在、实现缺席，而且长期没人发现**。
缺席的不是代码，是**那条本该失败的测试**：谓词有单测（所以谓词是对的），
谓词周围的行为没有 e2e（所以行为是错的）。

判据：**一个设计文档里以「mandatory」措辞写下的行为，必须有一条以它命名的测试。**
没有测试的 mandatory 是一句愿望。


---

## 7. 实施记录（2026.8.3.5）

落点：`src/version.cppm`（新）、`src/pm/index_snapshot.cppm`（新）、
`src/pm/index_contract.cppm`、`src/pm/index_refresh.cppm`、`src/build/prepare.cppm`、
`src/pm/package_fetcher.cppm`、`src/xlings.cppm`、
`tests/unit/test_index_snapshot.cpp`（新）、`tests/e2e/185_index_floor_degrades.sh`（新）。

三处与方案的偏差，都是实施时被现实证伪的假设：

**① 守卫装不进 `xlings::update_index` —— 编译器报了循环依赖。**
方案假定「全进程唯一刷新入口」就是落点。实际 `mcpp.xlings` 无法 import
`mcpp.pm.index_contract`：后者 import `mcpp.toolchain.fingerprint`
（只为读 `MCPP_VERSION`）→ `mcpp.toolchain.detect` → `mcpp.xlings`。
**「这个二进制是什么版本」传递依赖了整个工具链探测子系统**，这就是分层在报错。
把常量提到叶子模块 `src/version.cppm` 后环消失。
连带：`check_version_pins.sh` 与三处发布文档要改指向（版本号唯一真源换了文件）。

**② 快照绝不能用硬链接 —— 单测立刻抓到。**
`copy_options::create_hard_links` 是显然的优化（索引树是几千个小文件）。
它在这里是**错的**：硬链接快照**与活动树共享 inode**，任何原地重写
（截断写、tar 覆盖解包）会直接写穿链接、毁掉备份。而本模块要挺过的那件事
正是「有东西替换了这棵树」。改为真实复制；实测 912KB/2.2MB × 5 ≈ 15MB，`prune` 封顶。

**③ 最初的 e2e 是假绿 —— 在 pre-fix 二进制上照样通过。**
第一版断言写的是「输出里出现 E0006」。它**一直**出现，所以那条断言什么也没测到
（已用 pre-fix 二进制实测确认通过）。真正坏掉的是**终止构建的那条消息**：

```
error: dependency 'toonew.newlib': not found in local index at '...'
```

它**把责任推给包**（指向发布/命名），而真正的答案早已滚出屏幕。
断言改为针对**最后一条 error**，并要求它带上真正的原因；改完在 pre-fix 上精确变红。
> 这条同时验证了 §6 那句判据：**没有测试的 mandatory 是一句愿望** ——
> 而一条写得不够精确的测试，比没有测试更危险，因为它让人以为覆盖到了。

另：终止构建的 not-found 不止一处（`prepare.cppm` 有三处），
所以原因附加收敛为一个 `with_index_cause()` helper，而不是逐处拼字符串。

验证状态：unit 55/55（含新增 9 条）；e2e 185 在修复前后分别红/绿；
`check_version_pins.sh` 通过。
