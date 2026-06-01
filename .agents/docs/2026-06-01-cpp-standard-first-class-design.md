# C++ 标准一等配置设计

> 日期: 2026-06-01
> 状态: draft
> 范围: 让 `standard = "c++26"` 这类语言标准配置贯通 manifest、fingerprint、编译 flags、`import std` BMI cache、CDB 和文档。

## 0. 结论

这不是单个 `-std=c++23` 硬编码点的问题，而是 mcpp 同时存在四套没有闭环的语言标准表达:

1. manifest 有 `[package].standard` 和旧 `[language].standard` 两个入口。
2. fingerprint 有 `cppStandard` 字段，但构建命令没有真正消费同一个值。
3. backend 仍把 C++ 标准当作全局 flags 字符串的一部分，并且把用户 `cxxflags` 同时放进全局和 compile unit。
4. `import std` BMI cache 只看产物是否存在，构建命令和 cache 元数据都没有记录实际 C++ 标准。

合理修法是把 C++ 标准提升为一个归一化后的 build graph 属性: manifest 只负责解析和兼容旧字段，prepare/build plan 只传递一个已归一的 active standard，所有 compiler flag、fingerprint、std module build、cache metadata 和文档都消费这个同一个值。

## 1. 现状证据

### 1.1 manifest 只完成了解析入口

- `src/manifest.cppm:23-28`: `Package::standard` 默认是 `c++23`。
- `src/manifest.cppm:34-38`: `Language::standard` 也默认是 `c++23`。
- `src/manifest.cppm:319-331`: `[package].standard` 是新 home，旧 `[language].standard` 会镜像到 package。
- `src/manifest.cppm:335-339`: 统一后的校验仍只允许 `c++23` 和 `c++latest`，错误信息仍说 MVP only supports c++23。

这说明 schema 已经有新字段，但合法值、归一化、调用侧消费还没有完成。

### 1.2 fingerprint 有字段，但不等于构建命令闭环

- `src/toolchain/fingerprint.cppm:23-29`: `FingerprintInputs::cppStandard` 默认 `c++23`。
- `src/toolchain/fingerprint.cppm:101`: fingerprint 第 6 字段记录 `cppStandard`。
- `src/cli.cppm:2505-2512`: build 准备阶段把 `m->language.standard` 写入 fingerprint。
- `src/cli.cppm:584-604`: canonical compile flags 字符串也拼了 `-std=<m.language.standard>`。

问题是实际 C++ 编译命令不读这个值，fingerprint 记录和真实 compiler dialect 可以分裂。

### 1.3 编译 flags 仍硬编码 C++23

- `src/build/flags.cppm:221-224`: C 标准已经有 `build.c_standard` 模型。
- `src/build/flags.cppm:254-256`: C++ 规则仍硬编码 `-std=c++23`，用户只能用 `build.cxxflags = ["-std=c++26"]` 追加覆盖。

这会产生两个问题:

1. 标准配置没有一等入口，只能绕进附加 flags。
2. 当 `cxxflags` 后追加 `-std=c++26` 时，fingerprint 和 std module cache 仍可能以 `c++23` 的世界观工作。

### 1.4 `import std` BMI 构建命令固定 C++23

- `src/toolchain/stdmod.cppm:46-49`: `ensure_built()` 只接收 toolchain、fingerprint 和 cache root。
- `src/toolchain/stdmod.cppm:149-177`: 复用判断只检查 BMI 和 object 是否存在。
- `src/toolchain/gcc.cppm:105-120`: GCC std module build command 固定 `-std=c++23`。
- `src/toolchain/clang.cppm:174-228`: Clang std module build command 也固定 `-std=c++23`。
- `src/toolchain/clang.cppm:265-296`: `std.compat` build command 同样固定 `-std=c++23`。

所以项目源文件最终用 C++26 编译时，GCC/Clang 仍可能读到 C++23 预编译出的 std BMI。GCC 报 `language dialect differs 'C++23', expected 'C++26/contracts'` 是这个裂缝的直接结果。

