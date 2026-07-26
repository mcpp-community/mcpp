# 裸名 wire address 修复 — Implementation Plan（mcpp 0.0.109）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans。步骤用 `- [ ]` 跟踪。

**Design:** `.agents/docs/2026-07-26-bare-name-wire-address-design.md`

**Goal:** 修复三个失效面（A 裸名 wire address / B e2e 163 fixture 耦合 / C bootstrap 版本
漂移）→ 单 PR（0.0.109）→ CI 全绿 → bypass squash 合入 → release + xlings 全生态验证。

## Global Constraints

- **不改 `xpkg_lua_identity_matches` 的放宽规则**（裸名解析到 compat 是有意设计）。
- **不改 xlings 任何规范。**
- 描述符读不到时，行为**逐字节不变**（失败点留在原地）。
- 现网 48 个描述符：8 个 mcpplibs 不变、6 个其他 ns 够不到、34 个 compat 当前全坏 → 修好。
- 既有单测 + 164 例 e2e 全绿。
- 版本两处同步：`mcpp.toml:3` + `src/toolchain/fingerprint.cppm:21`（0.0.105 踩过）。

## Tasks

### P1 — 失效面 A：wire address 收敛

- [x] **A1** `src/manifest/xpkg.cppm`：新增 `struct XpkgWireAddress` +
      `xpkg_wire_address(luaContent, requestNs, shortName)`，语义见 design §2.2。
      导出到模块接口。
- [x] **A2** `tests/unit/test_manifest.cpp`：`xpkg_wire_address` 七格矩阵（design §3.1）。
      **先写，先看它红。**
- [x] **A3** `src/build/prepare.cppm:1864` 起：改用 `xpkg_wire_address`，
      target 由它给出。删掉就地的 `wireName` 推导。
- [x] **A4** `src/build/prepare.cppm` 兜底链加宽：`compat:<short>` 在前、
      `compat.<short>` 在后，各自与首选 target 去重（design §2.3）。
- [x] **A5** 新增 e2e `tests/e2e/165_bare_name_cross_namespace_wire_address.sh`
      （design §3.2）——hermetic 复现生产 bug，断言 store 目录 `compat-x-widget`。

### P2 — 失效面 B：e2e 163 去耦合

- [x] **B1** `tests/e2e/163_identity_first_resolution.sh:42` sed pattern 改为
      不依赖描述符旧拼写（design §3.3）。
- [x] **B2** 本地跑 163 验证四个 sub-case（step 4 依赖 xlings >= 0.4.69，按现有逻辑
      自动 skip 则接受）。

### P3 — 失效面 C：pin 与版本漂移（实施中范围扩大）

- [x] **C1** `.github/actions/bootstrap-mcpp/action.yml`：unix + windows 两段
      `xlings install mcpp -y` → 读 `.xlings.json` 的 pin 装指定版本。
- [x] **C2** 装完断言解析出的版本**精确等于** pin（子串匹配会让 `0.0.10` 被
      `0.0.109` 满足），不等则 fail。
- [x] **C3** 按「版本目录」定位二进制而非硬编码内部布局：Linux 是
      `~/.xlings/data/xpkgs/xim-x-mcpp/<v>/mcpp`，Windows 是
      `.../subos/default/data/xpkgs/xim-x-mcpp/<v>/bin/mcpp.exe`，根与层级都不同。
      **教训**：只凭 Windows 一条日志推断跨平台路径，白烧一轮 CI。
- [x] **C4** `.xlings.json` 用 `cd` 之前捕获的 `REPO_DIR` 绝对路径读取——Windows
      leg 解压后 `cd "$WORK"` 且从不切回；不用 `GITHUB_WORKSPACE`（git-bash 下
      是反斜杠 Windows 路径）。
- [x] **C5** `setup-macos-llvm` 同样是未 pin 的 bootstrap 点（冷启动拿到 0.0.105
      < 索引 floor 0.0.108 → 描述符全读不到 → 依赖回落 legacy 地址失败）。同法 pin。
- [x] **C6** bootstrap 打印 system / sandbox 两个 xlings 版本。沙箱那份才是真正
      解析依赖的,而它从不更新,此前完全不可见。

### P3b — 沙箱 xlings 版本（实测后追加，非推断）

- [x] **C7** 实测 CI 沙箱 xlings = **0.4.30**（先加零风险诊断打印，再动手）。
      索引短名迁移后同一 repo 有两个 `name = "lua"`（`compat:lua` 与
      `mcpplibs.capi:lua`，经 `mcpplibs.xpkg` 传递同时需要），0.4.69 前 xlings 按
      裸 name 建表 → 必有一个够不到，**够不到哪个取决于机器**。
