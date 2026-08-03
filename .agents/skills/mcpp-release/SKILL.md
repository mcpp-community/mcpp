---
name: mcpp-release
description: Use when releasing a new version of mcpp — bumps version, creates tag, triggers release CI, and monitors until all platforms succeed. Covers the full release checklist to avoid common pitfalls like version string mismatches.
---

# mcpp 版本发布流程

## 版本号规范

**格式：`YYYY.M.D.N`**（日期版本，月/日不补零），例如 `2026.7.27.1`。
自 `2026.7.27.1` 起启用，此前为 `0.0.x`。与 xlings 生态一致（xlings 于同日从 `0.4.70` 迁入）。

第 4 段的语义：

> **`.0` 保留给正式版本 / 稳定版本。日常迭代默认从 `.1` 开始。**

即一天内可发 `.1`、`.2`、`.3` …… 若干次常规版本；`.0` 只在该版本被认定为正式 release 或稳定版时使用。

跨方案的序是单调的：`0.0.109` < `2026.7.27.1`，第一段从 `0` 变 `2026`，不存在回退。

> 比较逻辑见 `src/version_req.cppm`。它支持 4 段；**改动那里时务必保证 `str()` 精确回写**，
> `src/pm/resolver.cppm` 用它重建依赖版本串，会流向 lock 文件与 xlings wire 地址。
> 尤其 `.0` 结尾的版本不能塌成三段。

## Overview

mcpp 有 **三个持久化版本位置**，以及 `ci-fresh-install` 的一个运行时推导值。它们分属
两组，在不同时间更新；把"正在构建的版本"与 bootstrap pin 一起前移会让 CI 尝试安装
尚未发布的 mcpp。

**第一组：正在构建的版本**（发布时改，走 bump PR）

1. `mcpp.toml` → `[package].version` — 构建系统读取的项目版本，release.yml 由它推导 tag
2. `src/toolchain/fingerprint.cppm` → `MCPP_VERSION` — 编译期硬编码常量（`--version` 输出、BMI 指纹、E0006 索引底线比较）

这两处必须**在同一个 commit 里**一起改：`tests/e2e/01_help_and_version.sh` 交叉比对
`mcpp.toml` 与 `mcpp --version`，只改一处 CI 立刻红。

**第二组：bootstrap pin —— CI 用哪个 mcpp 来自举**（发布并进索引之后才可改）

3. `.xlings.json` → `workspace.mcpp` — CI bootstrap 装哪个 mcpp

`.xlings.json` 必须指向一个**已经发布、镜像并进入索引**的版本。因此它在 bump PR 中
保持已有的可安装版本，直到发布收尾时才可前移。

`ci-fresh-install.yml` 的 `MCPP_PIN` 不是持久化 pin：`wait-index` 从最新 GitHub
Release 推导一次，所有安装 job 消费同一个输出。绝不能手工编辑或恢复字面量
`MCPP_PIN`，否则 index guard 和实际安装版本会再次漂移。

`.github/tools/check_version_pins.sh` 校验版本关系和 xlings pin：

```bash
bash .github/tools/check_version_pins.sh
```

**必须用 `bash` 跑，不能用 `sh`。** 脚本用了进程替换（`done < <(...)`），POSIX
`sh`/dash 解析不了，用 `sh` 调用会在第 95 行附近报 `Syntax error: redirection
unexpected`。那是调用它的 shell 的问题，不是脚本的缺陷 —— 它的 shebang 是
`#!/usr/bin/env bash`，CI 也是用 `bash` 调的。别据此把这条 guard 当成坏的而跳过：
它是唯一能机器化捕捉 pin 漂移的东西。

也不要通过修改文档或 workflow 绕开动态 `MCPP_PIN` 设计。`src/xlings.cppm` 的
`pinned::kXlingsVersion` 仍是 xlings 版本的唯一真源。

## 发布步骤

### 1. 确认 main 分支状态