### 1.5 package-owned flags 已经是 per-unit 方向，但主包仍重复

- `src/modgraph/scanner.cppm:376-379`: scanner 把每个 package 的 `cflags` / `cxxflags` 放到 compile unit。
- `src/build/ninja_backend.cppm:475-476`, `src/build/ninja_backend.cppm:523-527`, `src/build/ninja_backend.cppm:570-574`: Ninja 再把 per-unit flags 写入 `unit_cxxflags` / `unit_cflags`。
- `src/build/compile_commands.cppm:120-125`: CDB 也追加 per-unit package flags。
- `src/build/flags.cppm:213-214`, `src/build/flags.cppm:254-258`: root manifest 的 `cxxflags` / `cflags` 仍被加入全局 baseline。

这导致主包 flags 可能在 `cxxflags` 和 `unit_cxxflags` 中各出现一次。对 `-std=` 这种有顺序语义的 flag 来说，重复不仅是噪音，还会掩盖标准模型缺失。

### 1.6 文档落后于实现

- `docs/05-mcpp-toml.md:40-48`: `[package]` 示例没有 `standard`。
- `docs/05-mcpp-toml.md:67-78`: `[build]` 只写了 `c_standard`，没有 C++ 标准字段说明。
- `docs/05-mcpp-toml.md:281-290`: 默认值表只说标准默认 `c++23`，没有说明怎么配置。

用户自然会把 C++ 标准塞进 `cxxflags`，因为文档没有给出一等配置路径。

## 2. 设计目标

1. `standard = "c++26"` 是公开、稳定、文档化的配置，不再需要 `cxxflags = ["-std=..."]`。
2. 同一次 build graph 中，所有 C++ compile unit、module scan、std BMI 构建、CDB 都使用同一个 active C++ standard。
3. std module cache 复用前验证真实构建元数据，而不是只看 `std.gcm` / `std.pcm` 是否存在。
4. fingerprint 使用归一化标准值，避免 `c++2c` 和 `c++26` 这类别名产生无意义缓存分裂。
5. `cxxflags` 回归到附加编译参数语义，不承担语言标准选择职责。
6. 保持旧 `[language].standard` 和 xpkg Lua `language = ...` 兼容，但内部尽快归一到新模型。

## 3. 非目标

1. 不在本轮引入每个 package 独立 C++ 标准。C++ module BMI 和 `import std` 的 dialect 兼容性要求太强，第一阶段应采用 graph-wide active standard。
2. 不把 `c++latest` 变成可复现语义。`latest` 本质依赖 compiler 版本，fingerprint 必须包含 compiler 版本和实际 flag spelling。
3. 不重写整个 flags/backend 架构。只建立标准模型和必要去重边界。

## 4. 推荐架构

### 4.1 新增归一化标准模型

建议新增一个小的值类型，放在 `src/manifest.cppm` 或独立模块 `src/build/language.cppm`。如果要避免 manifest 过大，推荐独立模块:

```cpp
struct CppStandard {
    std::string canonical;   // "c++23", "c++26", "gnu++26", "c++latest"
    std::string compilerFlag; // "-std=c++23", "-std=c++2c", ...
    int level = 23;          // 23, 26, or max for latest
    bool gnuDialect = false;
};

std::expected<CppStandard, StandardError>
normalize_cpp_standard(std::string_view raw,
                       const mcpp::toolchain::Toolchain* tc = nullptr);
```

归一化规则:

| 输入 | canonical | 说明 |
|---|---|---|
| `c++23`, `c++2b` | `c++23` | `c++2b` 作为旧拼写兼容 |
| `gnu++23`, `gnu++2b` | `gnu++23` | 保留 GNU dialect，因为它影响 compiler 行为和 BMI |
| `c++26`, `c++2c` | `c++26` | 语义归一，flag spelling 可由工具链层决定 |
| `gnu++26`, `gnu++2c` | `gnu++26` | 保留 GNU dialect |
| `c++latest` | `c++latest` | 允许，但提示其随 compiler 漂移 |

