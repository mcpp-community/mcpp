# Fingerprint 稳定性与 Fast-Path 一致性 — 优化方案

> 2026-05-15 — 修复"改一行 .cppm 触发全量重编"的体感问题
> 关联背景报告：用户在 `codex/fix-bmi-cache-staging` 分支上观察到改一行 `src/cli.cppm` 后 `mcpp build` 耗时 26.18s，下一次再 build 仅 0.01s。

## 0. 一句话

体感上的"单文件增量失效"实际是 **fingerprint 漂移 → 切换到新的 outputDir → 新目录零缓存 → 看上去像全量重编**。
要修的是 **fingerprint 对工具链镜像位置过于敏感**，以及 **fast-path 与 fingerprint 之间不闭环**；P2/P3 都不在此 PR 范围。

## 1. 现象与证据

### 1.1 现象
```
$ mcpp build                  # baseline, 0.02s
$ vim src/cli.cppm             # 只加一行空行
$ mcpp build                   # 26.18s   ← 异常
$ mcpp build                   # 0.01s    ← 之后又正常
```

### 1.2 关键证据
- `target/x86_64-linux-gnu/` 下存在两个 fingerprint 目录：`8d0f404262222915`（昨天）、`b2f2305667355830`（今天 07:45 一次性全量生成）。
- 两份 `build.ninja` 的差异只有工具链绝对路径前缀：
  ```diff
  - cxx = /home/speak/.mcpp/registry/.../xim-x-gcc/16.1.0/bin/g++
  + cxx = /home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.14/registry/.../xim-x-gcc/16.1.0/bin/g++
  ```
- 这两个 `g++` 二进制 MD5 不一致：`5c7679...` vs `fa42790...`（名义版本同为 gcc 16.1.0）。
- `which mcpp` = `~/.xlings/subos/default/bin/mcpp` → 实际执行 `xim-x-mcpp/0.0.14/bin/mcpp`，它的工具链查找走 `~/.xlings/data/xpkgs/...`；而昨天的本地构建是 `target/.../bin/mcpp`，工具链走 `~/.mcpp/registry/...`。

### 1.3 触发链
`src/toolchain/fingerprint.cppm:96`：
```cpp
fp.parts[2] = tc.binaryPath.empty() ? "" : hash_file(tc.binaryPath);
```
fingerprint 第 3 字段是 **g++ 二进制的内容哈希**。两个 `g++` 内容不同 → fingerprint 不同 → outputDir 不同 → 新目录里没有 `.gcm/.m.o/.ninja_log` → 必然全量编。

### 1.4 Fast-path 没拦住的原因
`src/cli.cppm:2187` 的 `try_fast_build()`：
- 直接读 `.build_cache` 里上次记录的 outputDir；
- 只比较源文件 mtime 与该 outputDir 下 `build.ninja` 的 mtime；
- **不参与 fingerprint 计算**。

序列：
1. 今天第一次 `mcpp build`：`.build_cache` 指向旧目录 `8d0f4...`，mtime 无变 → 0.02s no-op。
2. 编辑 `cli.cppm` → cli.cppm.mtime > build.ninja.mtime → fast-path 返回 `nullopt`。
3. 回退到 `prepare_build()`：**第一次**真正算 fingerprint，结果是 `b2f23...`（因为 PATH 上的 mcpp 用的是另一个 g++ 副本）。
4. 落到全新目录，0 缓存 → 26.18s。
5. 写回 `.build_cache` 指向 `b2f23...`，从此 fast-path 又对了。

**关键的设计裂缝**：fast-path 在"旧 fingerprint 的世界观"里做判断，回退后却进入"新 fingerprint 的世界观"，二者之间没有任何一致性 gate。

## 2. 优化目标