```bash
git checkout main && git pull origin main
# 确认 CI 全部通过
gh run list --branch main --limit 3
```

以分支保护和 `gh pr checks <pr-number>` 显示的 actual required checks 为准。
在 main 上监控当前运行时，检查 `ci-linux`、`ci-linux-e2e`、`ci-macos`、
`ci-macos-e2e`、`ci-windows`、`ci-windows-e2e` 与 `cross-build-test` 的结果；
跳过或非 required 的 workflow 不是合入 gate。不要在 required CI 红的时候发版。

### 2. bump 版本号（第一组两处，单个 commit，走 PR）

**只改第一组的两个文件**，并且在同一个 commit 里。`.xlings.json` bootstrap pin
**不要动**；`MCPP_PIN` 是 workflow 运行时推导值，绝不能手工编辑，见 Overview。

```bash
# 日期版本：当天序号从 .1 起；.0 仅用于正式/稳定版
NEW_VERSION="2026.7.27.1"

git checkout -b "chore/bump-$NEW_VERSION"

sed -i "s/^version.*=.*/version     = \"$NEW_VERSION\"/" mcpp.toml
sed -i "s/MCPP_VERSION = \".*\"/MCPP_VERSION = \"$NEW_VERSION\"/" src/toolchain/fingerprint.cppm

# 校验：mcpp.toml 与 MCPP_VERSION 相等，.xlings.json 不领先于正在构建的版本。
bash .github/tools/check_version_pins.sh

# 自查：构建产物真的报新版本。注意 target/ 目录名带指纹哈希，
# 版本一变就是新目录 —— 用 `ls -dt` 取最新的那个，`head -1` 会拿到旧二进制。
mcpp build && "$(ls -dt target/*/*/bin/mcpp | head -1)" --version

git commit -am "chore: bump version to $NEW_VERSION"
git push -u origin "chore/bump-$NEW_VERSION"
gh pr create --title "chore: bump version to $NEW_VERSION" --body "..."
# CI 绿后合入；版本 bump 同样禁止直推 main（见 mcpp-contributing）
```

### 3. 创建并推送 tag

```bash
git tag "v$NEW_VERSION"
git push origin "v$NEW_VERSION"
```

Tag push 会自动触发 `release.yml` workflow。

### 4. 监控 Release CI

Release workflow 包含**四个平台**的构建，外加一个生态发布 job：

| Job | 平台 | 产物 | 依赖 |
|-----|------|------|------|
| `build-release` | Linux x86_64 | `mcpp-X.Y.Z-linux-x86_64.tar.gz` | 无（先执行） |
| `build-linux-aarch64` | Linux aarch64（交叉） | `mcpp-X.Y.Z-linux-aarch64.tar.gz` | 等 Linux x86_64 完成 |
| `build-macos` | macOS ARM64 | `mcpp-X.Y.Z-macosx-arm64.tar.gz` | 等 Linux 完成 |
| `build-windows` | Windows x86_64 | `mcpp-X.Y.Z-windows-x86_64.zip` | 等 Linux 完成 |
| `publish-ecosystem` | — | 镜像到 xlings-res 双端 + 开索引 bump PR | 等**全部四个**构建完成 |

`build-linux-aarch64` 是两段式的（bootstrap 先构出本 release 的 x86_64 mcpp，再用它交叉构建
aarch64），因为 bootstrap 装的是**上一个已发布版本**，可能不认新特性。

```bash
# 监控 release workflow
gh run list --workflow release.yml --limit 1

# 查看详细步骤状态
gh run view <run-id>

# 如果失败，下载日志分析
gh api repos/mcpp-community/mcpp/actions/runs/<run-id>/logs \
  -H "Accept: application/vnd.github+json" > /tmp/release-logs.zip
unzip -p /tmp/release-logs.zip "build + upload (linux _ x86_64)/8_Smoke-test the bundled tarball.txt"
```