`compilerFlag` 不应简单等于 canonical。不同 GCC/Clang 版本可能接受 `-std=c++2c` 而不是 `-std=c++26`。更通用的方式是让 toolchain 层提供 flag spelling:

```cpp
std::expected<std::string, ToolchainError>
cxx_standard_flag(const Toolchain& tc, const CppStandard& standard);
```

第一阶段可以用映射表实现:

- `c++23` -> `-std=c++23`
- `gnu++23` -> `-std=gnu++23`
- `c++26` -> 优先 `-std=c++26`，必要时 fallback `-std=c++2c`
- `gnu++26` -> 优先 `-std=gnu++26`，必要时 fallback `-std=gnu++2c`

如果要更稳，可以在 toolchain detect 阶段探测可接受的 `-std=` spelling，并把结果缓存进 `Toolchain`。

### 4.2 单一 active standard 流向

推荐数据流:

```text
mcpp.toml / xpkg lua
  -> manifest parse
  -> normalize_cpp_standard()
  -> BuildLanguageConfig
  -> FingerprintInputs.cppStandard
  -> BuildPlan.cppStandard
  -> compute_flags()
  -> stdmod::ensure_built()
  -> ninja + compile_commands
```

关键点:

1. manifest 解析后立即把 `[package].standard` 和旧 `[language].standard` 归一成同一个值。
2. `prepare_build()` 只读取归一后的 active standard，不再在 `package.standard` 与 `language.standard` 之间选择。
3. `BuildPlan` 增加 `cppStandard` 字段，backend 不再回读 manifest 字符串。
4. `canonical_compile_flags()` 和 fingerprint 使用 `cppStandard.canonical`，不是用户原始输入。
5. `compute_flags()` 使用 `cppStandard.compilerFlag`，不再硬编码 `-std=c++23`。

这样 `manifest -> fingerprint -> flags -> std BMI` 形成闭环。

### 4.3 graph-wide 标准策略

第一阶段建议采用 graph-wide active standard:

1. root package 的 `[package].standard` 选择 active standard，默认 `c++23`。
2. dependency package 的 `standard` 表示最低需求，而不是独立 dialect。
3. 如果 dependency 需要的标准高于 root active standard，直接报错:

```text
dependency 'foo' requires c++26, but root package uses c++23.
Set [package].standard = "c++26" in the root manifest.
```

4. 如果 root 是 `c++26`，dependency 默认 `c++23`，依赖也用 `c++26` 编译，保证同一 build graph 的 module BMI dialect 一致。

这个策略比 per-package standard 更保守，但符合当前模块管线和 cache 设计。未来如果要支持 per-package dialect，必须先设计独立 BMI namespace、跨 dialect module import 规则和 cache key，这不应混进本轮修复。

### 4.4 std module cache 元数据

`ensure_built()` 应改为:

```cpp
std::expected<StdModule, StdModError> ensure_built(
    const Toolchain& tc,
    std::string_view fingerprint_hex,
    const CppStandard& cppStandard,
    const std::filesystem::path& cache_root = default_cache_root());
```

std module build command 统一使用 `cppStandard.compilerFlag`:

- GCC: `g++ <std-flag> -fmodules ... bits/std.cc`
- Clang std: `clang++ <std-flag> --precompile std.cppm ...`
- Clang std object: `clang++ <std-flag> std.pcm -c ...`
- Clang std.compat: `clang++ <std-flag> -fmodule-file=std=...`

cache dir 下新增元数据文件，建议名为 `std-module.json`:

```json
{
  "schema": 1,
  "compiler": "gcc",
  "compiler_version": "16.1.0",
  "driver_identity": "hash-or-normalized-driver-ident",
  "target_triple": "x86_64-linux-gnu",
  "stdlib": "libstdc++",
  "stdlib_version": "16.1.0",
  "cpp_standard": "c++26",
  "std_flag": "-std=c++2c",
  "std_module_source": "/path/to/bits/std.cc",
  "std_module_source_hash": "hex",
  "build_command_hash": "hex",
  "mcpp_version": "0.0.41"
}
```

