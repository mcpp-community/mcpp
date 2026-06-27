# 分析报告:`abi:` 能力检查把「libc ABI」与「C++ stdlib」混为一谈 —— glfw 在 clang/libc++ 下误报 ABI mismatch

**日期**: 2026-06-27
**仓库**: `mcpp-community/mcpp`(core)
**触发来源**: `mcpp-community/mcpp-index` PR #48 把 CI 的 mcpp 从 `0.0.46` 升到 `0.0.67` 后,
`smoke_imgui_module.sh`(显式 pin `llvm@20.1.7`)的 `compat.glfw` 传递依赖报错。
**结论**: **是一个真实的 mcpp core bug**(非配方、非 smoke 脚本问题)。`abi:` 能力检查在推导
「当前工具链 abi」时,用 **C++ 标准库身份(libc++/libstdc++)** 抢占了 **libc/C-运行时 ABI(glibc/musl)**,
导致 `clang+libc++`(目标三元组仍是 `*-linux-gnu`,即 glibc)被判成 `abi=libc++`,与纯 C 库 glfw 声明的
`abi:glibc` 不匹配而**误报**。修复点单一、明确,见 §6。

---

## 1. 症状

```
error: ABI mismatch: dependency 'compat.glfw' requires abi=glibc but the resolved
       toolchain 'clang 20.1.7 (x86_64-unknown-linux-gnu)' is abi=libc++.
       fix: `mcpp toolchain default <glibc-compatible>` (e.g. gcc@16.1.0 for glibc),
       or set [toolchain] in mcpp.toml.
```

注意报错里工具链标签自己写着 **`x86_64-unknown-linux-gnu`** —— `gnu` 即 glibc。也就是说:
**这台工具链在 libc 层就是 glibc**,却被判成 `abi=libc++` 然后拒绝一个 glibc 的 C 库。这本身就自相矛盾。

## 2. 最小复现(本地 0.0.66 / 0.0.67 均复现,3 行工程)

```toml
# mcpp.toml
[toolchain]
default = "llvm@20.1.7"          # 换成 gcc@16.1.0 则通过
[dependencies.compat]
glfw = "3.4"
```
```cpp
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
int main(){ return glfwInit()?0:0; }
```
- `default = "gcc@16.1.0"` → `Finished`(stdlibId=libstdc++ → tcAbi 落到 `glibc` → 匹配)。
- `default = "llvm@20.1.7"` → **上面的 ABI mismatch**(stdlibId=libc++ → tcAbi=`libc++` → 不匹配 `abi:glibc`)。

CI 现场(mcpp-index run 28285632233,`smoke-full-linux`)同一 job 内 `gcc@16.1.0` 连续解析成功 11 次,
随后 `smoke_imgui_module.sh` 一行 `default = "${MCPP_INDEX_IMGUI_MODULE_TOOLCHAIN:-llvm@20.1.7}"`
解析到 `llvm@20.1.7 → clang++` 即触发本错误。**与 glfw 的 GLX/窗口/显示无关**,纯属依赖解析期的 ABI 门控。

## 3. 根因:`tcAbi` 推导把两条正交的 ABI 轴压成一条

`src/build/prepare.cppm:2471-2491`(能力驱动的 ABI 强制):

```cpp
const std::string tcAbi =
    ctx.tc.targetTriple.find("musl") != std::string::npos ? "musl"
    : ctx.tc.stdlibId == "libc++"                          ? "libc++"   // ← 病灶
    : ctx.tc.compiler == mcpp::toolchain::CompilerId::MSVC ? "msvc"
    :                                                         "glibc";
for (auto& cap : ctx.plan.runtimeCapabilities) {
    if (cap.rfind("abi:", 0) != 0) continue;
    std::string need = cap.substr(4);          // glfw: "glibc"
    if (need == tcAbi) continue;               // "glibc" != "libc++" → 落入报错
    ...
    return std::unexpected("ABI mismatch: ...");
}
```

