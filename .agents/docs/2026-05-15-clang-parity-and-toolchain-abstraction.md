# Clang 编译管线平权 + 工具链抽象层设计

> 2026-05-15 — 让 Linux Clang 达到与 GCC 同等的模块编译支持，并建立平台/工具链抽象为 macOS/Windows 铺路
> 基于 mcpp 0.0.15 代码库分析

## 0. 一句话

当前 Clang 能跑 `import std` 的单文件项目，但**多模块项目的整条核心管线（dyndep、BMI 路径、缓存布局、编译规则）都是按 GCC 硬编码的**。本文设计一层 `ToolchainTraits` 抽象，让 GCC/Clang 共享管线骨架、各自提供参数，同时为未来 MSVC/macOS 留好接口。

## 1. 差距总表（12 个 Blocker）

| # | 文件 | 行号 | 问题 | 类型 |
|---|---|---|---|---|
| G1 | `ninja_backend.cppm` | 129–130 | `dyndep` 被 `is_gcc()` 门控，Clang 直接回退到全量静态依赖 | Blocker |
| G2 | `ninja_backend.cppm` | 187–198 | `cxx_module` 规则缺少 Clang 必须的 `-fmodule-output=$bmi_out` | Blocker |
| G3 | `ninja_backend.cppm` | 234–239 | `cxx_scan` 规则传 `-fmodules` 给 Clang（GCC 专属旗标） | 中等 |
| G4 | `ninja_backend.cppm` | 260–265 | `bmi_path` lambda 硬编码 `gcm.cache/` + `.gcm` 扩展名 | Blocker |
| G5 | `flags.cppm` | — | 缺 Clang 必须的 `-fprebuilt-module-path=pcm.cache` | Blocker |
| G6 | `bmi_cache.cppm` | 41, 145, 230 | `gcmDir()` / `projectGcm` 硬编码 `gcm.cache/` | Blocker |
| G7 | `bmi_cache.cppm` | 47, 94–98, 110 | manifest 前缀 `gcm:` 不适用 Clang | Blocker |
| G8 | `dyndep.cppm` | 152–157 | `bmi_basename()` 硬编码 `.gcm` 扩展名 | Blocker |
| G9 | `dyndep.cppm` | 258, 307 | dyndep emit 硬编码 `gcm.cache/` 目录 | Blocker |
| G10 | `dyndep.cppm` | 289–313 | `emit_dyndep_*` 无工具链参数，无法区分 GCC/Clang | Blocker |
| G11 | `cli.cppm` | 3553–3573 | `cmd_dyndep` 子命令无工具链上下文传递给 dyndep 模块 | Blocker |
| G12 | `modgraph/p1689.cppm` | 342 | `scan_file` 对 Clang 仍传 `-fmodules` | 中等 |

### 已修复（无需改动）

| 功能 | 位置 | 说明 |
|---|---|---|
| `-fmodules` 跳过 Clang | `flags.cppm:127` | `isClang ? "" : " -fmodules"` |
| `-fmodule-file=std=` | `flags.cppm:129-131` | Clang 时注入 std pcm 路径 |
| `-static-libstdc++` 跳过 Clang | `flags.cppm:141` | `!isClang` 条件 |
| `-B<binutils>` 跳过 Clang | `flags.cppm:93-100` | 条件过滤 |
| stdmod 双路径 | `stdmod.cppm:96-128` | GCC/Clang 分别使用各自的 std 构建命令 |
| std BMI 路径分支 | `clang.cppm:127-133` | `pcm.cache/std.pcm` |
| std 构建两步流程 | `clang.cppm:135-159` | `--precompile` + `-c` |
| Clang 检测 | `clang.cppm:64-69` | 识别 `clang version` 和 `apple clang version` |

## 2. 核心设计：`ToolchainTraits`

### 2.1 设计目标

1. GCC/Clang **共享同一套** ninja 规则生成、dyndep 发射、BMI 缓存管线骨架
2. 差异点通过统一接口 **参数化**，不在调用侧 `if/else`
3. 接口为将来的 MSVC / Apple Clang 预留扩展点

### 2.2 接口定义

在 `src/toolchain/model.cppm` 新增：

