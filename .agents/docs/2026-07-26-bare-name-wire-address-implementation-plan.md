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

### P3c — 全仓 bootstrap 收敛（release 失败后追加）

0.0.109 的 release 在 `build + upload (linux/x86_64)` 挂掉,同一个症状:裸
`xlings install mcpp` 拿到 **0.0.105** < 索引 floor 0.0.108 → 描述符全读不到 →
依赖回落 legacy 地址 → 死在 `mcpplibs.cmdline@0.0.1`。P3 只修了两个 composite
action,`release.yml` 自己还有 4 处内联 bootstrap。

- [x] **C10** 全仓盘点:裸 `xlings install mcpp` 共 **6+ 处**(release.yml ×4、
      cross-build-test.yml ×2、两个 action、ci-aarch64-fresh-install、ci-linux-e2e),
      各自还有细微漂移(Windows 那处用 `find | head -1` 取到任意版本)。
- [x] **C11** 抽 `.github/tools/install_pinned_mcpp.sh` 作为**唯一实现**:读 pin →
      装 → 按版本目录定位 → 精确版本断言 → stdout 只输出路径、诊断走 stderr。
      六处调用它,而不是写第七份拷贝。
- [x] **C12** 修 `set -eo pipefail` 下的雷:`find ~/.xlings ... 2>/dev/null` 遇到
      不可读目录返回非零(`2>/dev/null` 只藏消息不藏状态)→ 整条管道非零 → 脚本
      在能报出任何有用信息之前就被 errexit 杀掉。全部 `|| true` + 显式 `return 0`。
      **这个雷在 P3 写进两个 action 的版本里同样存在**,只是循环提前 return 才没炸。
- [x] **C13** Windows release leg 同样在 `cd "$WORK"` 后不回仓库 → 用 cd 前捕获的
      `REPO_DIR="$(pwd)"`,不用 `GITHUB_WORKSPACE`(git-bash 下是反斜杠路径)。
- [ ] **C14**(follow-up,不在本次)`ci-aarch64-fresh-install` 的裸安装是**故意的**
      (验证用户 fresh-install 路径);`ci-linux-e2e` 的 hermetic job 目前是绿的,
      连跑六轮后为风格一致去动绿 job 是坏判断。两处单独评估。

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
