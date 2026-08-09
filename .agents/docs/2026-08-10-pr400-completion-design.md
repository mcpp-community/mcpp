# PR #400 收尾设计方案 —— 重新判定阻塞点，并把串行收口改成并行

> 日期：2026-08-10
> 对象：[mcpp-community/mcpp#400](https://github.com/mcpp-community/mcpp/pull/400)（Draft，HEAD `64803fc`）
> 关联：#398（实施）、#397（汇总）、#380/#392/#396、
> [mcpplibs/mcpp-index#197](https://github.com/mcpplibs/mcpp-index/pull/197)（已合）、
> [openxlings/xlings#521](https://github.com/openxlings/xlings/pull/521)（Draft）
> 本文只记录可公开的仓库事实，不含本机路径、用户名、凭据。

---

## 0. 这份文档做什么

PR #400 的实现主体已经完成（31 commit、111 文件、+15085/−1692）。现有交接文档
`.agents/docs/2026-08-09-pr400-handoff-zh.md` 把"还没做完的事"列得很完整，但它对**唯一一个
mcpp 内部失败**的根因判定是错的，而整条收口路线又建立在"等 xlings 发版"这个跨仓库串行依赖上。

所以本文不重述已完成的实现，只做三件事：

1. **§1** 用 CI 原始日志重新判定 Windows E2E 2/2 的根因（结论与 PR 正文不同）；
2. **§2** 给出 review 结论，标出**必须在合并前处理**的项；
3. **§3–§5** 给出把串行等待改成并行推进的收口设计与分阶段计划。

### 0.1 基线事实（已核验）

| 项 | 值 | 证据 |
|---|---|---|
| PR #400 HEAD | `64803fc` | `gh pr view 400` |
| main | `80291ca` | `git log` |
| main 的 Windows E2E | **success** | run `31275102045` |
| `64803fc` 矩阵终态 | **13 pass / 5 fail** | `gh pr checks 400` |
| 4 个 fail | 全部是裸 `ftxui` exact miss | job `93271797138` / `93271798469` / `93271802557` / `93271790099` |
| 第 5 个 fail | `12_add_command.sh` (exit 2) | job `93271793304` |
| `bare Windows: no Visual Studio` | 已 **pass**（`ed4cf64` 时是 cancelled） | job `93272767480` |

`64803fc` 比交接文档记录的 `ed4cf64` 快照好一格：cancelled 已消失，仍是 5 个 fail。

---

## 1. 重新判定：Windows E2E 2/2 的根因不是 workspace 索引继承

### 1.1 PR 现在的说法

PR 正文与交接文档 §5.5 都写：失败发生在 step (14)，workspace member 读不到根
`[indices] acme`；`ed4cf64` 用 lexical anchor 修但没修好；`9a47ccf` 加 `route:` 诊断，
"等原生 Windows 输出 root/pkgs 状态后再做最小修复"。

**证据已经到了**（run `31324173520`），但它指向另一处。

### 1.2 原始日志

```
2026-08-09T16:39:28.5388080Z error: package 'acme.util' not found in any configured index
2026-08-09T16:39:28.5388719Z   tried: (acme, util)
2026-08-09T16:39:28.5389134Z   route: local index 'acme': root absent, pkgs absent
2026-08-09T16:39:28.5946779Z FAIL: 12_add_command.sh (exit 2, 2.06s)
```

### 1.3 四条独立证据都排除 step (14)

**(a) 退出码。** step (14) 被包在 `|| { echo …; exit 1; }` 里：

```bash
workspace_add=$("$MCPP" add acme.util@2.0.0 2>&1) || {
    echo "$workspace_add"
    echo "workspace member could not read its root-owned local index"
    exit 1
}
```

它失败只可能是 **exit 1**。日志是 **exit 2** —— 那是 mcpp 自己的退出码经 `set -e` 直传，
只有 `"$MCPP" add … > /dev/null` 这种裸调用才会这样。

**(b) 缺失的横幅。** step (14) 失败必然打印
`workspace member could not read its root-owned local index`，日志里没有。
同一 harness 会显示脚本自身的 stdout —— 同一个 job 里
`118_purview_include_rebuild.sh` 的 `windows: depfile degradation reported as expected`
就是脚本 `echo` 出来的。

**(c) 顺序。** 失败前最后两条未被捕获的 mcpp 输出是 step (5) 的 `capi.lua` 迁移 warning
（16:39:27.10）和 step (12) 的 `unknownidx.thing` warning（16:39:27.98）。
step (13)/(14) 的输出全部被 `$( )` 捕获，所以不出现。下一条落到日志的 stderr
就是 step (15)。

**(d) `route:` 行本身。** step (15) 的 fixture 在
`tests/e2e/12_add_command.sh:250` 写的是：

```bash
[indices]
acme = { path = "$TMP/myapp/index" }
```

`$TMP` 来自 `mktemp -d`。在 Git Bash 下它是 **MSYS 路径** `/tmp/tmp.XXXX`，
而被写进 `mcpp.toml` 的是**文件内容**，MSYS 的 argv/env 路径转换对它不生效。
原生 `mcpp.exe` 拿到 `/tmp/tmp.XXXX/myapp/index`，按 Windows 语义那是"当前盘根相对"，
即 `C:\tmp\tmp.XXXX\myapp\index` —— 不存在。

**这正好就是 `root absent, pkgs absent`。**

### 1.4 为什么 main 是绿的

step (15) 是本 PR **新增**的（`git diff origin/main...HEAD` 中该段全为 `+`）。
step (14) 在 main 上就存在且一直通过。

全仓 e2e 里，只有这一处把绝对 `$TMP` 路径写进 manifest **并且在 Windows 上真正通过它解析包**：

- `43_indices_lockfile.sh:45` 也写绝对 `$INDEX_DIR`，但整个测试只做文本断言，从不解析包 → Windows PASS；
- `163_identity_first_resolution.sh` 用相对路径 `../idx1` → Windows PASS；
- `121_default_ns_redirect.sh` / `203_exact_selector_lock_migration.sh` / `44_*` 等
  被 `# requires: gcc` 挡掉 → Windows SKIP。

### 1.5 为什么 Wine 复现是假绿

交接文档 §5.5 记录："下载该 job 的 Windows artifact，在隔离 Wine 中重放，两条
`mcpp add acme.util@2.0.0` 都通过。"

Wine 默认把 **`Z:` 映射到 `/`**。于是 `\tmp\tmp.XXXX\myapp\index` 落回真实的
`/tmp/tmp.XXXX/myapp/index`，fixture 可读，测试自然过。

> **方法论条目（值得写进 CLAUDE.md / 记忆）：Wine 能验证 PE 产物"能跑"，
> 不能验证"路径语义"。** 凡是结论依赖盘符、驱动器映射、`/` 开头路径解释、
> 8.3 短名、UNC 的问题，Wine 通过 = 无信息，不是证据。

### 1.6 推论：`ed4cf64` 是一次无证据的产品改动

`ed4cf64` 把 `src/project.cppm` 的 `inherit_workspace_indices` 里的

```cpp
idx.path = std::filesystem::weakly_canonical(wsRoot / idx.path);   // main，绿
```

改成

```cpp
idx.path = (wsRoot / idx.path).lexically_normal();                 // PR
```

理由写的是"weakly_canonical 可能把 Windows 短名/大小写路径改写成另一种拼写"。

对照事实：

- **同样的失败在 `ed4cf64` 之前之后完全一致** —— run `31318089536`（`0108ee19`，改动之前）
  的错误文本与 `31321961040`（`ed4cf64`，改动之后）逐字相同；
- main 带着 `weakly_canonical` 是绿的；
- 该改动配的单测 `PmIndexRoute.WorkspaceMemberReadsRootAnchoredRelativeIndex`
  （`tests/unit/test_pm_index_route.cpp:184`）里有一行
  `EXPECT_EQ(indices.at("acme").path, (root / "index").lexically_normal());`
  —— 它断言的是**实现细节的路径拼写**，而不是能力。这正是它抓不到真 bug 的原因。

所以：**这条改动目前是"为了一个不存在的原因、改掉一个当时是绿的行为"，且没有任何
测试能证明它有必要。** 它不一定错（`weakly_canonical` 会解符号链接，
参见记忆 `issue344-cache-object-address`：`fs::relative` 解符号链接曾让包静默退出缓存），
但**它必须要么被证明，要么被还原**，不能就这么留在 diff 里。

---

## 2. Review 结论

按"合并前必须处理 / 合并前应处理 / 记录即可"三档。

### R1 · P0 · 修在了控制流到不了的地方

见 §1。当前 PR 的下一步动作（"等 Windows route 证据 → 修 workspace 继承"）
如果照做，会在一个本来就没坏的路径上继续改代码。

**处理**：按 §3 主线 A。

> 这是记忆 `repair-placed-where-flow-never-reaches` 的同一形状第 12 次出现：
> **修补被放在控制流永远到不了的地方。** 这次的变体是"断言的失败位置被错认"。

### R2 · P0 · 裸名精确化 = 让**已经发布**的数据失效

这是本 PR 最大的产品风险，而且它已经在用四个红 job 报警。

现状（`tests/e2e/162_bare_name_namespace_scope.sh` 的 diff 就是判据）：

| 写法 | main | PR #400 |
|---|---|---|
| `gtest = "1.15.2"` | 解析到 `compat.gtest` | **硬失败** |
| `ftxui = "6.1.9"` | 解析到 `compat.ftxui` | **硬失败** |
| `compat.gtest` | 解析 | 解析 |

四个 CI 失败（Linux integration / macOS LLVM / aarch64 / Windows toolchains）全部是
同一句：

```
error: dependency 'ftxui': no package found for exact selector
  tried: (mcpplibs, ftxui)
    compat.ftxui
    ftxui = "6.1.9"
```

PR 把它们归类为"已知跨仓库边界，由 xlings #521 收口"。**这个归类不成立**：

1. **mcpp 自己也被它打中**。本 PR 不得不把仓库根 `mcpp.toml` 的
   `[dev-dependencies] gtest` 改成 `[dev-dependencies.compat] gtest`。
   一个变更需要改自己的 manifest 才能自举，就是破坏性变更的定义。
2. **xlings 只是一个消费者**。索引里有 34 个 `compat.*` 包
   （`165_bare_name_cross_namespace_wire_address.sh` 在 main 上的注释记的数；
   本 PR 把那段注释删掉了）。
   任何用户 `mcpp.toml` 里写着裸 `gtest`/`ftxui`/… 的项目，
   在升级到 2026.8.9.1 的那一刻全部构建失败，而**修 xlings 帮不到他们**。
3. **它是记忆 `index-floor-must-degrade` 的镜像**。那条的教训是
   "索引是数据、mcpp 是程序，发布数据不得让程序失效"。
   这次反过来：**发布程序不得让已发布的数据与用户 manifest 失效。**
   两个方向的判据是同一条：**必须降级，不得变砖。**
4. **PR 自己已经给出了正确的先例**。同一个 PR 对 `ns:name → ns.name`
   给了一个 release 的迁移别名 + 可复制的替换提示。裸名迁移比冒号拼写影响面**大得多**，
   却只给了硬失败。这是内部不一致。

**处理**：按 §3 主线 B（加一个 release 的迁移窗口）。这同时会把四个红变绿，
**并把 xlings #521 从发布关键路径上摘下来**。

### R3 · P1 · 合并 #400 会立刻武装 AUR 自动发布

`.github/workflows/aur-publish.yml`：

```yaml
on:
  workflow_run: { workflows: [release], types: [completed] }
  schedule:
    - cron: '17 */6 * * *'
  workflow_dispatch: { inputs: { publish: … default: false } }
```

而发布判定是：

```bash
if [[ "$TRIGGER" == workflow_run || "$TRIGGER" == schedule ]]; then
  publish=true
else
  publish=${MANUAL_PUBLISH:-false}
fi
```

**`schedule` 直接 `publish=true`。** 也就是说：#400 一旦合入 main，
最多 6 小时后就会有一次**无人值守的、对第三方服务（AUR）的写操作**，
它会把当时"最新完整稳定 release"（合并瞬间是 `v2026.8.8.4`）推到 `mcpp-bin`，
而公开 AUR 现在还停在 `2026.8.1.1-1`。

这不一定是错的结果，但**它是一个由"合并"而不是由"明确放行"触发的对外副作用**，
而且这条路径**从未真跑过**（交接文档 §3.7：只做了非 root `makepkg --verifysource` dry-run，
"本轮没有发布"）。

**处理**：见 §3 主线 C 的 C3 —— 首次落地把 `schedule` 分支降级为 dry-run
（用一个仓库变量或直接先不带 `schedule` 触发器合入），
手工 `workflow_dispatch --publish` 验证一次成功后，再用一个独立小 PR 打开定时收敛。

### R4 · P1 · 身份/索引路由的 e2e 在 Windows 上几乎不跑

本 PR 的核心是"精确身份 + 索引路由"，而这类 e2e 的 `# requires:` 把它们挡在 Windows 之外：

| 测试 | requires | Windows |
|---|---|---|
| `203_exact_selector_lock_migration.sh` | `gcc fresh-sandbox` | SKIP |
| `121_default_ns_redirect.sh` | `gcc fresh-sandbox` | SKIP |
| `205_root_local_subos.sh` | `elf gcc` | SKIP（合理，Linux 语义） |
| `206_runtime_binding_physics.sh` | `elf gcc` | SKIP（合理） |
| `207_runtime_contract_provenance.sh` | `elf python3 unix-shell` | SKIP（合理） |

`205/206/207` 跳过是对的 —— 它们本来就是 Linux 运行时物理。
但 `203`（**exact selector 的 lock 迁移**）和 `121`（**索引 default 重定向**）
是纯路径 + 纯身份逻辑，跟编译器无关，却因为 `gcc` 这个 token 从来没在 Windows 上跑过。

**这正是本次 bug 能溜过去的结构性原因：身份与路径的 bug 只在 Windows 上出现，
而验证身份与路径的测试只在 Linux 上跑。**

**处理**：见 §3 主线 A 的 A3。

> 参见记忆 `issue375-retracted-c-runtime-and-subos-env`：
> `# requires:` 的死 token 让 `65_*` 从未在 CI 跑过。同一类问题，这次是"活 token 但选错了"。

### R5 · P2 · PR 体量与耦合

111 文件 / +15085，横跨四个互不依赖的产品域（包/模板身份、事务脚手架、
运行时绑定与 ELF 闭包、release manifest + AUR reconciler），**外加一次版本 bump**。

现在拆已经不划算（31 个 commit + 不得改写历史的约束），所以**不建议拆 #400**。
但两件事要做：

- **发布分级**：AUR（R3）与 release manifest 是第一次真跑，按 §3 C3 单独放行，
  不要和身份语义同时"上线"；
- **记录规则**：下次同类工作按域拆 PR。一个 PR 只应该有一条可以独立回滚的主线。

### R6 · P2 · 版本号日期已过期

`mcpp.toml` / `src/version.cppm` 是 `2026.8.9.1`，今天已是 2026-08-10。
按 `YYYY.M.D.N` 约定（记忆 `mcpp-date-version-convention`），
实际发版当天必须重新对齐（如 `2026.8.10.1`）。
**这不是收尾时顺手改的东西** —— 它牵动 CHANGELOG、docs、release manifest、
AUR 目标版本、bootstrap pin 的判据，必须作为独立 commit 在发版前执行。

### R7 · 待独立复核区（本文不下结论）

以下三块是本 PR 新增的高风险实现，本轮只做了结构性抽查，**没有**做逐行审计，
因此**不给通过或不通过的结论**，只登记为"合并前需要一次独立 review pass"：

- `src/platform/elf_runtime.cppm`（+722）：手写 ELF64 读取器。
  抽查到 `off <= bytes.size() && size <= bytes.size() - off` 这类正确的溢出安全写法
  和 `kMaxClosureObjects` 上界，形态是对的；但 verneed/verdef 遍历、
  SONAME 复用、`$ORIGIN` 展开需要针对畸形/截断输入的定向用例。
- `src/build/runtime_validation.cppm`（+433）：verdict 缓存。
  键包含 artifact stat + `runtimeBinding.contractHash`，
  且 `Inconclusive` 会把 summary 从 `Pass` 拉下来 —— 方向正确。
  需要专门确认的不变量：**缓存 miss/mismatch 必须保持失败，不得靠下一次运行变绿**
  （PR 声称已实现，需要一个明确的 RED 用例锁住）。
- `scripts/aur/reconcile_mcpp_bin.py`（+1132）：12/12 契约测试 + 真实 Arch dry-run，
  但 fast-forward-only、vercmp 单调性、AUR RPC 滞后分类这三条只在**真实推送**时才被检验。
  这就是 R3 要分级放行的原因。

---

## 3. 设计：三条主线

### 主线 A —— 让 latest-head Windows 变绿（修测试，不是修产品）

#### A1. 修 fixture：`tests/e2e/12_add_command.sh:250`

写进 manifest 的绝对路径必须是**宿主原生**路径。仓库已有先例：
`171_bmi_staging_locked_dest.sh:75` 用 `cygpath -w`。

TOML 里反斜杠要转义，所以用 **`cygpath -m`**（混合模式，`C:/Users/...`）：
Windows API 接受正斜杠，TOML basic string 也不需要转义。

在 `tests/e2e/run_all.sh` 里导出一个共享 helper（新建 `_paths.sh` 或直接放进 run_all）：

```bash
# 把一个 shell 侧路径转成"写进 mcpp.toml 后原生 mcpp 能解析"的形式。
host_path() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) cygpath -m "$1" ;;
        *)                    printf '%s' "$1" ;;
    esac
}
```

step (15) 改为：

```bash
IDX="$(host_path "$TMP/myapp/index")"
...
acme = { path = "$IDX" }
```

**不要**改成相对路径。改成相对路径能让测试变绿，但会**顺手删掉"绝对路径索引"
这条唯一在 Windows 上被真正解析的覆盖** —— 那正是这次暴露出的盲区。

#### A2. `ed4cf64` 的处置：证明它，或者还原它

两条路，选一条，不允许"留着不动"：

- **A2-a（推荐）**：写一个 RED —— *workspace root 通过符号链接访问时，
  继承的相对 index 必须仍锚在声明的 root 上*。
  如果 `weakly_canonical` 让它失败而 `lexically_normal` 让它通过，
  这条改动就被证明了，保留，并把该测试作为它的锁。
- **A2-b**：还原为 `weakly_canonical`（回到 main 的绿行为）。

无论哪条，都要修 `tests/unit/test_pm_index_route.cpp:221`：
把 `EXPECT_EQ(path, …lexically_normal())` 换成能力断言
（`route.describe(...)` 与 `lookup_descriptor` 命中），
**不要断言路径的字符串拼写**。

#### A3. 补 Windows 侧的身份/路径覆盖

- 把 `203_exact_selector_lock_migration.sh` 与 `121_default_ns_redirect.sh` 的
  `# requires:` 里的 `gcc` 去掉（若它们确实只做解析断言；若确有编译步骤，
  就把解析断言拆成一个不需要 gcc 的新用例）。目标：**exact selector 与索引路由
  在三个平台都跑**。
- 新增一条专门的 e2e：*绝对路径 `[indices]` 在三个平台都能解析*，
  内部使用 `host_path`，并**显式断言 `route:` 输出为 `root present, pkgs present`**。
  这条测试的存在本身就是防止"MSYS 路径写进 manifest"再次发生的护栏。
- 给 `run_all.sh` 加一条极简 lint：扫描 `tests/e2e/*.sh`，
  凡出现 `path = "$...` 形式且变量不是经 `host_path` 得来的，报错。
  （宁可稍微粗糙也要有 —— 这类错误人眼在 review 里看不出来。）

#### A4. 保留 `route:` 诊断

`9a47ccf` 加的 `IndexRoute::describe` 是本轮**唯一让根因可判定**的东西：
没有它，日志只会说 "not found"，谁都无法区分"索引没配"和"索引配了但路径不存在"。
它应该保留，并按 A3 被测试固定下来。

---

### 主线 B —— 裸名迁移窗口（把 R2 从"生态阻塞"降级为"一次弃用警告"）

#### B1. 语义

对**省略命名空间**的选择器：

1. 精确解析 `(mcpplibs, name)`；
2. **仅在 miss 时**，按上一版本的老顺序再试 `(compat, name)` 和 `(∅, name)`；
3. 若老 rung 命中：**正常解析**，同时打印一条弃用警告，内容包含
   - 实际选中的完整身份（`compat.ftxui`），
   - 可直接粘贴的 manifest 片段（复用 `src/build/prepare.cppm` 里 miss 分支
     已有的 "did you mean" 渲染，就在 `:2447` 那条 error 之前），
   - 该 rung 将在下一个版本移除；
4. 老 rung 也 miss → 保持现在的硬失败与提示（不变）；
5. **`mcpp.lock` 只写规范身份**（`compat.gtest`），绝不写裸拼写；
6. `mcpp add <裸名>` 命中老 rung 时，**把规范点分形式写进 `mcpp.toml`**
   —— 用户第一次碰它就自动迁移。

关键点：**原缺陷是"静默"，不是"回退"。**
`162_bare_name_namespace_scope.sh` 的注释原文写得很清楚：
"a bare name that matched nothing fell through to the FIRST candidate **SILENTLY**,
so mcpp carried on with a namespace it had invented"。
一条带完整身份的警告已经把"静默"消灭了，同时保住了已发布的数据。

#### B2. 落点

| 文件 | 改什么 |
|---|---|
| `src/pm/dependency_selector.cppm` | 新增 `legacy_bare_candidates(name)`，与已有的 `legacy_prefixed_coordinate` 对称：只服务于"exact miss 之后"，绝不进 exact 候选表 |
| `src/pm/index_route.cppm` | `Lookup` 增加"命中来自哪一 rung"，让上层能区分 exact 命中与 legacy 命中 |
| `src/build/prepare.cppm:2447` 附近 | miss 后先试 legacy rung；命中则 warning + 继续；仍 miss 则维持现有 error |
| `src/pm/commands.cppm` | `mcpp add` 命中 legacy rung 时写规范点分形式 + warning |
| `tests/e2e/162_bare_name_namespace_scope.sh` | 两个分支都锁：裸 `gtest` **命中 compat 且必须出现弃用警告**；裸 `widget`（无处可寻）**必须硬失败** |
| `mcpp.toml` | 保持 `[dev-dependencies.compat] gtest`（这是规范写法，不回退） |
| `docs/spec/package-identity.md` + zh | 明确写出窗口的起止版本 |

#### B3. 效果

- 四个红 job 立刻具备变绿条件，**不需要等 xlings #521 合并 + xlings 发版**；
- xlings #521 仍然是**正确**的修复，继续正常 review/合并/发版，
  但它从 mcpp 发布的关键路径上被摘下来；
- 用户升级不炸；
- 下一个版本移除窗口时，生态已经有一个完整 release 的警告期。

#### B4. 备选（不推荐，但记录）

- **B-alt-1 生态优先**：不加窗口，先把索引里 34 个 `compat.*` 全部加
  `mcpplibs.<name>` 桥接描述符（就是 mcpp-index #197 对 `capi.lua` 做的 Form-B bridge）。
  缺点：把裸拼写永久固化进索引，与本 PR 的精确身份目标直接冲突。
- **B-alt-2 硬切**：照现在合并，靠文档和 release note 通知。
  缺点：所有存量用户 manifest 在升级瞬间失败，而错误信息虽然给了替换建议，
  但**无法自动修复**，也无法回退（旧 mcpp 已被 upgrade 提示引导升级）。

---

### 主线 C —— 收口顺序：把串行改并行

#### C1. 当前（交接文档 §8）是一条全串行链

```
xlings#521 review → 合并 → xlings 发版 → mcpp 改 pin → mcpp 全矩阵
   → mcpp review → 合并 → mcpp 发版 → GitCode → AUR → fresh-home
```

任何一环卡住（尤其是跨组织的 xlings 发版）整条停摆。

#### C2. 加入主线 B 之后可以并行

```
轨道 1（mcpp #400，自足）
  A1 修 fixture ─ A2 处置 ed4cf64 ─ A3 补覆盖 ─ B 迁移窗口
      → latest-head 全矩阵 → 用户 review → 普通合并 → 发版

轨道 2（xlings #521，独立）
  用户 review → 普通合并 → xlings 发版
      → mcpp 独立小 PR 更新 pin（不阻塞轨道 1）

轨道 3（对外发布面，独立放行）
  release manifest 首跑验证 → AUR 手工 publish 验证 → 打开定时收敛
```

#### C3. AUR 分级放行（对应 R3）

1. **合并 #400 时**：`aur-publish.yml` 的 `schedule` 分支改为 `publish=false`
   （或整段先不带 `schedule` 触发器合入）。合并不产生任何对外写。
2. **手工验证一次**：`workflow_dispatch` 带 `publish=false` 看 plan，
   再带 `publish=true` 推一次，核验 AUR RPC 可见、`.SRCINFO`、两个架构 checksum、
   以及在真实 Arch 上 fresh install。
3. **验证通过后**，用一个只改触发器的独立小 PR 打开定时收敛。

这样"第一次真实对外推送"是一次**有人盯着的、可回退到上一步的**动作，
而不是合并后 6 小时内自己发生的事。

#### C4. 版本对齐（对应 R6）

发版当天，独立 commit 统一 `mcpp.toml` / `src/version.cppm` / CHANGELOG /
docs / release manifest 目标 / AUR 目标版本。
`.xlings.json` 的 mcpp bootstrap pin 保持 `2026.8.8.4`（上一个已发布版本）——
按记忆 `release-bootstrap-pin-two-groups`，bootstrap pin 是自举起点，不随本次发布走。

---

## 4. 分阶段计划与验收判据

每一阶段都遵循 PR #400 已确立的流程：先 RED、最小实现、GREEN、
隐私扫描、单文件暂存、一个逻辑变更一个 commit、立即 push、立即中文 checkpoint、
不 amend/rebase/squash/force-push、不 admin bypass。

| 阶段 | 内容 | 验收判据（**判据是原生 CI 终态，不是本机**） |
|---|---|---|
| **P0** | A1 `host_path` + step (15) fixture | Windows E2E 2/2 中 `12_add_command.sh` PASS；Linux/macOS 不回归 |
| **P1** | A2 处置 `ed4cf64`；重写 `test_pm_index_route.cpp:221` 为能力断言 | 若走 A2-a：符号链接用例先 RED 后 GREEN；若走 A2-b：还原后三平台全绿 |
| **P2** | A3 Windows 身份/路径覆盖 + `run_all.sh` lint | `203`/`121` 在 Windows 上由 SKIP 变 PASS；新绝对路径索引用例三平台 PASS；lint 对故意构造的坏 fixture 报错 |
| **P3** | B 裸名迁移窗口 | `162` 两个分支都 GREEN；**四个 xlings 相关 job 由 fail 变 pass**；`mcpp.lock` 里不出现裸拼写 |
| **P4** | R7 独立 review pass（ELF / verdict cache / AUR reconciler） | 三块各自补齐定向 RED；verdict 缓存 mismatch 必须保持失败的用例存在 |
| **P5** | C4 版本对齐 + 文档 + 验证账本 | 版本号 = 实际发版日期；`.agents/docs/...validation.md` 里所有 "not started" 段落被终态替换 |
| **P6** | 用户 review → 转 ready → 普通合并 | 全矩阵**同一个 HEAD**上 terminal 全绿；pending/skipped/cancelled/superseded 一律不算 PASS |
| **P7** | 发版 + release manifest 首跑核验 | tag、四平台资产、sidecar、不可变 `mcpp-release.json` 全部核验；索引 main 的 latest 指向它 |
| **P8** | AUR 分级放行（C3 三步） | AUR RPC 可见目标版本；真实 Arch fresh install 通过；之后才打开 `schedule` |
| **P9** | GitCode / xim-pkgindex / fresh-home 全生态 | 隔离 HOME/XLINGS_HOME/SubOS 下：安装、`[ns.]name[@version][:tname]`、唯一默认模板、多 SubOS/glibc、build/run/test、provider provenance 的 PASS/FAIL/NOT_EXERCISED 表 |
| **P10** | issue 收口 | #398 随 PR 关闭；#380/#392/#396 凭发布后二次验收证据关闭；残留写回 #397 |

**并行关系**：P0–P3 是 #400 的关键路径；xlings #521 的 review/合并/发版
与 P0–P6 完全并行，其 pin 更新是合并后的独立小 PR。

> 关于"发布完成"的判据，沿用记忆 `release-publish-pipeline`：
> **判据是索引 main 的 latest 指向它**，不是 tag 存在、也不是 PR 已合。
> `gh pr merge` 不带 `--admin` 可能是静默空操作（退出 0），合完必须回查 `state`。

---

## 5. 需要你拍板的决策点

| # | 决策 | 选项 | 我的建议 |
|---|---|---|---|
| **D1** | 裸名迁移窗口 | (a) 加一个 release 的弃用窗口（主线 B）<br>(b) 索引侧加 34 个 `mcpplibs.*` 桥接<br>(c) 硬切，只靠文档通知 | **(a)** —— 与本 PR 自己对 `ns:name` 的处理一致；能保住存量用户；并把 xlings 摘出关键路径 |
| **D2** | `ed4cf64` 的 `weakly_canonical → lexically_normal` | (a) 用符号链接 RED 证明并保留<br>(b) 还原到 main 的行为 | **(a) 先试**，一天内证不出来就走 (b)。不允许"留着不动" |
| **D3** | AUR 首次放行 | (a) 合并时 `schedule` 降级为 dry-run，手工验证后再打开<br>(b) 照现状合并 | **(a)** —— 第一次对外真实推送应该有人盯着 |
| **D4** | 发版版本号 | (a) 发版当天重新对齐为 `2026.8.{当天}.1`<br>(b) 保持 `2026.8.9.1` | **(a)**，按日期版本约定 |
| **D5** | R7 三块的 review 深度 | (a) 合并前补定向 RED<br>(b) 合并后跟进 | **(a) 只对 verdict 缓存那条不变量**（"mismatch 不得靠重跑变绿"），其余可 (b) |

---

## 6. 明确不做

- **不拆 #400**（31 commit + 不改写历史的约束下，拆的成本大于收益）；
  规则记录下来给下一次。
- **不在 mcpp 里恢复"静默"的跨命名空间回退**。主线 B 的窗口是**带警告、
  写规范身份进 lock、`add` 时自动迁移 manifest** 的，与被修掉的静默回退不是同一件事。
- **不动 `mcpp-m`**（文件、远端、发布路径）与 `mcpp-git`。
- **不引入 `--variant`、不引入 CLI SubOS override、不让 mcpp 探测 GPU/驱动/ICD**
  —— #398 冻结的四条产品边界不变。
- **不处理 #397 的 C1–C9**。它们是独立缺陷（fast path 不重建 CDB、
  模块扫描器 raw-string 假阴性、`--no-color` 空操作 …），
  与本 PR 无关，按 #397 自己的节奏走。
- **不碰特殊保留项**：#43、#260，以及标记为保留/Draft/do-not-merge 的项。

---

## 附：本文结论的可复核路径

| 结论 | 复核方式 |
|---|---|
| 失败在 step (15) 而非 (14) | `gh api repos/mcpp-community/mcpp/actions/jobs/93271793304/logs`，看 `exit 2` 与缺失的横幅 |
| step (15) 是本 PR 新增 | `git diff origin/main...HEAD -- tests/e2e/12_add_command.sh` |
| main 的 Windows E2E 是绿的 | run `31275102045` |
| `ed4cf64` 前后失败一致 | run `31318089536` vs `31321961040` |
| 四个 fail 同因 | job `93271797138` / `93271798469` / `93271802557` / `93271790099` |
| AUR `schedule` 即 publish | `.github/workflows/aur-publish.yml` 的 plan 步骤 |
| 身份 e2e 在 Windows 被跳过 | Windows E2E 日志中的 `SKIP: 203_… (missing capability: gcc)` |

---

## 附录 A：实施结果（2026-08-10 收尾）

本节是执行完 D1–D5 之后回填的**事实**，不是计划。凡与正文冲突的，以本节为准。

### A.1 D1–D5 的落地结果

| 决策 | 结果 |
|---|---|
| **D1** 裸名迁移窗口 | 已实施。精确未命中 + namespace 被省略 → 试 `(compat,name)` 与无 namespace rung；命中打印弃用警告 + 可粘贴片段，规范身份进 lock/install/cache；`mcpp add` 直接迁移 manifest。`2026.9` 移除，常量 `kBareNameFallbackRemovedIn`。 |
| **D2** `ed4cf64` 证明或还原 | **已证明，保留**。换回 `weakly_canonical` 让 `unit/test_pm_index_route` 失败（71 passed / 1 failed）。但理由与原提交描述不同：不是 Windows 短名，而是「anchoring 不得把 index 移出作者声明的那棵树」——经符号链接访问的 workspace 会被重定位，而 `prepare` 的报错会打印这个路径。 |
| **D3** AUR 首次放行 | 已实施。`workflow_run` 与 `schedule` 都需要仓库变量 `AUR_AUTOPUBLISH == "true"` 才推送；契约测试直接执行 workflow 自己的判定 shell。 |
| **D4** 版本对齐 | 已实施。`2026.8.9.1` → `2026.8.10.1`（版本号是日期，评审期间日期变了；`2026.8.9.1` 从未打 tag）。 |
| **D5** verdict 缓存不变量 | 已实施为 e2e `209`。**该不变量本来就是对的**（`validated_artifact_snapshot` 在任一 stored status ≠ Pass 时拒绝快速路径），只是没有测试；现在有了。 |

### A.2 P0 的根因判定被推翻了一次，值得单独记住

正文 §1 的推断（step (15) 的 MSYS 路径）经原生 CI 证实。三条可复用的判据：

1. **先看退出码，再看错误文本。** 被 `|| { …; exit 1; }` 包住的步骤失败只能是 exit 1；
   日志里的 exit 2 是 mcpp 自己的码经 `set -e` 直传，只可能来自裸调用。
2. **失败横幅缺席本身是证据。** harness 会显示脚本 stdout，所以「该步骤失败时必然打印的
   那行」没出现，就说明失败不在那一步。
3. **Wine 通过 = 零信息**（对路径语义而言）。Wine 把 `Z:` 映射到 `/`，
   `\tmp\…` 落回真实 `/tmp/…`。

修法按「一类」而不是「一行」：`_host_path.sh` + `00_fixture_path_hygiene.sh` lint +
全仓 24 文件 40 处迁移。大多数此前只是因为对应测试在 Windows 上因缺 capability 而 SKIP。

### A.3 计划外收进来的两项

- **#401（私有 glibc 泄漏进子进程环境）。** 已修，收敛为新模块
  `src/platform/runtime_env_contract.cppm` 的**作用域**决策（不是条件判断）。
  实测对照：已发布 mcpp `LDLP=[…/runtime:…/xim-x-glibc/2.39/lib64]`，本分支 `LDLP=[…/runtime]`；
  产物 RUNPATH 改动前后逐字节相同，说明这条环境项本来就没有收益。
- **覆盖盲区。** 身份/索引路由的 e2e 全部要 `gcc` 或 `fresh-sandbox`，Windows 两者皆无。
  新增 `210_local_index_addressing_on_every_host.sh`：无编译器、无沙箱、无网络，三平台都跑。

### A.4 主线 C 的实际阻塞点变了

正文假设瓶颈是「等 xlings #521 合并 + 发版」。加入 D1 的迁移窗口后，#521 确实离开了关键路径。

**但按要求把 xlings pin 提到最新 `2026.8.10.1` 之后出现了新的、更硬的阻塞：**
冷 home 装不上 `xim:gcc@16.1.0` —— 依赖解析下载 glibc 2.44，而 gcc 的 config hook
找不到该 payload（诊断建议的是 2.39）。同一 workflow、同一 runner 镜像、两次都确认
cache miss 的 A/B：

| xlings | 冷缓存 | 结果 |
|---|---|---|
| `2026.8.9.2` | 已确认 cache miss | gcc 安装成功（run `31317627461`） |
| `2026.8.10.1` | 已确认 cache miss | 失败（run `31335075557`，4/4 Linux job） |

所以不是「一直坏、被热缓存掩盖」。已带证据上报
[openxlings/xlings#524](https://github.com/openxlings/xlings/issues/524)，
mcpp 侧把 pin 停在 `2026.8.9.2`，等修复发布后用独立 commit 提升。

> 这条本身也是一个判据：**「pin 到最新」是一个需要被验证的动作，不是一次文本替换。**
> 它之所以在这里被抓到，只是因为换 pin 同时换掉了 CI 缓存键，把冷启动路径暴露了出来。
