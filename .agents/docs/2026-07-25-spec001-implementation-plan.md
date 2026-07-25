# SPEC-001 落地 — Implementation Plan(mcpp 0.0.106)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans。步骤用 `- [ ]` 跟踪。

**Goal:** 实现 `docs/spec/package-identity.md`(SPEC-001)与 `.agents/docs/2026-07-25-name-namespace-canonical-implementation-spec.md` 的全部待实现项 → 单 PR(附版本)→ CI 全绿 → bypass squash 合入 → 0.0.106 release + xlings 全生态验证 → mcpp-index 全仓迁移 + 文档 + PR + CI。

## 前置事实(已核实)

**xlings 侧基础设施已就绪**:`openxlings/xlings#381` 已关闭,**0.4.69** 已发布(PR #382,libxpkg 0.0.46 身份模型)。隔离环境实测 0.4.69:

| 场景(同一索引仓,两个包 `name="demo"`,ns 分别 alpha/beta,**短名形态**) | 结果 |
|---|---|
| `xlings search demo` | ✅ `alpha:demo` 与 `beta:demo` 都列出 |
| `xlings info alpha:demo` / `info beta:demo` | ✅ 各自精确命中 |
| `xlings info demo`(裸名) | ✅ ambiguous 并列出两个候选 |

**新契约(来自 xlings 设计文档 §2.2,已实测印证)**:

```
effectiveNamespace = package.namespace 非空 ? package.namespace : repo.defaultNamespace
PackageIdentity    = (effectiveNamespace, package.name)
canonicalName      = effectiveNamespace 非空 ? effectiveNamespace + ":" + package.name
                                             : package.name
```

**注意分隔符是 `:` 而非 `.`,且 `name` 是字面值。** 这直接决定 mcpp 该发的 target 形态。

## Global Constraints

- **不改动 xlings 任何规范**(SPEC-001 §1.2),只基于其机制实现。
- 现网 48 个 FQN 形态描述符**必须继续可装**(兼容矩阵只有「旧 mcpp + 新短名」一格破损)。
- 既有单测 + e2e(162 例)全绿。
- 版本两处同步:`mcpp.toml` + `src/toolchain/fingerprint.cppm`(0.0.105 踩过)。

## Tasks

### P1 身份层(spec §7.1 §7.5)

- [x] **T1.1** `canonical_xpkg_identity` 去 split-on-last-dot:身份直接取 `(declaredNs, declaredName)`,仅为 legacy FQN 形态剥 `<ns>.` 前缀。不再从 `name` 反推 ns。
- [x] **T1.2** `xpkg_name_form_violation` 语义反转:`name` **不得含点**;legacy FQN(恰以 `<ns>.` 开头)降级为可识别的兼容形态(不报错)。
- [x] **T1.3** `mcpp xpkg parse` 同步(错误文案 + `--json`);`--allow-split-name` 语义随之调整或退役。

### P2 wire 层(spec §7.2 §7.3)

- [x] **T2.1** `prepare.cppm` target 构造改为 `<effectiveNamespace>:<字面 name>@<ver>`;字面值取自已在作用域的 `luaContent`,读不到时回落现有渲染。
- [x] **T2.2** `install_dir_candidates` 以 `{ns}-x-{字面 name}` 为**首选**,其余降为过渡兼容项。
- [x] **T2.3** 运行期 fail-fast 谓词随 T1.2 反转。

### P3 发现层(spec §7.4)

- [x] **T3.1** `IdentityIndex`:扫描 `pkgs/*/*.lua` 读声明身份建表(结果按身份索引,文件名不参与)。
- [x] **T3.2** `read_xpkg_lua` / `read_xpkg_lua_from_path` / `read_xpkg_lua_from_project_data` 三入口改走 IdentityIndex;候选文件名降为可选加速探测。
- [x] **T3.3** 同一索引内 `(ns,name)` 重复 → 明确报错并列出两个描述符路径(对齐 xlings 行为)。

### P4 依赖升级

- [x] **T4.1** `kXlingsVersion` 0.4.68 → **0.4.69**
- [x] **T4.2** `release.yml` / `ci-linux-e2e.yml` / `cross-build-test.yml` 的 `XLINGS_VERSION` 同步
- [x] **T4.3** 复核 `index_spec.cppm` / `config_migration.cppm` 里 "xlings >= 0.4.68" 注释语义是否需更新

### P5 测试

- [x] **T5.1** 单测:`XpkgNameForm` 断言反转 + 身份归一化新用例(嵌套 ns、legacy FQN、短名)
- [x] **T5.2** e2e 161 断言反转(split 形态从「拒绝」变「接受」,含点短名变「拒绝」)
- [x] **T5.3** 新 e2e:**短名描述符可安装**
- [x] **T5.4** 新 e2e:**任意文件名可发现**(文件名与身份完全无关)
- [x] **T5.5** 新 e2e:**同索引内两个同短名不同 ns 的包各自可装**(端到端印证 xlings 0.4.69 契约)
- [x] **T5.6** 回归:现网 FQN 形态描述符仍可装(裸 `gtest` → `compat.gtest`)

### P6 文档 · 版本 · PR

- [x] **T6.1** SPEC-001 实现状态标记全面更新(❌→✅),删除过渡期条款,状态 `草案 → 评审中`
- [x] **T6.2** `docs/05-mcpp-toml.md` §2.5 + `docs/zh/` 的 xpkg 作者段改写为短名形态
- [x] **T6.3** CHANGELOG 0.0.106
- [x] **T6.4** 版本两处同步 0.0.106
- [ ] **T6.5** 单 PR → CI 全绿 → `gh pr merge --squash --admin`

### P7 生态

- [ ] **T7.1** tag v0.0.106 → 四平台 release → publish-ecosystem(gtc 大件预备手工补传 + GET 核验)
- [ ] **T7.2** xim-pkgindex bump PR → CI → 合入
- [ ] **T7.3** `xlings install mcpp@0.0.106` 真装验证 + bootstrap pin

### P8 mcpp-index 迁移

- [ ] **T8.1** 48 个描述符 `name` 去命名空间前缀(短名化)
- [ ] **T8.2** `check_package_name.lua` 规则反转;文件名 lint 不加(文件名自由)
- [ ] **T8.3** `index.toml` `min_mcpp`/`latest_mcpp` 抬到 0.0.106(**硬性**:低版本读新描述符会静默 E_NOT_FOUND)
- [ ] **T8.4** `validate.yml` MCPP_VERSION + 三个 matrix 条目 → 0.0.106
- [ ] **T8.5** `docs/repository-and-schema.md` 身份章节按 SPEC-001 改写
- [ ] **T8.6** PR → CI 全绿 → 合入

**⚠️ 已知阻塞**:mcpp-index CI 升版本时会撞上一个 **0.0.103–0.0.105 区间引入的 libxcb 链接回归**(`libX11.so: undefined reference to xcb_*`,0.0.102 同样冷装 xcb 却通过)。需在 P8 前二分定位(钉 0.0.103 / 0.0.104 各跑一次 linux workspace job)。记录见 memory `xcb-link-regression-0103-0105`。