### 5. 验证 Release 产物

```bash
gh release view "v$NEW_VERSION"
```

确认以下产物全部存在：
- `mcpp-X.Y.Z-linux-x86_64.tar.gz` + `.sha256`
- `mcpp-X.Y.Z-linux-aarch64.tar.gz` + `.sha256`
- `mcpp-X.Y.Z-macosx-arm64.tar.gz` + `.sha256`
- `mcpp-X.Y.Z-windows-x86_64.zip` + `.sha256`
- 上述四个的**无版本号别名**（`mcpp-linux-x86_64.tar.gz` 等）+ `.sha256`
- `mcpp-X.Y.Z.tar.gz`（源码包）
- `mcpp.lua`（xpkg 描述）
- `install.sh`
- `SHA256SUMS`

同时比较 Linux 资产与最近一次成功 release 的体积。若出现明显回升，先确认
strip 和打包步骤的断言仍然执行，再继续发布。

## Release CI 详解

### Smoke Test 检查项

每个平台的 smoke test 验证：

1. 二进制可执行 (`test -x`)
2. Linux: 静态链接 (`file ... | grep 'statically linked'`)
3. `mcpp --version` 输出包含版本号
4. `mcpp --help` 正常输出
5. Linux: `mcpp self env` 中 MCPP_HOME 正确解析
6. xlings 二进制已捆绑

### 载荷瘦身

每个 linux 平台在**打包后、打 tar 前**调用 `.github/tools/slim_linux_payload.sh`，
strip `bin/mcpp` 与 `registry/bin/xlings` 并**断言结果**（`file` 不得再含
`not stripped`）。

为什么必须断言：单独执行一次 `strip` 不足以证明最终 tarball 已变小，后续的
`mcpp pack` 可能重建并覆盖二进制。检查最终 payload 的 `file` 输出和资产体积，而非
依赖固定的 MB 数或历史发布大小。

macOS / Windows **故意不做**：strip Mach-O 会让 ad-hoc 签名失效；按各平台的
release 规则验证最终资产，不要套用 Linux 的 strip 判断。

### publish-ecosystem：镜像 + 索引（发布的后半程）

四个构建 job 全绿后自动执行，做两件事：

1. **镜像到 `xlings-res/mcpp` 双端**（GitHub + GitCode），由
   `.github/tools/mirror_res.sh` 完成 —— 单 leg 内资产**并发上传**
   （`MIRROR_MAX_PARALLEL`，默认 8），预算是**整条 leg 的 deadline**
   （`MIRROR_LEG_DEADLINE_GH` 600s / `_GTC` 2400s），不是 per-asset cap。
2. **向 `openxlings/xim-pkgindex` 开 bump PR**（带每平台 sha256）。

**为什么不是 per-asset cap**：实测（探针 PR #301）GitHub US runner 上传到
`file.gitcode.com`（单 IP 华为云北京）只有 **0.012 MB/s** —— 而同一台 runner
从同一个 IP **下载**有 3.87 MB/s、传 GitHub 有 16 MB/s、大陆本机传它有
1.84 MB/s。被限的是**国际入境方向**，且速率有 ~4.6× 抖动，所以任何固定
per-asset 值都不可能既安全又有用。限速是 **per-connection** 的（1/4/8 并发
= 76/80/93s 墙钟），所以并发能叠加；但预签名是 OBS **单次 PUT** 签名，无
multipart/无断点续传，**单文件拆不开** —— 这正是必须先把载荷 strip 小的原因。

**`gtc` 的退出码两个方向都会撒谎**：PUT 头里的 `x-obs-callback` 让 OBS 存完对象
再回调 GitCode API，回调失败就返回 `code:400 ... EOF`，而**对象其实已落盘**。
判定上传成功**只能靠回探下载 URL**，脚本就是这么做的。

### 发布后的收尾（必须做完，否则用户装不到）

