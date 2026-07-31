# `mcpp test --workspace`:跨平台模型一致性、输出差异与 macOS 停滞的根因分析

**分析对象**:mcpp `2026.7.31.1`(HEAD `0547324`)源码,+ Sunrisepeak/mbun CI(97 个 workspace 成员)三平台实测,+ mcpp-index CI 30 次运行横向对照,+ 本机 strace 复现。

**结论全部经实测,推断部分单独标注。**

> **状态**:本文描述的是 `2026.7.31.1` 及之前的行为。§7.1 列出的 mcpp 侧改动已在 **2026.8.1.1** 全部实现
> (实施计划见 [`2026-07-31-test-observability-implementation-plan.md`](2026-07-31-test-observability-implementation-plan.md),
> 回归锁在 `tests/e2e/178_test_observability.sh` 与 `tests/unit/test_test_options.cpp`)。
> §7.2 的 mbun 侧与 §7.3 的两项待验证仍然开放。

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| 编排/实现是同一个模型吗? | **是**。`--workspace` 扇出是一段纯 C++ 串行循环,三平台同一条路径,零平台分支。 |
| 执行是同一个模型吗? | **基本是**。只有 3 处平台分支,其中 macOS 的 `runtime_library_path_key()` 返回空串是唯一语义级差异。 |
| 输出是同一个模型吗? | **不是**,而且有两类原因:①`--workspace` 自身的 3 个输出契约缺口(§4);②**Linux/macOS 链接的 libc 不同,stdio 缓冲区差 64 倍**(§5.5)。②才是"连打印都不一样"的真因。 |
| macOS 慢非常多? | **前 21 个成员 macOS 比 Linux 快 2.1×**,然后在 `modules/jsc` 上 ≥25× 并被 CI 杀掉。**不是平台常数慢,是单个成员的链接阶段炸了。** |
| mcpp 有责任吗? | 有三条,且互相耦合:**stdout 不 flush**(诊断全丢)、**`--timeout` 兜不住构建**(卡在链接时形同虚设)、**扇出层零计时零进度**(无法归因)。见 §6。 |

---

## 1. 实现:`--workspace` 到底做了什么

入口 `src/cli/cmd_build.cppm:29 workspace_fanout_members()` + `:165 cmd_test()`。

扇出条件(build/test 共用同一判据):显式 `--workspace`,**或**处在 *virtual* workspace 根(`[package].name` 为空)且没给 `-p`。

扇出体(`cmd_build.cppm:165-185`):

```
for member in members:                       # 严格串行
    ui::status("Workspace", "testing member 'X'")
    rc |= run_tests(passthrough, {package_filter = member}, testOpts)
汇总: "all N member(s) passed" / "workspace test: k/N member(s) failed: ..."
```

即 **`mcpp test --workspace` ≡ 对每个成员依次执行一次完整的 `mcpp test -p <member>`**,continue-on-failure,首个非零退出码胜出。这段没有任何 `#if` / `is_windows` / `is_macos`。

每成员内部(`execute.cppm:828 run_tests`)固定 7 步:

1. `find_manifest_root` → `resolve_member_dir()` 把发现范围**收敛到成员目录**(否则各成员的 `tests/main.cpp` 会撞名)。
2. `expand_glob(memberDir, "tests/**/*.cpp")`;`expand_glob_one` 内部 `std::sort` + canonical 去重(`scanner.cppm:458`)⇒ **发现顺序三平台确定且一致**,不依赖目录遍历顺序。
3. 每个测试文件合成一个 `Target{kind = TestBinary}`,名字 = 相对 `tests/` 去扩展名的路径。
4. `prepare_build(includeDevDeps = true, extraTargets = 合成目标)` —— **每成员一次完整解析**(重新 resolve 工具链、重新合并 workspace deps/indices、重新算指纹目录)。
5. **Phase A**:一次 ninja,目标 = 所有非测试编译单元 + 非测试链接产物。此处失败报成*包*级错误,而非 N 个红测试。
6. **Phase B**:先一次 `-k 0` 批量 ninja 并行建出所有测试目标;再**逐测试**重新驱动 ninja(成功者为 no-op)以获得干净归因;然后逐个 exec 运行。
7. 汇总打印。

### 1.1 实测的进程模型(本机 strace)

2 成员 × 4 测试 + `import std` + `llvm@20.1.7`:

```
ninja 调用       = 12   = 2 成员 × (1 PhaseA + 1 bulk + 4 per-test)
clang++          = 28
clang-scan-deps  = 10
/bin/sh          = 54   ← Clang 的 .pcm restat 包装(cp -p / cmp -s)
execve 总计      = 175
```