这里把**两条正交的 ABI 轴**塞进了一个 `tcAbi` 字符串:

| 轴 | 取值 | 决定它的东西 | 谁在乎 |
|---|---|---|---|
| **A. libc / C 运行时 ABI** | `glibc` / `musl` | **目标三元组**(`-gnu` vs `-musl`) | 链接 **C 库**(glfw、zlib…)、syscall/CRT 兼容性 |
| **B. C++ 标准库** | `libstdc++` / `libc++` / `msvc-stl` | 编译器/`stdlibId` | 跨 **C++ TU** 传递 `std::*` 类型时的 C++ ABI |

`abi:glibc` 是 glfw 声明的 **A 轴**诉求(「我是 glibc 二进制,别在 musl 上用我」)。而代码里
`stdlibId == "libc++" ? "libc++"` 这一支,用 **B 轴**的值抢在 `glibc` 兜底之前返回,等于把
「这台工具链的 C++ 标准库是 libc++」错误地当成了「这台工具链的 libc ABI 是 libc++」。

**关键事实**:`clang+libc++` 在 `x86_64-unknown-linux-gnu` 上,**libc 仍然是 glibc**(libc++ 是 C++
标准库,不是 libc 替代品;它本身就链接 glibc)。`src/toolchain/clang.cppm:140` 一律把 Linux clang 的
`stdlibId` 设为 `libc++`,于是**任何** clang 工具链都会被本检查判成 `abi=libc++`,从而拒绝**所有**声明
`abi:glibc` 的(C)依赖——尽管把 glibc 的 C 库链进 libc++ 的 C++ 程序完全合法(glfw 导出的是 C 符号,
跟 C++ 标准库选择无关)。

三元组里 `musl` 那一支是**对的**(用三元组判 libc);错的是 `libc++` 这一支——它用 stdlibId 覆盖了
libc 轴。`abi:` 能力本应只看 A 轴。

## 4. 为什么 0.0.46 不报、0.0.67 报(不是行为回归,是「新增的过严检查」)

