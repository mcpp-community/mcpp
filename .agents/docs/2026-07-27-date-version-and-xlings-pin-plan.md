# 日期版本号 + xlings pin 收敛 — 实施计划

配套设计：`2026-07-27-date-version-and-xlings-pin-design.md`
目标版本：**2026.7.27.1**（常规迭代；`.0` 保留给正式/稳定版）
xlings 目标 pin：**2026.7.27.2**

单 PR 交付。阶段之间有依赖顺序：P1 必须先于 P5（版本一改，比较逻辑就得是对的）。

---

## P1 — `version_req` 支持 4 段（先做，其余都依赖它）

**文件**：`src/version_req.cppm`

1. `Version` 增加 `int revision = 0;` 与 `int components = 0;`
2. **删掉 `= default` 的 `<=>`**，改为显式比较 major → minor → patch → revision。
   `components` **不得**参与比较（否则 `"1.2"` ≠ `"1.2.0"`）。
3. `parse_version`：循环上界 3 → 4，记录实际解析到的段数进 `components`。
4. `str()`：按 `components` 回写。`components == 0` 时按 3 段回写（默认构造的 `Version{}` 走这条）。
5. `matches()` 的 Caret / Tilde 上界构造补 `upper.revision = 0`。

**头号风险**：`resolver.cppm:138` 用 `str()` 重建依赖版本串。任何回写不精确都会改变现存依赖的寻址。

**测试**（`tests/unit/test_version_req.cpp`，无则新建）：

| 用例 | 断言 |
|---|---|
| 序 | `2026.7.27.1 < 2026.7.27.2`；`2026.7.27.9 < 2026.7.27.10` |
| 跨方案序 | `0.0.109 < 2026.7.27.1` |
| 三段兼容 | `1.2` == `1.2.0`；`1` == `1.0.0` |
| 回写 | `1.15.2` → `1.15.2`（**非** `1.15.2.0`） |
| 回写 `.0` | `2026.8.1.0` → `2026.8.1.0`（**不塌成三段**） |
| 回写 4 段 | `2026.7.27.1` → `2026.7.27.1` |
| Caret 不回归 | `^1.2.3` 匹配 `1.9.9`、不匹配 `2.0.0` |
| Tilde 不回归 | `~1.2.3` 匹配 `1.2.9`、不匹配 `1.3.0` |
| Caret 上界含 revision | `^2026.7.27.3` 不匹配 `2027.0.0.1` |

**E0006**（`tests/unit/` 内 index_contract 相关）：
`floor_violation("2026.7.27.5", "2026.7.27.1")` 必须返回违规；反向必须放行。**这是当前会静默错放的那条。**

---

## P2 — 修格式敏感的脚本

**文件**：`.github/tools/install_pinned_mcpp.sh`（两处，L82 / L84）

```bash
# 旧：只截三段
grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
# 新：3 或 4 段
grep -oE '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
```

两处都要改 —— 只改一处会让 pin 与实测值按不同规则提取，比较结果无意义。

已确认 `release.yml` 从 `mcpp.toml` awk 取版本、`aur-publish.yml` 用 sed 取，均与段数无关，不需要改。

---

## P3 — 升 xlings pin 到 2026.7.27.2

按设计 §5.2 的 7 个位置逐一替换 `0.4.69` → `2026.7.27.2`：

- `src/xlings.cppm` `pinned::kXlingsVersion`（同时把那条已经不全的 lock-step 注释改为指向 P4 的守卫）
- `.github/actions/bootstrap-mcpp/action.yml`（default + 两个 cache key/lineage）
- `.github/actions/setup-macos-llvm/action.yml`
- `.github/workflows/bootstrap-macos.yml`
- `.github/workflows/ci-linux-e2e.yml`
- `.github/workflows/cross-build-test.yml`（×2）
- `.github/workflows/release.yml`（**这一份决定 release 内置的 xlings**）

Cache key 含 xlings 版本，改 pin 会自动换 lineage —— 这是 0.0.109 批次刻意做的，别再退回去。

**注意**：0.0.109 批次的教训是换 lineage 会**去掉热缓存的意外保护**，把原本被掩盖的问题暴露出来。所以 P3 之后必须跑一次冷缓存 CI，不能只看增量绿。

---

## P4 — 漂移守卫

**新增**：`.github/tools/check_version_pins.sh`

两组断言：
1. 所有 xlings pin == `src/xlings.cppm` 的 `kXlingsVersion`
2. `mcpp.toml` == `MCPP_VERSION` == `.xlings.json` == `MCPP_PIN`

实现约束：**纯文本提取，不依赖 mcpp 二进制** —— 构建挂掉时它必须仍然可用。

接进 `ci-linux.yml` 早期步骤（在构建之前，快速失败）。

**负向验证是验收的一部分**：故意改坏一个 pin，确认脚本失败并指名文件；改回后确认通过。只验证「正常时通过」等于没验证。

---

## P5 — 版本号切到 2026.7.27.1

四处同一个 commit 内改完（设计 §5.1）：`mcpp.toml`、`fingerprint.cppm`、`.xlings.json`、`ci-fresh-install.yml` 的 `MCPP_PIN`。

改完立刻跑 P4 的守卫做自检。

---

## P6 — 规范落文档

**`.agents/skills/mcpp-release/SKILL.md`**：
- 日期格式 `YYYY.M.D.N`，不补零
- **`.0` = 正式/稳定版；常规迭代从 `.1` 起**
- 版本落点从「两处」更正为**四处**（现文档只写了 `mcpp.toml` + `fingerprint.cppm`，漏了 `.xlings.json` 与 `MCPP_PIN`）
- 指向 P4 的守卫脚本

---

## P7 — 交付闭环

1. 本地：单元 + e2e 全绿
2. 单 PR（附版本），全平台 CI 绿
3. bypass squash 合入
4. 发布闭环：四平台 release → 独立复算 sha256 → gitcode 镜像双端核验 → xim-pkgindex PR → 干净 `XLINGS_HOME` 真装 `mcpp@2026.7.27.1` → pin bump

**发布运维已知坑**（0.0.109 批次实测，别重踩）：
- gitcode 单文件 180s 上限对大 tarball 必然超时 → 本地 `gtc` 补传 + `cmp` 逐字节核验
- `gtc` 失败输出会明文带 `access_token`，贴日志前必须打码
- `gtc` 在 `obs_callback` 路径上退出码会说谎 —— 以 GET 回内容为准，不信退出码
- 索引 artifact 传播滞后于 git；轮询 raw 是误导性的就绪信号

---

## 验收清单

- [ ] `2026.7.27.1 < 2026.7.27.2` 且 `2026.7.27.9 < 2026.7.27.10`
- [ ] `str()` 三种形态精确回写（含 `.0` 不塌段）
- [ ] E0006 在同日不同序号间正确触发
- [ ] 既有三段依赖解析逐字节不变（Caret/Tilde 回归用例）
- [ ] 漂移守卫负向验证通过
- [ ] 冷缓存 CI 全平台绿
- [ ] 真装新版本并跑通一次依赖解析