```cpp
// BMI layout traits — 编译器间差异的单一聚合点
struct BmiTraits {
    std::string_view bmiDir;     // "gcm.cache" | "pcm.cache"
    std::string_view bmiExt;     // ".gcm"      | ".pcm"
    std::string_view manifestPrefix; // "gcm"   | "pcm"

    // 编译 module interface unit 时，是否需要显式 -fmodule-output=<path>
    bool needsExplicitModuleOutput = false;

    // 是否需要 -fprebuilt-module-path=<dir>
    bool needsPrebuiltModulePath = false;

    // P1689 扫描时是否需要 -fmodules 标志
    bool scanNeedsFModules = true;
};

BmiTraits bmi_traits(const Toolchain& tc);
```

实现（可在 `model.cppm` 底部）：

```cpp
BmiTraits bmi_traits(const Toolchain& tc) {
    if (is_clang(tc)) {
        return {
            .bmiDir     = "pcm.cache",
            .bmiExt     = ".pcm",
            .manifestPrefix = "pcm",
            .needsExplicitModuleOutput = true,
            .needsPrebuiltModulePath   = true,
            .scanNeedsFModules         = false,
        };
    }
    // GCC (default)
    return {
        .bmiDir     = "gcm.cache",
        .bmiExt     = ".gcm",
        .manifestPrefix = "gcm",
        .needsExplicitModuleOutput = false,
        .needsPrebuiltModulePath   = false,
        .scanNeedsFModules         = true,
    };
}
```

### 2.3 为什么不用继承/虚函数

`BmiTraits` 是 **值类型聚合**，因为：
- 消费者（ninja_backend、dyndep、bmi_cache）只需要几个字符串/布尔值；
- 不需要多态行为，避免虚表开销；
- 未来 MSVC 只需 return 第三组值，无需新类；
- 所有差异在编译时可知，仅 `model.cppm` 一处 switch。

## 3. 逐模块改造方案

### 3.1 `ninja_backend.cppm` — Ninja 规则生成

#### 3.1.1 放开 dyndep 门控（G1）

```cpp
// Before (line 129-130)
bool dyndep = dyndep_mode_enabled()
           && mcpp::toolchain::is_gcc(plan.toolchain);

// After
bool dyndep = dyndep_mode_enabled();
// dyndep 支持 GCC 16+ 和 Clang 18+，两者都实现了 P1689
// MSVC 暂不支持 → 未来在此加 && !is_msvc()
```

#### 3.1.2 参数化 `bmi_path` lambda（G4）

```cpp
auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
auto bmi_path = [&traits](std::string_view name) {
    std::string s(traits.bmiDir);
    s += '/';
    for (char c : name) s.push_back(c == ':' ? '-' : c);
    s += traits.bmiExt;
    return s;
};
```

#### 3.1.3 分离 GCC/Clang 的 `cxx_module` 规则（G2）

思路：不拆成两个 rule，而是在 rule command 中用 `$module_output_flag` 变量：

```cpp
// Clang 需要 -fmodule-output=$bmi_out，GCC 不需要（隐式写 gcm.cache/）
std::string module_output_in_rule = traits.needsExplicitModuleOutput
    ? " -fmodule-output=$bmi_out" : "";

append("rule cxx_module\n");
append(std::format(
    "  command = "
    "if [ -n \"$bmi_out\" ] && [ -f \"$bmi_out\" ]; then "
      "cp -p \"$bmi_out\" \"$bmi_out.bak\"; "
    "fi && "
    "$toolenv $cxx $cxxflags{} -c $in -o $out && "
    "if [ -n \"$bmi_out\" ] && [ -f \"$bmi_out.bak\" ] && "
       "cmp -s \"$bmi_out\" \"$bmi_out.bak\"; then "
      "mv \"$bmi_out.bak\" \"$bmi_out\"; "
    "else "
      "rm -f \"$bmi_out.bak\"; "
    "fi\n",
    module_output_in_rule));
```

#### 3.1.4 `cxx_scan` 规则条件化 `-fmodules`（G3）

```cpp
std::string scan_modules_flag = traits.scanNeedsFModules ? "-fmodules " : "";
append(std::format(
    "rule cxx_scan\n"
    "  command = $toolenv $cxx $cxxflags {}-fdeps-format=p1689r5 "
    "-fdeps-file=$out -fdeps-target=$compile_target "
    "-M -MM -MF $out.dep -E $in -o $compile_target\n"
    "  description = SCAN $out\n\n",
    scan_modules_flag));
```

### 3.2 `flags.cppm` — 编译 flag 生成

#### 3.2.1 新增 `-fprebuilt-module-path`（G5）

