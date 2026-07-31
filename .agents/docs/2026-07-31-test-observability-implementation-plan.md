# `mcpp test` 可观测性与有界性 —— 实施计划

配套分析:[`2026-07-31-test-workspace-observability-analysis.md`](2026-07-31-test-workspace-observability-analysis.md)

目标:把 `mcpp test` / `mcpp test --workspace` 从「不报时间、不设期限、还不 flush」变成一个**输出实时、耗时可信、墙钟有界、失败可归因**的操作。

三者互相耦合(分析 §6.3),因此**一个 PR 一起做**。

---

## S1 — stdout 实时性(根因:libc 缓冲区差 64 倍)

**问题**:`ui::status/info/finished/plain` 与 `execute.cppm` 的测试结果行全是裸 `std::println(...)`,无一处 `fflush`;全仓库无 `setvbuf`。非 TTY 下缓冲区大小由 libc 决定 —— musl 固定 1 KB、Apple libc 取管道 `st_blksize`(64 KB)、MSVCRT 4 KB 且**不支持行缓冲**。同一份输出在三平台的可见时机差 64 倍,进程被 kill 时整段丢失。

**做法(两条腿,因为 setvbuf 在 Windows 上不成立)**:

1. `mcpp::ui::set_line_buffered()`(POSIX 上 `setvbuf(_IOLBF)`),由 `main()` 调用 —— 兜住所有不走 ui 的直接输出。**不能放 `main.cpp` 里**:非模块 TU 不允许开 global module fragment 引 `<cstdio>`(Clang 直接拒绝,GCC 静默接受)。**Windows 上不调用**:UCRT 的 `size` 合法范围是 `2..INT_MAX`,0 会走 invalid-parameter handler 直接 abort(0xC0000409 → git-bash 报 exit 127),而 MSVCRT 本来就把 `_IOLBF` 当 `_IOFBF`,靠 `ui::flush()` 即可。
2. `mcpp::ui` 新增 `flush()`,并在 **每个写 stdout 的 ui 函数**末尾调用(`status` / `info` / `finished` / `plain` / `diagnostic` 的 stdout 分支)。这条在 Windows 上也确定有效,不依赖 `_IOLBF` 语义。
3. `execute.cppm` 中 `mcpp test` 的裸 `std::println` 结果行(`... ok` / `FAIL (...)`)改走 `ui::plain`,从而继承 flush。

**验收**:e2e 断言 —— `mcpp test --workspace` 的 stdout 重定向到管道时,**逐行到达**(在扇出中途读到第一个成员的 `test result`,而不是等进程退出)。

---

## S2 — `--timeout` 有界化

**问题**:`TestOptions::timeoutSecs = 0`(不限)是默认值,`mcpp test` 因此不是有界操作。

**做法**:默认改 **300 秒**;`--timeout 0` 显式表示不限。help / `docs/` 同步。

**验收**:单测 + e2e(一个 `sleep` 测试在 `--timeout 1` 下报 `FAIL (timeout after 1s)` 且扇出继续)。

---

## S3 — `--build-timeout`(唯一能兜住链接卡死的闸)

**问题**:`--timeout` 只包住测试进程的**运行**。三处 `backend->build()`(`execute.cppm:1056` Phase A / `:1103` bulk / `:1126` per-test)全无期限。macOS 上 `modules/jsc` 卡在 14 次可执行链接上,`--timeout` 设多少都无效。

**做法**:
- `BuildOptions` 加 `buildTimeoutSecs`(0 = 不限,**默认 0 —— 见下**)。
- `ninja_backend` 把 `capture_exec` 换成 `capture_exec_deadline`,超时返回 `BuildError{"build timed out after Ns", ...}`。
- `run_tests` 三处 build 各自独立计时;超时报成**该成员的构建失败**,扇出继续下一个成员。
- **平台限制如实写明**:`capture_exec_deadline` 在 Windows 上忽略 deadline(`process.cppm:96-99`),因此 `--build-timeout` 目前是 POSIX-only。文档标注,不假装跨平台。

