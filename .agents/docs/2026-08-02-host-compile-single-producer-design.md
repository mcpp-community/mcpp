# 宿主编译单一生产者:让 build.mcpp 的能力等于 mcpp 的能力

日期:2026-08-02 · 基线:main @ 7169332(mcpp 2026.8.2.1)
起因:PR #332 复盘 —— 「MSVC 下 build.mcpp 不支持模块」的理由站不住

---

## 0. 核心原则

> **mcpp 能构建一个宿主程序,`build.mcpp` 就应该能被构建。**

`build.mcpp` 不该有自己的能力清单。它是「一个宿主 C++ 程序」,而编译一个宿主
C++ 程序正是 mcpp 的本职。今天它有一份**独立的、更窄的**能力清单,原因不是设计
选择,是一处接口形状不匹配导致的分叉(§2)。

判据落到可测形式(§5):对每个受支持的宿主工具链族,`build.mcpp` 必须支持
`#include` / `import std;` / `import mcpp;` / 两者并用四种形态。**新增一个工具链
族时不需要在 build.mcpp 侧再做一次。**

---

## 1. 现状:同一件事有三处装配

同一批「宿主编译 flag」被独立拼了三遍:

| # | 位置 | 服务对象 | 输出形态 |
|---|---|---|---|
| 1 | `flags.cppm::compute_flags` | 主构建的每个 TU | ninja 命令**字符串** |
| 2 | `stdmod.cppm:238-253` | std 模块自身的编译 | shell 命令**字符串** |
| 3 | `build_program.cppm::host_base_flags` | `build.mcpp` | **argv token 向量** |

三者都要处理同一组关注点:clang cfg 绕过、libc++ 头、sysroot / `-isystem` /
`-B`、`-Wl,-rpath`、`linkRuntimeDirs`、macOS deployment target、`-std=` 方言、
std BMI 引用、引号。

**证据一 —— 同一段 clang cfg 绕过写了三遍:**

```
flags.cppm:307,321        --no-default-config -nostdinc++ + libc++ headers
stdmod.cppm:240           " --no-default-config -nostdinc++ -stdlib=libc++"
build_program.cppm:227    push_back("--no-default-config") …
```

**证据二 —— 用注释强制跨文件不变量。** `stdmod.cppm:249`:

> Deployment target must mirror what flags.cppm emits for normal TUs

本仓库已经用一份 memory 记录过这个失败模式(*注释无法强制跨文件不变量*,
`check_version_pins.sh` 就是为此而生)。这里是同一个病的第二例。

**证据三 —— PR #332 的四个 bug 全是它的实例。** 每一个都是
「`flags.cppm` 早就做对了,另外两处不知道」:

| bug | `flags.cppm` 里的正确做法 |
|---|---|
| per-TU include 路径不加 shell 引号 | `:241` `shell_quote_arg` |
| BMI flag 路径不加 shell 引号 | 同一层 |
| build.mcpp 缺 macOS deployment target | `:331` 每个 TU 都发 |
| build.mcpp 无 MSVC 方言 | `ninja_backend` 全套 `/interface /TP /ifcOutput` |

而且不是第一次:memory 记着 0.0.9x 的同款 ——
*`build_program.cppm` 重推链接策略未复用 `prepare.cppm` 的 musl→static*。
**同一个文件,同一个病,第二轮。**

---

## 2. 为什么会分叉 —— 根因不是"忘了复用"

收敛其实做过一轮,而且做对了一半:`linkmodel.cppm` 导出了装配助手,签名带可插
拔的路径转义器:

```cpp
// linkmodel.cppm:61 / :111
std::string LinkModel::compile_flags(const PathEscape& esc) const;
std::string ClangDriverModel::compile_flags(const PathEscape& esc) const;
```

谁在用:

```
flags.cppm:322,337,349    dm.compile_flags(ninjaEsc) / lm.compile_flags(ninjaEsc)   ✓
stdmod.cppm:243,245       lm.compile_flags(shellEsc)  —— 但 dm 那半是手写的          ✗
build_program.cppm        两半都手写                                                  ✗
```