| 指标 | 现状 | 目标 |
|---|---|---|
| 改一行 .cppm 触发全量重编 | 偶发（取决于工具链路径） | 不发生 |
| 切换 mcpp 版本 / 工具链镜像位置 | 全量重编（合理） | **明确告知**用户而非沉默切目录 |
| Fingerprint 对同一工具链的不同副本 | 给出不同值 | **给出相同值**（同 `version + triple + stdlib + cppstd`）|
| Fast-path 命中率 | 高，但偶尔走错目录 | 命中即正确 |

## 3. 方案

按优先级 P0 → P2。每条都包含：**问题、改动点、改法、风险、验证**。

---

### P0 · Fingerprint 第 3 字段：用语义身份替换二进制内容哈希

**问题**：`hash_file(tc.binaryPath)` 把"工具链身份"和"工具链文件位置/副本"绑死了。同一个发行版的 GCC 16.1.0 通过 `xlings install` 安装到不同的命名空间（`~/.mcpp` vs `~/.xlings/data/xpkgs/xim-x-mcpp/0.0.14/registry`）会得到不同的二进制（不同的 strip/打包），从而得到不同的 fingerprint。结果是：用户每切换一次 mcpp 安装或在本地 dev/system-installed 之间切换，整个 BMI/obj 缓存都失效。

**改动点**：`src/toolchain/fingerprint.cppm:90-113` `compute_fingerprint()`、`src/toolchain/fingerprint.cppm:31-34` `Fingerprint` 结构、`src/toolchain/detect.cppm`（采集探针输出）。

**改法**：
1. 在 `Toolchain` 上新增字段（如果尚无）：`std::string driverIdent;` —— 由 `g++ -v` / `g++ -dumpversion` / `g++ -dumpmachine` 拼出的归一化字符串。
2. `compute_fingerprint()`：
   ```cpp
   // Before
   fp.parts[2] = tc.binaryPath.empty() ? "" : hash_file(tc.binaryPath);
   // After
   fp.parts[2] = hash_string(tc.driverIdent);
   ```
3. `driverIdent` 的归一化规则（去掉本机绝对路径）：
   - `gcc -v 2>&1` 输出里**剥离**含 `/home/`、`/tmp/`、`/var/`、`PWD=` 的行；
   - 保留 `gcc version X.Y.Z`、`Target:`、`Configured with:`（但把其中的绝对路径替换成 token，如 `<PREFIX>`）；
   - 把整个串做一次 trim + 折叠空格。

**风险**：
- 同一 gcc 不同 `--prefix=` 重编 → 行为可能存在细微差异（极少见，且 sysroot/-B 单独走 flags 链路）。可接受。
- driverIdent 的归一化要可测、可复现 → 单测覆盖。

**验证**：
- 增加单测：把同一 GCC 安装两次到不同前缀（或软链），fingerprint 必须一致。
- 增加单测：升级 gcc 大版本，fingerprint 必须改变。

---

### P1 · Fast-path 与 Fingerprint 闭环

**问题**：`try_fast_build()` 完全在 `.build_cache` 记录的旧 outputDir 上做判断，没和 fingerprint 对账。fast-path miss 后再算出来的 outputDir 可能与刚才查的不一致，用户会沉默地"换家"。

**改动点**：`src/cli.cppm:2187-2259` `try_fast_build()`。

**改法 A（轻量、推荐）**：在 fast-path 入口廉价地确认"当前世界观与 `.build_cache` 一致"。
```cpp
std::optional<int> try_fast_build(...) {
    if (no_cache) return std::nullopt;

    auto cachePath = projectRoot / kBuildCacheFile;
    if (!exists(cachePath)) return std::nullopt;

    // 读取 .build_cache（已有逻辑）
    ...

    // [新增] outputDir 必须真实落在 target/<triple>/<fp>/ 下。
    // 当 mcpp.toml 或工具链选择影响 fingerprint 时，fast-path 必须放弃。
    if (!fingerprint_dir_matches_current_environment(outputDir,
                                                     projectRoot,
                                                     currentTarget)) {
        // 主动失效 .build_cache,避免一直拿旧目录骗自己
        std::error_code ec;
        std::filesystem::remove(cachePath, ec);
        return std::nullopt;
    }

    // 原本的 mtime 比对逻辑保持不变
    ...
}
```

