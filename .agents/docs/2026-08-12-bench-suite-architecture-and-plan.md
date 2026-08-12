# `bench/` 构建引擎基准套件 —— 架构与实施计划

> 2026-08-12
> 前置分析:[2026-08-12-modular-build-performance-deep-analysis.md](./2026-08-12-modular-build-performance-deep-analysis.md)
> 目标:把一次性的对比脚本,变成一套**可复用、跨平台、可扩展**的构建引擎基准。

---

## 0. 为什么要重做一遍

上一轮分析用的一次性脚本(bash + hyperfine)能回答"mcpp 和 xmake 谁快",但它有四个结构性缺陷,直接决定了它不能长期用下去:

| 缺陷 | 后果 |
|---|---|
| 只支持 2 个引擎,加第 3 个要改 `run.sh` 主体 | 每加一个对比对象都动核心逻辑 |
| bash + hyperfine | **Windows 上跑不了**;而 mcpp 是三平台产品 |
| 被测对象只有 mcpp 自己 | 无法回答"模块化 vs 头文件"这个真正的问题 |
| 结果是 TSV,字段随手加 | 跨机器/跨时间的数据无法可靠合并 |

新套件按四个角度设计:**优雅(加引擎=加一个文件)、架构稳定(协议与实现解耦)、兼容(旧数据可读)、跨平台(不依赖 shell)**。

---

## 1. 顶层结构

```
bench/                              ← 顶层目录,与 src/ tests/ docs/ 平级
  README.md                         基准规范(可复用的那份文档)
  mcpp.toml                         基准工具本身就是一个 mcpp 工程
  src/
    main.cpp
    protocol.cppm                   ★ 协议:结果 schema / 版本 / 序列化
    spec.cppm                       矩阵与场景定义(数据,不是代码)
    runner.cppm                     计时循环:预热、重复、中位数
    registry.cppm                   引擎注册表
    engines/
      engine.cppm                   适配器契约
      mcpp.cppm  cmake.cppm  xmake.cppm  meson.cppm  bazel.cppm
    fixture/
      generate.cppm                 同一工程 → 头文件版 / 模块版
      emit_buildfiles.cppm          为每个引擎生成构建描述
    analysis/
      ninjalog.cppm graph.cppm report.cppm     构建剖析(--analyze)
    platform.cppm                   门面(主模块,export import 各分区)
    platform/
      posix.cppm                    分区:整文件宏控,非 POSIX 上不导出任何符号
      windows.cppm                  分区:同上
  results/                          结果 + NOTES.md
```

**为什么基准工具本身用 mcpp 写**:它要在 Linux/macOS/Windows 上跑同一套逻辑。bash 在 Windows 上不可用,hyperfine 需要额外安装,而 mcpp 是本仓库必然存在的东西。**用 mcpp 构建 mcpp 的基准工具,顺带也是一次 dogfooding。**

---

## 2. 协议模块(`bench.protocol`)—— 架构稳定性的锚点

这是整套设计里唯一"必须先定、之后不能随便改"的东西。

```cpp
export module bench.protocol;

// 结果 schema 的版本。字段增删必须动它,读取侧据此决定兼容策略。
export inline constexpr int kProtocolVersion = 1;

export struct HostInfo {      // 结果只有配上宿主才有意义
    std::string os, arch, cpu_model;
    int  logical_cores{}, physical_cores{};
    bool heterogeneous{};     // 13900K 的 8P+16E 不能当 24 个同构核读
    std::uint64_t ram_bytes{};
};

export struct CellKey {       // 一个测量单元的完整坐标
    std::string engine, compiler, profile, scenario, fixture, variant;
};

export struct Sample { double wall_s{}; int exit_code{}; };

export struct CellResult {
    CellKey key;
    std::vector<Sample> samples;
    double median_s{}, min_s{}, max_s{};
    std::string status;       // ok | failed | skipped | unavailable
    std::string note;         // 失败或跳过的原因,必填
};
```

**三条不变量**,写死在协议里:

1. **失败不得伪装成数据。** `status` 与 `median_s` 是两个字段;上一轮 `run.sh` 把失败写成 `0.000s`,就是因为没有这一层。
2. **跳过必须带原因。** "bazel 不在这台机器上"和"bazel 跑失败了"是完全不同的结论。
3. **宿主信息与结果同生共死。** 单独一个数字没有意义。

序列化为 JSON,字段名即上面的名字,顶层带 `protocol_version`。

---

## 3. 引擎适配器契约

```cpp
export struct Engine {
    virtual ~Engine() = default;
    virtual std::string_view name() const = 0;
    // 这台机器上有没有?没有就 unavailable,不是 failed。
    virtual Availability probe() const = 0;
    // 是否支持这个 fixture 变体(headers / modules)
    virtual bool supports(Variant) const = 0;
    virtual Result configure(const Job&) const = 0;
    virtual Result build(const Job&) const = 0;
    virtual Result clean(const Job&) const = 0;
};
```

**加一个引擎 = 新增一个 `engines/<name>.cppm` + 在 `registry.cppm` 注册一行。** 不动 runner、不动协议、不动 CI。

