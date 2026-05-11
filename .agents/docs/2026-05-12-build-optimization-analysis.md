# mcpp 构建优化深度分析报告

> 2026-05-12 — 模块化编译优化、缓存机制、增量编译分析
> 基于 mcpp 0.0.10 代码库分析

## 1. 当前构建流程与耗时分解

### 1.1 mcpp 自身项目的构建数据

| 场景 | 耗时 | 分析 |
|---|---|---|
| 全量构建（冷） | ~21s | 合理 |
| 无改动重新 build | **~10s** | ❌ 前端开销 |
| touch 一个文件 | **~21s** | ❌ 全量重编译 |
| ninja 直接 no-op | 0.023s | 参考基线 |

### 1.2 耗时分解

```
mcpp build (touch 一个文件):
├── mcpp 前端 (~10s)
│   ├── toolchain resolve (xlings interface 子进程)
│   ├── manifest parse + dep fetch
│   ├── regex scanner (扫描所有 .cppm 文件)
│   ├── modgraph validate
│   ├── fingerprint compute
│   ├── ensure_built std module
│   ├── make_plan (BuildPlan 构建)
│   ├── BMI cache check/stage
│   └── build.ninja + compile_commands.json 生成
│
└── ninja 执行 (~11s)
    ├── Phase 1: SCAN (1 个 .ddi 变化)  ~1s
    ├── Phase 2: COLLECT (build.ninja.dd 重生成)  ~0.1s
    ├── Phase 3: ALL 39 个模块重编译  ~10s ← 核心问题
    └── LINK  ~0.5s
```

## 2. 两大核心问题

### 2.1 问题一：全局 dyndep 导致全量重编译

**根因**：所有编译边依赖同一个 `build.ninja.dd` 文件。

```
cli.cppm 被 touch
    ↓
cli.cppm.ddi (scan) 重新生成
    ↓
build.ninja.dd 依赖 ALL 39 个 .ddi → build.ninja.dd 被重新生成
    ↓
所有 39 个 compile edge 都有 `| build.ninja.dd` 作为 implicit dep
    ↓
全部 39 个模块标记为 dirty → 全量重编译
```

**理想行为**：只有 `cli.cppm` 和直接依赖 `mcpp.cli` 模块的文件需要重编译。

**ninja 的 dyndep 机制本身支持 per-file dyndep**，当前实现选择了最简单的全局方案。

### 2.2 问题二：mcpp 前端每次全量重算

即使没有任何文件改动，mcpp 仍然花 ~10s 做：
- 启动 xlings 子进程解析工具链
- 扫描所有源文件的 module 声明
- 生成 BuildPlan
- 重新写入 build.ninja

这些步骤的结果在大多数增量编译场景下都不变。

## 3. 优化策略

### 3.1 策略一：per-file dyndep（影响最大）

**目标**：改一个文件只重编译该文件及其下游依赖。

**方案**：将全局 `build.ninja.dd` 拆分为 per-file dyndep。

```ninja
# 当前（全局 dyndep）：
build build.ninja.dd : cxx_collect obj/cli.cppm.ddi obj/ui.cppm.ddi ...
build obj/cli.m.o | gcm.cache/mcpp.cli.gcm : cxx_module src/cli.cppm | build.ninja.dd
  dyndep = build.ninja.dd

# 优化后（per-file dyndep）：
build obj/cli.cppm.dd : cxx_dyndep obj/cli.cppm.ddi
  restat = 1
build obj/cli.m.o | gcm.cache/mcpp.cli.gcm : cxx_module src/cli.cppm | obj/cli.cppm.dd
  dyndep = obj/cli.cppm.dd
```

**效果**：touch `ui.cppm` 时，只有 `ui.cppm.ddi` → `ui.cppm.dd` 变化，只有 `ui.m.o` 和依赖 `mcpp.ui` 的下游文件需要重编译。

**实现复杂度**：中等。需要修改 `ninja_backend.cppm` 的 emit 逻辑和 `dyndep.cppm` 的生成方式。

### 3.2 策略二：BMI restat + copy_if_different（减少级联重编译）

**目标**：当模块接口不变时（只改了实现），阻止级联重编译。

**方案**（业界标准做法，CMake 采用）：
1. 编译器输出 BMI 到临时文件
2. 比较临时文件与当前 BMI 内容
3. 内容不同才覆盖（保持旧时间戳）
4. ninja `restat = 1` 检测到 BMI 未变，跳过下游

```ninja
rule cxx_module
  command = $cxx $cxxflags -c $in -o $out.tmp && \
            (cmp -s $bmi_out.tmp $bmi_out && rm $bmi_out.tmp || mv $bmi_out.tmp $bmi_out) && \
            mv $out.tmp $out
  restat = 1
```