`fingerprint_dir_matches_current_environment()` 的实现成本必须**低于**重新做 `prepare_build()` 全流程，否则得不偿失。最便宜的做法：
- 提取 `outputDir` basename 作为 "cachedFp"；
- 用 toolchain detect 的**结果缓存**（同进程内）拿到当前 driverIdent + cppStandard + depLockHash + stdBmiHash；
- 调一次 `compute_fingerprint()`（FNV-1a，纯内存，<1ms）；
- 比较两个 hex。

**改法 B（替代）**：`.build_cache` 里**额外写入** fingerprint hex；fast-path 入口要求当前 fingerprint == cached fingerprint 才放行。简单但需要 `prepare_build()` 至少跑一次"轻量版"，等于改法 A。

**改动点扩展**：`write_build_cache()`（`src/cli.cppm:2096-2109`）增写一行 `fingerprint`：
```
<outputDir>
<ninjaProgram>
<targetTriple>
<fingerprintHex>   ← 新增第 4 行
```

**风险**：
- 老 `.build_cache` 文件少一行 → `try_fast_build` 必须把它当 miss（已有 `std::getline` 容忍空读取，沿用即可）。
- fingerprint compute 需要 toolchain detect，可能引入 1 次 `g++ -v` 调用 → 缓存到进程内 + 加 mtime gate（若 `g++` 二进制 mtime 没变就复用上次探针结果）。

**验证**：
- 单测：构造两个不同 outputDir 的 `.build_cache`，校验 fast-path 拒绝。
- E2E：切换 `which mcpp` 指向不同安装时，第一次 `mcpp build` 仍应跑 prepare_build 路径，但不能默默"搬家"。

---

### P1.5 · 用户可见的"fingerprint 切换"诊断

**问题**：26s 的体感最让人困惑的是"我什么都没做就触发全量"。用户没法快速判断是"工具链变了"还是"缓存失效了"。

**改动点**：`src/cli.cppm` `prepare_build()` 收尾处（写 `.build_cache` 之前）。

**改法**：在 `prepare_build()` 算出 outputDir 之后，若发现它与 `.build_cache` 里记录的不同，发一行 warn：
```
warning: fingerprint changed (cache miss → full rebuild)
         old:  8d0f404262222915  (last build)
         new:  b2f2305667355830  (this build)
         likely cause: toolchain binary changed
                       g++ = /home/speak/.xlings/.../xim-x-mcpp/0.0.14/.../g++
```

这条信息让用户立刻知道"不是 incremental 退化，是工具链/依赖换了"。

**风险**：仅打印，无功能副作用。

**验证**：人工 / 集成测试中观察输出。

---

### P2 · `cxx_scan` 添加 `restat = 1`

**问题**：`src/build/ninja_backend.cppm:235` 的 `cxx_scan` rule 没有 `restat = 1`，而 `cxx_dyndep`、`cxx_module`、`cxx_object`、`cxx_collect` 都有。在源 mtime 更新但 `.ddi` 内容不变的情况下（例如 `touch`、空行编辑、注释改动），ninja 会沿着 ddi → dd → compile edge 全链路标 dirty，即便下游内容并未变化。

**注意**：这个修法**修不到** §1 描述的 26s 现象（那个属于换目录），但它会让"同 fingerprint 内的微小编辑"更安静、更便宜。等价于把 CMake 的 `RESTAT` / build2 的行为对齐过来。

**改动点**：`src/build/ninja_backend.cppm:235-238`。

**改法**：
```cpp
append("rule cxx_scan\n");
append("  command = $toolenv $cxx $cxxflags -fdeps-format=p1689r5 "
       "-fdeps-file=$out -fdeps-target=$compile_target "
       "-M -MM -MF $out.dep -E $in -o $compile_target\n");
append("  description = SCAN $out\n");
append("  restat = 1\n\n");                  // ← 新增
```