**默认为什么是关的**(与 `--timeout` 不对称,实测而非风格):单个测试跑过 5 分钟不寻常,冷依赖构建跑过 15 分钟很平常 —— mcpp-index 有成员要从源码建 OpenCV,linux 1019s / windows 1289s。默认上限会把「慢但正确」的构建判红。构建能跑多久是工程的性质,由工程来说。

**验收**:e2e —— 一个故意慢的编译边在 `--build-timeout 1` 下被判超时且信息里带成员名。

---

## S4 — 耗时数字可信

**问题 A**:`auto t0` 在 `execute.cppm:1107`,即 Phase A + bulk build **之后**才起表,`finished in` 只覆盖 per-test 循环。实测 `modules/jsc` 打印 `6.53s`、真实 `93.5s`,**误差 14.3×**。

**问题 B**:per-test 耗时已在 `TestResult::durationMs` 里,但**只发给 JSON**,human 模式的 `t1 ... ok` 不带时间。

**做法**:
- `t0` 移到 Phase A 之前;分别累计 `buildMs`(Phase A + bulk + per-test build)与 `runMs`。
- 汇总行:`test result ok. 14 passed; 0 failed; finished in 93.5s (build 87.0s + run 6.5s)`。
- per-test human 行带时间:`t1 ... ok (0.31s)` / `t1 ... FAIL (exit 1, 2.40s)`。

---

## S5 — 扇出层可观测性

**问题**:97 个成员串行,过程中唯一线索是 `Workspace testing member 'X'`。无 `M/N`、无 per-member 耗时、无累计耗时、无 workspace 级汇总。

**做法**:
- `run_tests` 返回结构化结果(新增 `TestRunSummary{passed, failed, elapsed, buildMs, runMs}`),而不是只回 `int`。
- 扇出打:
  ```
     Workspace member 'modules/jsc' (23/97) ok — 14 passed in 93.5s
   workspace result ok. 97 members; 412 passed; 0 failed; finished in 355.2s
     slowest: modules/jsc 93.5s, modules/install 32.2s, modules/http_types 24.1s
  ```
- 失败时同样带耗时与 `M/N`。

---

## S6 — `--workspace-timeout`

整条扇出的累计墙钟上限(默认 0 = 不限,由 CI 的 `timeout-minutes` 兜底)。超时后**停止扇出**,如实汇总已完成成员、列出未跑成员,退出码非零 —— 而不是被外部 SIGKILL 掉、连汇总都拿不到。

---

## S7 — JSON 输出契约(分析 §4)

1. **表头不再污染 stdout**:`--message-format json` 时,扇出层在调用 `run_tests` **之前**就 `set_quiet(true)`(现在是第一个成员漏出去、第二个起被静音)。
2. **测试名带成员**:每条 test 记录加 `"member"` 字段。
3. **workspace 级 summary**:`{"workspace_summary":{"members":N,"passed":P,"failed":F,"elapsed_ms":E}}`。

---

## S8 — macOS `runtime_library_path_key() == ""` 的静默差异

`platform/env.cppm:163` 在 macOS 返回空串(理由正确:`DYLD_LIBRARY_PATH` 会波及 ninja 启动的每个可执行文件)。后果是依赖 `[runtime] library_dirs` 的测试在 Linux/Windows 过、macOS 以 dyld 错误失败,且**零诊断**。

**做法**:`run_tests` 在 macOS 上若 `plan.runtimeLibraryDirs` 非空而注入键为空,发一条 `diag::warning`,点名这条平台差异与 rpath 兜底。

---

## 落地顺序

S1 → S2 → S4 → S5 → S7 → S3 → S6 → S8,每步自带测试。

版本:`2026.8.1.1`(`2026.7.31.1` 已发布)。真源 `src/toolchain/fingerprint.cppm::MCPP_VERSION` + `mcpp.toml`,由 `.github/tools/check_version_pins.sh` 机器校验。