```cpp
auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
std::string prebuilt_module_flag;
if (traits.needsPrebuiltModulePath) {
    prebuilt_module_flag = std::format(" -fprebuilt-module-path={}", traits.bmiDir);
}

// 拼入 cxxflags:
f.cxx = std::format("-std={}{}{}{}{}{}{}{}{}", ...
                    prebuilt_module_flag, ...);
```

### 3.3 `dyndep.cppm` — 动态依赖发射

#### 3.3.1 `bmi_basename` 参数化（G8）

```cpp
// Before
std::string bmi_basename(std::string_view logicalName) {
    ...
    out += ".gcm";
    return out;
}

// After
std::string bmi_basename(std::string_view logicalName,
                          std::string_view ext = ".gcm") {
    std::string out;
    out.reserve(logicalName.size() + ext.size());
    for (char c : logicalName) out.push_back(c == ':' ? '-' : c);
    out += ext;
    return out;
}
```

#### 3.3.2 `emit_dyndep_single` / `emit_dyndep_from_files` 接受 traits（G9, G10）

```cpp
// Before (函数签名)
std::string emit_dyndep_single(const std::filesystem::path& ddi);
std::string emit_dyndep_from_files(std::span<const std::string> ddi_paths, ...);

// After
struct DyndepOptions {
    std::string_view bmiDir = "gcm.cache";  // default GCC
    std::string_view bmiExt = ".gcm";
};
std::string emit_dyndep_single(const std::filesystem::path& ddi,
                                const DyndepOptions& opts = {});
std::string emit_dyndep_from_files(std::span<const std::string> ddi_paths,
                                    ...,
                                    const DyndepOptions& opts = {});
```

内部所有 `"gcm.cache/"` 和 `.gcm` 替换为 `opts.bmiDir` / `opts.bmiExt`。

#### 3.3.3 `cmd_dyndep` 传递工具链信息（G11, G12）

**问题**：`cmd_dyndep` 是 Ninja 在构建时调用的子命令（`mcpp dyndep --single`），运行在 ninja 进程内，**不经过 prepare_build**，无法访问 `BuildPlan` 对象。

**方案**：通过命令行参数 `--bmi-dir` / `--bmi-ext` 传入：

Ninja 规则侧（`ninja_backend.cppm` 中 `cxx_dyndep` 规则）：

```cpp
// Before
append("rule cxx_dyndep\n");
append("  command = $mcpp dyndep --single --output $out $in\n");

// After
append(std::format(
    "rule cxx_dyndep\n"
    "  command = $mcpp dyndep --single --bmi-dir {} --bmi-ext {} --output $out $in\n",
    traits.bmiDir, traits.bmiExt));
```

`cli.cppm` 的 `cmd_dyndep` 解析这两个新参数并构造 `DyndepOptions`：

```cpp
int cmd_dyndep(const ParsedArgs& parsed) {
    DyndepOptions opts;
    if (auto d = parsed.value("bmi-dir"))  opts.bmiDir = *d;
    if (auto e = parsed.value("bmi-ext"))  opts.bmiExt = *e;
    ...
    body = emit_dyndep_single(ddi, opts);
}
```

### 3.4 `bmi_cache.cppm` — 跨项目 BMI 缓存

#### 3.4.1 `CacheKey` 感知 BMI 布局（G6, G7）

```cpp
struct CacheKey {
    ...existing fields...
    std::string bmiDirName   = "gcm.cache"; // "gcm.cache" | "pcm.cache"
    std::string manifestTag  = "gcm";       // "gcm" | "pcm"

    std::filesystem::path bmiDir() const { return dir() / bmiDirName; }
    // 原来的 gcmDir() 改为 bmiDir()
};
```

#### 3.4.2 Manifest 格式升级

```cpp
// serialize_manifest：使用 key.manifestTag
std::string serialize_manifest(const CacheKey& key, const DepArtifacts& a) {
    std::string out = "# Auto-generated by mcpp bmi_cache. Do not edit.\n";
    for (auto& g : a.bmiFiles) out += std::format("{}: {}\n", key.manifestTag, g);
    for (auto& o : a.objFiles) out += std::format("obj: {}\n", o);
    return out;
}

// parse_manifest：同时接受 "gcm:" 和 "pcm:" 前缀
std::expected<DepArtifacts, std::string> parse_manifest(...) {
    ...
    if (line.starts_with("gcm: "))      a.bmiFiles.push_back(line.substr(5));
    else if (line.starts_with("pcm: ")) a.bmiFiles.push_back(line.substr(5));
    else if (line.starts_with("obj: ")) a.objFiles.push_back(line.substr(5));
}
```