注意：`cxx_scan` 现在的命令把 ddi 写到 `$out`，**它会无条件改写文件**（GCC 不做"内容相同时不动文件"）。要让 `restat = 1` 真正生效，scan 命令应当**先写到 `$out.tmp` 再做 `cmp -s && mv -f $out.tmp $out`**：
```
command = $toolenv $cxx $cxxflags -fdeps-format=p1689r5 \
            -fdeps-file=$out.tmp -fdeps-target=$compile_target \
            -M -MM -MF $out.dep -E $in -o $compile_target && \
          if [ -f "$out" ] && cmp -s "$out.tmp" "$out"; then \
             rm -f "$out.tmp"; \
          else \
             mv -f "$out.tmp" "$out"; \
          fi
```

**风险**：
- `-fdeps-file=$out.tmp` 需要 GCC 接受 `.tmp` 后缀（应当无问题，它就是个文件路径）。
- `$out.dep`（depfile）也走 `$out` 路径 → 注意 ninja 的 `depfile` 关联还是 `$out.dep`，保持不变即可。

**验证**：
- 单测/E2E：在源文件上 `touch` 但不改内容 → `mcpp build` 应当**不**重编对应的 `.m.o`。
- 在源文件加一行 `// xxx` 但不改 imports → 也应当不重编下游模块。

---

### P3（可选 · 后续）· `.build_cache` 升级为多 fingerprint LRU

**问题**：当前 `.build_cache` 只记一个 outputDir。在本地 dev / system-installed mcpp 之间切换时，会反复让对方失效。

**改法**：把 `.build_cache` 升级为以 `(target_triple, fingerprint_hex)` 为 key 的 map，可存最近 N 个。`try_fast_build()` 用当前世界观的 key 直接查；miss 才走慢路径。

**风险**：磁盘多占一点空间，TTL/LRU 需要简单收割策略。

**验证**：E2E 反复在两个 mcpp 之间切换，每次 fast-path 都应当命中各自的目录。

---

## 4. 改动落地清单（按 PR 拆分建议）

| PR | 范围 | 影响面 | 测试 |
|---|---|---|---|
| #A | P0 + P1 + P1.5（fingerprint 稳定化 + fast-path 闭环 + 诊断） | `toolchain/fingerprint.cppm`、`toolchain/detect.cppm`、`cli.cppm`（fast-path/build cache） | 单测 + 一个 E2E：切换 mcpp 安装路径不再触发"沉默 26s" |
| #B | P2（cxx_scan restat） | `build/ninja_backend.cppm` | 单测：touch 文件不重编下游 |
| #C | P3（多 fingerprint cache） | `cli.cppm` | E2E：在两个 mcpp 之间切换都能 fast-path |

建议先合 #A 单独发版本（最小风险、解决用户体感问题），#B 后续跟进，#C 视实际使用频率决定。

## 5. 反例 · 不应该做的事

1. **不要去改 `bmi_cache.cppm`**。最近 commit `6482b5a` 的"避免触碰已存在的 BMI"是正确的、解决的是另一回事（跨项目 BMI cache → 本项目 staging 时不要覆盖+推 mtime）。本次现象在 fingerprint 层就分叉了，根本走不到 `stage_into`。
2. **不要在 fast-path 里去做更激进的"用 ninja 直接重算 dyndep"**。fast-path 的全部价值就是 "不再进 mcpp 前端"；任何让它变重的改动都是反向优化。
3. **不要让 fingerprint 忽略真实工具链差异以图稳定**。如果 `gcc -v` / `Configured with:` 确实不同，fingerprint **应当**变化 — 否则会把 stale BMI 跨工具链复用，违反 docs/06 的安全前提。

## 6. 验收指标

