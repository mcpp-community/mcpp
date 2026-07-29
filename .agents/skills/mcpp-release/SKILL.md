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

mcpp 的版本号存在于 **四个位置**，但它们分属**两组，在发布流程的两个不同时刻更新**。
把四处一起 bump 是一个会让全部 CI 变红的经典错误 —— 见下面第二组的解释。

**第一组：正在构建的版本**（发布时改，走 bump PR）

1. `mcpp.toml` → `[package].version` — 构建系统读取的项目版本，release.yml 由它推导 tag
2. `src/toolchain/fingerprint.cppm` → `MCPP_VERSION` — 编译期硬编码常量（`--version` 输出、BMI 指纹、E0006 索引底线比较）

这两处必须**在同一个 commit 里**一起改：`tests/e2e/01_help_and_version.sh` 交叉比对
`mcpp.toml` 与 `mcpp --version`，只改一处 CI 立刻红。

**第二组：bootstrap pin —— CI 用哪个 mcpp 来自举**（发布并进索引之后才改）

3. `.xlings.json` → `workspace.mcpp` — CI bootstrap 装哪个 mcpp
4. `.github/workflows/ci-fresh-install.yml` → `MCPP_PIN` — 全新安装验证的目标版本

这两处指向的是一个**已经发布、且已经进了索引**的版本。在 bump PR 里把它们一起挪到新版，
等于让每一个 CI job 去 `xlings install` 一个还不存在的 mcpp —— 全线红。
所以它们在 bump PR 里保持**上一个已发布版本**不动，直到发布收尾那一步才前移
（见「发布后的收尾」第 3 步）。`check_version_pins.sh` 正是按这个语义校验的：它只要求
两处 pin **彼此相等**、且**不得新于**正在构建的版本，并不要求等于它。

对照最近一次发布：`fd27314`（bump 到 2026.7.29.1）只动了第一组两个文件，第二组仍停在
2026.7.28.2；`fde3b70` 才在发布、镜像、进索引之后把 pin 推到 2026.7.29.1。

**版本不一致会导致 release smoke test 失败**（CI 检查 `mcpp --version` 是否匹配 tag）。
第二组历史上多次漂移（`MCPP_PIN` 曾落后五个版本），所以现在有机器校验：

```bash
bash .github/tools/check_version_pins.sh
```

它同时校验第二组不变量：**`.github/` 下所有 xlings pin 必须等于 `src/xlings.cppm` 的
`pinned::kXlingsVersion`**（当前 16 个 pin 点、7 个文件，含 release.yml 里三处硬编码的
aarch64 tarball 字面量）。`kXlingsVersion` 是唯一真源，也是 release 打进
`<install>/registry/bin/xlings` 的那一份。改 xlings 版本只改常量，然后跑这个脚本找出其余落点。

## 发布步骤

### 1. 确认 main 分支状态

```bash
git checkout main && git pull origin main
# 确认 CI 全部通过
gh run list --branch main --limit 3
```

所有 CI（ci / ci-macos / ci-windows）必须为 `success`。不要在 CI 红的时候发版。

### 2. bump 版本号（第一组两处，单个 commit，走 PR）

**只改第一组的两个文件**，并且在同一个 commit 里。bootstrap pin（`.xlings.json`、
`MCPP_PIN`）**不要动** —— 它们指向上一个已发布版本，见 Overview。

```bash
# 日期版本：当天序号从 .1 起；.0 仅用于正式/稳定版
NEW_VERSION="2026.7.27.1"

git checkout -b "chore/bump-$NEW_VERSION"

sed -i "s/^version.*=.*/version     = \"$NEW_VERSION\"/" mcpp.toml
sed -i "s/MCPP_VERSION = \".*\"/MCPP_VERSION = \"$NEW_VERSION\"/" src/toolchain/fingerprint.cppm

# 机器校验（building 是新版、bootstrap pin 仍是旧版，是预期状态）
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

**顺带核对体积**（2026.7.29.1 起，见下方"载荷瘦身"）：linux 两个 tarball 应在
**5MB 上下**。如果又回到 30MB 量级，说明 strip 断言被绕过了，先查再发。

## Release CI 详解

### Smoke Test 检查项

每个平台的 smoke test 验证：

1. 二进制可执行 (`test -x`)
2. Linux: 静态链接 (`file ... | grep 'statically linked'`)
3. `mcpp --version` 输出包含版本号
4. `mcpp --help` 正常输出
5. Linux: `mcpp self env` 中 MCPP_HOME 正确解析
6. xlings 二进制已捆绑

### 载荷瘦身（2026.7.29.1 起）

每个 linux 平台在**打包后、打 tar 前**调用 `.github/tools/slim_linux_payload.sh`，
strip `bin/mcpp` 与 `registry/bin/xlings` 并**断言结果**（`file` 不得再含
`not stripped`）。

为什么必须断言：在此之前，vendored 的 xlings 从来没被 strip 过（97.3MB，带
`debug_info`），而 x86_64 那句 `strip` 跑在 `mcpp pack` **之前** —— pack 会重建
二进制把它覆盖掉，于是直到 2026.7.28.2 发布的 `bin/mcpp` 一直是未 strip 的。
一个不校验效果的 `strip` 等于注释。修完 linux-x86_64 tarball 从 **34.81MB 降到
4.62MB（7.5×）**。

macOS / Windows **故意不做**：载荷本来就 6.1MB / 4.2MB，且 strip Mach-O 会让
ad-hoc 签名失效。

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

# 3) bootstrap pin 收尾 —— 第二组两处，到这一步才前移
#    新版此时已发布、已镜像、已进索引，CI 装得到，pin 才可以指向它
sed -i "s/\"mcpp\": \"[^\"]*\"/\"mcpp\": \"$NEW_VERSION\"/" .xlings.json
sed -i "s/MCPP_PIN: '[^']*'/MCPP_PIN: '$NEW_VERSION'/" .github/workflows/ci-fresh-install.yml
bash .github/tools/check_version_pins.sh
git commit -am "ci: workspace mcpp bootstrap pin -> $NEW_VERSION (released, mirrored, indexed)"
```

