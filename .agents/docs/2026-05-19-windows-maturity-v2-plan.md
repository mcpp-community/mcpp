# Windows 成熟度提升 V2 方案

> 基于 GPT-5.5 评审反馈，将 Windows 从 22/48 提升到 30+/48。

## 任务清单

### T1: 修复 fresh-sandbox 能力 + git clone 单引号
**目标：解锁 02, 24, 27_self, 32（+4 tests）**

- `cli.cppm:1963-1970` — git clone 用 `'{}'` 单引号，Windows cmd.exe 不支持
- `run_all.sh` — Windows 添加 `fresh-sandbox` 能力
- `27_self_contained_home.sh` — 误标 `elf`，实际逻辑是 Windows 可移植的

### T2: process.cppm 实际接入
**目标：消除散落的 popen/system 拼接**

优先替换 5 个高风险 call site：
- `probe.cppm:90` — compiler probing
- `pm/publisher.cppm:211,239` — sha256sum, git archive
- `toolchain/stdmod.cppm:64` — std module compilation
- `build/ninja_backend.cppm:98` — ninja invocation

### T3: provider.cppm 接入 flags/ninja
**目标：消除散落的 is_clang/is_gcc/isMusl 检查**

- `flags.cppm` — 用 `capabilities_for(tc)` 决定 compile/link flags
- `ninja_backend.cppm:155` — scanner 策略用 provider
- `stdmod.cppm:102,122` — BMI 路径用 provider

### T4: 更新过时文档
**目标：文档与代码同步**

- 更新 `.agents/docs/2026-05-19-windows-platform-maturity-plan.md`
- 48 tests（非 49），P1/P2/P3/P5 已完成基础设施

### T5: E2E 标签修正
**目标：最大化 Windows 可运行测试**

- `27_self_contained_home.sh` — `elf` → 空（逻辑是 Windows 可移植的）
- `10_env_command.sh` — 验证是否仍需 symlink