`DepArtifacts::gcmFiles` 重命名为 `bmiFiles`（语义中性化）。

#### 3.4.3 `stage_into` / `populate_from` 使用 `bmiDir()`

全部 `projectGcm` 替换为 `projectBmi`，由 `projectTargetDir / key.bmiDirName` 生成。

### 3.5 `modgraph/p1689.cppm` — 扫描命令（G12）

```cpp
// scan_file 中的 -fmodules 条件化
std::string modules_flag = mcpp::toolchain::is_clang(tc) ? "" : " -fmodules";
std::string cmd = std::format(
    "{} -std=c++23{}{} -fdeps-format=p1689r5 ...",
    shq(tc.binaryPath.string()), modules_flag, sysroot_flag, ...);
```

## 4. 目录布局变化

### 4.1 构建目录

```
target/<triple>/<fingerprint>/
├── build.ninja
├── gcm.cache/          ← GCC: BMI 文件 (.gcm)
│   ├── std.gcm
│   └── myapp.greet.gcm
├── pcm.cache/          ← Clang: BMI 文件 (.pcm)
│   ├── std.pcm
│   └── myapp.greet.pcm
├── obj/
│   ├── greet.m.o
│   └── main.o
└── bin/
    └── myapp
```

GCC 和 Clang 的构建**各自使用不同的 fingerprint 目录**（因为编译器不同 → fingerprint 不同），所以 `gcm.cache/` 和 `pcm.cache/` 不会在同一个目录下共存。但用中性化命名可以让代码统一：

**方案 A**（推荐）：保留各自命名 `gcm.cache` / `pcm.cache`，通过 `BmiTraits.bmiDir` 参数化。好处：与 GCC/Clang 的约定一致，开发者能一眼看出工具链。

**方案 B**：统一用 `bmi/` 目录。好处：代码更简单。坏处：丢失工具链信息，且 GCC 硬编码寻找 `gcm.cache/`（需要 `-fmodule-mapper` 覆盖）。

**选择方案 A**。

### 4.2 BMI 缓存目录

```
$MCPP_HOME/bmi/<fingerprint>/deps/<index>/<pkg>@<ver>/
├── gcm.cache/  或  pcm.cache/   ← 由 CacheKey.bmiDirName 决定
│   └── *.gcm 或 *.pcm
├── obj/
│   └── *.m.o
└── manifest.txt                  ← "gcm: ..." 或 "pcm: ..."
```

## 5. 实施计划

### Phase 1：核心管线（1 个 PR，必须）

| 步骤 | 改动文件 | 内容 |
|---|---|---|
| 1a | `model.cppm` | 新增 `BmiTraits` + `bmi_traits()` |
| 1b | `ninja_backend.cppm` | 放开 dyndep 门控；`bmi_path` 参数化；`cxx_module` 加 `-fmodule-output`；`cxx_scan` 条件化 `-fmodules`；`cxx_dyndep` 规则传 `--bmi-dir --bmi-ext` |
| 1c | `flags.cppm` | 加 `-fprebuilt-module-path=<bmiDir>` |
| 1d | `dyndep.cppm` | `bmi_basename(name, ext)` 参数化；`emit_dyndep_*` 接受 `DyndepOptions`；硬编码路径替换 |
| 1e | `cli.cppm` | `cmd_dyndep` 解析 `--bmi-dir`/`--bmi-ext` |
| 1f | `modgraph/p1689.cppm` | `scan_file` 条件化 `-fmodules` |

### Phase 2：BMI 缓存（1 个 PR）

| 步骤 | 改动文件 | 内容 |
|---|---|---|
| 2a | `bmi_cache.cppm` | `gcmDir()` → `bmiDir()`；`gcmFiles` → `bmiFiles`；manifest 双前缀兼容 |
| 2b | `cli.cppm` | `prepare_build` 中构造 `CacheKey` 时设置 `bmiDirName` / `manifestTag` |

### Phase 3：E2E 验证（1 个 PR）

