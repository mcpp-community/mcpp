# mcpp test 演进批次二 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落实 `2026-07-24-mcpp-test-design-review.md` 路线图 P1/P2/P3/P4/P6（P5 按该文档决策显式排除：出现第二个需求方前不立项），版本 0.0.103 → 0.0.104，单 PR 合入 main。

**Architecture:** 全部改动收敛在 `src/build/execute.cppm`（run_tests）、`src/build/backend.cppm`/`ninja_backend.cppm`（keepGoing）、`src/platform/process.cppm`（信号退出码规范化 + 带超时运行）、`src/cli*`（--list/--timeout 参数）与 docs。每项 TDD：先写 e2e（124–127）确认 RED 再实现。

**Tech Stack:** 既有栈；e2e bash 脚本。

## Global Constraints

- 语义回归零容忍：e2e 118–123 全程保持绿。
- JSON schema 只增不改（新增 `duration_ms`、`timed_out` 字段；`signal` 语义修正属 bug fix）。
- 每任务一个提交；最后版本提交（mcpp.toml 0.0.104 + CHANGELOG 定版 2026-07-24）。
- PR：单个，源分支 `feat/test-isolation-json` → main；CI 全绿后 `gh pr merge --squash --admin`（用户已明确授权 bypass squash）。
- 生态验证：合入后以 main 构建 musl 二进制,复跑 mcpp e2e 全量 + d2mcpp e2e all + d2x 构建/单测/checker 冒烟。

## Tasks

- [ ] **T1 (P1) Phase B 并行化**:`BuildOptions.keepGoing`(→ ninja `-k 0`);run_tests 在逐测试循环前插入一次携带全部过滤后 goal 的 bulk build(结果忽略,只为并行填充缓存);既有逐测试 build 保留(成功者缓存命中≈无操作,失败者快速重试取干净诊断)。验收:118–123 绿;d2mcpp cpp11 全量答案态构建墙钟显著下降(手测记录)。
- [ ] **T2 (P3a) 信号退出码规范化**:`normalize_exit_code` 增加 `WIFSIGNALED → 128+WTERMSIG`(shell 惯例;当前返回原始 status,JSON signal 推导永不触发的 bug)。e2e 124:段错误测试 → `"exit_code":139,"signal":11`。
- [ ] **T3 (P3b) per-test `duration_ms`**:JSON 记录增加字段(编译+运行墙钟);summary 已有 elapsed 不动。并入 e2e 124 断言字段存在。
- [ ] **T4 (P2) `mcpp test --list`**:列出(过滤后)测试名;`--message-format json` 时逐行 `{"test":…,"main":…}` + `{"summary":{"total":N}}`;不触发构建。e2e 125。
- [ ] **T5 (P4) `--timeout <secs>`**:平台层新增带截止的运行(POSIX spawn+WNOHANG 轮询,超时 SIGKILL;Windows 暂不支持并文档注明);超时 → `FAIL (timeout)` / JSON `"timed_out":true` + run_fail。e2e 126。
- [ ] **T6 (P6) docs**:"(gtest style)" → framework-agnostic 措辞;记录合成测试名含 `/` 的命名豁免;README/README.zh 同步;CHANGELOG 批次二条目。
- [ ] **T7 版本与回归**:mcpp.toml → 0.0.104;CHANGELOG 未发布 → [0.0.104] — 2026-07-24;全量 e2e(run_all)glibc + 新脚本 musl 双跑。
- [ ] **T8 PR + CI + 合入**:push 分支,gh pr create(标题含 0.0.104),等 CI 全绿,`gh pr merge --squash --admin`。
- [ ] **T9 生态验证**:main 重建 musl;mcpp e2e 全量;d2mcpp `e2e.sh all` + d2x mcpp build/test + checker 冒烟,全部以 main 产物执行;结果记录回本文件。