⇒ **ninja 进程数 = Σ_member (2 + tests(member))**。热跑重测:12 次 ninja、**0 次重新链接** —— per-test 重驱动确实是 no-op,**不是性能问题**。(曾怀疑它每个测试重链一次,逐行核对 strace 后证伪:成对出现的 `-o bin/tN` 是 `clang++` 驱动 + 它 exec 出的 `ld.lld`,同一次链接。)

---

## 2. 三平台共享的部分

- 扇出循环、成员解析、测试发现与命名、两阶段构建、结果收集与汇总:**全部平台无关**。
- 输出目录 `target/<triple>/<fingerprint>/`:同一套规则。
- `mcpp test -p X`(从 workspace 根)与 `cd X && mcpp test` **最终收敛到同一状态**:`prepare.cppm:772-806` 与 `:807-827` 两分支都做 `merge_workspace_deps` / 继承 `[toolchain]` / `inherit_workspace_indices`,并把 `root` 落到成员目录。唯一差别是前者多打一行 `Workspace building member 'X'`。

---

## 3. 三平台**不**一样的部分(执行层,共 3 处)

| 位置 | Linux | macOS | Windows |
|---|---|---|---|
| `platform/env.cppm:163 runtime_library_path_key()` | `LD_LIBRARY_PATH` | **`""`(空)** | `PATH` |
| `execute.cppm:1160` 注入 sandbox `subos/default/bin` 到子进程 `PATH` | 有 | 有 | **无** |
| `plan.cppm:241` / `ninja_backend.cppm:169` rpath / soname | `-Wl,-rpath,'$ORIGIN'` / `-soname` | `-Wl,-rpath,@loader_path` / `-install_name,@rpath/` | 无 rpath(靠 DLL 拷贝) |

第一处是**语义级**差异:`run_tests`(`execute.cppm:1109-1155`)用 `runtime_library_path_key()` 决定给测试子进程注入什么运行期库路径。**macOS 返回空串,`childEnv` 里根本没有这一项**,完全依赖链接期 rpath。注释写明了理由(`DYLD_LIBRARY_PATH` 会波及 ninja 启动的每个可执行文件,可能让系统框架加载到私有 libc++/libc++abi)——取舍正确,但后果是"依赖 `[runtime] library_dirs` 才能跑起来"的测试在 Linux/Windows 过、在 macOS 以 dyld 错误失败,**且没有任何诊断说出这条差异**。

除此之外,`mcpp test` 路径上没有其他平台条件分支。

---

## 4. 输出契约的 3 个缺口(与平台无关)

本机实跑 `mcpp test --workspace --message-format json`,原样输出:

```
   Workspace testing member 'a'                     ← ① 非 JSON 行混进 stdout
{"test":"t1",...}
{"summary":{"passed":4,"failed":0,"elapsed_ms":37}}  ← ③ 第一个 summary
{"test":"t1",...}                                    ← ② 又一个 "t1",无法区分成员
{"summary":{"passed":4,"failed":0,"elapsed_ms":47}}  ← ③ 又一个,且无 workspace 级 summary
```

① **JSON 模式下 stdout 被污染,且不对称**。`run_tests` 进入时才 `set_quiet(true)`(`execute.cppm:834`),而 `Workspace testing member` 由 `cmd_test` 在调用**之前**打 ⇒ 第一个成员的表头漏出去,第二个起被静音。

② **测试名不带成员前缀**。`t1` 可能在 96 个成员里各出现一次,消费者无法归因。

③ **每成员各发一条 `summary`,无 workspace 级 summary**。human 模式的 `all N member(s) passed` 在 JSON 里没有对应记录;失败列表只走 `ui::error` → stderr。

---

## 5. macOS:测量与归因

### 5.1 先澄清:mbun 两条 lane 跑的根本不是同一个命令

- `macos-probe` 的 Test 步 = `mcpp test --workspace`(mcpp 原生扇出输出)。
- `build-test (llvm@22.1.8)` 的 Test 步 = `tools/integration/test_members.sh`,内部 `(cd $member && mcpp test) > $log 2>&1`,**按设计静默**,且 `SKIP_MEMBERS="modules/jsc"`。实测该步日志 `11:24:43`→`11:27:17`,**输出零行**。

⇒ 两条 lane 的输出差异,一部分直接来自 mbun 用了两个不同驱动。属于 mcpp 的部分是 §4 与 §5.5。

### 5.2 实测数字(同一 commit,97 个成员)

run `30631334192`,commit `a054786`:

