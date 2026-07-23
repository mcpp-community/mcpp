# 批次三:#273 沙盒围栏 + CI wine 缓存 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 #273（patchelf_walk 经符号链接逃逸沙盒、损坏真实 `~/.xlings` payload）+ cross-build CI 的 wine 安装缓存;单 PR → CI 全绿 → bypass squash 合入 → 与批次二一起作为 0.0.104 release → xlings 生态验证。

**Architecture:**（实现中依整体评估修正为非启发式设计）信任根 = `cfg.registryDir` 这一**系统一等事实**,入口 `containment_root()` canonical 化一次后作为显式参数穿透到所有改写者(`patchelf_walk`/`fixup_gcc_specs`/`fixup_clang_cfg`),绝不从 payload 路径反推(先 canonical 化 payload 会把恶意符号链接解析掉,围栏沦为恒真式)。评估中发现入口层其实**已有** ownership guard 与内容指纹 marker(#109 起),本批次修复其三处残余缺口:①guard 复用 error_code(第二次成功清掉第一次失败)、②裸字符串前缀比较无组件边界(`registry-evil` 能穿过)、③binutils 兄弟目录的 walk 完全在 guard 之外。判定谓词独立导出并单测覆盖;`13_toolchain_pin.sh` 保留 symlink 种子作为真实环境金丝雀。

## Global Constraints

- 既有 e2e(含 152–160)与单测全绿;walker 的「补丁副本+原子 rename」行为不变。
- guard 覆盖三个写入者:`patchelf_walk`、`fixup_gcc_specs`、`fixup_clang_cfg`。
- 信任根解析失败时**fail-closed**(空根 → 一切视为逃逸,拒绝改写)。
- 跳过逃逸文件时 verbose 日志留痕。
- PR body 带 `Fixes #273`;合入后 issue 自动关闭。

## Tasks

- [ ] **T1 围栏谓词 + 三写入者接入**:post_install.cppm 导出 `containment_root(dir)`(拼写路径取 registry 父 + weakly_canonical)与 `escapes_containment(file, root)`(weakly_canonical 后前缀比较);patchelf_walk 入口算根、每候选 ELF 判定;fixup_gcc_specs/fixup_clang_cfg 写文件前同判。
- [ ] **T2 单测**:tests/unit/test_post_install_containment.cpp——真实临时目录 + `std::filesystem::create_directory_symlink` 复刻 issue 逃逸拓扑,断言:直连文件不逃逸、经 symlink 目录前缀的文件逃逸、无 registry 组件 → 无围栏、相对/`..` 路径归一。
- [ ] **T3 13_toolchain_pin.sh 注释更新**:说明 symlink 种子曾是 #273 事故向量、现受围栏保护并有意保留为金丝雀;引用 issue。
- [ ] **T4 CI wine deb 缓存**(已在工作树):首跑下载 .deb 闭包入 cache,命中跑 dpkg -i,apt-get -f 兜底。
- [ ] **T5 PR + CI + 合入**:单 PR(Fixes #273),15 检查全绿后 `gh pr merge --squash --admin`。
- [ ] **T6 release 0.0.104 + 生态验证**:按 release.yml 流程出版本(批次二已定版 0.0.104,本批次并入);xlings 安装链路/d2mcpp/d2x 以 release 产物复验;结果回记。