**顺序不能反**：pin 一旦领先于"索引里真实存在的版本"，每个 CI job 的 bootstrap 都会
`package 'mcpp@X.Y.Z' not found`。这就是 bump PR 里不许碰这两处的原因。

**索引传播有滞后**：索引 artifact 发布后，`latest` tag 上的指针文件在 GitHub
资产 CDN 上可能还要几分钟才更新。紧接着跑的 CI 可能仍拿到旧索引并报
`package 'mcpp@X.Y.Z' not found` —— 这不是 release 坏了，等指针稳定后重跑即可。

### 常见失败原因

| 症状 | 原因 | 修复 |
|------|------|------|
| `mcpp X.Y.Z-1` 但 tag 是 `vX.Y.Z` | `fingerprint.cppm` 版本未更新 | 更新 `MCPP_VERSION`，重新打 tag |
| bump PR 里**所有** CI job 都红在 bootstrap，报 `package 'mcpp@X.Y.Z' not found` | 把第二组的 bootstrap pin 也一起 bump 了，CI 去装一个还没发布的版本 | 把 `.xlings.json` / `MCPP_PIN` 回退到上一个已发布版本，发布收尾时再前移 |
| 自查 `--version` 显示旧版本，但源码已改 | `target/<triple>/<指纹>/` 的指纹随版本变，`ls \| head -1` 取到了上一次构建的目录 | 用 `ls -dt … \| head -1` 取最新构建 |
| Smoke test 输出旧版本 | CI 缓存了旧的 sandbox/target | 删除 GitHub Actions cache 后重跑 |
| e2e `01_help_and_version.sh` 挂 | 只改了 `mcpp.toml` 没改 `fingerprint.cppm`（它把两者交叉比对） | 同步四处版本；注意这个 e2e 只在部分分片里跑，可能表现为"只有某个平台红" |
| xlings bootstrap 失败 | xlings 版本不兼容 | 改 `src/xlings.cppm::kXlingsVersion`（**唯一真源**）后跑 `check_version_pins.sh` 找出其余 15 个 pin 点 |
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
| `.github/workflows/ci-fresh-install.yml` | `MCPP_PIN` — 全新安装验证目标（发布**后**才 bump） |
| `src/xlings.cppm` | `kXlingsVersion` — xlings pin 的**唯一真源**（其余 15 处由脚本校验） |
| `.github/tools/check_version_pins.sh` | 机器校验上述两组不变量，别靠肉眼 |
| `.github/tools/slim_linux_payload.sh` | linux 载荷 strip + 断言 |
| `.github/tools/mirror_res.sh` | 双端镜像（并发上传 + leg deadline + 完整性 gate） |
| `.github/tools/gtc` | GitCode CLI（release create/upload、PR） |
| `.github/workflows/release.yml` | Release workflow 定义（四平台 + publish-ecosystem） |
| `install.sh` | 安装脚本（随 release 发布） |
| `CHANGELOG.md` | Release notes 来源（按 `## [X.Y.Z]` 提取） |

> **注意版本 bump 的两个阶段**：`mcpp.toml` + `fingerprint.cppm` 在发版**前**改
> （它们定义要发什么）；`.xlings.json` + `MCPP_PIN` 在发版**成功后**改（它们指向
> bootstrap 用哪个已发布版本）。`check_version_pins.sh` 认得这个差异，不会因为
> bootstrap pin 落后一版就报错。