复用条件:

1. BMI 文件存在。
2. object 文件存在。
3. metadata 存在。
4. metadata 的 compiler、stdlib、target、standard、std flag、source hash、build command hash 与当前请求一致。

若 metadata 缺失，按 stale cache 处理，重建一次。这样可以兼容旧 cache 目录，又不会继续复用 C++23 生成的 std BMI。

### 4.5 fingerprint 与 std BMI hash 的边界

当前 fingerprint 第 10 字段是 `stdBmiHash`，但 prepare 阶段先算 fingerprint 再用它作为 std module cache 目录名，存在 chicken-and-egg 注释。

本轮可以不重构这个顺序，只做两个约束:

1. fingerprint 第 6 字段必须使用 active standard canonical 值。
2. std module metadata 必须独立校验 actual std flag 和 build command。

原因:

- active standard 变了，fingerprint 目录自然变。
- 同一 fingerprint 目录内，如果旧版本 mcpp 产物或命令变化导致 metadata 不匹配，`ensure_built()` 会重建。

后续如果要把 std BMI content hash 真正放进 fingerprint，第 10 字段需要重新设计为两阶段 build 或独立 std module cache key，不适合和 C++26 修复绑在一起。

### 4.6 flags 语义整理

推荐采用更清晰方案:

1. 全局 `cxxflags` 只放 build graph baseline:
   - C++ standard flag。
   - module flags。
   - toolchain/sysroot/backend flags。
   - project-wide include baseline。
2. 所有 package-owned `cflags` / `cxxflags` 只通过 compile unit 追加:
   - root package 也是一个 package，不特殊放进全局。
   - dependencies 与 root 行为一致。
3. `build.cxxflags` 中出现 `-std=` 时给迁移诊断:

```text
build.cxxflags must not set the C++ language standard.
Use [package].standard = "c++26" instead.
```

兼容策略可以分两步:

- P0: 如果 `-std=` 与 active standard 冲突，报错。相同则警告。
- P1: 全部 `-std=` in `cxxflags` 报错，彻底收口语义。

这个策略能避免 `cxxflags` 继续绕过 std BMI dialect 和 fingerprint。

### 4.7 compile_commands 与 Ninja 一致性

CDB 应继续从 `BuildPlan + CompileFlags` 生成，但需要满足两个不变量:

1. `compile_commands.json` 与 `build.ninja` 中每个 compile unit 的标准 flag 完全一致。
2. 标准 flag 只出现一次。

建议新增一个共享 helper，避免 Ninja 和 CDB 各自拼接:

```cpp
std::vector<std::string> cxx_args_for_unit(const BuildPlan& plan,
                                           const CompileFlags& flags,
                                           const CompileUnit& cu);
```

Ninja 可以继续输出字符串变量，CDB 输出 arguments array，但二者应消费同一组分层 flag:

```text
local includes -> baseline flags -> unit package flags -> -c source -o output
```

## 5. 错误提示优化

需要在两个位置给直接诊断。

### 5.1 manifest/cxxflags 层

检测到 `build.cxxflags` 含 `-std=`:

```text
build.cxxflags contains '-std=c++26'.
C++ language standard is a first-class package setting.
Move it to:

[package]
standard = "c++26"
```

### 5.2 std BMI metadata 层

如果 metadata 与当前 active standard 不一致:

```text
import std requires std module BMI built with the same C++ standard.
Current build uses c++26 (-std=c++2c), but cached std.gcm was built as c++23 (-std=c++23).
Rebuilding std module cache.
```

如果重建失败，再附加 compiler 原始输出。

## 6. 文档更新

`docs/05-mcpp-toml.md` 需要补三块:

### 6.1 `[package].standard`

推荐示例:

```toml
[package]
name     = "myapp"
version  = "0.1.0"
standard = "c++26"
```