- 该能力驱动 ABI 检查随 **`0.0.54`(#129,`e9ed0e9`)** 引入,**晚于 0.0.46**。mcpp-index CI 之前 pin `0.0.46`
  → 根本没有这道门 → clang/libc++ 构建 glfw 一直「能过」(且实际可链接运行)。升到 `0.0.67` 才第一次撞上。
- 因此这是「**新检查本身判据有误**」,不是某次行为退化。但表现为「升级即坏」,需在 mcpp 侧修。

## 5. 这是不是 bug?—— 是,且会误伤真实用户

- **正交性**:libc++ 与 glibc 不互斥,是两条轴。把它们对立起来在模型上就是错的。
- **真实影响面**:`src/toolchain/clang.cppm:140` 使 Linux 下 clang 恒为 `stdlibId=libc++`。任何用户**主动选 clang**
  (这正是「`import std` 体验更好」的常见动机,也是 imgui-m 模块包 pin `llvm@20.1.7` 的原因)再依赖任何声明
  `abi:glibc` 的 **C 库**,都会被无理由挡下。当前 index 里 `grep` 到声明 `abi:glibc` 的只有
  `pkgs/c/compat.glfw.lua` 一个,但这是**给所有未来 C 库挖的坑**——按本应的语义,zlib/lua/mbedtls 等纯 C
  运行时库若也补 `abi:glibc`(逻辑上它们都该有),就会集体在 clang 下挂掉。
- **诊断误导**:报错建议 `mcpp toolchain default gcc@16.1.0`——把用户从 clang 赶到 gcc,而真实情况是
  clang 本可正确构建,只是检查判据错了。

## 6. 修复建议(mcpp core)

### 6.1 主修复(最小、精准):`abi:` 只看 libc 轴(三元组),不看 C++ stdlib
把 `tcAbi` 的推导改成只表达 **libc/C-运行时 ABI**,删掉 `stdlibId == "libc++"` 这一支:

```cpp
// abi: 能力 = libc/C-运行时 ABI(glibc / musl / msvc),由目标三元组决定,与 C++ 标准库无关。
const std::string tcAbi =
    ctx.tc.targetTriple.find("musl") != std::string::npos ? "musl"
    : ctx.tc.compiler == mcpp::toolchain::CompilerId::MSVC ? "msvc"
    :                                                         "glibc";
```
这样 `clang+libc++ @ *-linux-gnu` → `glibc`,与 glfw 的 `abi:glibc` 匹配,clang 也能直接构建 C 库;
`*-linux-musl` 仍正确判 `musl`;MSVC 仍 `msvc`。**单点改动,覆盖本 bug 全部表现。**
(更稳妥可显式按三元组判:含 `-gnu`/`gnueabi*` → glibc,含 `musl` → musl,Windows/MSVC → msvc。)

### 6.2 如确需限定 C++ 标准库,另立一条独立能力轴(不要塞进 `abi:`)
若将来有**C++ 库**需要约束 libstdc++/libc++(跨 stdlib 的 C++ ABI 才有意义),用单独命名空间,例如
`cxxstdlib:libstdc++` / `cxxstdlib:libc++`,且只对「在接口里暴露 `std::*` 的 C++ 库」施加;纯 C 库
(glfw 等)**不该**带这条。两轴分开校验,互不抢占。

### 6.3 跟进项(代码注释里已挂的 TODO):abi 驱动的工具链「重选」而非「只诊断」
`prepare.cppm:2467-2470` 注释自陈:工具链在依赖图之前解析,本检查只「诊断/强制」、不「重选」。
真正的体验改进是:当依赖声明 `abi:musl` 而当前是 glibc(**真**不兼容)时给出明确指引;当只是缺省工具链与
依赖偏好不一致、且存在兼容工具链时,优先**自动重选**而非直接报错。属增强,非本 bug 必需。

### 6.4 index 侧(可选,治标)
- `mcpp-index` 已把 GL window/imgui-module 两 smoke 移到 nightly,不阻塞 PR;6.1 落地后
  `smoke_imgui_module.sh` 即便 pin `llvm@20.1.7` 也能过,可回收进阻塞集。
- 复核 `compat.glfw` 的 `abi:glibc` 是否应理解为「libc 轴」语义(应是);若按 6.1 修复,语义自洽,无需改配方。

## 7. 建议的回归测试
- core 单测:构造 `stdlibId=libc++` 且 `targetTriple=x86_64-unknown-linux-gnu` 的工具链 + 一个声明
  `abi:glibc` 的依赖,断言**不报** ABI mismatch;`...-linux-musl` + `abi:glibc` 断言**报**。
- e2e:本报告 §2 的三行工程,`llvm@20.1.7` 与 `gcc@16.1.0` 都应 `Finished`。

## 8. 一句话给维护者
病灶是 `src/build/prepare.cppm:2474` 那一行 `stdlibId == "libc++" ? "libc++"`——它用 C++ 标准库身份
冒充了 libc ABI。删掉它(§6.1),`abi:` 能力回归「只管 glibc/musl」的本义,clang/libc++ 用户即可正常使用
glfw 及所有 glibc C 库。复现/证据见 §2–§4。

---

## 9. 更通用的架构方案(分层演进,§6.1 只是 L0)

§6.1 的一行修复能**立刻**止血,但它没有回答根本问题:**「依赖 ↔ 工具链」的兼容性本就是多维的,而当前用
一个扁平 `abi:<x>` 字符串 + 临时 `tcAbi` 推导去表达,迟早还会在别的维度上重蹈覆辙**(如 `_GLIBCXX_USE_CXX11_ABI`、
libstdc++↔libc++ 的真·C++ 库、cross/musl、macОS system libc、Windows msvc-stl…)。下面给一套**基于现有类型**
(`Toolchain` 已有 `targetTriple/stdlibId/compiler/stdlibVersion`;已有 `ProviderCapabilities`/`capabilities_for`
与包侧 `runtimeCapabilities/runtimeProviders` 两套能力机制)的演进方案,**不推倒重来**。

### 9.0 三条设计原则
1. **每个维度单一可信来源**:libc/arch/os 由**目标三元组**决定;cxxstdlib/cxxabi 由**编译器**决定。不许跨维推导(本 bug 即跨维)。
2. **未声明即「不在乎」(don't-care)**:依赖只约束它**真正关心**的维度,其余维度匹配器一律跳过。扁平串的根本毛病是「无法表达不在乎」。
3. **相关性由「暴露面」决定,而非一刀切**:某维度是否需要匹配,取决于依赖**跨边界暴露了什么**(见 9.3)。这是比 §6.1 更深一层的通用化。

### 9.1 L1 —— 结构化 ABI 画像(取代不透明的 `tcAbi` 串)
给 `Toolchain` 配一个**派生视图**(字段都已存在,只是没结构化使用):
```cpp
struct AbiProfile {
    std::string libc;       // glibc | musl | msvcrt | system(darwin)   ← 来自 targetTriple
    std::string cxxStdlib;  // libstdc++ | libc++ | msvc-stl            ← 来自 stdlibId
    std::string arch;       // x86_64 | aarch64 …                       ← 来自 targetTriple
    std::string os;         // linux | darwin | windows                 ← 来自 targetTriple
    std::string cxxAbi;     // itanium | msvc (+ 可选 cxx11abi 标记)    ← 来自 compiler
};
AbiProfile abi_profile(const Toolchain&);   // 唯一推导处,杜绝散落的 ad-hoc 串
```
`prepare.cppm` 的 `tcAbi` 整段删除,改为查 `AbiProfile`。**本 bug 在 L1 自然消失**(libc 维独立于 cxxStdlib 维)。

### 9.2 L1 —— 能力维度化(`abi:glibc` → 带维度的能力;旧串作糖)
包侧把单串 `abi:glibc` 升级为**带维度**的约束(或保留旧串作为 `abi:libc=glibc` 的语法糖,**零配方改动即兼容**):
```lua
-- 维度化:只约束自己关心的轴,其余 don't-care
requires_abi = { libc = "glibc" }                       -- glfw:纯 C 库,只认 libc
-- requires_abi = { libc="glibc", cxxStdlib="libstdc++", cxxAbi="itanium" }  -- 假想的预编译 C++ 库才需要
```
匹配器 = **逐维**比对 `requires_abi[d]` 与 `AbiProfile[d]`,未声明的维跳过。`abi:glibc` ≙ `{libc=glibc}` →
clang(libc=glibc)通过。这把「能不能表达不在乎」从根上解决,而不只是删一支 if。

### 9.3 L2 —— 由「linkage / 暴露面」推断相关维度(让作者基本不用手写)
最通用的洞见:**一个依赖该约束哪些维度,取决于它跨边界暴露什么**。mcpp 多数 compat 包是**源码构建**,
源码库用项目工具链编译 → 天然 ABI 自洽。据此可由描述符**自动推断**,免去作者手填、也根除「给 C 库套 cxxstdlib」这类误配:

| 依赖形态(可由描述符推断:有无源码/预编译产物、公开头是 C 还是 C++) | libc | arch/os | cxxStdlib / cxxAbi |
|---|---|---|---|
| 源码构建的 **C 库**(glfw、zlib) | 看运行期/host 假设(如 dlopen glibc GL 栈) | — | **不约束** |
| 源码构建的 **C++ 库** | — | — | **不约束**(与项目同工具链编译,构造即兼容) |
| 预编译 **C** 二进制 | ✔ | ✔ | 不约束 |
| 预编译 **C++** 二进制且接口暴露 `std::*` | ✔ | ✔ | **✔** |

即:cxxStdlib 维**仅**对「预编译且在接口里跨 TU 传 `std::*` 的 C++ 库」才相关。glfw 是源码 C 库 → 永远不该被 cxxStdlib 卡住。
（glfw 的 `abi:glibc` 实质是**运行期/host**诉求:它 dlopen `libGL.so.1` 等、链系统 glibc;这天然落在 libc 维,与构建期 C++ 标准库无关。)

### 9.4 L1 —— 对称的能力模型(删掉 prepare.cppm 里的特例)
现状有**两套并行**机制:工具链侧 `ProviderCapabilities`(强类型)、包侧 `runtimeCapabilities`(扁平串),
而 ABI 检查在 `prepare.cppm` 里**另起炉灶**搓 `tcAbi`。统一为:**工具链把自己的 ABI 维度也 advertise 成能力**
(扩展 `capabilities_for` 产出 `abi:libc=glibc`、`abi:cxxstdlib=libc++`…),检查退化为**逐维的能力集合包含**——
与其它 host capability(`x11.display`、`opengl.glx.driver`)**同一条通路**,`prepare.cppm` 的 ABI 特例整段删除。

### 9.5 L3 —— 解析即「约束求解」(把「解析后只诊断」升级为「解析出兼容工具链」)
`prepare.cppm:2467-2470` 注释自陈:工具链在依赖图**之前**解析,故只能诊断、不能重选。通用做法:
1. 由目标先定**可在图之前确定的维**(arch/os/libc——它们本就来自 target/triple,不依赖依赖图);
2. 解析依赖图,**收集各依赖的 `requires_abi` 约束并集**;
3. **有 pin**:校验 pin 满足**硬约束**(如 musl 依赖 vs glibc 工具链——真不兼容),否则给**逐维**精确报错;
   **无 pin**:从候选工具链里**挑一个满足全部约束**的(按偏好序),而不是默认一个再报错。
这把「resolve-then-diagnose」变成「solve-for-compatible」,正是注释里挂的 TODO 的正解;且因 libc/arch/os
在图前已知、只有细粒度约束来自图,**不是完整 CSP,只是一步 refine**,工程可控。

### 9.6 分层落地路线(每层独立可发、向后兼容)
| 层 | 内容 | 收益 | 风险/工作量 |
|---|---|---|---|
| **L0** | §6.1 一行:`abi:` 只看三元组,删 libc++ 支 | 立刻修好本 bug | 极小 |
| **L1** | `AbiProfile` + 维度化能力(旧串作糖)+ 对称能力模型,删 `tcAbi` 特例 | 模型自洽,杜绝「跨维冒充」全类问题 | 中,改 `prepare`/`provider`/`runtimeCapabilities` 解析 |
| **L2** | 由 linkage/暴露面**推断**相关维度 | 作者免手写、根除误配默认 | 中,需描述符元信息(已有 language/sources 可推断大部分) |
| **L3** | 解析期**约束求解 + 自动选工具链** | 声明式、无 pin 也能跑、精确报错 | 较大,触及工具链解析顺序 |

> **建议**:L0 随手发(止血);L1 作为本 issue 的「正解」单独成 PR(它独立消除一整类「维度冒充」bug);
> L2/L3 作增强按需推进。L1 落地后,`mcpp-index` 可把 imgui-module/glfw 的 GL smoke 从 nightly 收回阻塞集。

### 9.7 与既有设计对齐
本方案是这些既有文档的自然延伸,不冲突:`2026-06-02-usage-requirements-architecture.md`(依赖暴露=usage requirements,
正对应 9.3 的「暴露面」)、`2026-05-15-clang-parity-and-toolchain-abstraction.md`(工具链抽象,正对应 9.1 的
`AbiProfile`)、`2026-06-20-package-resolution-architecture.md`(解析架构,正对应 9.5 的求解化)。建议把
「ABI/能力维度模型」作为 usage-requirements 的一个子维并入,而非另起体系。
