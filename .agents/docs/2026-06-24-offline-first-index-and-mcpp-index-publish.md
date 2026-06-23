# 离线优先的索引刷新 + mcpp-index 发布机制

**日期**: 2026-06-24
**范围**: mcpp 侧 —— `mcpp build` 等命令的索引刷新策略(离线优先)、`mcpp index`
命令、以及 `mcpp-index`(mcpp 的库索引)的发布机制。
**配套**: xlings 侧的 `xim-pkgindex` 发布解耦见 xlings 仓
`.agents/docs/2026-06-24-pkgindex-publish-decoupling-ci.md`。
**背景**: 2026-06 Termux 适配中发现两个痛点 —— ① `mcpp build` 每次自动 `xlings update`
联网(被墙网络下卡 5 分钟,见 termux-android-adaptation §3);② 改了索引仓后默认客户端
拿不到(artifact 不刷新)。

---

## 已就绪的基础设施(2026-06-24)
- **资源仓**:`xlings-res/mcpp-index`(github + gitcode 两端均已创建)—— 放 mcpp 索引 artifact + 指针。
  复用 xlings-res(mcpp 二进制本就在 `xlings-res/mcpp`),不另起 mcpp-res 组织。
- **secrets**:`mcpp-community/mcpp-index` 仓已配 `XLINGS_RES_TOKEN` + `GITCODE_TOKEN`。
- **Actions**:mcpp-index 仓已启用。
- **逐仓自文档**:发布机制说明见 `mcpp-community/mcpp-index` 仓
  `.agents/docs/2026-06-24-artifact-publish-mechanism.md`。
- 剩待办:① mcpp-index 仓加 `publish-artifact.yml`;② mcpp 侧加 artifact 拉取 + 离线优先(本文档 P0/P1)。

## 0. 现状

- **`mcpp build` 自动联网刷索引**:`package_fetcher` 在装 xim 包前调
  `ensure_official_package_index_fresh()` → 若 TTL 过期则 `xlings update`(git 同步多个
  索引仓,含被墙的 github 子索引)。**每次构建都可能联网**,离线/弱网体验差。
- **mcpp 已有 `index` 子命令**:`mcpp index list|add|remove|update|pin|unpin`(`cli.cppm`)。
  即"显式刷新"的入口已存在,但 build 仍走"隐式自动刷新"。
- **mcpp-index 是 git-only**:mcpp git-clone `mcpp-community/mcpp-index` 到
  `~/.mcpp/registry/data/mcpplibs`,受 mcpp 的 index TTL 缓存影响("改了不刷"根因)。

---

## 1. 设计原则:首次 bootstrap 保证有索引,之后离线优先

区分**两个阶段**:

### 1.a 首次初始化(必须保证有索引)
mcpp 第一次跑 / 沙盒初始化时(当前日志:`Initialize mcpp sandbox layout (one-time)` +
`Fetching package index (one-time)`),**必须确保本地有一份可用索引** —— 这是**唯一允许
"自动联网"的时刻**:
- 优先用**随发行版内置/种子索引(seed)**:mcpp 二进制包里捎带一份索引快照,首次直接落地,
  **零网络也能起步**(离线装机也能用,后续再显式刷新)。
- 无 seed 或 seed 不可用时,**首次自动拉一次**(指针+artifact,失败回退 git);拉不到则
  **明确报错**"首次初始化需要网络/或提供离线索引包",而不是静默半残。
- 首次落地后写入索引的**版本/时间/sha 标记**,作为后续 `index status` 与 TTL 的依据。

### 1.b 之后(steady-state):离线优先,刷新是显式动作
**索引一旦存在,命令默认只用本地索引,绝不为"顺手刷新"而联网;联网刷新由用户显式触发。**

| 命令 | 联网? | 行为 |
|---|---|---|
| 首次 init | **是(仅此一次)** | seed 落地 / 或自动拉一次,保证有索引 |
| `mcpp build` / `run` / `add` | **否(默认)** | 只读本地索引;缺包才提示 `mcpp index update` |
| `mcpp index update [--force]` | 是 | 显式刷新(steady-state 唯一默认联网入口) |
| `mcpp index status` | 否(可选 `--check` 联网比指针) | 显示本地索引版本/时间/来源 |
| `mcpp index list/search` | 否 | 本地查询 |

要点:
- **build 不再自动 `xlings update`**(首次 init 已保证有索引)。本地索引存在即用;仅当**解析
  失败**(包/版本本地查不到)时,提示"运行 `mcpp index update` 刷新",而不是默默联网卡住。
- 保留一个**很长的软 TTL**(如 24h)做"温和提醒"(stderr 一行 hint),但**不阻塞、不自动拉**。
- `mcpp index status`:打印本地索引的 **版本/时间戳/sha/来源(seed|artifact|git)**,
  让用户/CI 知道是否该刷。
- **不变量**:任何 steady-state 命令在**离线**下都能跑(只要首次 init 成功过)。

---

## 2. 自动刷新若保留,必须"低成本 + 可离线降级"

如果某些场景(如 CI 首次)仍要自动刷,刷新动作要满足:
1. **先查轻量指针**:只 GET 一个小 JSON(`*-latest.json`,几百字节)比对本地 sha;
   **命中(sha 相同)→ 零下载、零 git**。这是"低成本重查"的核心,远比 git pull / 全量
   `xlings update` 便宜。