```bash
# 1) 索引 PR：CI 绿后合入，合入即自动发布索引 artifact
gh pr merge <n> --repo openxlings/xim-pkgindex --squash --admin

# 2) 真实验证（注意：不带 @版本 不会升级已装的旧版）
xlings update && xlings install mcpp@$NEW_VERSION -y

# 3) bootstrap pin 收尾 —— 仅 .xlings.json；新版此时已发布、已镜像、已进索引
sed -i "s/\"mcpp\": \"[^\"]*\"/\"mcpp\": \"$NEW_VERSION\"/" .xlings.json
# 不编辑 ci-fresh-install.yml 的 MCPP_PIN：它由 wait-index 运行时推导。
bash .github/tools/check_version_pins.sh   # 复核 pin 关系
git commit -am "ci: workspace mcpp bootstrap pin -> $NEW_VERSION (released, mirrored, indexed)"
```

**顺序不能反**：pin 一旦领先于"索引里真实存在的版本"，每个 CI job 的 bootstrap 都会
`package 'mcpp@X.Y.Z' not found`。这就是 bump PR 里不许碰这两处的原因。

**索引传播有滞后**：索引 artifact 发布后，`latest` tag 上的指针文件在 GitHub
资产 CDN 上可能还要几分钟才更新。紧接着跑的 CI 可能仍拿到旧索引并报
`package 'mcpp@X.Y.Z' not found` —— 这不是 release 坏了，等指针稳定后重跑即可。

### 下游分发渠道（都是自动的，只需核验）

两条渠道都挂在 `release` workflow 的 `workflow_run: completed` 上，不需要人工推：