- [ ] 在本项目根目录下，分别用 `~/.xlings/.../bin/mcpp` 和本地新编 `target/.../bin/mcpp` 各跑一次 `mcpp build --print-fingerprint`，输出 fingerprint **必须相同**（前提：底层 g++ 实际是同一发行版的同一版本）。
- [ ] 改一行 `src/cli.cppm` 后再 build，仅 `cli.m.o`+ link 重做，不超过 5s（当前 26.18s）。
- [ ] `touch src/cli.cppm`（mtime 变、内容不变）后再 build，**0 个**模块重编，仅 ddi rescan + restat 命中。
- [ ] 切换 mcpp 安装位置时，控制台明显打印 `fingerprint changed` 警告。

## 7. 关联

- 背景报告：聊天记录（用户提供的终端会话 + 本文 §1 证据链）。
- 上一份相关分析：`2026-05-12-build-optimization-analysis.md`（P0 fast-path、P1 per-file dyndep、P2 BMI 保留 — 都已落地，本次是它们之上的稳定性补完）。
- 设计文档：`docs/06-toolchain-and-fingerprint.md`（10 字段 fingerprint 规范，本次 P0 修改的是第 3 字段语义）。

## 8. 当前 PR 对比与复用结论

### 8.1 PR 对比

| PR | 状态 | 主题 | 与本问题关系 |
|---|---|---|---|
| #29 | merged | `28_target_static` E2E 选择 GNU/musl 工具链 | CI 稳定性修复，不解决 fingerprint 漂移 |
| #32 | merged | LLVM/Clang/import std + mirror config | 引入/整理工具链层能力，不是当前增量退化根因 |
| #33 | open, CI passed on `6482b5a` | BMI cache staging 不覆盖已有 target 产物 | 已修复一类“prepare 后误打脏依赖 BMI”的问题；适合作为本次增量构建体验修复的承载 PR |

结论：复用 #33 是合理的。#33 已经覆盖 BMI staging 误 touch 的问题，本次 fingerprint 稳定性属于同一条“增量构建体验”问题链。直接新开 PR 会把两个强相关的性能修复拆散，反而不利于 0.0.15 的发布说明和回归验证。

### 8.2 当前已落地范围

本轮先落地 P0，保持改动面集中：

- `Toolchain` 增加 `driverIdent`，由 `detect()` 从 compiler `--version` 输出归一化得到。
- `compute_fingerprint()` 第 3 字段优先使用 `hash(driverIdent)`；只有缺少 `driverIdent` 时才回退到旧的 `hash_file(binaryPath)`。
- `normalize_driver_output()` 会去空白、折叠空行，并把 `/home/`、`/tmp/`、`/var/` 开头的本地路径片段替换为 `<PATH>`，避免不同安装前缀污染 fingerprint。
- 单测覆盖：
  - driver 输出归一化；
  - `detect()` 填充 `driverIdent`；
  - 同一 `driverIdent` 下 `binaryPath` 变化不改变 fingerprint；
  - driver 版本变化仍改变 fingerprint。

P1/P1.5/P2/P3 暂不混入 #33：

- P1/P1.5 需要改 `.build_cache` 格式和 fast-path 诊断，属于缓存协议升级，建议单独跟进，避免影响 0.0.15 的修复范围。
- P2 需要调整 `cxx_scan` 的输出写入策略，不只是加 `restat = 1`，需要单独验证 GCC `-fdeps-file=$out.tmp` 行为。
- P3 是缓存结构升级，不属于当前 release blocker。

### 8.3 当前验证记录

- `bin/test_toolchain_detect`：7 tests passed。
- `bin/test_fingerprint`：5 tests passed。
- 使用新构建的 mcpp，在新 fingerprint 目录完成一次初始化构建后：
  - no-change：`ninja: no work to do`
  - `touch src/main.cpp` 后经 mcpp build：只运行 4 个 edge（scan/dyndep/main.o/link），约 `0.36s`

注意：fingerprint 算法改变后，首次升级到 0.0.15 时会进入新的 `target/<triple>/<fingerprint>` 目录，因此第一次 build 全量是预期行为；后续增量应稳定。