2. **未命中才下 artifact**(整包 tar.gz,一次,带 sha 校验)。
3. **指针/artifact 拉取失败 → 静默降级用本地**(离线可用),不报错中止。
4. **绝不在 build 主路径上做 git clone/pull**(慢、易卡、被墙)。

> 对比现状:`ensure_official_package_index_fresh → xlings update` 是"全量 git 同步多个
> 仓",最重最易卡。改成"指针 sha 比对"后,绝大多数 build 不产生任何索引网络流量。

---

## 3. mcpp-index 发布机制:补齐 artifact + 发布 CI

现状 git-only 的两个问题:① 客户端 git clone 慢/弱网易失败;② 没有"指针 sha"可做
低成本比对(只能 git fetch)。建议**与 xim-pkgindex 统一为 artifact 模型**:

1. **mcpp-index 仓加 `publish-artifact.yml`**(`push` paths `pkgs/**` + `workflow_dispatch`
   + 可选 `schedule`):打包 `pkgs/` → 生成 `mcpp-index-<gitsha>.tar.gz` + manifest
   + 更新指针 `mcpp-index-latest.json`,发到资源仓(github + gitcode,复用
   `XLINGS_RES_TOKEN`/`GITCODE_TOKEN` 或 mcpp-res 等价物)。**改索引 push → 自动发布**,
   不必发 mcpp 版本。
2. **mcpp 侧加 artifact 拉取**:`mcpp index update` 优先拉指针+artifact(命中 sha 跳过),
   git clone 仅作回退(`MCPP_INDEX_SOURCE=artifact|git|auto`,默认 auto),对齐 xlings 的
   `XLINGS_INDEX_SOURCE` 模型。
3. **artifact 按内容哈希命名**(解绑 mcpp/xlings 版本),指针随改随移。

收益:① 改 mcpp-index 即时生效(不绑发版);② build 的自动刷新可降级成"指针 sha 比对",
离线可用;③ 两套索引(xim-pkgindex / mcpp-index)发布+拉取模型统一,维护成本低。

---

## 4. 落地顺序(建议)

1. **P0(离线优先)**:① 首次 init 保证有索引(优先内置 seed,否则自动拉一次,失败明确报错);
   ② `mcpp build` 去掉"自动 `xlings update`",改为"本地优先 + 解析失败提示 `mcpp index update`";
   ③ 加 `mcpp index status`。**纯 mcpp 改动,立刻提升弱网/离线体验。**
2. **P1(低成本刷新)**:`mcpp index update` 走"指针 sha 比对 → 命中跳过 / 未命中下 artifact"。
   依赖 mcpp-index 有指针(见 P2)。
3. **P2(发布解耦)**:mcpp-index 仓加 artifact 发布 CI + mcpp 侧 artifact 拉取(与
   xim-pkgindex 对齐)。

---

## 4.5 追加计划:first-init 细粒度带时间戳 debug log(WS5)

**动机**:mcpp 第一次运行常卡很久(Termux 实战),但日志太粗,看不出卡在哪一步。
需要在**首次 init 全过程**加 **`--verbose` 才可见**的细粒度 debug log,且**每条带时间戳**,
方便事后定位"卡很长时间"的具体步骤与耗时。

**要覆盖的步骤**(日志现状只有一行,需细化):
```
Initialize mcpp sandbox layout (one-time)
Fetching package index (one-time)
Bootstrap patchelf into mcpp sandbox (one-time)     ← 重点
Bootstrap ninja into mcpp sandbox (one-time)        ← 重点
First run no toolchain configured — installing gcc@15.1.0-musl ...  ← 重点
```
每个步骤前后打 `[verbose][HH:MM:SS.mmm] <step> start/done (Δ=<ms>)`,尤其:
- 每个 bootstrap(patchelf/ninja)的**下载 URL、镜像、connect/下载/解压各阶段**用时;
- toolchain 安装的 resolve / download / extract 各阶段用时;
- 索引 fetch 的 pointer 比对 / 下载 / git 各阶段用时。

**约束**:
- 仅 `--verbose`(或 `MCPP_LOG=debug`)可见,日常用户输出不变。
- 时间戳统一(单调时钟取 Δ,墙钟取绝对时间)。
- **和最新版一起发布**(随 mcpp 下个版本)。

**配套铁律(已并入发布流程)**:**每次 xlings / mcpp 发新版,对应索引也要更新到 latest**
—— xim-pkgindex(`xim:mcpp`/`xim:xlings` 版本块 + artifact)、mcpp-index(若涉及库)都要随发版同步,
否则默认客户端装不到新版(见本仓 + xim-pkgindex 仓发布机制文档)。现已有 push 触发的
publish-artifact CI,改索引即自动发 artifact;版本块(mcpp.lua/xlings.lua 的 `latest` ref)
仍需在发版流程里 bump。

## 5. 一句话

**首次 init 保证有索引(内置 seed 优先,否则自动拉一次);之后离线优先(默认不联网刷索引,
缺包才提示显式 `mcpp index update`),刷新走"轻量指针 sha 比对"而非全量 git;mcpp-index
补齐 artifact + push 触发发布 CI,与 xim-pkgindex 统一模型。**

相关:`.agents/docs/2026-05-31-index-refresh-cache-labels-plan.md`、
`.agents/docs/2026-05-08-package-index-config.md`;Termux 背景见
`.agents/docs/2026-06-23-termux-android-adaptation.md` §3。