| 步骤 | linux gcc@16.1.0(4 核) | linux llvm@22.1.8 | **macOS arm64 llvm@20.1.7(3 核)** |
|---|---|---|---|
| `mcpp build --workspace` | 646 s | 304 s | **399 s** |
| `mcpp run -- --version` | 322 s | (skipped) | **153 s** |
| `mcpp test --workspace` | **355 s(跑完)** | 154 s(跳过 jsc) | **≥2713 s,被 45 min step timeout 杀掉** |

### 5.3 归因:不是平台常数慢,是单个成员的链接炸了

用**子测试进程的输出**(它们是独立进程、退出时各自 flush,时间戳可信)做 landmark:

| landmark | macOS | linux gcc |
|---|---|---|
| Test 步开始 | 12:47:35 | 12:47:14 |
| `test_dependency: 86 checks`(`modules/install` 最后一个测试) | 12:48:21 = **T+46 s** | **T+98 s** |
| 之后 | **44 m 27 s 零输出**,13:32:48 被杀 | `modules/js` 12.4 s + `modules/jsc` 93.5 s,全程 355 s 跑完 |

⇒ **前 21 个成员 macOS 比 Linux 快 2.1×**;紧接着的成员上 Linux 106 s、macOS ≥2667 s ⇒ **≥25×**。

**卡点是 `modules/jsc`**,规模(从 Linux 日志数出):**32 个 path 依赖** + `mbun.jsc-prebuilt`(预编 JavaScriptCore)+ **14 个 test 可执行文件**,每个都要链到这一整坨。

决定性佐证 —— 同一 macOS job 的 Build 步里,`modules/jsc` **只发了一条边**:

```
llvm-ar rcs bin/libjsc.a        ← 零个可执行链接,145 s 就过了
```

`mcpp build` 对这个成员只打包静态库;`mcpp test` 才**把 14 个可执行文件分别链到整个预编 JSC 上**,dev profile 是 `-O0 -g`。**差异面是链接,不是编译** —— 这就是"Build 步 macOS 只慢 1.31×、Test 步炸掉"的全部原因。

macOS 侧链接为何这么贵(**推断**,按可信度排):
1. Apple `ld` 在 `-g` 下要建 **debug map**,读进每个被拉入 object 的 DWARF;Linux 的 lld/bfd 不做这步,DWARF 留在 `.o` 里。对着几百 MB 静态归档 ×14 次。
2. arm64 上每个可执行文件链接时**强制 ad-hoc 签名**,哈希整个二进制每一页。
3. macOS runner 3 核 vs Linux 4 核(常数,非主因)。

**横向对照**:mcpp-index 仓库(46 个成员,同样 `mcpp test --workspace`)最近 30 次 CI 里 **macOS 每一次都是三平台最快**(2446 s vs linux 4685 s / windows 4535 s),逐成员对比无一例外。⇒ 不存在"macOS 上 `mcpp test --workspace` 普遍慢"这回事。

### 5.4 为什么 Linux 的 Test 步看得到构建信息、macOS 完全看不到

**这不是代码差异,是两个平台链接的 libc 不同。**

`ui::status / info / finished / plain`(`ui.cppm:228/241/253/293`)全是裸 `std::println(...)`,**无一处 `fflush`**;全仓库**无 `setvbuf`**。所以非 TTY 下 stdout 全缓冲,**缓冲区大小由 libc 决定**:

| | Linux 发行版二进制 | macOS 发行版二进制 |
|---|---|---|
| 链接方式 | **musl 静态链接**(`file` 实测 `statically linked`) | Mach-O,动态链接 Apple libSystem |
| stdio 缓冲区 | musl **写死 `BUFSIZ = 1024`**,不看 `st_blksize` | BSD stdio 取 `fstat(1).st_blksize`;CI 里 stdout 是管道,macOS 管道报 **65536**(*待在 runner 上一条命令确认*) |

本机 strace 实测 Linux 侧(30 成员 workspace):

```
write(1, …) = 34, 1048, 1027, 1030, 1030, 1055, 1083, 1083, 1083, 1083, 1083, 228
```

全部 ~1024。**而本机管道的 `st_blksize` 是 4096,mcpp 照样按 1024 走** ⇒ 坐实是 musl 的固定值,不是从 OS 取的。

对上实际现象 —— 从 Test 步开始到 `test_dependency`(macOS 停住的同一点),mcpp 自身输出量 **13 639 B ≈ 13.3 KB**:

- **Linux(1 KB 缓冲)**:13.3 KB ÷ 1 KB ≈ **13 次 flush** → 边跑边一块块吐出,与子进程输出交错。铁证是这一行:
  ```
     Workspace testing member 'modules/semver'  huge prefix runs: 12 ms
  ```
  mcpp 的行还没写到换行符,1 KB 边界就到了被冲出去,子进程输出紧跟着落在同一行 —— **只有定长块 flush 会长这样,行缓冲永远不会**。