`supports(Variant)` 是必要的:并非所有引擎都支持 C++20 模块(bazel 的模块支持仍很有限),此时应报 `unavailable` 并说明,而不是硬跑出一个误导性的数字。

---

## 4. Fixture:同一工程的两种形态

**生成而非手写。** 手写两份"等价"的代码,几乎必然在某处不等价,而那正是被测量的东西。

生成器参数:单元数 `N`、依赖深度 `D`、每单元代码量 `L`。产出:

```
fixtures/synth-<N>x<D>/
  headers/    include/unit_k.hpp + src/unit_k.cpp     (传统头文件 + 分离实现)
  modules/    src/unit_k.cppm                          (模块接口单元)
  modules-impl/ src/unit_k.cppm + src/unit_k_impl.cpp  (接口 + 实现单元 ★)
```

第三种变体直接对应上一轮分析的 **F4 / §6.3**:把实现移出接口单元。有了它,"改一行函数体"的代价差异就是**测出来的**,不是推断的。

同时保留 `self` fixture —— 即 mcpp 自身(137 模块),因为真实工程的依赖形状不是合成器能编出来的。

---

## 5. 场景矩阵

| 维度 | 取值 |
|---|---|
| engine | mcpp, mcpp-opt(优化后), cmake, xmake, meson, bazel |
| variant | headers, modules, modules-impl |
| profile | release, debug |
| scenario | cold, noop, touch-hub, edit-body, touch-leaf |
| compiler | gcc, clang, msvc(平台可用者) |

**`mcpp` vs `mcpp-opt`**:同一份源码、同一编译器,区别只在是否启用上一轮验证过的优化(BMI 时间戳归一 + BMI 落盘即释放)。这让"优化前后"成为矩阵里的**一个正交维度**,而不是另做一次实验。

矩阵是笛卡尔积但**不是全跑**:`spec.cppm` 用显式的 include/exclude 规则裁剪,CI 默认跑一个小集合,`workflow_dispatch` 可放开。

---

## 6. 平台拆分

采用 **xlings `src/platform/*.cppm` 的既定约定**:模块分区 + 整文件宏控。

| 关注点 | 位置 |
|---|---|
| 进程启动 + 墙钟计时 + 退出码 | `platform/posix.cppm`、`platform/windows.cppm` |
| CPU 型号 / 核数 / 异构判定 | 同上 |
| 环境变量读写 | 同上(`setenv` vs `SetEnvironmentVariableA`) |
| 组装与可移植部分(std::filesystem) | 主模块 `platform.cppm` |

每个分区把**整个 body** 包在一个宏里,非目标平台**不导出任何符号**;两侧导出同名函数,于是任一构建中每个名字只有一份定义,**编译期自动选中**——不需要 stub,也不需要 `if constexpr` 派发。主模块 `export import :posix; :windows;` 后用 `export using` 提升。

结果:`#if defined(_WIN32)` 只出现在这两个分区里,runner / engines / protocol / fixture 全部零平台条件。

---

## 7. CI

新增 `.github/workflows/bench.yml`:

- `on: workflow_dispatch`(**只手动触发** —— 基准是重活,不该挂在每个 PR 上)
- 输入:`engines`、`scenarios`、`variants`、`fixture_size`、`runs`
- 矩阵:`ubuntu-24.04` × `macos-14` × `windows-2022`,各自的默认工具链
- 产出:上传 `results/*.json` 为 artifact
- **不设阈值断言**:基准用于观察趋势,不用于 gate。把噪声变成红叉只会让人忽略它。

---

## 8. 实施阶段

| 阶段 | 内容 | 完成判据 |
|---|---|---|
| **A** | `bench/` 骨架:protocol + platform + runner + registry + mcpp 引擎 | 三平台能跑 `bench --engine mcpp --scenario cold --fixture self` 并产出合法 JSON |
| **B** | fixture 生成器(headers / modules / modules-impl) | 三个变体编译产物行为一致(同一断言集通过) |
| **C** | cmake / xmake / meson / bazel 适配器 | 缺失工具报 `unavailable` 且带原因,不是崩溃 |
| **D** | 构建剖析并入 `--analyze` + 结果合并 | 关键路径与 Python 实现交叉验证一致 |
| **E** | `bench.yml` CI | 手动触发在三平台跑通并上传 artifact |
| **F** | 文档 / 测试 / 版本 / PR / 验证 / 合入 / 发布 | 见目标清单 |

**顺序是有依赖的**:A 定协议,之后所有阶段都写向它;B 之前 C 无处可跑;D 依赖 A 的结果格式。

---

## 9. 明确不做

- **不把基准挂进 PR CI**。噪声会淹没信号。
- **不设性能回归阈值**。宿主差异(异构 CPU、云厂商邻居噪声)远大于多数真实回归。
- **不重新实现计时统计学**。中位数 + min/max 足够;不做置信区间,因为样本量本来就小。
- **不追求引擎功能对等**。bazel 不支持模块就报 unavailable —— 强行凑一个数字比没有数字更糟。