**根因:这个 seam 只产字符串。** `build_program` 需要的是 argv token 向量
(它直接 `capture_exec(argv)`,不经 shell),字符串接不上,于是整段重写。
`stdmod` 需要 shell 字符串,能用一半,另一半因为要拼 `shellEsc` 就顺手抄了。

这正是 PR #332 里 `split_ws` 的同一形状:**表里只有字符串形态,argv 消费者只能
自己切**。那次的修法(方言表同时存 `span<string_view>` token)已经证明了方向。

---

## 3. 明确排除:不让 build.mcpp 走 ninja

`build.mcpp` 的输出**是构建计划的输入**:

```
prepare.cppm:3863 / 3990   run_build_program()   ← 编译并运行 build.mcpp
prepare.cppm:4086          scan()                ← 扫模块图(要看到它生成的源文件)
prepare.cppm:4202          make_plan()           ← 生成构建计划
```

`mcpp:generated=` / `mcpp:source=` / `include-dir=` / `cxxflag=` 全部在扫描之前
改写 `buildConfig`。**它不可能是它自己所喂的那张图里的节点**——循环依赖,不是
未实现。Cargo 的 `build.rs` 同形。

「那就单独生成一张小 ninja 图」也不采纳:一个 TU 拿不到增量收益,却给**每次
构建的关键路径**加一次 ninja 生成 + 进程启动;而且真正重复的东西在 flag 装配层,
换个执行器并不能消掉它。

**结论:执行方式保持 `capture_exec`。要共享的是 flag 装配,不是构建图。**

---

## 4. 设计:token 优先的单一生产者

### 4.1 反转数据形状

新增 `mcpp.toolchain.hostflags`(或并入 `linkmodel.cppm`),产出 **token**:

```cpp
struct HostCompileFlags {
    std::vector<std::string> compile;   // argv token,已是最终形态
    std::vector<std::string> link;
};

struct HostFlagOptions {
    std::string_view cppStandardFlag;      // 已解析的 -std= / /std:
    std::string_view macosDeploymentTarget;// 已解析值(platform::macos)
    bool             staticRuntime = false;// musl / mingw 自包含策略
};

HostCompileFlags host_compile_flags(const Toolchain& tc, const HostFlagOptions&);
```

**token 是唯一真源,字符串由 token 渲染得到**,而不是反过来:

```cpp
// 一处渲染,两种转义
std::string render(const std::vector<std::string>& tokens, const PathEscape& esc,
                   bool shellQuote);
```

### 4.2 三个消费者,三种渲染

| 消费者 | 用法 |
|---|---|
| `flags.cppm` | `render(f.compile, ninjaEsc, /*shellQuote=*/true)` → 现有 `compile_toolchain_flags` |
| `stdmod.cppm` | `render(f.compile, shellEsc, true)` → 现有 `sysroot_flag` |
| `build_program.cppm` | **直接用 `f.compile`**,不渲染 —— 它本来就要 argv |

`host_base_flags` 随之删除。

### 4.3 build.mcpp 成为一等消费者

在 token 生产者之上,再给「编译一个宿主单 TU 程序」一个入口,把
`build_program.cppm` 里手写的那部分也收进去:

```cpp
struct HostProgram {
    std::filesystem::path source;        // build.mcpp
    std::filesystem::path output;        // build.mcpp.bin / .exe
    std::vector<std::string> imports;    // "std", "std.compat", "mcpp"
    std::filesystem::path workDir;       // BMI staging 目录
};

std::expected<std::vector<std::string>, std::string>
host_program_argv(const Toolchain& tc, const HostFlagOptions&, const HostProgram&);
```

它内部负责:方言(`dialect_for`)、语言强制(`forceCxxLangArgv`)、输出
(`-o` vs `/Fe:`)、静态运行时、以及**模块**——按 `bmi_traits(tc)` 决定
`gcm.cache` 暂存 / `-fmodule-file=` / `/reference`,和主构建同一张表。

于是 §0 的原则变成结构性事实:**MSVC 的 `.ifc` 支持不需要在 build.mcpp 侧单独
实现**,它来自共用的 `bmi_traits` + `dialect`。今天那道
「`import mcpp;` / `import std;` not yet supported under MSVC」的门可以直接删掉。

---

## 5. 能力对等:可测判据

原则若只写在文档里,就会重蹈 §1 证据二。因此落成矩阵测试:

