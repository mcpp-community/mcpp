---
name: mcpp-release
description: Use when releasing a new version of mcpp — bumps version, creates tag, triggers release CI, and monitors until all platforms succeed. Covers the full release checklist to avoid common pitfalls like version string mismatches.
---

# mcpp 版本发布流程

## Overview

mcpp 的版本号存在于 **两个位置**，发布时必须同步更新：

1. `mcpp.toml` → `[package].version` — 构建系统读取的项目版本
2. `src/toolchain/fingerprint.cppm` → `MCPP_VERSION` — 编译期硬编码的版本常量（用于 `--version` 输出和 BMI 缓存指纹）

**两者不一致会导致 release smoke test 失败**（CI 检查 `mcpp --version` 输出是否匹配 tag 版本）。

## 发布步骤

### 1. 确认 main 分支状态

```bash
git checkout main && git pull origin main
# 确认 CI 全部通过
gh run list --branch main --limit 3
```

所有 CI（ci / ci-macos / ci-windows）必须为 `success`。不要在 CI 红的时候发版。

### 2. 同步更新两处版本号（单个 commit）

**关键：在同一个 commit 中更新两个文件，避免版本不一致。**

```bash
# 确定新版本号
NEW_VERSION="X.Y.Z"

# 更新 mcpp.toml
sed -i "s/^version.*=.*/version     = \"$NEW_VERSION\"/" mcpp.toml

# 更新 fingerprint.cppm 中的硬编码版本
sed -i "s/MCPP_VERSION = \".*\"/MCPP_VERSION = \"$NEW_VERSION\"/" src/toolchain/fingerprint.cppm

# 验证两处一致
grep 'version' mcpp.toml | head -1
grep 'MCPP_VERSION' src/toolchain/fingerprint.cppm

# 单个 commit 提交
git add mcpp.toml src/toolchain/fingerprint.cppm
git commit -m "chore: bump version to $NEW_VERSION"
git push origin main
```

### 3. 创建并推送 tag

```bash
git tag "v$NEW_VERSION"
git push origin "v$NEW_VERSION"
```

Tag push 会自动触发 `release.yml` workflow。

### 4. 监控 Release CI

Release workflow 包含三个平台的构建：

| Job | 平台 | 产物 | 依赖 |
|-----|------|------|------|
| `build-release` | Linux x86_64 | `mcpp-X.Y.Z-linux-x86_64.tar.gz` | 无（先执行） |
| `build-macos` | macOS ARM64 | `mcpp-X.Y.Z-macosx-arm64.tar.gz` | 等 Linux 完成 |
| `build-windows` | Windows x86_64 | `mcpp-X.Y.Z-windows-x86_64.zip` | 等 Linux 完成 |

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
- `mcpp-X.Y.Z-macosx-arm64.tar.gz` + `.sha256`
- `mcpp-X.Y.Z-windows-x86_64.zip` + `.sha256`
- `mcpp-X.Y.Z.tar.gz`（源码包）
- `mcpp.lua`（xpkg 描述）
- `install.sh`
- `SHA256SUMS`

## Release CI 详解

### Smoke Test 检查项

每个平台的 smoke test 验证：

1. 二进制可执行 (`test -x`)
2. Linux: 静态链接 (`file ... | grep 'statically linked'`)
3. `mcpp --version` 输出包含版本号
4. `mcpp --help` 正常输出
5. Linux: `mcpp self env` 中 MCPP_HOME 正确解析
6. xlings 二进制已捆绑

### 常见失败原因

| 症状 | 原因 | 修复 |
|------|------|------|
| `mcpp X.Y.Z-1` 但 tag 是 `vX.Y.Z` | `fingerprint.cppm` 版本未更新 | 更新 `MCPP_VERSION`，重新打 tag |
| Smoke test 输出旧版本 | CI 缓存了旧的 sandbox/target | 删除 GitHub Actions cache 后重跑 |
| xlings bootstrap 失败 | xlings 版本不兼容 | 更新 `XLINGS_VERSION` |
| macOS/Windows 构建失败 | 需要等 Linux job 先完成 | 检查 Linux job 是否成功 |

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
| `mcpp.toml` | `version = "X.Y.Z"` — 项目版本 |
| `src/toolchain/fingerprint.cppm` | `MCPP_VERSION = "X.Y.Z"` — 编译期版本常量 |
| `.github/workflows/release.yml` | Release workflow 定义 |
| `install.sh` | 安装脚本（随 release 发布） |
| `CHANGELOG.md` | Release notes 来源（按 `## [X.Y.Z]` 提取） |