- **macOS(64 KB 缓冲)**:13.3 KB ÷ 64 KB = **0 次 flush** → 一行 mcpp 输出都没出来;45 min 后 step timeout **SIGKILL**,缓冲区连同 13.3 KB 一起丢弃。

而两边都能实时看到 `test_core_alloc: 128 checks` —— 那是**子测试进程**,独立进程各自退出时 flush,直接落到同一个 fd 1,与 mcpp 的缓冲区无关。

这也解释了 macOS 的 **Build 步为何正常**:`mcpp build --workspace --verbose` 把 ninja 输出也走 stdout,几百 KB,轻松反复越过 64 KB 阈值。**不是 test 与 build 的代码不同,是输出量跨没跨过阈值。**

**验证命令(在 macOS runner 上)**:
```bash
python3 -c "import os;print(os.fstat(1).st_blksize)"   # 管道里跑,预期 65536
script -q /dev/null mcpp test --workspace              # 伪 TTY → 行缓冲 → 立刻实时输出
```

---

## 6. 计时与超时:现状缺陷 + 建议补充的功能

### 6.1 现状(全部经代码 + CI 日志核对)

| 能力 | 现状 | 位置 |
|---|---|---|
| 单测试耗时 | **测了但只发给 JSON**。`TestResult::durationMs` 由 `test_ms()` 填,`emit_json` 发 `duration_ms`;human 模式的 `t1 ... ok` **不带时间** | `execute.cppm:1010,1117,1028` |
| 单成员总耗时 | 打印 `test result ok. N passed; 0 failed; finished in X.XXs`,**但起表点是错的** | `execute.cppm:1107,1228` |
| workspace 总耗时 | **完全没有**。扇出只打 `all N member(s) passed` | `cmd_build.cppm:176-183` |
| 单测试超时 | `--timeout N` 有,**默认 0 = 不限** | `execute.cppm:801` |
| 构建超时 | **没有** | — |
| workspace 总超时 | **没有** | — |

**缺陷 A —— `finished in` 漏算构建时间。** `auto t0` 在 `execute.cppm:1107`,即 **Phase A(包级构建)与 Phase B 批量构建都跑完之后**才起表,只覆盖 per-test 循环。CI 实测:

| 成员 | 打印值 | 真实墙钟 | 误差 |
|---|---|---|---|
| `modules/jsc` | `finished in 6.53s` | 93.5 s | **14.3×** |
| `modules/install` | `finished in 33.02s` | 32.2 s | ~1× |

差别在于时间花在哪:`install` 主要在 per-test 循环里(算得进),`jsc` 的 87 秒全在 Phase A + 批量构建(**完全不算**)。⇒ **越是构建重的成员,这个数字越不可信** —— 恰好是最需要它的时候。

**缺陷 B —— `--timeout` 兜不住构建。**

```
execute.cppm:1056  backend->build(Phase A)      ← 无期限
execute.cppm:1103  backend->build(bulk)         ← 无期限
execute.cppm:1126  backend->build(per-test)     ← 无期限
execute.cppm:1181  capture_exec_deadline(...)   ← 只有这里受 --timeout 约束
execute.cppm:1186  run_exec_deadline(...)       ← 只有这里受 --timeout 约束
```

⇒ **macOS 那次卡在 `modules/jsc` 的链接上,`--timeout` 设多少都救不了。**

**缺陷 C —— 扇出层零可观测性。** 97 个成员串行,过程中唯一线索是 `Workspace testing member 'X'` 一行(而在 §5.4 的缓冲问题下 macOS 上根本出不来)。没有 `M/N` 进度、没有 per-member 耗时、没有累计耗时 ⇒ 这次定位 `modules/jsc` 只能靠反解 GitHub 日志时间戳。

### 6.2 建议补充的功能

**计时(3 项)**

1. **per-test 耗时进 human 输出**:`t1 ... ok (0.31s)` / `t1 ... FAIL (exit 1, 2.40s)`。数据已在 `durationMs` 里,只差打出来。
2. **修 `t0` 起表点,并拆两段**:起表移到 Phase A 之前,汇总行改成
   `test result ok. 14 passed; 0 failed; finished in 93.5s (build 87.0s + run 6.5s)`。
   构建/运行分开远比一个合并数字有用 —— `jsc` 这种 93% 时间在链接的成员一眼可见。