| | `#include` | `import std;` | `import mcpp;` | 两者并用 |
|---|---|---|---|---|
| gcc(glibc / musl / mingw) | ✓ | ✓ | ✓ | ✓ |
| clang(libc++ / MSVC STL) | ✓ | ✓ | ✓ | ✓ |
| msvc(cl.exe) | ✓ | ✓ | ✓ | ✓ |

- e2e 181 扩成参数化的四形态(已覆盖 gcc/clang 三种,补 MSVC 一列);
- e2e 180 的第三段从「断言拒绝」改为「断言可用」;
- **新增守卫**:一个单测断言 `host_compile_flags` 对每个已知 `CompilerId` 都返回
  非空且自洽的结果(方言、std flag、输出前缀齐备),让"新增族忘了接"变成编译期/
  测试期失败,而不是运行期的 `not yet supported`。

---

## 6. 迁移约束与风险

### 6.1 字节等价是硬约束(不是"最好如此")

`stdmod` 的缓存**目录名派生自元数据,而元数据里含 `std_build_commands`**
(`stdmod.cppm:102,135`)。flag 字符串变一个字节 → 所有用户的 std BMI 缓存全部
失效并重编(memory 记过本机 26GB / 1198 目录的量级)。

因此:**GCC / Clang 路径上,重构后渲染出的字符串必须与今天逐字节相同。** token
按现有顺序拼接即可做到。验证方法用仓库既有的:**归一化 diff `build.ninja`**,
再加一条 `std_build_commands` 的前后对比。只有 MSVC 是行为新增。

### 6.2 clang 模块误编译雷区(实测,机制未明)

**结论先行:新生产者放在自己的模块里,不要往 `build_program.cppm` 的匿名
namespace 里加函数。** 这不是一般性风格规则,是一条基于实测的避让。

PR #332 期间,macOS CI 上所有 build.mcpp e2e(89/92/179/181)段错误。崩溃点在
`contract_env()` —— 那批改动**完全没有碰过**的函数,而且发生在 build.mcpp 被编译
之前。ASAN 定位到:

```cpp
e.emplace_back("MCPP_TARGET_OS", t.os);   // 写向 0x0
```

埋点打出的局部 vector 状态,在**第一次成功的 emplace_back 之后**就已经坏了:

```
e.size=6148912096828808697   cap=1   data=0x7142c3fe6150   t.os='linux'
```

`data` / `capacity` 正常,`__end_` 是垃圾。

**逐块剥离的结果**(每轮 `rm -rf target && mcpp cache clean`):

| 拿掉 | 结果 |
|---|---|
| `import mcpp.toolchain.dialect` | 仍崩 |
| compileArgv 方言化 / `capture_exec` 传 env / `dial` 局部变量 / `host_base_flags` 提前返回 | 仍崩 |
| **`split_ws` 函数本身**(此时它的调用点已全部删除) | **好了** |
| 换成平凡的 `int dummy_probe(int)` 放同一位置 | 不复现 |

也就是说:一个**从未被调用**的函数,仅因存在于该匿名 namespace,就让邻居函数里
的局部 `std::vector` 被写坏;而换一个形状简单的函数则不会。环境 clang 22.1.8 +
C++20 modules + `-O2` + libc++;GCC 下从不出现,所以 Linux 本地全绿而 macOS /
Windows 自举全红。

**已知边界**:代码里没有 UB —— 一个不被调用的函数不可能暴露另一个函数局部变量
的 UB。但**未归约成最小复现、未上报上游**,所以确切机制不明。这里记录的是可复
现的经验事实,不是理论。

**如何识别复发**:崩溃点在本次改动没碰过的函数里、只在 clang 平台出现、ASAN 报
局部容器指针为垃圾 —— 就先怀疑它,直接走"逐块剥离到只剩新增代码"的路子,不要
读 diff 找 UB(那次读 diff 完全无效,代码里确实没有错)。

顺带:`split_ws` 之所以存在,是因为方言表把 `-x c++` 存成字符串而 argv 消费者要
token。改成表里直接存 token 后它根本不需要 —— 这正是 §4.1「token 是唯一真源」
的由来,**本方案在消除重复的同时也移除了这个触发源**。

### 6.3 blast radius

