# 设计:ABI/工具链兼容性「维度化」模型 —— 一步到位的单 PR 方案

**日期**: 2026-06-27
**仓库**: `mcpp-community/mcpp`
**关联**: 根因分析见同目录 `2026-06-27-glfw-abi-glibc-vs-libcxx-conflation-analysis.md`(§9 给了 L0–L3 分层路线)。
**本文目标**: 把那套架构**收敛成一个可评审、可合并的 PR**——交付**完整且正确的「依赖↔工具链」兼容性*模型与校验***,
彻底替换 `prepare.cppm` 里临时搓 `tcAbi` 串的做法。

---

## 1. 一句话范围

**单 PR = 用一个结构化、维度化、按暴露面推断、未声明即 don't-care 的「ABI 兼容性子系统」替换扁平 `abi:` 门控;
保持旧 `abi:glibc` 配方零改动可用;给逐维精确诊断。** 工具链解析顺序**不动**(仍 resolve-then-validate),
只是把「validate」做对、做全。约束求解式的**自动重选**(L3)显式不在本 PR(理由见 §8),但留好接缝。

判据:本 PR 落地后,§关联文档 §2 的三行工程在 `gcc@16.1.0` 与 `llvm@20.1.7` 下**都** `Finished`;
`*-linux-musl` + `abi:glibc` 仍正确报错;且这套修复对**任意维度**的冒充(不止 libc++/glibc)都成立。

## 2. 设计要点(为什么这样就「架构合理」)

1. **每维单一可信来源**:libc/arch/os ← 目标三元组;cxxStdlib/cxxAbi ← 编译器/`stdlibId`。**禁止跨维推导**(本 bug 之源)。
2. **未声明即 don't-care**:依赖只约束它真正在乎的维,匹配器跳过其余维。扁平串的死穴正是「无法表达不在乎」。
3. **相关性随暴露面**:某维是否相关,由依赖**跨边界暴露了什么**决定(源码 C 库 vs 预编译 C++ 库),可由描述符推断。
4. **对称同构**:工具链与依赖用**同一套** `AbiDimensions` 词汇表达;匹配 = 逐维比对。删掉 `prepare.cppm` 的特例分支。
5. **向后兼容**:旧 `abi:glibc` 字符串解析期映射到 `{libc=glibc}`,**配方零改动**;新结构是可选增强。

## 3. 新增模块 `mcpp.toolchain.abi`（核心类型 + 纯函数,易测）

```cpp
export module mcpp.toolchain.abi;
import std; import mcpp.toolchain.model;
export namespace mcpp::toolchain {

enum class AbiDim { Libc, CxxStdlib, Arch, Os, CxxAbi };

// 工具链的 ABI 坐标(派生视图,字段都来自已有 Toolchain)
struct AbiProfile {
    std::string libc;       // glibc | musl | msvcrt | macos        ← targetTriple
    std::string cxxStdlib;  // libstdc++ | libc++ | msvc-stl         ← stdlibId
    std::string arch;       // x86_64 | aarch64 | …                  ← targetTriple
    std::string os;         // linux | darwin | windows              ← targetTriple
    std::string cxxAbi;     // itanium | msvc                        ← compiler/triple
};
AbiProfile abi_profile(const Toolchain&);     // 唯一推导处,杜绝散落 ad-hoc 串

// 依赖的约束:每维 optional,nullopt = don't-care
struct AbiRequirement {
    std::optional<std::string> libc, cxxStdlib, arch, os, cxxAbi;
    std::string source;     // 诊断用来源:"compat.glfw (abi:glibc)" / "inferred" / "requires_abi"
};

struct AbiMismatch { AbiDim dim; std::string need, got, source; };

// 逐维比对;只看 req 里非空的维。返回所有不匹配(空 = 兼容)。
std::vector<AbiMismatch> abi_check(const AbiProfile&, const AbiRequirement&);
std::string_view dim_name(AbiDim);            // 诊断/序列化
}
```