3. **workspace 级汇总 + 进度**:
   ```
      Workspace member 'modules/jsc' (23/97) ok — 14 passed in 93.5s
    workspace result ok. 97 members; 412 passed; 0 failed; finished in 355.2s
      slowest: modules/jsc 93.5s, modules/install 32.2s, modules/http_types 24.1s
   ```
   JSON 模式补 `{"workspace_summary":{"members":97,"passed":412,"failed":0,"elapsed_ms":355200}}`,并给每条 test 记录加 `"member"` 字段(解决 §4 ②)。

**超时(3 层,语义必须分清)**

| 开关 | 约束对象 | 建议默认 | 触发时 |
|---|---|---|---|
| `--timeout <s>` | 单个测试**进程的运行** | **300**(现为 0=不限) | `FAIL (timeout after Ns)`,继续下一个测试 |
| `--build-timeout <s>` | **单条 ninja 驱动**(Phase A / bulk / per-test 各自独立计) | 900 | 报成该成员的 `build timeout`,继续下一个成员 —— **唯一能兜住 macOS 那种链接卡死的闸** |
| `--workspace-timeout <s>` | **整条扇出**的累计墙钟 | 0(不限,由 CI `timeout-minutes` 兜底) | 停止扇出,**如实汇总已完成成员**并列出未跑的,退出码非零 |

三条硬要求:

- **超时必须能归因**:报文带 `<member> / <test> / <phase>`,而不是只有 "timed out"。
- **超时触发时必须 `fflush(stdout)`**:否则在 macOS 上(§5.4)这条报告本身也会被吞掉。
- **`--timeout 0` 显式表示"不限"**,默认必须有界 —— `mcpp test` 才是一个能放进 CI 的操作。

### 6.3 为什么这一组是同一件事

本次排查的全部困难来自同一个缺口:**`mcpp test --workspace` 既不报时间、也不设期限、还不 flush**。三者任缺其一,另外两个的价值都打折:

- 只加超时不修 `setvbuf` ⇒ 超时报告被缓冲区吞掉,CI 上仍是一片空白;
- 只加计时不加超时 ⇒ 知道谁慢,但仍会被 CI 的 `timeout-minutes` SIGKILL,连汇总都拿不到;
- 只修 flush 不加计时 ⇒ 看得到卡在哪个成员,但不知道正常该多久、慢了多少倍。

建议作为**一个 PR** 一起做。

---

## 7. 行动项

### 7.1 mcpp 侧(按性价比排序)

| # | 改动 | 规模 | 解决 |
|---|---|---|---|
| 1 | `std::setvbuf(stdout, nullptr, _IOLBF, 0)` 于 `main.cpp` 入口 | 1 行 | 跨平台输出行为统一;被 kill 时不丢诊断;mcpp 的行不再被从中间切开 |
| 2 | `--build-timeout`(包住三处 `backend->build`) | 中 | 链接/编译卡死可归因、可继续 |
| 3 | `--timeout` 默认改 300,`0` 显式表示不限 | 小 | `mcpp test` 成为有界操作 |
| 4 | per-test 耗时进 human 输出 + 修 `t0` 起表点(build/run 分列) | 小 | 数字可信 |
| 5 | 扇出层 per-member 计时 + `M/N` 进度 + workspace 级汇总(human & JSON) | 中 | 可观测性 |
| 6 | JSON 模式:表头改成 JSON 记录 / 提前 `set_quiet`;测试名加成员前缀 | 小 | §4 ①② |
| 7 | `--workspace-timeout` | 中 | 兜底 |
| 8 | macOS `runtime_library_path_key() == ""` 时,若成员声明了 `[runtime] library_dirs` 给一条诊断 | 小 | §3 静默差异 |

### 7.2 mbun 侧

- **`--timeout` 对本次问题无效**(卡在链接,不在运行)—— 需要 mcpp 的 `--build-timeout`,或先在 macOS lane 也 `SKIP_MEMBERS="modules/jsc"`。
- 对 `modules/jsc` 用 release profile(去掉 `-g` 能砍掉 macOS debug map 的大头),这是当前唯一不改 mcpp 就能试的解法。
- macOS lane 与 llvm lane 对齐驱动:要么都用 `mcpp test --workspace`,要么都用 `test_members.sh`。现在的不对称本身就是"输出不一样"的来源之一。

### 7.3 待验证(唯一还没实测的两点)

1. macOS runner 上 `python3 -c "import os;print(os.fstat(1).st_blksize)"`(管道内)是否为 65536 —— 确认 §5.4 的缓冲区数字。
2. macOS 上单跑 `mcpp test -p modules/jsc`,看时间是否几乎全在 14 次可执行链接上(`-O0 -g` vs `--release` 对照)—— 确认 §5.3 的链接归因。