`compute_flags` 服务每一次构建,是全仓库最高风险的函数之一。缓解:
分两步走(§8),第一步只做「token 化 + 渲染」且要求字节等价,第二步才接
build.mcpp 和删门。

### 6.4 验证必须双工具链、双清缓存

PR #332 的教训:只改 `mcpp.toml` 的工具链就重建,会把 gcc/libstdc++ 编的依赖链进
clang/libc++ 的 mcpp,崩得和真 bug 一样。每轮验证:
`rm -rf target && mcpp cache clean`,且先在基线 commit 上做同法对照。

---

## 7. 不做

| 项 | 理由 |
|---|---|
| build.mcpp 走 ninja / 生成第二张图 | §3:排序约束 + 一个 TU 无增量收益 |
| 把 build.mcpp 塞进主构建图 | §3:循环依赖 |
| 统一**主构建的**链接侧 | 主构建链接的是 target 产物、build.mcpp 链接的是宿主 helper,策略本就不同(`staticHostHelper`)。`flags.cppm` 的链接装配保持原样 |
| 给 build.mcpp 加多源文件 / C / 汇编支持 | 它是单 TU 程序,这是 L3 的设计选择,不在本方案范围 |

---

## 7.5 实施中相对本设计的两处偏差(已落地)

| 偏差 | 原因 |
|---|---|
| **补了 `host_link_tokens`**(§7 原写「链接侧不做」) | `build.mcpp` 是**一次驱动调用同时编译和链接**,`host_base_flags` 里本就含 `-fuse-ld=lld` / `--rtlib` / `-L` / `-rpath` / `--dynamic-linker`。不覆盖链接侧就根本迁不动它。§7 的排除项因此收窄为「不动**主构建的**链接装配」——`flags.cppm` 的链接侧确实一行没改 |
| **`stdmod` 的 deployment target 仍由它自己追加** | 三处的 flag **顺序**不同:`flags.cppm` 是 dm→deployment→lm,`stdmod` 是 dm→lm→deployment。让生产者统一顺序会改变 `std_build_commands` 字符串 → 按 §6.1 会让每个用户的 std BMI 全量失效。取舍:共享**装配**(手写的 dm 块已删除),只把这一个 flag 的**位置**留在本地并写明原因。位置统一留作后续 —— 届时应与一次本就会改变 std 身份的变更搭车 |

| 序 | 内容 | 规模 | 验收 |
|---|---|---|---|
| 1 | `host_compile_flags` token 生产者 + `render()`;`flags.cppm` / `stdmod.cppm` 改调 | 中 | **归一化 diff build.ninja 零差异** + `std_build_commands` 逐字节相同(三平台) |
| 2 | `build_program.cppm` 删 `host_base_flags`,改用 token | 小 | e2e 89/92/110/111/112/124/125/143/144/145/164/168/179/181 全过,双工具链 |
| 3 | `host_program_argv` 收编模块处理;删 MSVC 模块门 | 中 | e2e 180 第三段改为断言可用;e2e 181 补 MSVC 列 |
| 4 | 能力矩阵单测(§5 第三条) | 小 | 新增 `CompilerId` 时测试失败 |

第 1 步单独成 PR 且必须零 diff —— 它是纯重构,任何行为变化都说明抽错了。

---

## 附:核对过的坐标

| 坐标 | 内容 |
|---|---|
| `src/toolchain/linkmodel.cppm:61,111` | 已有的字符串装配 seam(`compile_flags(esc)`) |
| `src/build/flags.cppm:307-349` | 装配 #1,唯一正确使用 seam 的一处 |
| `src/toolchain/stdmod.cppm:238-253` | 装配 #2;`:249` 是"注释强制不变量" |
| `src/toolchain/stdmod.cppm:102,135` | 缓存身份含 `std_build_commands` → §6.1 |
| `src/build/build_program.cppm:187-270` | 装配 #3(`host_base_flags`) |
| `src/build/prepare.cppm:3863,3990,4086,4202` | 排序约束的证据(§3) |
| `src/build/ninja_backend.cppm:645,820` | 主构建已有的 MSVC 模块管线 |
| `tests/e2e/99_msvc_native_build.sh` | 证明 MSVC 模块在主构建可用(产出 `.ifc`) |