**效果**：修改 `ui.cppm` 的函数体但不改接口 → BMI 不变 → 依赖 `mcpp.ui` 的下游不重编译。

**GCC 注意**：GCC 每次都会重新生成 BMI 文件（即使内容相同时间戳也变），所以必须在构建系统层面做 copy_if_different。

### 3.3 策略三：mcpp 前端缓存（减少 10s 前端开销）

**目标**：无改动时 mcpp 应在 <1s 内完成。

**方案**：

1. **快速脏检查**：在调用 scanner/make_plan 之前，检查 `build.ninja` 是否比所有源文件更新。如果是，直接跳到 ninja 执行。

2. **增量 scanner**：缓存上一次的扫描结果（module graph），只重新扫描修改过的文件。

3. **工具链缓存**：toolchain resolve 结果缓存到 `.mcpp/cache/toolchain.json`，避免每次启动 xlings 子进程。

**效果**：无改动 → ~0.1s（直接 ninja no-op），改一个文件 → ~1s（增量 scan + ninja）。

### 3.4 策略四：Clang 两阶段编译（未来多工具链支持）

**当前**：GCC 一次生成 BMI + .o，串行依赖。

**Clang 支持两阶段**：
```
Phase 1: clang --precompile A.cppm -o A.pcm    (生成 BMI)
Phase 2: clang -c A.pcm -o A.o                 (BMI → .o)
```

**好处**：A 的 BMI 就绪后，B 可以开始编译 BMI，同时 A 继续编译 .o。并行度更高。

**Clang 还支持 Reduced BMI**（`-fmodules-reduced-bmi`）：BMI 只包含接口信息，不包含实现细节，更小、更少级联。

## 4. 架构设计建议

### 4.1 构建后端抽象层

当前 `Backend` 接口已经有抽象，但实际只有 NinjaBackend。建议扩展：

```
Backend (abstract)
├── NinjaBackend          (当前，GCC + Ninja)
├── NinjaClangBackend     (未来，Clang + Ninja，两阶段编译)
├── MSBuildBackend        (未来，MSVC)
└── DirectBackend         (未来，无 ninja，mcpp 直接调度编译)
```

### 4.2 Scanner 抽象层

```
ModuleScanner (abstract)
├── RegexScanner          (当前，快速但不精确)
├── P1689Scanner          (当前，GCC -fdeps-format=p1689r5)
├── ClangScanDepsScanner  (未来，clang-scan-deps)
└── CachedScanner         (装饰器，缓存上一次结果，增量更新)
```

### 4.3 BMI 管理层

```
BmiManager
├── ProjectBmiCache       (per-project target/ 目录)
├── GlobalBmiCache        (当前 $MCPP_HOME/bmi/，跨项目共享)
├── BmiRestatHelper       (copy_if_different + restat 机制)
└── BmiContentHash        (未来，基于 BMI 内容哈希而非时间戳)
```

### 4.4 工具链抽象层

```
Toolchain
├── GccToolchain          (当前，GCC 16.1)
├── ClangToolchain        (未来)
├── MsvcToolchain         (未来)
└── ToolchainCache        (缓存 resolve 结果)
```

## 5. 优先级建议

| 优先级 | 策略 | 预期收益 | 实现复杂度 |
|---|---|---|---|
| P0 | 前端快速脏检查 | 无改动 10s → <0.5s | 低 |
| P1 | per-file dyndep | 改一文件 21s → ~3s | 中 |
| P2 | BMI restat + copy_if_different | 改实现不改接口 → 0 级联 | 低 |
| P3 | 增量 scanner | scanner 耗时减少 80%+ | 中 |
| P4 | 工具链 resolve 缓存 | 减少 1-2s 启动开销 | 低 |
| P5 | Clang 两阶段编译支持 | 并行度提升，减少级联 | 高 |

## 6. 业界参考

| 构建系统 | 模块编译策略 | 增量方案 |
|---|---|---|
| CMake 3.28+ | per-file scan + per-target collation dyndep | restat + copy_if_different |
| build2 | GCC module mapper 协议（编译时动态发现依赖） | 无需 scan 阶段 |
| xmake | 编译器原生 scan + jobgraph 并行 | 增量 scan |

## 7. 总结

mcpp 的模块构建基础架构是正确的（三阶段 dyndep pipeline + BMI 缓存 + 指纹隔离），但在增量编译效率上有显著优化空间。最大的两个 win 是：

1. **前端脏检查**（P0）— 即刻将无改动场景从 10s 降到 <0.5s
2. **per-file dyndep**（P1）— 将单文件修改场景从 21s 降到 ~3s

这两个优化不影响正确性，不需要改变架构，可以增量实施。