- [x] **C8** `bootstrap-mcpp` / `setup-macos-llvm` 的 xlings 0.4.30 → **0.4.69**，
      与 `release.yml`／`cross-build-test.yml`（早已 0.4.69）对齐；
      `bootstrap-macos.yml`（dispatch-only）一并跟上。
- [x] **C9** xlings 版本进入**缓存血统**而非仅 key：`restore-keys` 是前缀匹配，
      只放进 key 的话 0.4.69 的 bootstrap 仍会恢复 0.4.30 的沙箱。代价=一次冷启动。

### P4 — 版本 + 收口

- [x] **D1** 版本 0.0.108 → 0.0.109，两处同步。
- [x] **D2** 本地全量单测。
- [x] **D3** 本地全量 e2e：155 passed / 唯一非预期失败是本机 g++ shim 环境问题（22）。
      **负向对照**用 `git checkout main -- <files>` 重建（`git stash` 做不了——
      修复已在 commit 里时它只回退未提交部分，测试会假通过）：165 与 69-L8 都失败。
- [x] **D4** 单 PR #286（0.0.109）→ 15 个 check 全绿。
- [ ] **D5** bypass squash 合入 main。

### P5 — xlings 全生态验证

- [ ] **E1** release v0.0.109（四平台）。
- [ ] **E2** 镜像 xlings-res 双端（github + gitcode），sha256 独立核验。
- [ ] **E3** xim-pkgindex PR。
- [ ] **E4** 干净 XLINGS_HOME 真装 `xlings install mcpp@0.0.109` 并跑通。
- [ ] **E5** bootstrap pin `.xlings.json` → 0.0.109（独立 PR）。

## 风险与对策

| 风险 | 对策 |
|---|---|
| `compat:compat.zlib`（legacy FQN + 裸名请求）xlings 不认 | e2e 163 step 2 已覆盖等价形态（`acme:acme.widget` → `acme-x-acme.widget`），迁移前它是绿的 |
| 改动波及 8 个 mcpplibs 包 | 单测显式锁 `mcpplibs:cmdline` 不变；e2e 全量兜底 |
| CI 热 cache 掩盖 C1 效果 | C2 的版本断言让漂移立刻可见 |
| 索引侧假绿复发 | A5 是 hermetic 的，不依赖远端索引内容 |

## P6 — 发布运维中发现、不在本批次修的问题

- [ ] **F1 沙箱 xlings 永不刷新(用户侧静默中招)** — `xlings_binary.cppm` 首行
      `if (exists(destBin)) return destBin;`,mcpp 把 xlings vendor 进
      `~/.mcpp/registry/bin` **只在 self init 一次**,之后永不重访;而**沙箱那份才是
      真正解析依赖的**。索引短名迁移后同一 repo 有两个 `name="lua"`,0.4.69 前 xlings
      按裸 name 建表 → 随机一个包 not found,**换机器报错的包还不一样**,且无任何
      "你的 xlings 太旧"提示。**影响面**:发布 tarball 自带 0.4.69,所以全新安装不
      中招;只有"已有旧沙箱 + 原地升级"的用户会。**建议只做诊断**(检测版本不足时
      明确报错并指向 `mcpp self init --force`),不自动改写用户沙箱。

- [ ] **F2 `gtc` 错误信息明文泄露 access_token** — 上传失败时打出
      `...obs_callback?access_token=<真token>`,会进 CI 日志。

- [ ] **F3 `gtc` 退出码在 obs_callback 路径不可信** — macosx 包上报
      `failed: ... obs_callback code:400 err:EOF`,但**对象存储上传已完成、文件正常
      服务**。判断成败必须靠真实 GET + 字节比对,不能靠工具自报。

- [ ] **F4 gitcode 上传 180s 上限对整批失效** — 本次**四个 tarball 全部超时**,
      连 3.9MB 的都超。不是体积问题,是跨境链路整体卡住 → 需要本地补传兜底流程
      (或把 gitcode leg 改成异步/重试策略)。

- [ ] **F5 索引 artifact 传播滞后于 git** — `raw.githubusercontent` 上已有 0.0.109,
      指针 JSON 也已指向新 commit,但客户端(GLOBAL 经 `ghfast.top` 代理)仍解析到
      上一个 artifact。**轮询 raw 是误导信号**,要轮询客户端实际解析结果。

- [ ] **F6 索引 URL 两种拼写** — `mcpp-community/mcpp-index.git`(4 处)与
      `mcpplibs/mcpp-index.git`(6 处),靠 GitHub 改名重定向存活;#267 org 迁移残留。
