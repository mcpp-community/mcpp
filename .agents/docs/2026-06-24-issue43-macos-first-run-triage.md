# Issue #43 分诊与关闭记录 — macOS 全新安装首跑

**日期**: 2026-06-24
**Issue**: [#43](https://github.com/mcpp-community/mcpp/issues/43)（标题 NULL，正文是一段 macOS 真机
`mcpp build --verbose` 首跑日志 + VS Code 安装告警截图）
**结论**: 头号阻塞（链接失败）已修复并加了跨平台回归守护；其余为已记录的非阻塞遗留项。本文是把
该 issue 的多个症状逐一归档、给出状态与去向的**收口文档**，细节不在此重复（指向各自的专文）。

---

## 0. 为什么用户今天复现仍失败

Issue 正文的 `--verbose` 时间戳是 `2026-06-24 09:0x`，但其报的 `library not found for -lSystem`
**根因修复 PR #162 在今天 09:55 才合入 main**。用户复现时跑的是**不含 #162 的已安装版 mcpp**，
所以仍报错——不是 #162 没生效，而是复现环境早于修复。升级到含 #162 的版本即消失（见 §1）。

---

## 1. 头号阻塞：链接 `library not found for -lSystem`（已修复 · PR #162）

```
ld64.lld: error: library not found for -lSystem
ld64.lld: error: undefined symbol: __stack_chk_fail / __stdoutp / _Unwind_Resume / fflush / memcpy / ...
```

**根因**：macOS 链接命令 `f.ld`（`src/build/flags.cppm`）**从不显式传 SDK**，链接侧靠 clang 的
隐式 SDK 探测（xcrun/`SDKROOT` → ld64 `-syslibroot`）去找 `<SDK>/usr/lib/libSystem.tbd`。干净的
CI Xcode runner 上探测正常、缺陷被掩盖；真机上一旦 `xcode-select` 指向异常 / 只装 CLT / 新装
bundled clang 探测失效，ld64.lld 拿不到 `-syslibroot` → 找不到 libSystem → 所有 libc 符号未定义。
**「编译过、链接挂」**正是此特征（编译用 bundled libc++ 头不依赖 SDK 探测，链接才需 libSystem）。

**修复**（PR #162，根因+方案专文 `2026-06-24-macos-link-lsystem-sdk.md`）：
1. `flags.cppm`：macOS `f.ld` 显式追加 `-isysroot <SDK>`（SDK 取自 `macos::sdk_path()`）。
2. `macos.cppm::sdk_path()` 加固多级回退：`SDKROOT` → `xcrun --show-sdk-path` →
   `xcrun --sdk macosx ...` → `xcode-select -p` 推导 → 固定路径（CLT / Xcode.app 内 SDK）。
   即使 xcrun 返回空也能定位 SDK，把链接从「碰运气」变「确定」。

**回归守护（本批 · PR #165）**：见 §5——跨平台 `mcpp build` e2e，macOS 上一旦链接再退化即 CI 红。

## 2. 首跑需手动回车 / stdin 挂起（已修复 · PR #163）

首跑装 POSIX 工具链时进程等待 stdin 回车才继续。修复：POSIX 路径也 seal stdin
（`</dev/null`），不再要求交互按键。

## 3. ninja bootstrap ~145s（已记录 · 待处理 · #164）

```
[VERBOSE 09:00:49.470] init: bootstrap ninja@1.12.1: start
[VERBOSE 09:03:15.090] init: bootstrap ninja@1.12.1: done (Δ=145618ms)
```

单步 ~2.4 分钟，是首跑「卡住」的主观感受来源。**不阻断构建**，但体验差。下一步是把 bootstrap 拆成
connect/download/extract 子计时再定位瓶颈（镜像 / 并行 / 复用 brew ninja）。详见
`todos/2026-06-24-macos-first-run-remaining.md` §A。

## 4. `xlings update` 子索引 artifact 404 + VS Code 安装告警（已记录 · xlings 侧 · 非 mcpp）

Issue 第二段日志的两类告警**都不在 mcpp 核心范畴**，归档供追溯：
- **CN 下子索引 404**：GitCode 不支持 GitHub 式 `releases/download/<tag>/<asset>` 直链，xlings 子索引
  fetch 拼了 GitHub 直链格式 → 404；回退 git via ghproxy 能成（慢）。详见
  `todos/2026-06-24-macos-first-run-remaining.md` §B。属 xlings 内部 fetch 逻辑。
- **VS Code 安装 `xattr -rd com.apple.quarantine` `Operation not permitted` → config hook failed**：
  xlings `xim-x-code` 配方对 `.app` bundle 去隔离属性失败（SIP/权限/受保护文件），导致该包安装
  「3 成功 / 1 失败」。与 mcpp 构建链路无关，不阻断 `mcpp build`。属 xlings 配方侧。

## 5. 跨平台回归守护 e2e（本批 · PR #165）

`tests/e2e/76_compile_commands_generated.sh`：`mcpp new` + `mcpp build` 一个最小工程，断言根目录
生成合法的 `compile_commands.json`（JSON 数组、`directory`/`file`、`command|arguments`、含
`src/main.cpp` 条目；有 python3 时再做 `json.load` 结构校验）。

**为何它同时守护本 issue 的链接缺陷**：测试用 `set -e` + `"$MCPP" build`，**build 包含链接步骤**——
macOS 上若 §1 的 `-lSystem` 再退化，`mcpp build` 非零退出，e2e 直接 FAIL。因此它既守护 CDB 生成契约，
又是 #43 链接回归的跨平台哨兵。

**「每个平台真实在跑」已用 CI 硬证据确认**（非仅「加了文件」）：
- harness `run_all.sh` 以 `[0-9]*.sh` glob 发现，line2 空 `# requires:` → 三平台都不跳过；
  三个 workflow（`ci-linux-e2e` / `ci-macos` / `ci-windows`）都无条件 `bash tests/e2e/run_all.sh`，
  无分片/白名单（#157 仅拆并行 workflow，#160 sharding 仍是 TODO 未实现）。
- PR #165 当次 CI 日志实测三平台**均执行且通过**：

| 平台 | 结果 | 耗时 | JSON 校验 |
|------|------|------|-----------|
| linux x86_64 | PASS | 2.99s | OK (1 entries) |
| macOS ARM64 | PASS | 1.00s | OK (1 entries) |
| windows x64 | PASS | 2.41s | OK (1 entries) |

## 6. 关闭决策

| # | 症状 | 根因 | 状态 |
|---|------|------|------|
| 1 | `library not found for -lSystem` 链接失败 | 链接侧不显式传 SDK，靠隐式探测，真机失效 | **已修复 PR #162** + 回归守护 #165 |
| 2 | 首跑需回车 / stdin 挂起 | POSIX 工具链装未 seal stdin | **已修复 PR #163** |
| 3 | ninja bootstrap ~145s | 单步无细分计时，瓶颈未定位 | 已记录 #164，非阻塞，待优化 |
| 4 | 子索引 404 / VS Code xattr 失败 | xlings fetch/配方侧（非 mcpp） | 已记录 #164，非阻塞 |

**#43 头号阻塞已闭环**（修复 #162 + 跨平台回归守护 #165）。剩余项为已建档的非阻塞遗留/xlings 侧，
单独追踪。据此关闭 #43，并在 issue 留指向本文与各 PR 的归档评论。

## 7. 用户侧升级与自查

- 升级到含 #162 的 mcpp 版本（≥ 0.0.61 线，含本批修复的发布）后重跑即可。
- 若链接仍失败：`xcrun --show-sdk-path`（应打印有效 SDK）/ `xcode-select -p`（应指向
  CommandLineTools 或 Xcode.app/Contents/Developer）；为空则 `xcode-select --install`。加固后 mcpp
  即便 xcrun 异常也会走回退路径定位 SDK。

## 8. 相关文档
- `2026-06-24-macos-link-lsystem-sdk.md` — §1 链接失败根因+修复专文（PR #162）。
- `todos/2026-06-24-macos-first-run-remaining.md` — §3/§4 遗留项（ninja 145s、子索引 404）。
- xlings `.agents/docs/2026-06-05-macos-min-version-support.md` — macOS 部署底线 / 静态 libc++。