| 步骤 | 改动文件 | 内容 |
|---|---|---|
| 3a | `tests/e2e/38_llvm_modules.sh` | 多模块 Clang 项目：`greet.cppm` + `main.cpp`（`import myapp.greet`），验证编译+运行 |
| 3b | `tests/e2e/39_llvm_incremental.sh` | Touch 单个 .cppm → 只重编该文件（增量验证） |
| 3c | `tests/e2e/40_llvm_bmi_cache.sh` | 依赖包的 BMI 缓存命中（Clang 路径） |

### Phase 4：平台抽象预留（后续）

在 `BmiTraits` 基础上，未来只需：
- macOS Apple Clang → 沿用 `is_clang` 分支（BMI 格式相同）
- Windows MSVC → 新增 `is_msvc` 分支，返回 `.ifc` 扩展名 + MSVC 专有 flag 集

## 6. GCC vs Clang 编译命令对比速查

### P1689 扫描

```bash
# GCC
g++ -std=c++23 -fmodules \
    -fdeps-format=p1689r5 -fdeps-file=foo.ddi -fdeps-target=foo.o \
    -M -MM -MF foo.dep -E src/foo.cppm -o foo.o

# Clang (相同标志集, 去掉 -fmodules)
clang++ -std=c++23 \
    -fdeps-format=p1689r5 -fdeps-file=foo.ddi -fdeps-target=foo.o \
    -M -MM -MF foo.dep -E src/foo.cppm -o /dev/null
```

### 模块编译

```bash
# GCC — 隐式写 gcm.cache/myapp.greet.gcm
g++ -std=c++23 -fmodules -c src/greet.cppm -o obj/greet.m.o

# Clang — 显式指定 pcm 输出
clang++ -std=c++23 \
    -fprebuilt-module-path=pcm.cache \
    -fmodule-output=pcm.cache/myapp.greet.pcm \
    -c src/greet.cppm -o obj/greet.m.o
```

### 消费模块

```bash
# GCC — 自动从 cwd/gcm.cache/ 查找
g++ -std=c++23 -fmodules -c src/main.cpp -o obj/main.o

# Clang — 需要 -fprebuilt-module-path
clang++ -std=c++23 \
    -fprebuilt-module-path=pcm.cache \
    -c src/main.cpp -o obj/main.o
```

### std 模块预编译

```bash
# GCC — 单步，隐式产出 gcm.cache/std.gcm + std.o
g++ -std=c++23 -fmodules -c bits/std.cc -o std.o

# Clang — 两步
clang++ -std=c++23 --precompile std.cppm -o pcm.cache/std.pcm
clang++ -std=c++23 pcm.cache/std.pcm -c -o std.o
```

## 7. 验收指标

- [ ] `mcpp new hello && cd hello && mcpp build` 在 `[toolchain] default = "llvm@20"` 下成功
- [ ] 多模块项目（`greet.cppm` + `main.cpp`）在 Clang 下编译+运行正确
- [ ] `touch greet.cppm && mcpp build` 只重编 `greet` + link（dyndep 增量）
- [ ] `mcpp test` 在 Clang 工具链下通过
- [ ] BMI 缓存命中：第二次 build 显示 `Cached` 而非 `Compiling`
- [ ] 所有现有 GCC E2E 测试继续通过（无回归）
- [ ] `--print-fingerprint` 对 GCC 和 Clang 产出不同的 fingerprint

## 8. 不应该做的事

1. **不要引入虚函数/继承**。`BmiTraits` 作为值类型聚合足够，避免 vtable 开销和继承层次膨胀。
2. **不要统一目录名为 `bmi/`**。GCC 硬编码寻找 `gcm.cache/`，除非用 `-fmodule-mapper` 覆盖（增加复杂度）。
3. **不要在 flags.cppm 里用 `if(isGCC) ... else if(isClang) ...`**。用 `BmiTraits` 的布尔字段驱动，消费者不感知编译器类型。
4. **不要合并 GCC 和 Clang 的 std 模块构建流程**。两者本质不同（单步 vs 两步），`stdmod.cppm` 的分支是正确的。
5. **不要在这个 PR 里动 macOS/Windows 的事**。只做 Linux Clang 平权 + 抽象接口预留。

## 9. 关联

- 前置 PR：mcpp-community/mcpp#33（fingerprint 稳定化，已合入）
- 上一份分析：`2026-05-15-fingerprint-stability-and-fastpath-coherence.md`
- LLVM 设计文档：`2026-05-13-llvm-clang-toolchain-support-design.md`
- 跨平台分析报告：见本会话聊天记录（macOS 6 个 blocker、Windows 10+ 个 blocker）