`abi_profile` 推导规则(单点实现):
- `libc`: triple 含 `musl`→musl;含 `-gnu`/`gnueabi`→glibc;含 `windows-msvc`→msvcrt;含 `apple`/`darwin`→macos。
- `arch`/`os`: 解析 triple。
- `cxxStdlib`: 直接取 `tc.stdlibId`(已正确)。
- `cxxAbi`: `compiler==MSVC` 或 triple 含 `-msvc`→msvc;否则 itanium。

> 注:这些都是**已有信息**的重新组织;不引入新探测。`abi_profile` 即「把 §关联文档 §6.1 的正确推导,扩展到全维度」。

## 4. 依赖约束的三个来源（解析期 → `AbiRequirement`，优先级由高到低）

1. **显式结构化**(新增,可选):描述符 `mcpp` 段里
   ```lua
   requires_abi = { libc = "glibc" }                         -- 只认 libc
   -- requires_abi = { libc="glibc", cxxstdlib="libstdc++" } -- 预编译 C++ 库才需要
   ```
2. **旧能力串**(`RuntimeConfig.capabilities` 里的 `abi:<x>`):`abi:glibc`→`{libc=glibc}`、`abi:musl`→`{libc=musl}`。
   **本 PR 不要求改任何现有配方**——glfw 的 `abi:glibc` 自动变成「只约束 libc」,clang 即通过。
3. **暴露面推断**(§5):仅填**前两者未指定**的维。显式恒胜推断。

解析落点:`src/manifest.cppm` 的 `RuntimeConfig` 旁加 `AbiRequirement abi;`;xpkg/manifest 解析处把
`capabilities` 里 `abi:` 前缀项与新 `requires_abi` 表归并进去(`abi:` 仍保留在 capabilities 里供 doctor 展示,不破坏现状)。

## 5. 暴露面推断（L2，安全:只「相关性」判定,绝不凭空收紧）

由描述符已有信息判「形态」,从而决定哪些维**相关**:
- `isPrebuilt` = 有 `runtime.libraryDirs`/`dlopenLibs` 且无源码编译产物(预编译二进制,ABI 已烘焙)。
- `isCOnly` = `cStandard` 非空且无导出模块/无 `.cpp/.cppm`(纯 C,接口只 C 符号)。

规则(只对**未显式声明**的维生效):

| 形态 | libc | arch/os | cxxStdlib / cxxAbi |
|---|---|---|---|
| 源码构建(C 或 C++) | 仅当链系统库/`dlopenLibs` 非空时相关 | 同左 | **永不相关**(与项目同工具链编译,构造即兼容) |
| 预编译 **C** 二进制 | 相关 | 相关 | 不相关 |
| 预编译 **C++** 且接口暴露 `std::*` | 相关 | 相关 | **相关** |

**关键不变式:推断只决定「某维是否纳入校验」,从不替依赖编造更严的取值。** 因此对**所有现存包**(都未显式声明 cxxstdlib)
本 PR 只会**放宽**、不会**误伤**——这是它能「零配方改动且不回归」的根本保证。glfw=源码 C 库 → cxxStdlib 永不纳入 → clang 通过。

> 为控规模:L2 实现成一个纯函数 `infer_relevant_dims(manifest) → set<AbiDim>`,`abi_check` 调用时按它过滤。
> 即便先只接「源码 C/C++ → 不纳入 cxxStdlib」这一条最关键规则,也已覆盖全部现网场景;其余行可留 TODO 但**接口先就位**。

## 6. 替换 `prepare.cppm` 的校验块

`src/build/prepare.cppm:2467-2491` 整段删除,替换为:
```cpp
const auto prof = mcpp::toolchain::abi_profile(ctx.tc);
const auto relevant = mcpp::toolchain::infer_relevant_dims(/*per-dep manifest*/);
for (auto& [depId, req] : ctx.plan.abiRequirements) {     // 由 plan 收集(见 §7)
    for (auto& mm : abi_check(prof, req.filtered_by(relevant[depId]))) {
        return std::unexpected(render_abi_error(depId, mm, prof));   // §7 诊断
    }
}
```
`tcAbi` 字符串及其 `stdlibId == "libc++"` 病灶分支随之消失。

## 7. 诊断（逐维、可操作）