| 渠道 | workflow | 核验 |
|------|----------|------|
| AUR (`mcpp-bin` / `mcpp-m`) | `.github/workflows/aur-publish.yml` | `gh run list --workflow aur-publish.yml --limit 1` |
| Homebrew tap (`mcpp-m`) | `.github/workflows/homebrew-publish.yml` → ping [`mcpp-community/homebrew-mcpp`](https://github.com/mcpp-community/homebrew-mcpp) | `gh api repos/mcpp-community/homebrew-mcpp/contents/Formula/mcpp-m.rb --jq .content \| base64 -d \| grep '^  version'` |

Homebrew 那条**不写公式**，只发一个 `repository_dispatch`；公式重写由 tap 仓库自己的
`bump-formula.yml` 完成（它读 release 的 `.sha256` 边车）。这条 ping 依赖仓库 secret
`HOMEBREW_TAP_TOKEN`；**没配也不会让发布失败** —— tap 有每日 schedule 兜底，24h 内自己跟上。
想立刻跟上就手动触发一次：

```bash
gh workflow run bump-formula.yml -R mcpp-community/homebrew-mcpp
```

### 常见失败原因

| 症状 | 原因 | 修复 |
|------|------|------|
| `mcpp X.Y.Z-1` 但 tag 是 `vX.Y.Z` | `fingerprint.cppm` 版本未更新 | 更新 `MCPP_VERSION`，重新打 tag |
| bump PR 里**所有** CI job 都红在 bootstrap，报 `package 'mcpp@X.Y.Z' not found` | 把 `.xlings.json` bootstrap pin 一起 bump 了，CI 去装一个还没发布的版本 | 把 `.xlings.json` 回退到上一个已发布版本；不要修改运行时推导的 `MCPP_PIN` |
| 自查 `--version` 显示旧版本，但源码已改 | `target/<triple>/<指纹>/` 的指纹随版本变，`ls \| head -1` 取到了上一次构建的目录 | 用 `ls -dt … \| head -1` 取最新构建 |
| Smoke test 输出旧版本 | CI 缓存了旧的 sandbox/target | 删除 GitHub Actions cache 后重跑 |
| e2e `01_help_and_version.sh` 挂 | 只改了 `mcpp.toml` 没改 `fingerprint.cppm`（它把两者交叉比对） | 同步两处正在构建的版本；注意这个 e2e 只在部分分片里跑，可能表现为"只有某个平台红" |
| xlings bootstrap 失败 | xlings 版本不兼容 | 改 `src/xlings.cppm::kXlingsVersion`（唯一真源），再核对引用它的 workflow 与脚本；当前 pin-check 脚本修复前不能依赖它完成扫描 |
| macOS/Windows 构建失败 | 需要等 Linux job 先完成 | 检查 Linux job 是否成功 |
| `slim: FAIL: ... still not stripped` | strip 工具没生效／被 pack 覆盖 | 别绕过断言——它就是为了拦住 34.8MB 的 tarball 再次发出去 |
| mirror leg 报 `missing/unverified` | 资产没传上去或还没传播 | 先 GET 核验（**必须 GET，`curl -I` 会骗你**），gitcode 用 `gitcode.com` 直链而非 `api.` 主机；确认缺件后本地补传再 `gh run rerun --failed`（脚本幂等，已验证的资产会跳过） |

### 缓存管理

Release CI 使用多层缓存加速。如果怀疑缓存问题：

```bash
# 列出缓存
gh cache list | grep release

# 删除特定缓存
gh cache delete <cache-id>

# 重跑 release（不改 tag）
gh workflow run release.yml --ref "v$NEW_VERSION"
```

## 紧急修复发版

如果发版后发现问题需要紧急修复：

```bash
# 1. 在 main 上修复
git checkout main && git pull
# ... 修改代码 ...
git commit -m "fix: 描述"
git push origin main

# 2. 更新 tag 指向新 commit（包含修复）
git tag -d "v$NEW_VERSION"
git tag "v$NEW_VERSION"
git push origin "v$NEW_VERSION" --force

# 3. 删除旧 release（如果已创建）
gh release delete "v$NEW_VERSION" --yes

# 4. 重跑 release
gh workflow run release.yml --ref "v$NEW_VERSION"
```

**注意**：如果修复涉及版本号变化，应该 bump 到新的 patch 版本而不是覆盖旧 tag。

## 文件清单

| 文件 | 版本相关内容 |
|------|-------------|
| `mcpp.toml` | `version = "X.Y.Z"` — 项目版本，release.yml 由它推导 tag |
| `src/toolchain/fingerprint.cppm` | `MCPP_VERSION = "X.Y.Z"` — 编译期版本常量 |
| `.xlings.json` | `workspace.mcpp` — CI bootstrap 装哪个 mcpp（发布**后**才 bump） |
| `.github/workflows/ci-fresh-install.yml` | `MCPP_PIN` — 由 `wait-index` 从最新 release 推导，**从不手工 bump** |
| `src/xlings.cppm` | `kXlingsVersion` — xlings pin 的**唯一真源** |
| `.github/tools/check_version_pins.sh` | 版本/pin 校验 guard（**用 `bash` 跑，不能用 `sh`**） |
| `.github/tools/slim_linux_payload.sh` | linux 载荷 strip + 断言 |
| `.github/tools/mirror_res.sh` | 双端镜像（并发上传 + leg deadline + 完整性 gate） |
| `.github/tools/gtc` | GitCode CLI（release create/upload、PR） |
| `.github/workflows/release.yml` | Release workflow 定义（四平台 + publish-ecosystem） |
| `install.sh` | 安装脚本（随 release 发布） |
| `CHANGELOG.md` | Release notes 来源（按 `## [X.Y.Z]` 提取） |

> **注意版本 bump 的两个阶段**：`mcpp.toml` + `fingerprint.cppm` 在发版**前**改
> （它们定义要发什么）；`.xlings.json` 只在发版成功、镜像并进索引后才可更新
> （它指定 bootstrap 使用的已发布版本）。`MCPP_PIN` 是被测版本的运行时推导值，
> 不属于任何手工 bump 阶段。这些关系由 `bash .github/tools/check_version_pins.sh` 校验。
