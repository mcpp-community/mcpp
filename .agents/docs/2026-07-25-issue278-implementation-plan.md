# #278 包身份双侧收敛 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `.agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md` 的两条不变式 → 单 PR(`Fixes #278`)→ CI 全绿 → bypass squash 合入 → 0.0.105 release → xlings 全生态验证。

**Architecture:** 一个纯谓词 `xpkg_name_form_violation` 两处调用(lint + 运行期),消灭"同一决策两处推导";依赖侧候选阶梯收敛为封闭三档,全索引扫描降级为**仅失败路径**的 did-you-mean 诊断,绝不回灌解析。

## Global Constraints

- `canonical_xpkg_identity` / `xpkg_lua_identity_matches` **一个字不改** —— 收紧只发生在发布侧(lint)与依赖解析调用点。
- 谓词必须收窄为"声明了非空 namespace 且 name 不以 `ns.` 开头",**不得**写成"字面 name != 推导 fqname"通用比较(会打断裸 `gtest` → `compat.gtest`)。
- did-you-mean 扫描三约束:仅失败路径触发、只进错误文案、扫描空不改退出码。
- 无 `namespace` 声明的上游包(`opencv`/`musl-gcc`)空 ns 是**合法身份**,T10 不得强行填充。
- T11 收紧 scoped 到 `selectDependencyCandidate`,不动闸门(保 `mcpp new --template`)。
- 既有单测与 e2e(160 例)全绿。

## Tasks

### C1 索引侧谓词与两个调用点

- [x] **T1** `xpkg_name_form_violation(declaredNs, declaredName)` + `_from_lua` 导出(`src/manifest/xpkg.cppm`)
- [x] **T2** `mcpp xpkg parse` 接入谓词,违规 exit 1;`--json` 输出 `error` 字段(`src/cli/cmd_xpkg.cppm`)
- [x] **T3** `loadVersionDep` install 分支 fail-fast;已解析到安装物的路径 `ui::warn`(`src/build/prepare.cppm`)

### C2 生成源

- [x] **T4** `emit_xpkg` 输出 `namespace` + FQN `name`;无 namespace 时 stderr 提示;`--namespace` 覆盖开关(`src/pm/publisher.cppm` + emit CLI)

### C3 依赖侧收敛

- [x] **T9** `selectDependencyCandidate` 全候选落空 → 明确失败(`prepare.cppm:1516-1541`)
- [x] **T10** discovery 档命中后 `extract_xpkg_namespace(*lua)` 回填真实 ns(声明为空则保持空)
- [x] **T11** discovery 档拒绝声明了非空 `namespace` 的描述符
- [x] **T12** did-you-mean:仅失败路径的全索引扫描,三约束写进实现注释

### C4 测试

- [x] **T5a** 单测:谓词 6 例(含 `CompatAliasIsClean` 回归锁)
- [x] **T5b** 单测:依赖侧解析(三档成功 / 第三方 ns 裸名失败 / P3 空 ns 锁)
- [x] **T5c** e2e:split 描述符 → 秒级自解释失败;改 FQN → 通过
- [x] **T5d** e2e:裸名请求第三方 ns 包 → 失败 + did-you-mean;改写后通过

### C5 文档与版本

- [x] **T13** 用户文档 §2.5(`docs/05-mcpp-toml.md` + `docs/zh/05-mcpp-toml.md`)
- [x] **T14** CHANGELOG 0.0.105 段(含 breaking:裸名不再解析第三方命名空间包)
- [x] **T15** `mcpp.toml` version → 0.0.105
- [x] **T11b** 修订 `2026-06-26 §4.4/§4.6(a)` 表述;更新 `prepare.cppm:1456-1459` 注释

### C6 验收与合入

- [x] **T6** 全索引 `mcpp xpkg parse` 回归,结果贴 PR。**实测 49/49 mcpplibs 描述符全过、0 违规**(非设计预期的 2 —— 上游 mcpp-index 已先行修好两例);完整 registry(200+)另见 §2.1b 的 xlings-native 修正
- [ ] **T7** 单 PR(`Fixes #278`)→ CI 全绿 → `gh pr merge --squash --admin`

### C7 生态

- [ ] **T16** release 0.0.105 四平台 + 镜像 xlings-res 双端 + xim-pkgindex PR + `xlings install mcpp` 真装验证
- [x] **T8** mcpp-index:**上游已完成** —— `chriskohlhoff.asio` / `tensorvia-cpu` 均已是 FQN,#116 的 `tests/check_package_name.lua` 已合入。该 Lua 守卫现与 `mcpp xpkg parse` 冗余但无害,可留作保险(其 CI 本就跑 `mcpp xpkg parse pkgs/*/*.lua`,升级 MCPP_VERSION 到 0.0.105 后自动获得引擎侧防护)