说明:

- 默认 `c++23`。
- `c++26` 可用于需要 C++26 语言特性的项目。
- `c++2c` 作为别名兼容，但文档推荐写 `c++26`。
- `gnu++26` 表示 GNU dialect，会进入 fingerprint 和 std BMI cache key。

### 6.2 `[language]` 迁移说明

```toml
[language]
standard = "c++26"
```

仍可读，但标记为 legacy compatibility。新项目使用 `[package].standard`。

### 6.3 `cxxflags` 边界

明确写:

```toml
[build]
cxxflags = ["-Wall", "-Wextra"]  # 附加 C++ flags，不用于 -std=
```

语言标准只能通过 `[package].standard` 配置。

## 7. 实施拆分

### P0.1 标准归一化与 manifest 校验

改动:

- 新增 `normalize_cpp_standard()`。
- `[package].standard` 和 `[language].standard` 都进入归一化。
- `synthesize_from_xpkg_lua()` 中的 `language = ...` 同步填充归一后的 package standard。
- 支持 `c++26`、`c++2c`、`gnu++26`、`gnu++2c`。

验证:

- `tests/unit/test_manifest.cpp`: `[package] standard = "c++26"` 通过。
- `tests/unit/test_manifest.cpp`: `[language] standard = "c++2c"` 映射到 `c++26`。
- `tests/unit/test_manifest.cpp`: 非法值给出包含 allowed values 的错误。

### P0.2 BuildPlan 与 compute_flags 消费 active standard

改动:

- `BuildPlan` 增加 active `CppStandard` 字段。
- `prepare_build()` 填入 active standard。
- `FingerprintInputs.cppStandard` 使用 canonical。
- `compute_flags()` 使用 `cppStandard.compilerFlag`。
- 移除 C++ 标准硬编码 `-std=c++23`。

验证:

- unit test 生成 `standard = "c++26"` 的 plan，`build.ninja` 只含 `-std=c++26` 或工具链选出的 `-std=c++2c`。
- `compile_commands.json` 与 `build.ninja` 标准 flag 一致。

### P0.3 std module build 跟随 active standard

改动:

- `ensure_built(tc, fp.hex, cppStandard)`。
- GCC/Clang/std.compat build command 都使用 active standard flag。
- cache metadata 写入并在复用前校验。
- metadata 缺失或不匹配时重建。

验证:

- unit test 或 command-generation test: C++26 下 GCC std module command 含 C++26 flag，不含 C++23。
- Clang std 和 std.compat command 同样覆盖。
- metadata mismatch 时不复用旧 BMI。

### P1 cxxflags 去重与标准绕行收口

改动:

- `compute_flags()` 不再把 root `buildConfig.cxxflags` 和 `cflags` 放进全局 baseline。
- root 与 dependency 一样，只通过 compile unit `packageCxxflags` / `packageCflags` 进入 Ninja/CDB。
- 检测 `-std=` in `cxxflags`，先报冲突错误或迁移 warning。

验证:

- 主包 `-DROOT=1` 在每个 root compile unit 只出现一次。
- dependency flags 仍只作用于 dependency compile units。
- `-std=` in `cxxflags` 给出迁移提示。

### P1 文档对齐

改动:

- `docs/05-mcpp-toml.md` 增加 `[package].standard` 字段。
- 默认值表写明配置方式。
- 增加 `[language]` legacy note。
- 增加 `cxxflags` 不承担语言标准的说明。

验证:

- 文档示例中不再推荐或暗示用 `cxxflags` 配置 `-std=`。

### P2 更直接的错误提示

改动:

- std BMI metadata mismatch 输出清晰原因。
- 如果 compiler 不支持选定 standard，提示当前 toolchain 和建议 fallback。

验证:

- 人工构造 metadata mismatch，错误或 warning 能说明 cached/current standard。

## 8. 测试矩阵

