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

### P3 — 失效面 C：bootstrap pin

- [x] **C1** `.github/actions/bootstrap-mcpp/action.yml`：unix + windows 两段
      `xlings install mcpp -y` → 读 `.xlings.json` 的 pin 装指定版本。
- [x] **C2** 装完断言 `mcpp --version` == pin，不等则 fail（不再静默漂移）。

### P4 — 版本 + 收口

- [x] **D1** 版本 0.0.108 → 0.0.109，两处同步。
- [x] **D2** 本地全量单测。
- [ ] **D3** 本地 e2e（至少 100 / 163 / 165 / 78 / 79 + 全量能跑的部分）。
- [ ] **D4** 单 PR（附版本）→ 7 个 workflow 全绿。
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
