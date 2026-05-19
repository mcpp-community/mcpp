# Windows E2E 与 macOS 对齐方案

> 目标：Windows E2E 从 20/49 提升到 ~32/49，与 macOS 33/49 对齐。

## 根因分析

| 类别 | 测试数 | 问题 | 修复方式 |
|------|--------|------|----------|
| mcpp run/test 单引号 | 1 (02) | `cli.cppm` 用 POSIX 单引号执行 binary | `_WIN32` 改双引号 |
| clang-scan-deps 查找 | 1 (16) | `cli.cppm` 硬编码无 .exe | 调用已有 `find_scan_deps()` |
| symlink 依赖 | 4 (10,24,27,32) | `_inherit_toolchain.sh` 用 `ln -sf` | 加 `cp -r` fallback |
| bash-specific 语法 | 1 (19) | `compgen -G` 不在 Git Bash | 改用 `find` |
| unix-shell 误标 | 1 (38_mirror) | 实际只需 symlink fallback | 改标签 |
| import-std-libcxx 硬编码路径 | 4 (37,38,40,41) | 测试用 Linux 路径 | 加 Windows 路径 |

## 修复计划

### Fix 1: cli.cppm 单引号 → 双引号 (解锁 02)
- `src/cli.cppm:2611` — `mcpp run` 执行 binary 用 `'{}'` → Windows 改 `"{}"`
- `src/cli.cppm:2522` — fast-path ninja 同上
- `src/cli.cppm:3159` — test PATH prefix 是 POSIX 语法，Windows 跳过

### Fix 2: clang-scan-deps 查找 (解锁 16)
- `src/cli.cppm:2162-2167` — 直接查找 `clang-scan-deps`，不走 `find_scan_deps()`
- 改为调用 `mcpp::toolchain::clang::find_scan_deps(*tc)` 已正确处理 .exe

### Fix 3: _inherit_toolchain.sh cp fallback (解锁 10,24,27,32)
- 当 `ln -sf` 失败时用 `cp -r` 替代
- 自动检测 symlink 可用性

### Fix 4: 19_bmi_cache_reuse.sh bash 兼容 (解锁 19)
- `compgen -G` → `find ... | grep -q .`

### Fix 5: LLVM 测试 Windows 路径 (解锁 37,38,40,41)
- 参照 36_llvm_toolchain.sh 的模式加 Windows 路径和 .exe 处理

### Fix 6: 标签修正
- `38_self_config_mirror.sh` 改标签
- `run_all.sh` 移除已修复测试的标签限制

## 预期结果

修复后：**~32 passed, 0 failed, ~17 skipped** (与 macOS 33 passed 对齐)