```
error: ABI incompatibility for dependency 'compat.glfw'
       dimension : libc
       required  : musl            (from: compat.glfw → abi:musl)
       toolchain : glibc           (llvm@20.1.7, x86_64-unknown-linux-gnu)
       fix: pick a musl toolchain (e.g. `gcc@16.1.0-musl`) or set [toolchain] in mcpp.toml.
```
比现状强在:**点名是哪一维**、给出该维的来源与该维专属的修复建议(而非笼统「abi=libc++」)。
L3 落地后,这里可升级为「已自动选用 X 满足约束」。

## 8. 明确不在本 PR（L3:约束求解式自动重选）与接缝

- **不在本 PR**:把「resolve-then-validate」改成「solve-for-compatible-toolchain / 无 pin 自动选」。
  理由:它**改动工具链解析顺序**(`package-resolution-architecture` 的核心路径),风险面与本「修正校验语义」正交;
  混进来会让 PR 不可评审。本 PR 让校验**正确**;重选是**解析策略**的独立演进。
- **接缝(本 PR 先铺好,L3 即插即用)**:
  1. `ctx.plan.abiRequirements`(§7 收集的并集)就是 L3 求解器的输入,本 PR 已产出。
  2. `abi_check` 是纯函数 → L3 的「候选工具链 × 约束」打分直接复用。
  3. 诊断渲染层留 `auto-selected` 文案位。

## 9. 文件级改动清单(单 PR)

| 文件 | 改动 |
|---|---|
| `src/toolchain/abi.cppm` (新) | `AbiDim/AbiProfile/AbiRequirement/AbiMismatch`、`abi_profile`、`abi_check`、`dim_name` |
| `src/toolchain/model.cppm` | 无需改字段;`abi.cppm` 读现有 `targetTriple/stdlibId/compiler` |
| `src/manifest.cppm` | `RuntimeConfig`(或 manifest)加 `AbiRequirement abi`;解析 `requires_abi` 表 + 归并 `abi:` 串 |
| `src/build/plan.cppm` | 收集每依赖的 `AbiRequirement` 与形态信息 → `plan.abiRequirements` + `infer_relevant_dims` |
| `src/build/prepare.cppm` | 删 2467-2491 的 `tcAbi` 块,改调 `abi_check`;新诊断渲染 |
| `src/doctor.cppm`(可选) | `mcpp doctor`/`why` 展示工具链 `AbiProfile` 与各依赖逐维匹配状态 |
| `tests/` | §10 |
| 配方 | **零改动**(`abi:glibc` 经映射继续生效) |

## 10. 测试计划(随 PR）

- **单测(`abi.cppm` 纯函数,最高价值)**:
  - `abi_profile`:`x86_64-unknown-linux-gnu`+libc++ → `{libc=glibc, cxxStdlib=libc++}`;`...-linux-musl` → `{libc=musl}`。
  - `abi_check`:`prof{libc=glibc,cxxStdlib=libc++}` × `req{libc=glibc}` → **空**(本 bug 回归锁);× `req{libc=musl}` → libc 维 mismatch;× `req{cxxStdlib=libstdc++}`(仅当 relevant)→ cxxStdlib mismatch。
  - `infer_relevant_dims`:源码 C 库 → 不含 CxxStdlib;预编译 C++(暴露 std)→ 含。
- **e2e**:§关联文档 §2 三行工程,`gcc@16.1.0` 与 `llvm@20.1.7` 均 `Finished`;构造 musl 目标 + `abi:glibc` 断言报错且**点名 libc 维**。
- **回归**:mcpp-index 全量 smoke 在统一 `0.0.67` 下转绿后,把 glfw/imgui-module 的 GL smoke 从 nightly 收回阻塞集(本仓改完发版后,index 侧一行 CI 调整)。

## 11. 评审小结

- **改对一处语义,删一类 bug**:不是补丁式删 if,而是把「ABI 兼容」建模成它本来的多维 don't-care 问题,
  之后任何维(`_GLIBCXX_USE_CXX11_ABI`、msvc-stl、cross/musl…)都走同一通路。
- **零配方改动、不回归**:靠「推断只放宽、显式才收紧」的不变式(§5)。
- **规模可控**:核心是一个纯函数模块 + 一处调用替换;L3 显式划走但接缝就位。