| 层级 | 用例 | 目的 |
|---|---|---|
| Manifest | `[package] standard = "c++26"` | 新字段可用 |
| Manifest | `[language] standard = "c++2c"` | 旧字段兼容和别名归一 |
| Manifest | `build.cxxflags = ["-std=c++26"]` | 给迁移诊断 |
| Fingerprint | `c++2c` 与 `c++26` | canonical 相同，fingerprint 不分裂 |
| Fingerprint | `c++23` 与 `c++26` | fingerprint 必须不同 |
| Flags | `standard = "c++26"` | Ninja/CDB 没有 `-std=c++23` |
| Flags | root `cxxflags = ["-DROOT=1"]` | 不重复 |
| stdmod GCC | C++26 + `import std` | std.gcm build command 使用同一 standard |
| stdmod Clang | C++26 + `import std.compat` | std.pcm 和 std.compat.pcm 使用同一 standard |
| Cache | metadata 缺失 | 旧 cache 触发重建 |
| Cache | metadata standard mismatch | 不复用旧 BMI |
| E2E | C++26 非 import std 项目 | 编译命令贯通 |
| E2E | C++26 import std 项目 | std BMI dialect 一致 |

E2E 应按工具链能力 gate。不是所有 CI compiler 都一定支持 C++26 std module，不能让环境能力不足变成主线失败。

## 9. 风险与取舍

### 9.1 `c++26` flag spelling

风险: 某些 compiler 支持 `-std=c++2c` 但不支持 `-std=c++26`。

取舍: 用户-facing canonical 推荐 `c++26`，compiler-facing flag 由 toolchain 层决定。fingerprint 记录 canonical 和实际 flag spelling，避免 cache 误用。

### 9.2 GNU dialect

风险: `gnu++26` 和 `c++26` 不应混用同一 BMI cache。

取舍: canonical 保留 `gnu++` 前缀，metadata 记录 `std_flag`。这会分出不同 cache，是正确行为。

### 9.3 dependency standard

风险: dependency 声明 `c++23` 但 root 用 `c++26` 编译，可能遇到语义变化。

取舍: 第一阶段把 dependency standard 当作 minimum requirement。模块 BMI 兼容性比逐包保留原 dialect 更重要。如果未来需要 per-package exact dialect，必须设计单独的模块 ABI 边界。

### 9.4 `cxxflags` 兼容性

风险: 现有用户可能已经用 `cxxflags = ["-std=c++26"]` 绕过。

取舍: 支持一等 `standard` 后，应给明确迁移提示。短期可以 warning，长期必须禁止，否则 std BMI cache 和 fingerprint 仍会被绕过。

## 10. 最小落地顺序

1. 先打通 `standard -> active standard -> compute_flags`，让 `build.ninja` 不再硬编码 C++23。
2. 同一 PR 内把 `active standard` 传入 `stdmod::ensure_built()`，否则 `import std` 仍会撞 BMI dialect。
3. 写入 std module cache metadata，修复存在即复用的问题。
4. 再整理 root `cxxflags` 重复和 `-std=` 迁移诊断。
5. 最后补 `docs/05-mcpp-toml.md` 和更友好的错误信息。

如果要切 PR，建议:

- PR 1: 一等 standard + stdmod 同步 + metadata。解决 C++26/import std 的正确性问题。
- PR 2: cxxflags 去重和 `-std=` 迁移诊断。解决语义边界和输出一致性。
- PR 3: 文档与示例。也可以并入 PR 1，但单独做更容易 review。

## 11. 后续可选优化

1. 把 C、C++ 标准都统一进 `LanguageConfig`，让 `c_standard` 也获得别名归一和工具链 capability probe。
2. 把 Ninja 和 CDB 的 per-unit args 生成合并成共享 helper，减少未来 flags 顺序漂移。
3. 将 std module cache key 从 output fingerprint 中独立出来，形成 `std-module/<toolchain>/<standard>/<stdlib>/...` 的显式 cache namespace。
4. 在 `mcpp doctor` 中展示当前 active C++ standard、compiler flag spelling、std module cache metadata 和 std BMI 状态。
