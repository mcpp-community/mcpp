# mcpp 裸机 / freestanding 支持 — 架构设计方案(2026-08-19)

- Date: 2026-08-19
- Status: **方案,待 review**(第 10 节是决策回执)
- 关联 issue: [#403](https://github.com/mcpp-community/mcpp/issues/403)(RFC,已 CLOSED)、#276(hosted 嵌入式,另一条线)
- 关联决策: `.agents/docs/2026-07-24-embedded-platform-support-design.md` 决策 #15(裸机推迟)—— 本方案更新其依据,见 §10
- **⇒ 发现与证据独立成文:`.agents/docs/2026-08-18-freestanding-baremetal-analysis.md`(E1–E6 / X1–X12 / Y1–Y5 / Z1 / C1–C4 全部实测与复现命令)。本文是方案,那份是发现。** 本文每条设计都标注支撑它的实测编号。

---

## 0. 一句话方案

**引擎只加三条缝(目标规格 / 链接模型 / runner),裸机知识全部下沉为包;库的 freestanding 能力用「声明 + 证据 + 兜底」三元组表达,其中只有兜底是门。**

```
┌─ L5 生态层 ── BSP 包 · mcpplibs.std.freestanding · 载荷(builtins/picolibc/libc++)
│                     ↑ 全部是普通包/载荷,引擎不认识任何一块板子
├─ L4 契约层 ── 声明(作者,给人看) · 证据(mcpp 算,给人看) · 兜底(裸机链接,唯一的门)
├─ L3 运行层 ── runner argv 模板 + 产物路径追加;test 走 semihosting 退出码
├─ L2 链接层 ── CLibMode::Freestanding(正向态):-nostdlib -nostartfiles -static -T
└─ L1 目标层 ── triple 收 os=none + TargetSpec 一张表(isa/abi/code-model/linker/env-caps)
```

**为什么这么切**:E2 实测证明 mcpp 现有 LLVM 载荷**已经能**从 C++20 模块接口单元编出并链成完整 RISC-V 裸机内核。缺的不是能力,是模型。所以引擎的活是「把已有能力接上」,不是「造新能力」。

---

## 1. 范围

### 做

| | 内容 | 支撑 |
|---|---|---|
| P0 | **修 E1 的静默失败**(与路线无关的义务) | E1 |
| P0 | L1 目标层:`os=none` + TargetSpec 表 | E1/E6-2/§4.1 |
| P0 | L2 链接层:`CLibMode::Freestanding` | E2/E6-1 |
| P0 | `import std` 在 `os=none` 上关断 + 诊断 | E4 |
| P1 | L3 runner(`mcpp run`) | 全仓 runner 概念命中 1 处 |
| P1 | L4 契约层(声明/证据/兜底) | Y5/Z1/C1–C4 |
| P2 | L5:BSP 包 + `mcpplibs.std.freestanding` | X1–X10/Y3 |
| P2 | `mcpp test` semihosting 退出码 | X12 |
| P3 | 载荷:per-target builtins + picolibc + T3 libc++ | E3/X11 |

### 不做(明确)

- **板子数据库**(PlatformIO/Zephyr 形态)—— 与 2026-07-24 决策 #1 冲突。板子是包。
- **「哪些设施算 freestanding」的引擎判断** —— 决策 #6:领域知识归包。
- **libc 实现** —— 采纳 picolibc/newlib,不自造。
- **把声明做成门** —— §5.3 论证。
- **ESP32 / Xtensa / MCU 级(<1MB)** —— 决策 #15 的这半仍然成立。

---

## 2. L1 目标层:目标规格是数据

### 2.1 triple 收下 `os=none`

canonical 拼写 `riscv64-none-elf`(arch=riscv64, os=none, env=elf),与 Rust/LLVM 一致。

⚠️ **实施第一坑**:`src/toolchain/triple.cppm:299` 今天把 `none` 当 vendor 段跳过:

```cpp
if (k == "unknown" || k == "pc" || k == "w64" || k == "none") continue;
```

规则改为:**`none` 只在「整串没有其它 OS token」时才是 OS**;否则维持 vendor 语义(`x86_64-none-linux-gnu` 仍解析为 linux)。这条必须有单测正反两侧钉住。

派生:
- `family()` 对 `os=none` 返回空 ⇒ `cfg(unix)` / `cfg(windows)` 自动为假(**现状即正确**,无需改)
- 新增 `cfg(freestanding)` 谓词(词表扩展,不是新段)

### 2.2 TargetSpec:一张表,一处推导

```cpp
struct TargetSpec {
    std::string_view isa;        // "rv64gc"   → -march=
    std::string_view abi;        // "lp64d"    → -mabi=
    std::string_view codeModel;  // "medany"   → -mcmodel=
    Linker           linker;     // Lld | DriverDefault
    CLibMode         clib;       // Freestanding | Sysroot | PayloadFirst
    EnvCaps          provides;   // heap/exceptions/rtti/threads/hosted-std 的默认值
};
```

`TargetInfo` 增一列 `spec`。裸机行:

```cpp
{ "riscv64-none-elf", "planned", "bare", "llvm@22.1.8", /*defaultStatic=*/true,
  .spec = { "rv64gc", "lp64d", "medany", Linker::Lld, CLibMode::Freestanding,
            EnvCaps::none() } },
```

hosted 行的 `spec` 留空 = 驱动默认(零行为变化)。

**⚠️ 为什么必须是数据而不是 flags**(§4.1 of 分析文):`-march/-mabi/-mcmodel` 必须对链接进同一镜像的**每个 TU 和每个依赖**完全一致。`[build].cxxflags` 的作用域是本包,依赖拿不到 —— issue #403 那份 manifest 即使修好宿主解析,**一引入第二个包就碎**。Rust(target-spec JSON)、CMake(toolchain file)、Conan(profile)三家独立收敛到同一答案。

**⚠️ 一处推导的执行判据**:所有消费方从**同一个** `resolve_target_spec()` 取值,不各自推。消费方清单(实施时逐个核对):

```
compile flags · link flags · cfg 求值上下文 · 产物命名 · runner 选择
· BMI 缓存键 · hermetic 白名单
```

`linkmodel.cppm` 头注释记着 #195 的原始病灶就是「四份发散的副本」——**这条不是风格建议**。

### 2.3 逃生舱

`[target.<triple>]` 可覆写 spec 的每个键(开放词表):

```toml
[target.riscv64-none-elf]
toolchain    = "llvm@22.1.8"
isa          = "rv64imac"
abi          = "lp64"
code-model   = "medany"
linker-script = "boards/virt.ld"
```

⚠️ **但逃生舱必须走门**(见 §2.4),这是 E1 的教训。

### 2.4 ⚠️ P0:堵掉 E1 的静默失败

**今天的行为**(实测,mcpp 2026.8.15.3):`--target riscv64-none-elf` + 逃生舱段 → `Finished dev … 0.04s`,产物是 `target/x86_64-linux-gnu/…` 下的 **x86-64 glibc 动态 PIE**,`build.ninja` 里 `--target` 出现 **0 次**。

机制:`prepare.cppm:1264` 的门条件是 `!known && !hasExplicitSection`,`prepare.cppm:1301` 的 `host_can_serve` 门条件是 `known && …` ⇒ **不可解析的 triple 两道门都不经过**;`prepare.cppm:1458-1462` 的注释直书「leave the spec on the host target」。

**修法**:门的判据从「triple 认不认识」改成「**这次构建到底为哪个 target 发了 `--target`**」。具体:

1. 逃生舱 triple 若 `parse()` 失败 → **不再静默落回宿主**,而是要求 `[target.X]` 显式给出 `isa/abi` 或 `llvm-target`,否则硬失败;
2. 任何 `--target` 非空的构建,若最终 spec 的 target 等于宿主 target,**必须是用户显式要求的**,否则报错;
3. 产物目录用**解析后的 target**,与 `--target` 不一致时报错而不是改名。

**验收判据**(必须是这句,不能是「构建失败」):`mcpp build --target riscv64-none-elf` 在**没有**裸机配置时给出指名道姓的错误;在有配置时产物 `file` 输出含 `UCB RISC-V`。

---

## 3. L2 链接层:`CLibMode::Freestanding`

### 3.1 为什么是新态而不是复用 `None`

`linkmodel.cppm:27-29` 的 `CLibMode::None` 语义是**否定式**的:「什么都没找到 —— 驱动默认(宿主)生效」。**那正是 E1 的静默失败本身。** 把裸机接到它上面 = 把新功能建在已知缺陷上。

| | `None`(今天) | `Freestanding`(新) |
|---|---|---|
| 语义 | 没找到 C 库 | **明确不要** C 库 |
| CRT | 驱动默认 | `-nostartfiles`,CRT 由 BSP 提供 |
| loader / rpath | 驱动默认 | **禁止**(产物不得有 PT_INTERP) |
| 宿主路径泄漏 | 报告 | **致命** |
| 链接脚本 | — | 必需 `-T <path>` |
| 链接器 | 驱动默认 | **载荷内 `ld.lld` 的绝对路径** |

### 3.2 发什么

**编译侧**:`-ffreestanding` + spec 的 `-march/-mabi/-mcmodel` + 由 EnvCaps 决定的 `-fno-exceptions` / `-fno-rtti`(**不是硬编码** —— 一个提供异常的 BSP 应当能打开它)。

**链接侧**:`-nostdlib -nostartfiles -static -T <script>`;**不发** `--dynamic-linker`、`-rpath`、`-B` crt 前缀。

### 3.3 ⚠️ 链接器必须按路径寻址,不能按名字

实测(E6-1):本机 `ld.lld` 经 xlings shim 解析到 **GNU ld 2.42**,对 riscv 报 `cannot represent machine 'riscv'` —— 一条完全不像「找错链接器」的错误。

⇒ `Linker::Lld` 解析为**载荷内的绝对路径**,并在使用前校验 `--version` 含 `LLD`。

### 3.4 hermetic 检查语义反转

`hermetic.cppm` 今天的模型是「dry-run `-###`,断言 CRT + loader 落在允许前缀内,泄漏则报告」。裸机下:

- 没有 CRT / loader 可断言(它们不存在)
- 断言改为:**产物无 PT_INTERP、无 DT_NEEDED、无未定义符号、链接输入不含任何宿主路径**
- 泄漏从「报告」升级为「致命」;`allow_host_libs` 逃生舱在 `os=none` 下**不生效**(裸机链宿主库没有正当用例)

### 3.5 链接顺序:唯一的新 provision 语义

OSDev 的硬约束:`crti.o → crtbegin.o → 用户 .o → crtend.o → crtn.o`,顺序错会出「奇怪的 bug」;这两组共同实现 `_init`/`_fini`,**即 C++ 全局构造的调用点**。

`src/build/provisions.cppm` 的 `kTable` 设计意图正好是「一个 provision KIND 一行,复用同一套 fixpoint 传播」。新增两行:

| Kind | 内容 | ⚠️ 新语义 |
|---|---|---|
| `LinkerScript` | 链接脚本路径 | 单值,冲突即报错(两个 BSP 不能都给) |
| `StartupObjects` | 启动目标文件 + **序号** | **现有 provision 全是无序集合,这是第一个带序的** |

⚠️ 带序是这一层唯一真正的新概念,实施时不要顺手把它做成集合。

---

## 4. L3 运行层:runner

### 4.1 `mcpp run`

全仓 `runner|qemu` 的有效命中今天是 **1 处**(且无关)—— 这是纯新增,无兼容负担。

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-bios", "default", "-kernel"]
```

语义与 Cargo 一致:**产物路径追加到 argv 尾部**。BSP 包也可提供 runner(作为 provision),`[target]` 覆写它。

### 4.2 `mcpp test`

需要退出码约定。实测(X12):picolibc 载荷自带 `crt0-semihost.o` + `libsemihost.a` ⇒ **semihosting 退出码通路是现成的**,不需要自造。

`mcpp test --target riscv64-none-elf` = 为每个测试目标链一个 semihost crt0 的镜像,qemu 跑,读退出码。

⚠️ 见 §9:这条能否算数完全取决于 CI 上有没有 qemu。

---

## 5. L4 契约层:声明 / 证据 / 兜底

**这一层回答的问题**:mcpp-index 上,开发者怎么看出「这个库在裸机上能不能用」。

### 5.1 先承认语言的形状

⚠️ **Z1 实测**:同一个纯头模板库,消费者 A 用 `Ring<int>` → 符号表空(freestanding-clean);消费者 B 用 `Vec<int>` → `_Znam _ZdaPv`(要堆);**库自身编出的目标文件无实例化,无任何符号可审**。

⇒ **C++ 里「库 X 支不支持 freestanding」普遍不是一个库级事实。** Rust 的 `#![no_std]` 是 crate 级 + 编译器强制,C++ 侧**做不到等价的静态保证**。

**但这不意味着不要声明。** 它意味着声明的语义必须写对:

| | Rust `#![no_std]` | mcpp `[freestanding]` |
|---|---|---|
| 是什么 | **证明**(编译器强制) | **契约**(作者承诺:在文档化的 API 范围内成立) |
| 可信度来自 | 语言 | **证据**(§5.4) |
| 违反时 | 编译失败 | 消费者链接期硬错(§5.5) |

### 5.2 三元组

| | 谁产生 | 存哪 | 服务谁 | 是门吗 |
|---|---|---|---|---|
| **声明** | 库作者 | `mcpp.toml` `[freestanding]` → 发布进 xpkg 描述符 | **人**:索引浏览、选型 | ❌ |
| **证据** | mcpp(publish / CI 算) | xpkg 描述符 `[freestanding.verified]` | **人 + 索引展示** | ❌ |
| **兜底** | 消费者的裸机链接 | — | **机器** | ✅ **唯一的门** |

### 5.3 声明:字段与语义

```toml
[freestanding]
tier     = "core"                          # core | heap | hosted(缺省)
requires = ["float"]                       # 预设之外的额外能力(可选)
example  = "examples/freestanding.cpp"     # ⭐ 一致性示例
```

#### 5.3.1 它解决的是谁的问题

一个开发者在 mcpp-index 上看到 `mcpplibs.tinyfsm`,他正在写 RISC-V 内核。他要回答一个问题:**「这个库我能用吗?」** 今天没有任何办法知道,只能拉下来试。

`[freestanding]` 段就是这个问题的答案栏。**它不参与构建决策**(§5.7 规则 2),它是**索引页上的一行字**,以及支撑那行字的证据。

#### 5.3.2 `tier`:我这个库对环境的最低要求

不是「我用哪个标准库」,也不是「我支不支持裸机」,而是「**跑我需要环境提供什么**」:

| tier | 意思(库作者视角) | 环境要满足 | 库能用的标准设施 |
|---|---|---|---|
| `core` | 我只要编译器和一点栈 | 无 libc / 无堆 / 无异常 / 无 RTTI / 无线程 | Y4 实测的 **52 头两家交集**(`span array algorithm ranges optional expected atomic string_view tuple bit concepts type_traits memory numeric mdspan coroutine …`) |
| `heap` | 我内部要 `new` / `vector` | + libc(picolibc)+ malloc | 额外 `vector string format charconv`(X4/X9 实测) |
| `hosted` | 我要完整 OS | 现状 | 全部 |

**缺省是 `hosted`** —— 不写 `[freestanding]` 段 = 没声明 = 按 hosted 处理(§5.7 规则 1:这不等于「不支持裸机」,只是「没说」)。

举例说明它怎么用:

| 库 | tier | 为什么 |
|---|---|---|
| `tinyfsm`(状态机) | `core` | 纯计算,状态存在成员里 |
| 定长缓冲区格式化 | `core` | 输出写进调用方给的 `char[]` |
| `tinyjson`(解析) | `heap` | 节点数量运行期才知道,要动态数组 |
| 用 `std::thread` 的 | `core` + `requires=["threads"]` | ⚠️ **不是 `hosted`** —— 见 §5.3.4,线程是可插拔能力,RTOS BSP 能提供 |
| 用 `<filesystem>` / `import std` 的 | `hosted` | 真的需要 OS |

⚠️ **档位是环境的函数,不是标准库实现的函数**(Y4 实测):libc++ 在裸机上能给 `vector` 不是因为 libc++ 比 libstdc++ 强,**是因为 picolibc 给了 malloc**。所以 `heap` 描述的是环境提供了什么,不是库选了谁。

#### 5.3.3 `requires`:为什么不能只有三个档位

三个档位是**能力集上的命名预设**,不是枚举的全部:

```
core   = {}
heap   = {heap}
hosted = {heap, exceptions, rtti, threads, hosted-std}
```

完整的能力轴 —— **每一条的本质都是「一组符号契约」**,见 §5.3.4:

| 能力 | 含义 | 契约(符号)由谁定义 | 谁能实现 |
|---|---|---|---|
| `heap` | `new` / `delete` 可用 | **C++ 标准** [new.delete]:可替换的 `operator new/delete` 重载集 | **BSP / 任何库 / picolibc**(T3b 实测) |
| `exceptions` | `throw` / `catch` | **Itanium C++ ABI**:`__cxa_throw` + `_Unwind_*` | unwinder 载荷 + 链接脚本保 `.eh_frame` |
| `rtti` | `dynamic_cast` / `typeid` | **Itanium C++ ABI**:`__dynamic_cast` + typeinfo 布局 | 基本自足(typeinfo 是编译器生成的数据) |
| `threads` | `std::thread` / `std::mutex` | ⚠️ **两家不同**:libstdc++ = gthreads(`__gthread_*`);libc++ = `_LIBCPP_HAS_THREAD_API_EXTERNAL` | **RTOS / BSP**(T1/T2 实测) |
| `hosted-std` | `import std` / `<iostream>` / `<filesystem>` | POSIX / OS | 只有真 OS |
| `float` | 硬件浮点 | 目标 ISA | 目标 ISA 含 `f`/`d`(有些 MCU 没 FPU) |
| `lockfree-atomics` | 原生 CAS | 目标 ISA | 目标 ISA 含 `a`(X11 的 `rv64i` 变体就没有) |

**布尔为什么立刻不够用**:一个库可能要堆但不要异常;可能只有开了某个 feature 才要堆(⚠️ §5.9-5:声明必须能 per-feature);可能在没有 FPU 的目标上就是不能用。`freestanding = true` 一条都表达不了,而且加第二个布尔会立刻长出第三个。

所以:**常见情况写 `tier`(一个词),预设之外的用 `requires` 补**。

```toml
[freestanding]
tier     = "core"        # 不要堆/异常/RTTI/线程
requires = ["float"]     # 但我要硬件浮点
```

**resolver 怎么用它**(§5.5:这不是门,是诊断):目标提供的能力集 = 目标规格表的 `EnvCaps` ∪ BSP 提供的;库需要的 = `tier` 预设 ∪ `requires`。差集非空 ⇒ **解析期给一条指名道姓的诊断**,而不是等到链接期看 mangled 符号。

#### 5.3.4 ⚠️ 能力的接口由谁定义 —— mcpp 一个都不发明

这是整个契约层最容易做错的一步。先回答一个具体质疑:**`std::thread` 为什么不是 `hosted`?**

**因为线程在两个标准库里都是可插拔层,不是 hosted 专有设施**(实测):

| 标准库 | 机制 | 证据 |
|---|---|---|
| **libstdc++** | **gthreads** —— `gthr.h` / `gthr-default.h` / `gthr-posix.h` / **`gthr-single.h`**;`_GLIBCXX_HAS_GTHREADS` 在构建期决定 | T2:四个 gthr 头齐全,含单线程变体 |
| **libc++** | **`_LIBCPP_HAS_THREAD_API_EXTERNAL`** —— 「线程原语由外部提供」模式,用户给 `__external_threading` | T1:`__config:622/662` 明确有这条分支 |

FreeRTOS / Zephyr 接上任一侧,裸机上就有 `std::thread`。**所以正确的声明是 `tier="core", requires=["threads"]`,不是 `hosted`。** 原表那行是错的,已改。

**同样的道理适用于 heap —— T3b 实测**:BSP 侧一个 **bump 分配器**(替换 `operator new/delete` 重载集,约 12 个符号),**零 libc、零 malloc**,`std::vector<int>` 在 `riscv64-none-elf` 上直接可用:

```
heapdemo.elf: ELF 64-bit LSB executable, UCB RISC-V, statically linked
   text 5911   bss 12304     ← 未定义符号:空
```

C++ 标准 [new.delete] **本来就规定 `operator new/delete` 可被用户替换** —— 这不是绕过标准,这是标准写好的扩展点。

##### 结论:能力 = 符号契约,契约早已存在

| 问法 | 答案 |
|---|---|
| 是 C++ 语言模型里的吗? | **heap 是**(标准 [new.delete] 可替换函数);**exceptions / rtti 是 Itanium C++ ABI**;**threads 不是标准的**,是两家标准库各自的可插拔层 |
| 只要某种实现提供支持即可吗? | ✅ **是,这就是答案。而且接口早就存在,不用发明。** |
| mcpp 出一个库定义标准能力接口? | ❌ **不应该。** 那会变成**第三套 ABI**,与 libstdc++ 和 libc++ 都不兼容,谁都不会用 |
| mcpp 本身做成模块? | ❌ **不。** mcpp 不实现能力,也不定义能力接口 |

**mcpp 只做三件事**,一件都不多:

1. **记账** —— 目标提供哪些能力(TargetSpec ∪ BSP),库需要哪些(`tier` ∪ `requires`)
2. **验证** —— 符号审计(Y5)
3. **诊断** —— 差集非空时在解析期说清楚,而不是让用户在链接期读 mangled 符号

这与 2026-07-24 决策 #6(mcpp 定机制、领域专家定语义)和决策 #5(C-ABI 是唯一稳定的二进制互操作契约)是同一条线。

##### ⭐ 由此闭环:审计不是启发式

**能力的「契约」和能力的「判据」是同一组符号。**

```
heap       契约 = operator new/delete 重载集   判据 = 产物里有没有 _Znwm / _ZdlPvm
exceptions 契约 = __cxa_throw + _Unwind_*      判据 = 产物里有没有 __cxa_throw
rtti       契约 = __dynamic_cast + typeinfo    判据 = 产物里有没有 __dynamic_cast
threads    契约 = __gthread_* / __external_*   判据 = 产物里有没有这些符号
```

所以 §5.4 的符号审计**不是一个近似规则**,它检查的就是真正的 ABI 契约;§5.5 的链接期兜底之所以是硬门,也是因为契约缺实现在 `-nostdlib` 下必然是未定义符号。**整套设计的三层(声明 / 证据 / 兜底)咬在同一组符号上。**

##### ⚠️ 唯一需要生态出力的一处

`threads` 的接口 **libstdc++ 与 libc++ 不同**(gthreads vs `__external_threading`)。一个 RTOS 想同时服务两边,得写两个 shim。

**这是包的工作,不是引擎的工作** —— 生态可以出 `mcpplibs.rtos-threads-shim` 之类的适配包,把 FreeRTOS/Zephyr 接到两个标准库各自的扩展点上。⚠️ 但**接口仍然是标准库定义的那两个**,mcpp 不在中间再插一层。

#### 5.3.5 ⭐ `example`:把「作者说」变成「机器能验」

这是整段的支点,也是最容易被略过的一条。

**为什么需要它**:⚠️ Z1 实测 —— 同一个纯头模板库,消费者用 `Ring<int>` 符号表是空的(clean),用 `Vec<int>` 就出现 `_Znam`(要堆)。**库自身编出的目标文件没有实例化,没有任何符号可审。** 所以 `tier = "core"` 光写在那里,**没有任何东西能检验它**。

**`example` 做的事**:作者提供一个**只使用他声明为该档位的 API** 的示例程序。然后 CI 在**真实的裸机目标**上编它,并审计产物:

```console
$ mcpp build --target riscv64-none-elf        # 编 examples/freestanding.cpp
$ llvm-nm -u <产物>                            # 审计符号
```

三种结果,全部自动判定,**不需要人工 review**:

| 结果 | 判定 | 索引显示 |
|---|---|---|
| 编不过 | 声明是假的 | ⚠️ 已声明,验证失败 |
| 编过了,但符号含 `_Znwm` | 声明 `core` 实际要堆 | ⚠️ 已声明,实测 tier=heap |
| 编过了,符号干净 | 声明得到证据支持 | ✅ 已验证 |

**类比**:`tier` 之于「这个库能在裸机上用」,就像一句注释之于「这个函数没 bug」;`example` 就是那个单元测试 —— **声明本身不可信,是可执行的检验让它可信。**

**顺带的两个好处**:

- 它同时**就是文档** —— 用户点开这个库,第一眼看到的是「怎么在裸机上用它」的可编译代码,而不是一段散文。
- **复用现有机制**:`examples/` 目录 + e2e 已经存在,不是新子系统。

**⚠️ 它仍然不是完备保证**(必须写进文档,见 §5.9):示例只覆盖示例用到的 API。一个消费者用了示例没碰过的模板,可能仍然要堆。这就是为什么门是链接期兜底(§5.5),而不是这个示例。

### 5.4 证据:机器算,人不能写

`mcpp publish`(以及库自己的 CI)产出,写进 xpkg 描述符:

```toml
[freestanding.verified]
targets = ["riscv64-none-elf"]     # 实际编/链过的目标
tier    = "core"                   # 实测达到的档位
symbols = []                       # 符号审计结果;空 = clean
example = "examples/freestanding.cpp"
at      = "2026-08-19"
by      = "mcpp 2026.8.19"
```

**符号审计**(Y5 实测,零元数据、零生态配合):

| 产物里出现 | 判定 |
|---|---|
| `_Znwm` `_Znam` `_ZdlPvm` `_ZdaPv` / `malloc` | 需要 **heap** |
| `__cxa_throw` `__cxa_begin_catch` `__cxa_allocate_exception` | 需要 **exceptions** |
| `__dynamic_cast` `_ZTI*` | 需要 **RTTI** |
| `pthread_*` `__cxa_thread_atexit` | 需要 **threads** |
| (空) | freestanding-clean |

`llvm-nm` 已在载荷里。审计对象 = **一致性示例编出的产物**(Z1:库自身没有产物可审)。

**⚠️ 手写的 `[freestanding.verified]` 在发布时被覆盖。** 证据字段是机器的输出,不是人的输入 —— 否则它会立刻退化成第二个声明。

### 5.5 兜底:唯一的门,而且是免费的

⚠️ **关键洞察**:X7–X10 实测,裸机链接会**逐条点名**缺什么(`memmove`、`__libcpp_verbose_abort`、`basic_string::__init`)。因为 `-nostdlib` 下**没有 libc/libstdc++ 来静默满足这些符号**,而 hosted 链接恰恰会。

⇒ **只要 §3 的 `CLibMode::Freestanding` 是真的 `-nostdlib`,「这个库用了它不该用的东西」就已经是硬错误,不需要任何新机制。**

声明和证据的职责因此**不是正确性,是把错误提前并翻译成人话**:

| | 有声明/证据 | 无 |
|---|---|---|
| 正确性 | ✅ | ✅(链接期硬错) |
| 错误在哪发生 | 解析期,「包 `foo` 声明 tier=heap,目标 `riscv64-none-elf` 未提供 heap」 | 链接期,`undefined symbol: _Znwm` |
| 用户能否行动 | 能 | 难 |

### 5.6 索引展示:三态,永不合并

```
mcpplibs.tinyfsm 1.2.0   freestanding: core   ✅ 已验证  riscv64-none-elf · 2026-08-19
mcpplibs.foo     0.3.0   freestanding: core   ⚠️ 已声明,未验证
mcpplibs.bar     2.0.0   freestanding: —      未声明(≠ 不支持)
```

### 5.7 四条硬规则(实施时不得违反)

1. **未声明 ≠ 不支持。** 解析期永不因缺声明而拒绝依赖。(库作者 L0 永远可用)
2. **声明不做门。** 唯一的门是消费者的裸机链接。
3. **声明与证据分两栏,永不合并。** 合并 = 把「作者说」冒充「已验证」。
4. **证据由机器算,人不能写。**

⚠️ 规则 1 是硬承诺:一旦「不标注就被排除」,生态立刻分裂成两半,而 mcpp-index 里已发布的包不会为裸机回头改 manifest。

### 5.8 ⚠️ 兼容性(实测支撑,C1–C4)

| 侧 | 实测 | 结论 |
|---|---|---|
| manifest 未知顶层段 `[freestanding]` | C1:静默容忍,构建成功 | ✅ 安全 |
| manifest 未知 `[package]` 键 | C2:静默容忍 | ✅ 安全 |
| `--strict` | C3:**也不报** | ✅ 安全 |
| xpkg 描述符未知键 | C4:键是**闭词表** `kKnownXpkgKeys`(26 个),未知键收进 `xpkgUnknownKeys` **静默跳过** | ✅ 安全 |

⚠️ **但有一条必须写进 checklist**:`closest_known_xpkg_key` 会对「像是拼错」的未知键喷 did-you-mean 警告。**新键名必须与那 26 个现有键的编辑距离足够远**,否则老客户端会在整个生态上刷噪音。`freestanding` / `freestanding.verified` 距离足够,安全 —— 但换名字时要重新验。

### 5.9 ⚠️ 符号审计抓不到什么(必须写进文档)

否则它会被当成完备保证,那比没有保证更危险:

1. **inline asm / 裸 syscall / MMIO** —— 不留任何可疑符号。
2. **纯头库在自己这侧没有产物**(Z1)—— 所以审计对象必须是**一致性示例**,而且「我本地审计过了」≠ 消费者那边干净。
3. **能力够 ≠ 能用** —— T0-clean 的库仍可能需要 `<format>`(T1)。审计答的是「用了什么」,不是「够不够」。
4. **弱符号 / COMDAT 不能与 `U` 混算** —— 与 `origin-precedence` 那次「判据必须是 GLOBAL 计数」同类错误。
5. **feature 会改变答案** —— 一个 feature 开了就要堆 ⇒ 声明与证据都必须能 per-feature,否则又是一个「一个名字量两件事」。

---

## 6. L5 生态层

### 6.0 BSP 包是什么,和普通库有什么区别

#### 6.0.1 它解决什么问题

裸机程序开机时,CPU 从复位地址开始执行。此刻:

- **没有栈** —— `sp` 是垃圾值,任何函数调用都会炸
- **`.bss` 没清零** —— 全局变量的初值不可信
- **全局对象的构造函数没跑** —— `.init_array` 没人遍历
- **没有人调用你的 `kmain`**
- **链接器不知道代码该放在哪个地址** —— qemu `virt` 是 `0x80200000`,别的板子不是

hosted 世界里这些活是 libc 的 `crt0.o` + 动态加载器干的。裸机上那些东西都不存在,**所以必须有人提供**。BSP(Board Support Package)包就是提供这些的包。

#### 6.0.2 `riscv-virt-rt` 里到底有什么

名字沿用 Rust embedded 的惯例(`cortex-m-rt` / `riscv-rt`,`-rt` = runtime):

```
riscv-virt-rt/
├── src/start.S        _start:设 sp、清 .bss、遍历 .init_array、跳 kmain
├── src/rt.cpp         __libcpp_verbose_abort 默认实现(3 行:for(;;) wfi)
│                      可选 sbrk —— 实现了它,这个目标就有 heap 能力
├── boards/virt.ld     链接脚本:qemu virt 的内存布局(.text 从 0x80200000)
└── mcpp.toml          声明它提供:linker-script / startup-objects / runner
```

`mcpp.toml` 里那段 runner 就是 `mcpp run` 能工作的原因:

```toml
[provides]
linker-script   = "boards/virt.ld"
startup-objects = ["src/start.S"]          # ⚠️ 带序,见 §3.5
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-bios", "default", "-kernel"]
env-caps = ["heap"]                        # 因为实现了 sbrk
```

换一块板子 = 换一个 BSP 包(`hifive-rt`、`k210-rt`、`stm32f4-rt`)。**你的 `kmain` 一行不用改** —— 变的是内存布局和启动方式,不是业务逻辑。这正是用户不用手写 `link.ld` 的原因。

#### 6.0.3 与普通库的区别

| | 普通库 | BSP 包 |
|---|---|---|
| 回答的问题 | 「给我一个**功能**」 | 「让程序能在**这块板子**上启动起来」 |
| 贡献什么 | 模块接口 + 目标文件/静态库 | 链接脚本、启动目标文件、runner、内存布局 |
| **数量** | 一个工程可以有很多个 | **恰好一个** —— 两个链接脚本 = 硬冲突 |
| **顺序** | 无序集合(链接器自己解析) | **有序** —— `crti→crtbegin→user→crtend→crtn` 错了会出「奇怪的 bug」 |
| 按什么选 | 按功能 | **按硬件**,与业务代码无关 |
| 在能力模型里的角色 | **消费**能力(§5.3.3 的 `requires`) | **提供**能力(实现 `sbrk` ⇒ 目标获得 `heap`) |

**三条区别里最深的是第三条**:BSP 是 §5 契约层里「目标提供什么」的**实际提供者**。目标规格表(§2.2)给出裸机目标的能力**默认值**(全关),BSP 再往上加。所以:

```
目标能力集 = TargetSpec.provides ∪ BSP.env-caps
库可用性   = 库的 requires ⊆ 目标能力集     ← §5.3.3 的诊断判据
```

一个不实现 `sbrk` 的 BSP,配上一个 `tier = "heap"` 的库,mcpp 就能在**解析期**说清楚:「`tinyjson` 需要 heap,目标 `riscv64-none-elf` + `riscv-virt-rt` 未提供 —— 换一个提供 heap 的 BSP,或改用 `tier=core` 的库」。这比链接期的 `undefined symbol: _Znwm` 好得多。

**前两条区别是引擎必须支持的新语义**:唯一性(冲突即报错)和有序性(§3.5),这也是 BSP 无法用现有 provision 机制直接表达的全部原因。

### 6.1 三类组件

| 组件 | 内容 | 状态 |
|---|---|---|
| **BSP 包**(如 `riscv-virt-rt`) | 见 §6.0 | 机制近乎已有;缺 §3.5 的带序 provision |
| **`mcpplibs.std.freestanding`** | **实现中立**的名字表(Y3:同一份 `core.cppm` 在 libstdc++`-ffreestanding` 与 libc++`__config_site` 两边都编过) | X5–X10 已跑通;**长期维护品** |
| **载荷** | per-target compiler-rt builtins(E3)· picolibc(⚠️ 发行版包无 rv64gc,X11)· T3 档的目标版 libc++.a · libc++ 每目标 `__config_site` | **唯一真工程量**;可采纳 LLVM Embedded Toolchain |

### 6.2 生态契约层(HAL):非标准库能力怎么办,以及「实现一次两边都亮」

§5.3.4 讲的是**语言运行时**能力(heap/exceptions/rtti/threads)——它们的契约由标准和 Itanium ABI 定义,mcpp 不发明。但裸机上还有一大类能力**根本没有标准契约**:UART / 定时器 / 随机源 / 存储 / 中断注册 / 电源。

这类能力怎么办?**Rust 的答案是 `embedded-hal`** —— 一组 trait,HAL 实现提供、驱动 crate 消费,一个驱动写一次就能跑在所有实现了该 trait 的芯片上。⚠️ 关键:**`embedded-hal` 不属于 cargo**,它是社区 crate,cargo 对它一无所知。

#### 6.2.1 分两层,不是一层

```
Layer A  生态契约层(embedded-hal 形状)     ← 接口包,内容由生态定义,mcpp 不定义
             ↕  BSP / 适配包同时实现两侧
Layer B  标准 / ABI 扩展点                   ← operator new · gthreads · picolibc hooks(早已存在)
```

**「实现一次,生态库和 std 都亮」是成立的,但机制是 Layer B 而不是新 ABI**(实测):

| BSP 实现了 | 自动点亮 | 实测 |
|---|---|---|
| `operator new/delete` 重载集 | `std::vector` / `std::string` / 所有 std 容器 | T3b(零 libc) |
| gthreads / `__external_threading` | `std::thread` / `std::mutex` | T1/T2(机制确认) |
| `hal::Console` concept | 所有依赖该 concept 的生态驱动库 | H3 |

⇒ **一个 BSP 包同时实现 Layer A 的 concept 和 Layer B 的扩展点,两侧一起亮 —— 不需要 mcpp 发明任何契约。** H3 实测:同一个 `bsp.virt` 让生态驱动 `mcpplibs.shell` 和 `std::vector` 同时工作,链接零未定义符号。

#### 6.2.2 ⚠️ 契约的三种形态,只有两种可行

| 形态 | 跨包提供实现 | 多提供者共存 | 成本 | 结论 |
|---|---|---|---|---|
| **A** 模块内声明,别的包定义 | ❌ **语言禁止** | — | — | **不可用** |
| **B** `extern "C"` 缝 | ✅ | ❌ 单一全局符号 | 链接期绑定,不可内联 | 单例型能力 |
| **C** concept 静态注入 | ✅ | ✅ | **零成本**,可内联 | **生态 HAL 主形态** |

**⚠️ H1 实测 —— 形态 A 为什么被语言禁止**:模块归属被烙进符号名。

```
消费者要:  _ZN3halW8mcpplibsW3halW7console5writeEPKcm
           → hal::write@mcpplibs.hal.console(char const*, unsigned long)
提供者给:  _ZN3hal5writeEPKcm
           → hal::write(char const*, unsigned long)
ld.lld: error: undefined symbol: hal::write@mcpplibs.hal.console(...)
```

附着到模块的函数,**定义必须来自同一个模块**(接口单元或模块实现单元)。**另一个包在物理上给不了它。**

⇒ 这为 2026-07-24 决策 #5(「C-ABI 是唯一稳定的二进制互操作契约」)补上了一条**新的、更硬的理由**:在 C++20 模块世界里,跨包提供实现**只有 C ABI 一条路** —— 这不再只是 ABI 稳定性论证,而是**语言层面的物理约束**。

**形态 C 实测(H3)—— 可组合性的完整形态:**

```cpp
// 契约包 mcpplibs.hal —— 纯接口,零实现
export namespace hal {
    template <class T> concept Console = requires(T& t, std::span<const char> s) { t.write(s); };
    template <class T> concept Clock   = requires(T& t) { { t.ticks() } -> std::same_as<unsigned long>; };
}

// 驱动包 mcpplibs.shell —— 只依赖 concept,不知道任何板子
export template <hal::Console C, hal::Clock K>       // ← 两个能力组合
void banner(C& c, K& k);

// BSP 包 bsp.virt —— 提供实现;⚠️ Uart 与 Uart2 可同时存在
struct Uart  { void write(std::span<const char>); };
struct Uart2 { void write(std::span<const char>); };
struct Timer { unsigned long ticks(); };
```

实测结果:`shell::banner(u, t)` 与 `shell::banner(u2, t)` 同时存在,**同一个驱动被两个提供者各实例化一次,零成本静态派发**;整体链接零未定义符号,9040 字节 text。

#### 6.2.3 ⇒ 生态 HAL 该长什么样

- **以形态 C(concept)为主** —— 设备/驱动类能力:可组合、多实例、零成本、跨包无障碍。
- **形态 B(`extern "C"`)只留给天然单例的运行时能力** —— 堆、panic 落点、系统时钟。
- ⚠️ **而这些单例能力恰好已经被 Layer B 覆盖了**(`operator new`、`__libcpp_verbose_abort`、gthreads)⇒ **生态基本不需要自己发明 `extern "C"` 契约**。

#### 6.2.4 mcpp 在这一层做什么(仍然只有三件)

**契约的内容由生态定义,mcpp 一条都不定义。** mcpp 提供的是让「契约—实现—消费」三角能被表达和验证的机制,而这套机制**已经存在**:`2026-06-29-feature-capability-model-design.md` 的 `provides` / `requires` / `--cap` 提供者选择 —— 「依赖声明一个 capability 而非具体包,resolver 绑定恰好一个提供者」,**正是 embedded-hal 的形状**。

需要做的只是让它在**构建/链接层**可用(今天 `RuntimeConfig.capabilities` 的语义是运行期宿主能力),而不是新造一套。

⚠️ 这与 Rust 的分工一致:`embedded-hal` 不属于 cargo。**HAL 属于生态,不属于构建工具。**

### 6.3 `mcpplibs.std.freestanding` 的形态

**不**转发 libc++ 的 `std/*.inc`(那会绑定实现),而是自己写标准规定的名字表:

```cpp
module;
#include <span>
#include <optional>
// … 约 52 个头(Y4 交集)
export module mcpplibs.std.freestanding;

export namespace std {
    using std::span; using std::dynamic_extent;
    using std::optional; using std::nullopt;
    // …
    namespace ranges { using std::ranges::sort; /* … */ }
}
```

⚠️ **导出的是同一个实体,不是影子命名空间** —— 所以它与任何 `#include <span>` 的代码 ODR 兼容,且**用户代码从裸机搬回 hosted 一行不用改**。这是对开发者最重要的承诺。

代价:要维护那张约 52 项的清单。换来:两家标准库通用,且「可移植的 freestanding 子集」本身成为这个包的卖点。

---

## 7. 使用侧:最小案例

### 7.1 工程(三个文件,零裸机知识手写)

```
toy_kernel/
├── mcpp.toml
└── src/
    └── main.cppm
```

**`mcpp.toml`**

```toml
[package]
name    = "toy_kernel"
version = "0.1.0"

[dependencies]
riscv-virt-rt = "1.0"            # BSP:启动代码 + 链接脚本 + runner

[target.riscv64-none-elf]
toolchain = "llvm@22.1.8"
```

**`src/main.cppm`**

```cpp
export module toy_kernel;
import mcpplibs.std.freestanding;      // 可选;不 import 就是纯裸机 C++

namespace { volatile char* const kUart = reinterpret_cast<volatile char*>(0x10000000); }
struct Task { int prio; char id; };

export extern "C" void kmain() {
    std::array<Task, 4> t{{{3,'c'}, {1,'a'}, {4,'d'}, {2,'b'}}};
    std::ranges::sort(t, {}, &Task::prio);
    std::span<Task> sp{t};
    std::atomic<int> seen{0};
    for (auto const& x : sp) { seen.fetch_add(1); *kUart = x.id; }
    std::optional<Task> top = t.empty() ? std::optional<Task>{} : std::optional{t.back()};
    *kUart = top->id;
}
```

**命令**

```console
$ mcpp build --target riscv64-none-elf
$ mcpp run   --target riscv64-none-elf     # → qemu-system-riscv64 … -kernel <artifact>
$ mcpp test  --target riscv64-none-elf     # → qemu + semihosting 退出码
```

### 7.2 谁提供了什么

| 谁 | 提供 |
|---|---|
| **你写的** | `kmain` 的业务逻辑。**裸机知识 0 行。** |
| **BSP 包** | `_start` / 栈 / `.bss` 清零 / `link.ld` / 链接顺序 / `__libcpp_verbose_abort` / runner argv |
| **`mcpplibs.std.freestanding`** | `std::array` `std::ranges::sort` `std::span` `std::atomic` `std::optional` —— **真 `std::` 实体** |
| **目标规格表(引擎)** | `-march=rv64gc -mabi=lp64d -mcmodel=medany` —— ⚠️ 不在你的 manifest 里,因为它必须对每个依赖一致 |
| **链接模型(引擎)** | `-nostdlib -nostartfiles -static -T`、载荷内 `ld.lld` 绝对路径、无 PT_INTERP |
| **载荷** | clang / lld / llvm-objcopy(**E2 实测已够用**)+ builtins + picolibc |

### 7.3 不用 std 子集的版本(零依赖)

```toml
[package]
name = "toy_kernel"
version = "0.1.0"

[target.riscv64-none-elf]
toolchain     = "llvm@22.1.8"
linker-script = "link.ld"
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic", "-bios", "default", "-kernel"]
```

```cpp
export module toy_kernel;
namespace { volatile char* const kUart = reinterpret_cast<volatile char*>(0x10000000); }
export extern "C" void kmain() {
    for (const char* s = "mcpp bare metal\n"; *s; ++s) *kUart = *s;
}
```

**这个版本 E2 已经在本机真实跑通过**(clang + ld.lld + llvm-objcopy,136 字节裸镜像,零未定义符号)——只是今天得手敲命令行,方案做的就是把这几行命令收进引擎。

### 7.4 库作者侧的最小案例

```toml
[package]
name    = "mcpplibs.tinyfsm"
version = "1.0.0"

[freestanding]
tier    = "core"
example = "examples/freestanding.cpp"
```

```cpp
// examples/freestanding.cpp —— CI 在 riscv64-none-elf 上编它;编不过则证据栏变红
import mcpplibs.tinyfsm;
extern "C" void kmain() { /* 只用文档化的 core 档 API */ }
```

**L0 的库作者什么都不用做** —— 未声明 ≠ 不支持,hosted 用户零影响,裸机用户靠链接期兜底。

---

## 8. 分期与验收判据

每期的判据都写成**可执行的断言**,不是「跑通了」。

| 期 | 内容 | 验收判据(必须是这个,不是「构建成功」) |
|---|---|---|
| **S0** | 堵 E1 静默失败 | `mcpp build --target riscv64-none-elf` 无裸机配置时**指名道姓报错**;不存在「成功且产物是 x86-64」的路径。回归测试直接断言 `file` 输出 |
| **S1** | L1 目标层 + L2 链接层 | 产物 `file` 含 `UCB RISC-V`;`readelf -l` **无 PT_INTERP**;`llvm-nm -u` 为空;`build.ninja` 里 `--target` 出现次数 > 0 |
| **S2** | `import std` 关断 | `os=none` 下 `import std;` 给出说明「为什么」的诊断,而不是 libstdc++ 内部报错 |
| **S3** | L3 runner(前置:qemu 进 xim + harness 硬需求语义) | `mcpp run --target riscv64-none-elf` 在 qemu 里打印出预期串;⚠️ 判据是**「裸机 e2e 实际执行条数 > 0」**,不是「全绿」(§9.1) |
| **S4** | L4 契约层 | 声明缺失不影响解析;声明存在但证据缺失时索引显示「已声明,未验证」;手写证据字段被覆盖 |
| **S5** | L5 包 | BSP + std 子集包进索引;§7.1 的三文件工程端到端通过 |
| **S6** | 载荷 | ⚠️ 验收用例**必须含一次 64 位除法或软浮点**(E3:hello-kernel 是 builtins 缺口的假绿) |

---

## 9. ⚠️ 必须先回答的问题与已知假绿点

按危险程度排序。**前两条要在动手前回答,不是收尾时。**

1. **⚠️ CI 上谁真的跑裸机?** 这条分成两半,**而且只有前一半已经有答案**。

   **(a) 装得上 —— 已解决:qemu 进 xim-pkgindex。** qemu 走 xlings 生态的包索引,与工具链同一套安装机制,mcpp 侧零新增基建,且 xlings/mcpp 两个生态复用同一份包。这与 2026-07-24 决策 #16(「qemu 交叉 test-run = xlings provisioning 免费 + 薄接线」)一致。工程量:一个 xim 包 + `[xlings] deps` 一行。

   **(b) ⚠️ 真的跑了 —— 未解决,而且机制上天然会静默跳过。** 实测 `tests/e2e/run_all.sh`:

   ```
   check_requires():  # requires: 的 token 不在 CAPS 里 → 返回 "should skip"
   CAPS 由 command -v 探测填充
   ```

   **harness 里没有「硬需求」语义** —— 没有任何办法说「这条必须跑,缺能力就让 job 失败」。所以即使 qemu 可安装,只要 CI 没装它,`# requires: qemu` 的裸机 e2e 会**全部静默跳过、整条线永远绿**。这正是 PR#451 的形状(十个新 e2e 因 `# requires: gcc` 在 macOS/Windows 全跳过,放开后才暴露 Windows/macOS 真实失败)。

   **⇒ 需要的改动**(小,但必须在 S3 之前):

   - e2e harness 增加**硬需求**语义(如 `# requires-hard: qemu`):能力缺失时 **FAIL 而不是 SKIP**;
   - 或/并且:CI 矩阵里显式断言 `qemu ∈ CAPS`,把「这个 job 到底跑了几条」变成可见数字。

   ⚠️ **判据不能是「裸机 e2e 全绿」,必须是「裸机 e2e 实际执行条数 > 0」。** 前者在零执行时也成立。
2. **⚠️ hello-kernel 是 builtins 缺口的假绿。** E2 的内核 `llvm-nm -u` 为空、一路全绿,而载荷里 compiler-rt builtins 只有 x86_64(E3)。验收用例必须**故意**触发编译器内建运行时。
3. **⚠️ 探测类代码在 `os=none` 下会假失败。** CMake 因此才有 `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`。mcpp 里所有「编个小程序链一下」的探测(doctor、工具链能力探测、`hasImportStd` 探测)**要在 S1 之前普查一遍**。
4. **⚠️ 单包验证会得出错误结论。** ISA/ABI/code-model 若走 `[build].cxxflags`,toy_kernel 单包能跑通,**加第二个包才碎**(§2.2)。验收工程必须至少有一个依赖。
5. **⚠️ BMI 缓存键必须含 freestanding 轴。** 同一份源码在 hosted / freestanding 下的 BMI 不可互换(cpp20 档位「跨档位 BMI 硬拒」先例)。
6. **⚠️ 新门必须自己回答「逃生舱走不走这里」。** E1 就是活例:守卫挂在 `known` 分支,逃生舱走 `!known`。
7. **⚠️ picolibc 发行版包无 rv64gc**(X11)。用 `rv64imac/lp64` 验证通过不能推广到最常见的 RISC-V profile。载荷的 ISA 覆盖必须是**可查询的数据**,不能是「试了才知道」。
8. **⚠️ 符号审计不是完备保证**(§5.9)。当成完备门比没有门更危险。
9. **⚠️ 「libc++ 子集更宽」是错的排序**(Y1/Y2)。103 vs 53 里 libc++ 的一半是能 parse 的空壳(`std::mutex` 根本不存在),libstdc++ 是硬 `#error`。**别用 include 成功率当能力指标。**

---

## 10. 决策回执(D1–D16 在本方案里怎么落的)

| # | 决策 | 本方案 |
|---|---|---|
| D1 | E1 何时修 | **S0,独立于路线** —— 已发布能力可达的错误行为 |
| D2 | 战略定位 | **Route B**(三条缝 + 包);C 留作方向 |
| D3 | triple 收 `os=none` | **收**,§2.1(即使选 A 也需要它来「识别并拒绝」) |
| D4 | 目标规格放哪 | `kKnownTargets` 加列 + `[target.<triple>]` 逃生舱,§2.2/2.3 |
| D5 | `CLibMode` 新态 | **加 `Freestanding`**,不复用 `None`,§3.1 |
| D6 | BSP 走包还是引擎 | **包**,§6 |
| D7 | `import std` | **硬关断 + 诊断**,S2 |
| D8 | 首个目标 | `riscv64-none-elf` @ qemu virt |
| D9 | CI 怎么真跑 | **(a) 装得上 = qemu 进 xim-pkgindex(已定,xlings/mcpp 双生态复用)**;⚠️ **(b) 真的跑了 = 未决** —— e2e harness 缺硬需求语义,缺能力时是 SKIP 不是 FAIL(实测)。§9.1,S3 的前置 |
| D10 | builtins 来源 | S6;倾向采纳 LLVM Embedded Toolchain |
| D11 | std 子集包时机 | **可与 B 并行**(纯包,不依赖引擎改动) |
| D12 | 裸机线编译器 | **两家都支持,库中立**(Y3);载荷优先 clang(E2 多目标优势) |
| D13 | libc 来源 | **picolibc**;⚠️ 需自建 multilib 或采纳成品(X11) |
| D14 | 子集包形态 | **自写实现中立名字表**,不转发 `.inc`,§6.1 |
| D15 | 生态如何表示 | **声明 + 证据 + 兜底三元组**;声明有(给人看),门只有兜底,§5 |
| D16 | 档位命名 | `core` / `heap` / `hosted`,由 Y4 实测定义;能力集为底,档位是预设 |

### 10.1 对 2026-07-24 决策 #15 的更新

原文:「裸机 / freestanding-modules / ESP32:出范围 / 推迟 —— 需 freestanding modules(无 hosted import std)核心特性;ESP32 三难合一;512KB RAM MCU 上 C++23-modules 优势无关。」

**更新(不是推翻整条,是更新依据):**

| 原理由 | 实测后 |
|---|---|
| 「需要 freestanding modules 核心特性」 | ❌ **模块在裸机上直接可用**(E2:模块接口单元编出 RISC-V 裸机 object)。真正缺的只有 `import std`(E4),而它可以硬关断 |
| 「512KB MCU 上 modules 优势无关」 | ✅ 对 MCU 成立;❌ 对 RISC-V SoC / 内核不成立(toy_kernel 正是后者) |
| 「ESP32 三难合一」 | ✅ 仍然成立 —— **本方案不含 ESP32/Xtensa** |

---

## 11. 更大的方向:openhal / openkal 与「可组合内核」

> ⚠️ **本节不属于本方案范围,也不应捆绑决策。** 记在这里是因为它与 §6.2 的契约形态直接相连,而且本轮为它做了三组探测(K1–K3)。本方案的 L1–L3(引擎三条缝)**无论这个方向做不做都需要,且不依赖它** —— 所以这个方向可以推迟,不阻塞任何东西。

### 11.1 愿景

mcpp-community 官方维护 **openhal**(硬件)+ **openkal**(kernel ABI)+ **AAL**(arch 机制),以此为标准实现一套**可组合的内核/OS**,不限嵌入式,涵盖现代 x86 到无 MMU 的 MCU;freestanding 默认支持一切,KAL 覆盖几乎所有 CPU/kernel,HAL 覆盖所有硬件。

### 11.2 三层在三个不同的位置,KAL 是其中门槛最低的一层

**⚠️ 先纠正一个措辞歧义**:「KAL」有两种可能含义,可行性天差地别。

| 读法 | 含义 | 可行性 |
|---|---|---|
| **KAL-A:抽象内核内部** | 把调度器/内存管理/IPC 做成可替换接口 | ❌ 见 §11.3 —— 横切且策略耦合,无成功先例 |
| **KAL-B:统一 syscall / kernel ABI,转发到不同内核后端** | 应用面对一套 ABI,后端是 Linux / 自研内核 / WASI / 无内核 | ✅ **三层里先例最多的一层** |

本节以下讲的是 **KAL-B**。它与 AAL/HAL 处在**三个不同的位置**:

```
应用 / 生态库 / mcpplibs.std.freestanding
     ↕  KAL   syscall / kernel ABI     后端:Linux · 自研内核 · WASI · 无内核(裸机)
内核实现(任选,不被抽象)
     ↕  AAL   arch 机制                后端:x86_64 · riscv64 · aarch64
硬件
     ↕  HAL   设备                     后端:各种外设
```

**⇒ 做 KAL 完全不需要写内核。第一个后端就是 Linux。** 这是三层里门槛最低、收益最直接的一层。

#### 11.2.1 为什么 KAL-B 可行而 KAL-A 不可行

| | KAL-A(抽象内核内部) | **KAL-B(统一 kernel ABI)** |
|---|---|---|
| 消费者 | 内核自己的子系统 | **应用 / 生态库** |
| 耦合 | 横切 —— 内存模型约束调度器 | **叶子** —— `write()` 不约束调度器 |
| 缝是新造的吗 | 是,要在紧耦合处切一刀 | **否 —— syscall 早就是每个 OS 的稳定 ABI 边界**,只是把已有的缝标准化 |
| 缺能力时 | 内核跑不起来 | **优雅降级** —— 后端不提供 `fork` 就是不提供,落回能力集(§5.3.3) |

#### 11.2.2 先例(这是三层里最厚的)

**KAL-B 不是新想法,是被反复验证的形状:**

- **WASI** —— 字面意义上就是这个:syscall 级 ABI + 多后端实现
- **Rust `std`** —— `sys/unix` / `sys/windows` / `sys/wasi` 三套后端撑同一套 `std` API。**这就是 KAL-B**
- **POSIX** —— 最早的 kernel ABI 统一层
- **libc(musl/glibc)** —— 事实上的 kernel ABI 统一层
- **NT subsystems**(Win32 / POSIX / OS2 跑在同一个 NT executive 上)
- Zircon vDSO · wasi-libc · Go runtime

#### 11.2.3 KA1–KA3 实测:同一份源码,两个内核后端

契约走 C ABI 缝(H1 已证明跨包只有这一条路):

```cpp
export module openkal.io;
extern "C" long kal_write(int fd, const char* buf, unsigned long n);   // 契约
export namespace kal { inline long write(int, const char*, unsigned long); }
```

**同一份 `app.cppm`,零 `#if`,两个世界:**

| 后端 | 实现 | 结果 |
|---|---|---|
| **hosted x86_64-linux** | 转发到真 `::write(2)` | ✅ **实际运行输出 `hello from one source`** |
| **riscv64-none-elf 裸机** | 转发到 MMIO UART | ✅ 链接零未定义符号,**157 字节** |

**KA3 —— 应用对 KAL 的全部依赖面是一个可审计的符号集**,而且两侧完全相同:

```
裸机构建的 app.o   外部符号:  kal_write
hosted 构建的 app.o 外部符号:  kal_write
```

**KA2 —— 换后端不重编应用**:同一个 `app_b.o` 换掉后端目标文件重链即成(C ABI 缝的性质)。

#### 11.2.4 ⚠️ KAL 用 C ABI 是对的 —— 与 AAL 相反

这两层的正确 ABI 形态**恰好相反**,而且两边都有实测支撑:

| | **AAL** | **KAL** |
|---|---|---|
| 典型操作 | `local_irq_disable()` —— **本该是一条 `csrci`** | `write()` —— 本来就要陷入内核 |
| 一次 call 的相对成本 | **灾难性**(1 条指令 vs 一次调用) | 可忽略(远小于 syscall 本身) |
| 正确形态 | **concept + LTO**(K3) | **C ABI**(KA2:换后端不重编) |
| 依据 | K3:跨模块 `-O2` 退化成 `jal`,LTO 恢复 | H1 + KA1–KA3 |

⚠️ 这也正是 Linux 的实际做法(`asm/` 里的 `static inline` vs 导出的 syscall 入口)—— **不是巧合**。

#### 11.2.5 ⚠️ KAL 最大的风险:它会想长成 POSIX

然后你就重写了一遍 musl,并且继承了 POSIX 所有不可移植的部分。

**WASI 的教训是最值钱的一条:不要模仿 POSIX,而是定义真正可移植的那部分。** POSIX 模拟层(Cygwin / MSYS)出了名地漏;能力式重设计(WASI 的 preopened dirs 而不是假装有全局文件系统)才立得住。

⚠️ **K2 的语义鸿沟在这一层会重现且更严重**:`open()` 在没有文件系统的裸机上是什么?返回 `ENOSYS` 假装存在,就是 K2 那个「类型对、语义错」的老问题放大版。

⇒ **正确做法:KAL 的每个操作都挂在能力轴上**,后端声明提供哪些,**不提供的就是编译期不存在**(而不是运行期报错)。这正好复用 §5.3.3 的能力集,不需要新机制。

#### 11.2.6 ⭐ openkal 的第一个真实用户应该是 std,不是假想内核

**KAL = `std` 的后端接口。** Rust 的 `std` 有 `sys/unix` / `sys/windows` / `sys/wasi` —— 那就是 KAL。

而 mcpp 侧的证据已经在手:`std::vector` 在裸机上能用,是因为 `operator new` 这个缝(T3b);`std::print` 需要一个 sink。**把这些缝集合起来命名,就是 KAL。**

⇒ **`mcpplibs.std.freestanding` 是 openkal 天然的第一个消费者**,这给了它一个立刻可验证的验收标准:*同一份 std 子集,在 hosted 与裸机两个后端下行为一致。* 比「为某个假想内核设计 ABI」踏实得多。

#### 11.2.7 后端选择是 target 轴的函数,不是运行期插件

⚠️ 若做成运行期分发(函数指针表),每次调用付一次间接跳转,且丢掉 LTO。**后端应在构建期由 target 决定** —— 零成本,且落回 mcpp 已有的 target 模型(§2.2),不新增轴。

#### 11.2.8 线程模型建在 KAL 上:可行,但有两条边界(TH1–TH6 实测)

**契约面小,而且两家形状几乎一样** —— 这是这条路可行的关键数字:

| | 契约面 |
|---|---|
| libc++ `__external_threading` | **36 个名字**(~30 函数 + 8 类型):mutex×4 · recursive_mutex×5 · condvar×5 · thread×9 · once×1 · tls×3 |
| libstdc++ gthreads | 71 个,**但 17 个是 Objective-C 遗留** ⇒ 真实面 ~30,与 libc++ 高度重合 |

而且 libc++ 已经有 `__thread/support/{pthread,windows,c11,external}.h` **四个后端** —— **这个形状在标准库里已经存在**,openkal 只是在它下面统一。

**经济账:**

```
没有 KAL:  N 个内核后端 × 2 个标准库 = 2N 份实现
有   KAL:  2 个 shim + N 个后端     = 2 + N
```

##### ⚠️ 边界一:KAL 转发调度器,KAL 不包含调度器

契约里的 `thread_create` / `condvar_wait` / `sleep_for` 都要求**有东西能调度**。裸机无 RTOS 时:

- `thread_create` —— 无处可创建
- `condvar_wait` —— 阻塞即死锁(没人能运行来唤醒)
- `mutex_lock` —— 单核无调度器只能自旋,而自旋等一个永不被调度的持有者 = 死锁

⇒ 准确说法:**KAL 让「有调度器的后端」自动适配;「没有调度器的后端」不是适配问题,是它没有这个能力。**

落回能力集(§5.3.3)。X4 已实测不提供时行为是干净的:`no type named 'mutex' in namespace 'std'` —— **编译期不存在,不是运行期坏掉**。

⭐ **推论**:想在裸机上要线程,就**把 RTOS 作为 KAL 后端引进来** —— FreeRTOS / Zephyr 成为 openkal 的后端实现。比「自己写调度器」清晰,且生态可复用。

##### ⚠️ 边界二:`thread_local` 不在 KAL 里,而且今天会静默坏掉

契约里的 `__libcpp_tls_create/get/set` 是**动态 TLS**(pthread_key 风格),KAL 能转发。但 C++ 的 `thread_local` **关键字**用的是编译器 + ABI 级 TLS。TH4–TH6 实测:

```
thread_local int counter;   →  编译 ✅  链接 ✅  零未定义符号 ✅  零诊断 ✅

bump():
    lw  a0, 0x0(tp)      ← 通过 tp 寄存器寻址
    sw  a0, 0x0(tp)

start.S 里设过 tp 吗?  → 没有,只设了 sp
```

⇒ **编过、链过、零诊断,运行期静默读写垃圾地址。**

`thread_local` 属于 **AAL(TLS 寄存器约定)+ BSP(`start.S` 设 `tp` + 链接脚本 `.tdata/.tbss` 布局)**,KAL 转发不了。⚠️ 而且 libstdc++/libc++ 内部有些地方自己用 `thread_local` —— 所以这不是「用户不写就没事」。

**⇒ BSP 契约必须包含「初始化 TLS 基址」这一条**,并且裸机验收用例里要有一个 `thread_local`,否则这个静默失败会活很久。

#### 11.2.9 三层的形态不一样 —— 统一的是能力模型,分层的是绑定机制

**⚠️ 「AAL/KAL/HAL 都是接口包、都用 concept 吗」的答案是:都是接口包,但形态不同,而且理由都是实测的。**

| 层 | 典型操作 | 一次 call 的相对成本 | 绑定时机 | **正确形态** | 证据 |
|---|---|---|---|---|---|
| **AAL** | `local_irq_disable()` = **1 条 `csrci`** | **灾难性** | 编译期,**单选** | **concept + LTO** | K3 |
| **KAL** | `write()` = 本来就要陷入内核 | 可忽略 | **链接期,单选** | **C ABI** | KA2 / H1 |
| **HAL** | `uart.write(span)` = MMIO 循环 | 中等,但**需要多实例** | 编译期,**多选** | **concept** | H3 |

三条判据各自独立:AAL 因为**内联是刚需**;KAL 因为**要能换后端不重编**;HAL 因为**一块板子有两个 UART**。

**⇒ 统一的是能力模型(§5.3.3 的声明 / 检查 / 诊断),分层的是绑定机制。** 不要为了「一致」把三层压成同一种形态 —— 那会在 AAL 上丢掉内联,或在 HAL 上丢掉多实例。

##### ⚠️ 可组合的失败模式:链接器只在三分之一的情况下会报错

实测「两个提供者提供同一个 C ABI 能力」:

| 组合 | 链接器行为 |
|---|---|
| 两个 `.o` 都定义 `operator new` | ✅ **`duplicate symbol` 硬错误**(响亮) |
| 一个 `.o` + 一个 `.a`(如 BSP 分配器 + picolibc malloc) | ⚠️ **静默选 `.o`**(归档只在缺符号时才拉) |
| 两个 `.a` | ⚠️ **静默选第一个** |
| concept 形态的两个提供者(H3) | ✅ 正常共存,各自实例化 |

**⇒ 「恰好一个提供者」必须由 resolver 在包图层面保证,不能指望链接器** —— 三种情况里两种是静默的。

⭐ **好消息:mcpp 的能力模型本来就是这么设计的**(`2026-06-29-feature-capability-model-design.md`:「依赖声明一个 capability 而非具体包,resolver 绑定**恰好一个**提供者」)⇒ **复用它,不要新造。**

#### 11.2.10 ⭐ KAL 的后端不必是内核

**「基于 KAL 实现时不关心 backend 的具体实现,不是内核也没关系」—— 这条已被 KA1 实测。**

裸机后端就是 `kal_write` 转发到 MMIO UART,**根本没有内核**,而应用侧完全不知道(两侧 `app.o` 的外部符号完全相同,都是 `kal_write`)。

⇒ **KAL 的后端是「能力的提供者」,内核只是其中最丰富的一种。** T3b 的 bump 分配器连一行内核代码都没有,但它是一个合格的 heap 能力提供者。

heap 最能说明可组合:

```
std::vector
   ↕ operator new / delete          ← C++ 标准的扩展点(已存在,openkal 不发明)
heap 提供者:picolibc malloc  |  BSP bump 分配器  |  KAL 后端的分配器
```

**三种不同层次的东西都能提供同一个能力,应用完全不知道。**

⚠️ **但正因如此,openkal 不该重新定义 `kal_alloc`** —— heap 的契约 C++ 标准已经定义了(§5.3.4)。**openkal 只定义标准没定义的那些**(文件、时钟、进程、网络、随机源…)。否则就是那条被否掉的「第三套 ABI」。

#### 11.2.11 ⭐ openkal 的正确位置:在 libc 之下,C++ std 建在它上面

**⚠️ 修正 §11.2.10 末尾那条**:那里说「openkal 不该定义 `kal_alloc`,因为 heap 契约 C++ 标准已定义」——**那是把 `operator new` 当成了契约。在这个分层里它是适配器,契约在它下面。**

正确的分层是:

```
std::vector · std::print · std::thread                    ← C++ 标准 API
    ↕  适配器:operator new · __external_threading · stdio sink
                                                          ← C++/标准库自己的扩展点(已存在)
openkal SPEC(POSIX 形状,精简)                            ← ⭐ 要定义的就是这一层
    ↕  后端
Linux syscall · 自研内核 · 裸机 BSP · WASI · semihosting
```

`operator new` **不是契约,是适配器**:它把 C++ 的分配请求转成 openkal 的 `kal_alloc`。T3b 那个 bump 分配器实际上就是这个形状 —— 只是当时把两层写在了一起。

##### SY1–SY3 实测:这个位置是真实存在的,而且面很小

picolibc 已经**把这个接口留好了** —— 它要求下层实现一组 host 函数。从 `libsemihost.a`(一个完整后端)反查,去掉 semihosting 实现细节(`sys_semihost_*`),真正的**契约面**是:

```
close · _exit · fstat · getentropy · getpid · gettimeofday · isatty · kill
lseek · lseek64 · open · read · sysconf · times · unlink · write · _map_stdio
```

**17 个 POSIX 形状的函数。** 另一端 `libdummyhost.a` 是最小后端 —— **只定义了 `_exit` 一个**。

⇒ **openkal v0 的规模是可估的:**

| 组成 | 数量 | 来源 |
|---|---|---|
| libc 位置原语(文件/时间/随机/进程/退出) | **~17** | SY2 实测 |
| 线程原语 | **~30** | TH1/TH2 实测 |
| 分配(`sbrk` 或 arena) | ~2 | T3b |
| **合计** | **≈ 50** | |

**≈50 个函数是一个季度能写完 SPEC 的规模,不是 POSIX 那 1000+。**

⭐ 而且 **openkal 的第一个后端不用从零写**:picolibc 的 `libsemihost.a` 就是现成的一个,`libdummyhost.a` 是最小骨架。**openkal 要做的是把这个位置标准化,不是发明它。**

##### ⚠️ 三个必须先回答的问题

**1. openkal 与 libc 的关系**

| 选项 | 后果 |
|---|---|
| (a) openkal **替代** libc | 等于重写 musl —— §11.2.5 警告的那条路 |
| (b) openkal 在 libc **之上** | 只是包装,价值小 |
| **(c) openkal 在 libc 之下,libc 与 C++ std 都建在它上面** | ✅ **WASI / picolibc 的实际形态** |

⇒ **(c)**。picolibc 已经把接口留好了(SY2 的那 17 个),这不是假设。

**2. ⚠️ 为什么不直接用 WASI?**

WASI 已经占了这个位置,有完整 SPEC 和成熟的能力式设计。**这个问题必须先回答,否则会被反复问。** 诚实的立场:

- WASI 的 **ABI 是 wasm-specific**(线性内存、模块 import),映射到裸机 ELF 不自然;
- 但 WASI 的**语义设计极其值得借鉴** —— 能力式、preopen、拒绝「全局环境」的假设。

⇒ **借鉴 WASI 的语义,不采用它的 wasm ABI。** 并且这条要写进 SPEC 的开头,而不是留给别人猜。

**3. ⚠️ 范围纪律**

50 是 v0;POSIX 是 1000+。**每加一个原语都必须回答「哪个后端做不到它」** —— 做不到的就该是**能力**(§5.3.3),不该是原语。⚠️ 这是 §11.2.5「KAL 会想长成 POSIX」那条风险的具体防线。

#### 11.2.12 ⚠️ 方向性修正:openkal 不能是 POSIX/libc 形状

§11.2.11 用「libc 位置的 17 个函数」来估规模,**那已经把 POSIX 偏见带进了 SPEC**。openkal 的核心价值是**「基于它开发的库天然跨 OS」**,而 Windows 的原生层不是 libc —— 是 Win32/NT(句柄不是 fd、没有 `lseek` 语义、错误模型不同、没有 `fork`)。

**把 openkal 定义成 POSIX 形状 ⇒ Windows 后端只能靠模拟 ⇒ 就是 Cygwin**,出了名地漏且慢,Windows 永远二等公民。

##### 正确的形状:看什么真的做到了「一套 API 多个 OS」

**Rust `std` / Go / .NET / Java NIO 全都不是 POSIX 形状。** `std::fs::File` 没有 fd、没有 `lseek`,它自然映射到 `open()` **和** `CreateFile()` 两边。

| POSIX 形状(会锁死在 Unix) | 中立形状(两边都自然) |
|---|---|
| `int open(path, flags)` → fd | `kal_file_open(path, mode)` → **handle** |
| `lseek(fd, off, whence)` | `kal_file_seek(handle, off, origin)` |
| `fstat` → POSIX `struct stat` | → 中立 `kal_file_info`(size/kind/mtime) |
| **`errno` 全局变量** | **返回值携带错误码**(errno 是线程局部全局,跨 OS 极别扭) |
| `fork()` | ❌ **不进 SPEC**(Windows 没有,且它是能力不是原语) |

##### ⚠️ 但上面这张表的推导方向仍然是错的 —— 见 §11.2.14

那张表做的是「**POSIX 的中立化重命名**」(`open` → `kal_file_open`),它仍然继承了 POSIX 的世界观:文件是字节流、路径是全局命名空间、句柄是整数、错误在一个统一空间里。**正确的方向是反过来的**,见下一节。

##### ⭐ 可执行的设计规则(这条仍然成立)

> **每个原语必须能在 Linux syscall / Win32 / 裸机 / WASI 四个后端上「自然实现」,不需要模拟层。做不到的就不是原语,是能力(§5.3.3)。**

⚠️ **反直觉的推论:这会让 SPEC 更小,不是更大。** POSIX 里大量东西(`fork` / signal / fd 继承 / `/proc` / 权限位)在中立形状下根本进不去。

重估 v0 规模(形状变了,数字反而更清楚):

| 组 | 数量 |
|---|---|
| 文件(open/close/read/write/seek/info/remove/rename) | 8 |
| 目录(WASI 式 preopen) | 3 |
| 时间(monotonic / realtime / sleep) | 3 |
| 随机 · 退出 · 控制台 sink · 分配 | 6 |
| **中立核心** | **≈ 20** |
| 线程(**是能力,不是核心**) | ~30 |

⚠️ **Windows 后端要直接接 Win32,不经过 MSVC CRT 的 POSIX 兼容层**(`_open`/`_read` 那套)—— 否则绕一圈又回到模拟。

#### 11.2.13 openhal 能否统一现代 OS 与嵌入式:能,但有一堵硬墙

**能统一的部分已被生态验证,不是设想。** `linux-embedded-hal`(rust-embedded 官方 WG)把 `embedded-hal` 的 trait 实现在 **Linux userspace** 上,走 spidev / i2cdev / gpio-cdev / serialport ⇒ **同一个驱动 crate 既跑裸机 MCU,又跑树莓派 Linux。**

**⚠️ 不能统一的部分是一堵硬墙**:现代 OS 的应用**碰不到设备寄存器**,也**注册不了 ISR** —— 那是内核的领地。抽象跨不过去。

⇒ 干净的切分:

| 层 | 内容 | 能否统一 |
|---|---|---|
| **openhal(设备服务)** | Console / Serial / Spi / I2c / Gpio / Pwm / Adc | ✅ 裸机走 MMIO,Linux 走 `/dev/*`,Windows 走对应 API |
| **寄存器 / ISR / DMA** | 直接 MMIO、中断向量、DMA 描述符 | ❌ 只有 kernel / 裸机侧能做 ⇒ 属 **AAL / 驱动层**,不进 openhal |

##### ⚠️ K2 的语义鸿沟在这里必然重现,而且更隐蔽

一个假设 **µs 级 GPIO 翻转**的 openhal 驱动,在 Linux 上**类型完全正确、能编能跑,但慢 1000 倍且非确定性**(syscall + 调度抖动)。

⇒ **时序 / 实时性必须建模成能力**(`realtime` · `direct-mmio` · `isr`),**不是可以藏起来的实现差异** —— 与 `mmu`(§11.4)是同一类问题、同一个解法。

#### 11.2.14 ⭐ 正确的推导方向:openkal 是可组合的能力接口 SPEC,不是 POSIX 的重命名

**两种推导方向,结果完全不同:**

| | 方向 A(§11.2.12 那张表) | **方向 B(正确)** |
|---|---|---|
| 怎么来的 | 看 POSIX 有什么 → 中立化 → openkal | **openkal 独立定义可组合的能力接口 → 各 kernel 用自己的机制去实现它** |
| 结果 | 换了名字的 POSIX,继承其世界观 | 真正中立的 SPEC |

**⚠️ 这不是理论之争 —— WASI 自己走过这两步并付了学费:**

- **WASIp1** = POSIX-like(fd、POSIX 风格文件系统调用)= 方向 A
- **WASIp2** = **推倒重来**,「capability-based,**redesigned around interfaces instead of POSIX-ish file descriptors**」,引入 **WIT interface + world + resource**
- 官方定性:**"Preview 2 isn't an upgrade to Preview 1. It's a complete redesign."**

⇒ **openkal 应该直接从方向 B 起步,不要重走一遍 p1。**

##### 形状上具体变了什么

| | 方向 A | **方向 B** |
|---|---|---|
| SPEC 的单位 | **函数列表**(~50 个) | **能力接口**(io/streams · clocks · random · filesystem · threads …) |
| 目标环境怎么描述 | 「支持哪些函数」 | **world = 接口的组合** |
| 句柄 | 整数 fd / handle | **resource:有类型、有所有权、生命周期明确** |
| 错误 | 返回码 / 统一错误空间 | **每个接口定义自己的错误类型**(`result<T, E>`) |
| 后端职责 | 实现这 50 个函数 | **用自己的机制实现它能实现的那些接口** |
| 缺能力时 | 返回 `ENOSYS` | **接口根本不在这个 world 里 ⇒ 编译期不存在** |

⭐ **最后一行与前面所有实测对齐**:X4 测到 `std::mutex` 不提供时是 `no type named 'mutex' in namespace 'std'` —— **编译期不存在,不是运行期 ENOSYS**。**能力缺失应该在类型系统里,不在返回值里。**

##### ⇒ 「openkal v0 ≈ 50 个函数」这个说法要换掉

正确说法:**openkal v0 = 少数几个接口 + 少数几个命名 world。**

```
接口(各自独立版本化、独立实现):
  kal:io/streams            字节流读写      —— 不预设「文件」
  kal:clocks/monotonic      单调时钟
  kal:clocks/wall           墙钟
  kal:random                随机源
  kal:memory/alloc          分配
  kal:filesystem            文件系统        —— 只有有 FS 的后端提供
  kal:concurrency/threads   线程            —— 只有有调度器的后端提供(§11.2.8)
  kal:sockets               网络

world(= 能力集的命名预设,与 §5.3.3 同一条原则):
  world bare    = { io/streams, clocks/monotonic, memory/alloc }
  world rtos    = bare + concurrency/threads
  world posix   = rtos + filesystem + clocks/wall + random + sockets
  world windows = 同 posix,后端不同
```

⭐ **KA1 实测跑通的那个形态,正好就是 `world bare` 的一个片段**(`kal_write` 一个函数撑起 hosted + 裸机两个后端)。起点已经在手。

##### ⚠️ 四条必须先想清楚的

**A. 与 C ABI 的张力 —— 这是真实工作量,不能省。**
WIT 有一整套 canonical ABI 表达 resource / `result<T,E>` / string。**纯 C ABI 上表达「有类型的 resource + 结构化错误」要自己定 calling convention。** 而 H1 已证明 C++20 模块下跨包提供实现**只有 C ABI 一条路** ⇒ **openkal 必须自带一份「C ABI 映射规范」。**

**B. 组合爆炸要靠命名 world 压住。**
20 个独立接口理论上 2^20 种组合。必须定义**少量命名 world** 作预设 —— 与 §5.3.3「档位是能力集上的命名预设」是同一条原则,在 SPEC 层复用。

**C. ⚠️「后端用自己的东西实现」会撞上语义不可实现。**
例:接口定义「原子重命名」,FAT 后端做不到。此时是接口分裂,还是能力再细分?**WASI 在 filesystem 上就卡了很久。** 这是 SPEC 设计里最耗时的部分,是设计问题不是编码问题。

**D. ⭐ 最重要:先做窄的。**
WASIp1 想一步到位覆盖 POSIX,结果被迫推倒重来。**openkal 从最小可用 world 起步**(`io/streams` + `clocks/monotonic` + `memory/alloc`)—— 正好是 KA1 已跑通的那个形状。

#### 11.2.15 现代 C++ 惯用法:每个特性解决哪个具体问题(M1–M5 实测)

抽象层要同时做到 **可组合 / 简单使用 / 能组合出复杂**,靠的是几个各司其职的特性 —— 不是「用上新特性」,而是**每个特性对应一个具体问题**。

| 问题 | 特性 | 怎么用 |
|---|---|---|
| **能力契约** | `concept` | `OutputStream` / `MonotonicClock` / `Allocator` / `Threads` |
| **world = 接口的组合** | **concept 的 `&&`** | `RtosWorld = BareWorld<W> && requires(W& w){{ w.threads() }->Threads;}` |
| **结构化错误(替代 errno)** | `std::expected` | `write() -> expected<size_t, io_error>`;⭐ E5 实测 `<expected>` **零 libc 即可用** |
| **后端只写最小面** | **deducing this**(C++23) | 后端只实现 `write()`,基类的 `write_all()` 用 `this auto&& self` 免费给出 |
| **可选能力做增强** | `if constexpr` | 有 `MonotonicClock` 就加时间戳,没有就不加 |
| **能力缺失的诊断** | 受约束模板 / `static_assert` | `static_assert(!RtosWorld<W>)` 双向钉住 |
| **零成本** | 模板单态化 + **LTO** | K3:跨模块必须 LTO |

##### M1/M2 实测:整套在裸机上跑通

```cpp
// SPEC:接口 = concept,world = concept 的组合
template <class W> concept BareWorld = requires(W& w){{ w.out() }->OutputStream;}
                                    && requires(W& w){{ w.clock() }->MonotonicClock;}
                                    && requires(W& w){{ w.mem() }->Allocator;};
template <class W> concept RtosWorld = BareWorld<W> && requires(W& w){{ w.threads() }->Threads;};

// deducing this:后端只实现 write(),write_all() 免费
struct stream_ops {
    auto write_all(this auto&& self, std::span<const char> b) -> std::expected<void, io_error> {
        while (!b.empty()) { auto n = self.write(b); if (!n) return std::unexpected(n.error());
                             b = b.subspan(*n); }
        return {};
    }
};

// if constexpr:可选能力是「增强」,不是「降级」
template <BareWorld W> void log(W& w, std::span<const char> msg) {
    if constexpr (MonotonicClock<decltype(w.clock())>) { /* 加时间戳 */ }
    (void)w.out().write_all(msg);
}
```

应用侧用 `static_assert` **双向**钉住:

```cpp
static_assert( kal::BareWorld<bsp::BareWorld>);
static_assert(!kal::RtosWorld<bsp::BareWorld>);   // ⭐ 反向也钉,防止 world 悄悄变宽
```

链接零未定义符号,**text 698 字节**。

##### ⭐ M3:零成本是实测的,不是声称的

`kmain` 的反汇编里 **`jal` 条数 = 0** —— `log` → `write_all` → `write` → `expected` 整条链、`rdtime` 读时钟、取模运算,全部内联成直线代码。

⚠️ **前提是 LTO**(K3):跨模块 `-O2` 会退化成真实调用。

##### ⭐ M4/M5(已修正):诊断本身是优秀的 —— ICE 的真因是 BMI flag 不一致

⚠️ **本节初稿断言「clang 22 + 模块 + concept 诊断 = ICE,是路线前置阻塞」。那是错的**,而且错在一个不合格的对照:崩溃组与对照组**同时换了两个变量**(模块 vs 头文件、以及 target/标准库配置)。逐个隔离后:

| 隔离步骤 | 结果 |
|---|---|
| ISO-1 宿主 + 正常 libc++ + 模块 + 失败 concept | ✅ 诊断完美 |
| ISO-2 加回 `--target=riscv64-none-elf -ffreestanding` | ✅ 正常 |
| ISO-3 概念里用 `std::` 类型(import std 子集) | ✅ 正常 |
| ISO-4 两个 `import` 写同一行 | ✅ 正常(clang 接受;⚠️ GCC 会报解析错) |
| ISO-6 `deducing this` + 跨模块继承 | ✅ 正常 |
| **真因:BMI 建于 `-O2`,另一个 BMI 建于 `-O2 -flto`,混用** | ❌ **ICE** |
| **同一组 flag 重建全部 BMI** | ✅ **诊断完美** |

flag 一致后的实际诊断质量(比预期好得多 —— 精确到缺哪个成员):

```
error: static assertion failed: world 缺 threads 能力
note: because 'bsp::BareWorld' does not satisfy 'RtosWorld'
note: because 'w.threads()' would be invalid: no member named 'threads' in 'bsp::BareWorld'
```

GCC 16.1 + 模块同样干净,并标出模块归属(`bsp::BareWorld@bsp.bare`)。

⇒ **惯用法完全成立,不存在路线阻塞。**

##### ⚠️ 但这条更正带出一个更重要的结论

**「BMI 的 flag 一致性」不是优化问题,是「不一致会让编译器崩」。**

- 这**抬高了 §9 风险 5(BMI 缓存键必须含 freestanding 轴)的等级**:后果不是「结果不对」,而是**编译器崩溃栈**。
- ⭐ **而 mcpp 恰恰有整套指纹/缓存键系统就是为了防这个**(`toolchain/fingerprint.cppm` + BMI cache key)。我这次是手敲命令行**绕过了 mcpp 的保护**才踩到 —— **mcpp 的用户按设计不会遇到**。
- 与 C++20 档位设计「跨档位 BMI 硬拒」是同一族问题,那条先例现在有了更硬的理由。

⚠️ ICE 本身仍是 clang 的 bug(正确行为应是「BMI 以不兼容的 flag 构建」这类诊断,而不是崩溃),值得上报 —— 但**它不阻塞这条路线**。

⚠️ 另有一条 GCC 模块小坑(与 ICE 无关):`import a; import b;` 写在**同一行**会被 GCC 报 `expected end of line before 'import'`(clang 接受)。**每行一个 import。**

##### 两层 API:可组合在下,简单在上

⚠️ 别让所有用户都写模板参数。正确做法是**两层**:

| 层 | 形态 | 给谁 |
|---|---|---|
| **底层(可组合)** | 模板 + concept,**显式传 world / device 对象** | 库作者、多实例、可测试(注入假 world 做单测) |
| **上层(简单)** | 对**构建期选定的那一个 world** 的薄封装,自由函数 | 90% 的应用作者 |

```cpp
// 底层:可组合、可注入
template <BareWorld W> void log(W& w, std::span<const char> msg);

// 上层:简单
namespace kal { inline void log(std::span<const char> msg) { log(default_world(), msg); } }
```

⭐ **而 world 由构建期 target 轴选定(§11.2.7),所以上层那个 `openkal` 模块接口可以「按 world 生成」** —— 后端没有的能力**在模块接口里根本不导出**。这给出最干净的「编译期不存在」:`kal::spawn` 是**未声明的标识符**,而不是一个受约束模板报 constraint 失败(也就绕开了上面那个 clang ICE)。

这与 libc++ 每目标一份 `__config_site`(E5)是同一个模式,不是新发明。

### 11.3 ⚠️ AAL 与「内核策略」:这条线必须画死

以下针对的是 **KAL-A 那种读法**(抽象内核内部),它与 §11.2 的 KAL-B 是两件事。

| | **HAL** | **KAL** |
|---|---|---|
| 抽象的对象 | UART / SPI / GPIO / 定时器 | 上下文切换 / 陷入向量 / 页表 / 调度 / 锁 |
| 结构 | **叶子** —— 一个 UART 驱动不关心系统里任何别的东西 | **横切且相互约束** —— 内存模型约束调度器,中断模型约束加锁 |
| 实例 | 多个(两个 UART) | 单例(一个 MMU 模型、一套陷入向量) |
| 现实中的成功先例 | ✅ **`embedded-hal`(Rust)—— 广泛采用** | ❌ **没有成功的「可复用生态契约」先例** |

**KAL 不是没人试过,而是既有先例全都在「一个内核内部」**:Windows NT 的 `HAL.DLL`、Linux 的 `arch/`、NetBSD 的 `sys/arch/`。它们都服务于**一套固定的内核策略**,不是跨生态的契约。

⇒ **建议把野心拆成两半,分别对待:**

| 层 | 内容 | 可行性 |
|---|---|---|
| **AAL(arch 机制抽象)** | 上下文保存/恢复、陷入向量安装、页表项操作、原子/屏障/cache、per-CPU、tick 源、boot 交接 | **形状已被证明**(Rust 的 `x86_64` / `riscv` crate、Linux `arch/` 的大部分)。签名清晰、按 arch 不同、**不编码策略** |
| **内核策略** | 调度算法、内存分配策略、IPC 设计、安全模型 | **这就是内核本身**。抽象它 = 写内核框架,是另一件事,历史成功率低得多 |

**⚠️ 命名会招来范围蔓延**:叫 "KAL" 会不断把策略吸进来。建议边界写死在名字里(AAL),或在文档第一行就把「机制 vs 策略」的线画出来。

### 11.4 ⚠️ K1/K2:「不管有没有 MMU」是一条藏不住的缝

实测:同一个 `AddressSpace` concept 下

```
RiscvSv39::map(0x8000_0000, 0x9000_0000) → true    真重映射
NoMmu    ::map(0x8000_0000, 0x9000_0000) → false   只能恒等
```

**两者都完全满足 concept,编译期无法区分。** 通用代码做非恒等重映射,在 NoMmu 上静默失败。

**concept 表达语法,不表达语义。** MMU 的有无决定了上面的代码分成两族(有 fork / demand paging / 地址空间隔离 vs 没有),**不是一个统一接口能覆盖的**。

⇒ 正确建模:**MMU 是能力轴(`cfg(mmu)` / `requires=["mmu"]`),不是契约的一个实现**。这恰好落回 §5.3.3 的能力集模型 —— 好消息是不需要新机制,坏消息是「一套 KAL 通吃有无 MMU」这个具体承诺不成立。

### 11.5 ⚠️ K3:零成本抽象跨模块默认失效,LTO 是必需项

同一段通用代码 `setup(arch, va)`,三种构建:

| 构建 | `disable()`(应为一条 `csrci`) | |
|---|---|---|
| 同 TU + `-O2` | **完全内联**:`csrci`/`csrsi` + 3 条指令 | ✅ |
| **跨模块 + `-O2`** | **真实 `jal` 调用** | ❌ |
| 跨模块 + `-flto` | `jal` = 0,内联进来 4 条 csr | ✅ |

⇒ **内核热路径(irq 开关、per-cpu、页表项)一旦跨包,默认退化成函数调用。** `local_irq_disable()` 本该是一条指令。

**KAL 的构建模型必须把 LTO 当必需项,不是优化项。** 而 LTO 在内核上有自己的代价(链接脚本 section 放置、inline asm 约束、编译期),这条要提前进设计,不能等热路径慢了再补。

⚠️ 同时回顾 §6.2.2-H1:跨包提供实现**只有 C ABI 一条路**(模块归属烙进符号名)。两条合起来给出 KAL 的形态约束:

- **inline 关键的原语**(irq、per-cpu、页表项、原子)→ **concept/模板 + LTO**,不能走 C ABI 边界
- **粗粒度操作**(boot 交接、驱动注册)→ C ABI 可以
- 这恰好是 Linux 的实际做法(`asm/` 里的 `static inline` vs 导出函数)—— **不是巧合**

### 11.6 战略边界(必须守住的两条)

1. **⚠️ mcpp 引擎永远不认识 HAL/KAL 任何概念。** 引擎只有 L1–L3 三条缝,openhal/openkal 是**包**。一旦引擎开始理解 KAL,§1「不做板子数据库」的失败模式会以大得多的规模回来。
2. **这是与「mcpp = 构建现代 C++23 应用的最佳工具」不同的产品。** 十年尺度、不同用户群。这是维护者的战略选择;但它**必须是显式决策**,而不是作为 freestanding 工作的自然延伸悄悄发生。

### 11.7 如果要走,第一步该测什么

**不是设计整套 KAL,而是拿最硬的两个原语试抽象会不会碎** —— 碎在这里,后面全是幻觉:

1. **上下文切换** —— 寄存器保存布局按 arch 完全不同,且与调度器 ABI 耦合;
2. **页表项** —— x86-64 四/五级、riscv Sv39/48/57、aarch64 stage-1/2、无 MMU,**差异巨大且语义不对齐**(K2 已经在最简单的形态上碎了一次)。

判据不是「编过了」,而是:**一段通用内核代码能不能在两个真实不同的 arch 上既类型正确又语义正确,且 LTO 后无额外指令。**

---

## 12. 三层架构合成:成形了什么,没成形什么

> 本节把 §11 全部讨论收敛成一张可 review 的表。**结论:机制层面成形了,内容层面没有。**

### 12.0 成形度诚实评估

| | 状态 |
|---|---|
| **每层的形态**(concept vs C ABI、绑定时机) | ✅ **实测确定**(K3 / H1 / KA2 / H3) |
| **每层的边界**(什么进来、什么不进来) | ✅ 确定,且每条边界都有反例支撑(K2 / TH6 / linux-embedded-hal) |
| **横切模型**(能力集 + 编译期不存在) | ✅ 确定,且复用 mcpp 已有机制 |
| **现代 C++ 惯用法** | ✅ 实测跑通且零成本(M1–M3:`jal` = 0) |
| **具体接口清单**(每层有哪些接口、每个什么形状) | ❌ **未定** |
| **KAL 的 C ABI 映射规范**(resource / `result<T,E>` 怎么在 C ABI 上表达) | ❌ **未定,且是真实工作量** |
| **AAL 两个最硬原语**(上下文切换、页表项)会不会碎 | ❌ **未验证** —— §11.7 的第一步 |
| 语义不可实现的处理策略(FAT 做不到原子重命名那类) | ❌ 未定,SPEC 设计里最耗时的部分 |
| 治理(谁维护、怎么提案、怎么版本化) | ❌ 未定 |

**⇒ 架构成形 ≠ SPEC 成形。** 现在可以开始写 SPEC 了,但 SPEC 本身还没写。

### 12.1 三层各自是什么

#### AAL — Arch Abstraction Layer

| | |
|---|---|
| **解决的问题** | 内核 / bootloader / RTOS 的代码怎么跨 CPU 架构,**而不失去零成本** |
| **设计** | **concept + LTO**(K3:跨模块 `-O2` 会退化成 `jal`,LTO 恢复到 0);编译期单选;**只含机制不含策略** |
| **边界** | 上下文切换 · 陷入向量 · 页表项 · 原子/屏障 · per-CPU · tick · boot 交接。**调度算法 / 分配策略 / IPC / 安全模型不进来** —— 那就是内核本身 |
| **⚠️ 已知裂缝** | K2:MMU 有无**不是一个接口能藏的**(`NoMmu` 满足 concept 但只能恒等映射)⇒ 是能力轴 `cfg(mmu)`,不是实现差异 |
| **应用场景** | **最窄的一层** —— 只有写内核 / hypervisor / bootloader / RTOS 的人用。应用开发者永远不接触 |
| **生态** | 每个 arch 一个包(`aal-riscv64` / `aal-x86_64` / `aal-aarch64`)。数量少(arch 就那么几个)但每个都深。对标 Rust 的 `riscv` / `x86_64` crate、Linux 的 `arch/` |
| **风险** | 最高。两个最硬原语(上下文切换、页表项)**没验证过**;先例全在「单个内核内部」,没有跨生态成功案例 |

#### KAL — Kernel ABI Layer

| | |
|---|---|
| **解决的问题** | 库 / 应用怎么跨 OS(**包括「没有 OS」**)而只写一遍 |
| **设计** | **C ABI**(H1:跨包提供实现只有这一条路;KA2:换后端不重编);**能力接口 + world 组合**(WASIp2 形状,不是 POSIX 函数列表);位置**在 libc 之下**,libc 与 C++ std 都建其上 |
| **边界** | 后端**不必是内核**(KA1:裸机后端就是 MMIO 转发,没有内核)。KAL **转发**调度器,**不包含**调度器。⚠️ `thread_local` 不在 KAL 里(TH6:属 AAL + BSP,且今天会静默坏) |
| **⚠️ 已知裂缝** | 语义不可实现(FAT 做不到原子重命名);POSIX 化冲动(WASIp1 的教训) |
| **应用场景** | **最宽的一层** —— 任何想跨 hosted/裸机的库与应用;也是 `mcpplibs.std.freestanding` 的后端接口 |
| **生态** | 接口包(openkal SPEC)+ N 个后端包(linux / windows / bare / wasi / rtos)+ **2 个 stdlib shim**(gthreads / `__external_threading`)。⭐ **第一个后端是 Linux,不用写内核** |
| **风险** | 中。形状已被 WASI/POSIX/Rust `std` 反复验证;主要风险是**范围失控**和 C ABI 映射规范的工作量 |

#### HAL — Hardware Abstraction Layer

| | |
|---|---|
| **解决的问题** | 外设驱动怎么跨板子,**以及跨「裸机 vs 现代 OS」** |
| **设计** | **concept + 对象注入**(H3:`Uart` 与 `Uart2` 共存、零成本);编译期**多选**(一块板子多个 UART) |
| **边界** | **设备服务层**(Console / Serial / Spi / I2c / Gpio / Pwm / Adc)。⚠️ **寄存器 / ISR / DMA 进不来** —— 现代 OS 应用碰不到,那是 AAL/驱动层 |
| **⚠️ 已知裂缝** | 假设 µs 级 GPIO 的驱动在 Linux 上**类型全对、能跑、慢 1000 倍且非确定** ⇒ 时序/实时性必须是能力(`realtime` / `direct-mmio` / `isr`) |
| **应用场景** | 写外设驱动的人 + 用驱动的应用。嵌入式最常见的日常工作 |
| **生态** | **规模最大的一层** —— 每个外设一个驱动包,每个板子一个 BSP。对标 `embedded-hal` 生态(数百个驱动 crate)。⭐ **`linux-embedded-hal` 已证明同一套 trait 能同时服务裸机 MCU 与 Linux** |
| **风险** | 最低。先例最厚、机制已验证(H3) |

### 12.2 三层怎么互相依赖(以及不依赖)

```
写应用的人        →  KAL          (+ HAL 的设备,如果碰硬件)
写驱动的人        →  HAL
写内核的人        →  AAL  +  提供 KAL
```

⭐ **HAL 与 KAL 互不依赖**(一个管设备,一个管 OS 服务);**AAL 只有内核作者接触**。三层不是一个栈,是**三个正交的接缝**。

### 12.3 唯一的横切概念:能力模型

三层的绑定机制不同,但**声明 / 检查 / 诊断是同一套**:

| | |
|---|---|
| 谁提供 | 目标规格(§2.2)∪ BSP ∪ KAL 后端 ∪ HAL 提供者 |
| 谁需要 | 库的 `tier` ∪ `requires`(§5.3) |
| 判据 | **缺能力 = 编译期不存在**(X4 实测:`no type named 'mutex'`),不是运行期 `ENOSYS` |
| ⚠️ 唯一性 | **resolver 必须保证「恰好一个提供者」** —— 链接器只在三种情况里的一种会报错(两个 `.o` 冲突响亮;`.o`+`.a` 与两个 `.a` **都静默**) |
| 验证 | 符号审计(Y5);裸机链接天然是硬门(§7.5.3) |

⚠️ **一条贯穿所有层的硬约束**:**BMI 的 flag 必须一致** —— 不一致不是「结果不对」,是**编译器崩溃**(ISO 系列)。mcpp 的指纹/缓存键系统正是防这个的。

### 12.4 建议的推进顺序(按「门槛 × 先例厚度」排)

| 顺序 | 层 | 为什么排这里 |
|---|---|---|
| **1** | **KAL 最小 world** | KA1 已跑通(`kal_write` 一个函数撑起两个后端);**第一个后端是 Linux,不用写内核**;而且它是 `mcpplibs.std.freestanding` 的天然后端,验收标准立刻可验证 |
| **2** | **HAL** | 先例最厚(`embedded-hal` + `linux-embedded-hal`),机制已验证(H3),生态规模最大 |
| **3** | **AAL** | 只有真要写内核时才需要;⚠️ 且有两个未验证的硬原语(§11.7)—— **碎在那里,后面全是幻觉** |

⚠️ 三条都建立在 §1–§10 的引擎三条缝之上,而**那三条缝无论这三层做不做都需要**。

---

## 13. 未定项收敛(本节把 §12.0 的 ❌ 逐条做掉)

### 13.1 ✅ AAL 两个最硬原语:不碎,但裂缝分两类(AAL-1/AAL-2 实测)

§11.7 说「碎在这里后面全是幻觉」。实测做了:`Context`(不透明)+ `PageTable`(map/unmap/translate)两个 concept,两个**真实不同**的 arch 实现(riscv Sv39 风格 PTE 位 V/R/W/X/U vs x86-64 风格 P/RW/US/NX,Context 布局完全不同),一段通用内核代码。

**AAL-1:通过。** 同一段 `setup_task()` 对两个 arch 各实例化一次,**`call` 条数 = 0**(零成本)。

**AAL-2:主动找裂缝,找到三条,而且分成两类 —— 这是本节的核心结论。**

| # | 裂缝 | 表现 | 类型 |
|---|---|---|---|
| 1 | **execute-only 权限**:riscv 支持 X 不带 R,x86-64 经典分页做不到 | **两边都编过**,x86-64 实现静默给了 R+X | **A 类** |
| 2 | **页大小按下标取** `page_sizes[1]` | 编过;riscv/x86 恰好都是 2M,但 aarch64 16K granule 下是 32M | **A 类** |
| 3 | **通用代码想设 syscall 返回值** `c.a0 = v` | `error: no member named 'a0' in 'RvSv39::Context'` | **B 类** |

| 类 | 特征 | 处理 |
|---|---|---|
| **A 类:语义鸿沟(危险)** | **类型对、能编、静默错** | **必须提到能力轴**,不能留在接口里 |
| **B 类:表达缺失(安全)** | **编译期硬错** | 说明这件事**本来就不该通用** —— 走 arch 专属接口 |

**⇒ 定下来的设计规则:**

> **凡是「能编但语义可能不同」的,提到能力轴;凡是「编不过」的,承认它不属于通用层。**

具体修正(三条裂缝各对应一条):

1. `Perm` **不做自由 bitset**,改成**命名组合**(`RO` / `RW` / `RX` / `RWX`),`XO`(execute-only)是**能力** `perm-xo`,arch 声明支不支持;
2. 页大小**不按下标取**,按**语义命名**(`PageSize::Base` / `Large` / `Huge`),或直接传字节数由 arch 拒绝;
3. Context 的**细粒度寄存器访问不进通用层** —— 需要它的操作(如 syscall 返回值)是 arch 专属接口。

⚠️ 这与 K2(MMU)、§11.2.13(实时性)是**同一条规则的三次应用**。它现在是 AAL/KAL/HAL 三层共用的判据。

### 13.2 ✅ KAL 的 C ABI 映射规范 v0(CABI 实测)

§11.2.14-A 说这是「真实工作量」。规范定如下,**编码选择有实测支撑**。

#### resource → 不透明有类型指针

```c
typedef struct kal_stream_s* kal_stream_t;   // C 层就类型安全,不同 resource 不能互串
```

⚠️ **所有权 C ABI 表达不了** ⇒ 约定 `kal_<t>_close(h)`;**所有权归 C++ 包装层的 RAII**,SPEC 只文档化不变量。

#### `result<T,E>` → **2 字结构返回**(不是出参)

```c
typedef struct { kal_err err; size_t value; } kal_result_usize;   // 0 == ok
```

**实测两个 arch 都更便宜:**

| 编码 | x86-64 | riscv64 |
|---|---|---|
| **2 字结构返回** | **9 条指令** | **10 条指令,2 次内存存取** |
| 出参 + 错误码 | 12 条指令,3 次栈存取 | 12 条指令,4 次内存存取 |

riscv 反汇编显示结果直接走 `a0`(err)/`a1`(value),`snez`/`and` 就地做了分支消除,**不落栈**。

⚠️ **限制:`T` 必须 ≤ 一个机器字**,否则结构返回退化成隐藏指针。更大的载荷走 resource 句柄。

#### 其余四条

| 项 | 规范 |
|---|---|
| 错误码 | **每个接口自己的 `kal_err` 枚举空间**,高位段标接口 ID(不是全局 errno) |
| 字符串 / 缓冲 | `(const char*, size_t)` 对,**不拥有**;返回值走调用方缓冲或 resource |
| 缺失能力 | **符号根本不存在**(不返回 `ENOSYS`)⇒ 链接期硬错;上层模块接口也不导出(§11.2.15) |
| 命名 | `kal_<interface>_<op>`,接口名与 SPEC 的 interface 一一对应 |

### 13.3 ✅ 接口清单 v0(三层各自)

**原则:先做窄的**(§11.2.14-D)。以下是 v0,**不是终态**;每加一条都要过 §13.1 的规则。

**KAL** —— world `bare` 是必须先落地的最小集:

| interface | 操作 | 属于 |
|---|---|---|
| `kal:io/streams` | `write` · `read` · `flush` · `close` | **bare** |
| `kal:clocks/monotonic` | `now_ns` · `resolution_ns` | **bare** |
| `kal:memory/alloc` | `alloc` · `free` · `realloc` | **bare** |
| `kal:process/exit` | `exit` | **bare** |
| `kal:random` | `fill` | rtos+ |
| `kal:clocks/wall` | `now_unix_ns` | posix+ |
| `kal:concurrency/threads` | ~30(TH1/TH2 实测的面) | rtos+(**需调度器**) |
| `kal:filesystem` | `open` · `close` · `read` · `write` · `seek` · `info` · `remove` · `rename` · 目录 3 | posix+ |
| `kal:sockets` | 待定 | posix+ |

**HAL** —— 对标 `embedded-hal` 的已验证集合:`Console` · `Serial` · `SpiBus` · `I2c` · `Gpio` · `Pwm` · `Adc` · `Delay`。

**AAL** —— 只含机制:`Context`(init/switch)· `Trap`(vector 安装)· `AddressSpace`(map/unmap/translate)· `Atomics/Barriers` · `PerCpu` · `Tick` · `BootHandoff`。

### 13.4 ✅ 语义不可实现的处理策略(FAT 做不到原子重命名那类)

**三选一,按顺序试:**

| 顺序 | 做法 | 何时用 | 例 |
|---|---|---|---|
| **1** | **提到能力轴**(细分能力) | 差异是「有/没有」,且两边都合法 | `perm-xo` · `atomic-rename` · `mmu` · `realtime` |
| **2** | **接口分裂** | 差异大到语义完全不同,共享签名会误导 | `filesystem` vs `filesystem-flat`(无目录) |
| **3** | **明确不支持,硬错** | 差异无法用能力表达,且勉强支持会静默错 | 通用代码碰 arch 寄存器(§13.1 B 类) |

⚠️ **禁止的第四种:返回 `ENOSYS` / 静默降级。** 那正是 K2 / TH6 那类「类型对、语义错」的来源。

**判据**:如果一个操作在某后端上「能调用但行为不同」,它**必须**走 1 或 2;只有「根本没法表达」才走 3。

### 13.5 ✅ 治理与版本化

| 项 | 定下来 |
|---|---|
| 维护方 | **mcpp-community 官方维护 SPEC**(openkal / openhal / AAL 各一份),对标 rust-embedded WG |
| 仓库形态 | **SPEC 与 mcpp 引擎分仓** —— §11.6 的硬边界:引擎永远不认识这三层任何概念 |
| 版本化单位 | **每个 interface 独立版本**(不是整份 SPEC 一个版本)⇒ 加接口不动老的 |
| 兼容规则 | interface 内**只增不改**;要改就出新版本 interface,老版本保留一个发布列 |
| world 的角色 | **命名预设**,可加不可减;减 = 新 world 名 |
| 提案流程 | 新 interface 需附:① 至少 **2 个真实后端**的实现草案 ② 过 §13.1 规则的分析 ③ 一个 §5.3 的一致性示例 |
| ⚠️ 准入判据 | **「能在 Linux / Win32 / 裸机 / WASI 四个后端自然实现,不需模拟层」**(§11.2.12);做不到就是能力不是原语 |

### 13.6 ✅ D9(b):CI 硬需求语义

§9.1 留的唯一未决项。定下来:

- e2e harness 增加 **`# requires-hard: <cap>`** —— 能力缺失时 **FAIL 而不是 SKIP**(现有 `# requires:` 保持 SKIP 语义,用于真正可选的能力);
- 裸机相关 e2e **一律用 `requires-hard`**;
- CI 汇总输出**实际执行条数**,判据是 **「裸机 e2e 执行条数 > 0」**,不是「全绿」。

### 13.7 收敛后的 §12.0 状态

| 项 | 原状态 | 现状态 |
|---|---|---|
| 每层形态 / 边界 / 横切模型 / C++ 惯用法 | ✅ | ✅ |
| 具体接口清单 | ❌ | ✅ **§13.3(v0)** |
| KAL 的 C ABI 映射规范 | ❌ | ✅ **§13.2(编码选择有实测)** |
| AAL 两个最硬原语会不会碎 | ❌ | ✅ **§13.1 —— 不碎,裂缝分两类,规则已定** |
| 语义不可实现的处理策略 | ❌ | ✅ **§13.4(三选一 + 禁止第四种)** |
| 治理 / 版本化 | ❌ | ✅ **§13.5** |
| D9(b) CI 硬需求 | ❌ | ✅ **§13.6** |

**⇒ 剩下的不再是「未定」,而是「未写」** —— SPEC 正文、接口签名细节、以及每个 interface 的一致性示例。那是执行,不是设计。

---

## 14. mcpp 引擎侧适配总清单

> 前面各节把引擎改动散落在 §2–§7 和 §11–§13。本节**汇总成一张可逐条 review 的清单**,并补上一个之前未定的关键项(§14.0)。
>
> ⚠️ **贯穿原则**:引擎**永远不认识 AAL / KAL / HAL 任何概念**(§11.6)。下表每一项都必须能在「不知道这三层存在」的前提下成立。

### 14.0 ⚠️ 撤回:后端选择根本不是 mcpp 的事(本节初稿是过度设计)

初稿把「KAL/HAL 后端怎么被选中」当成一个 mcpp 待定项,并「补定」为 capability provider。**那个前提本身就是错的** —— 它假设了 mcpp 需要为后端选择做点什么。

**实测:已有机制完全够用,零新增。** 条件依赖(`ConditionalConfig::dependencies`)已实现,且在**依赖解析之前**合并(`prepare.cppm:1387`:「#229: merge_conditional_config MUST run here — before dependency resolution」);谓词不匹配的依赖**根本不会被解析**(用一个不存在的包名验证过,构建正常通过)。

```toml
[target.'cfg(freestanding)'.dependencies]
openkal-bare  = "1.0"

[target.'cfg(unix)'.dependencies]
openkal-linux = "1.0"
```

⇒ **后端选择 = 写一个依赖。** 要可替换就用已有的 capability provider 机制;不要可替换就直接写包名。**两条路都不需要 mcpp 认识「后端」这个概念,也不需要任何新轴。**

⚠️ **教训**:「这个决策放哪一层」如果答案是「mcpp 要新增一个轴」,先问一遍**「用户能不能用已有的依赖/条件依赖表达它」**。这次的答案是能。

### 14.0b ⚠️ 修正:契约形态 与 绑定时机 是两个正交的轴

初稿把「运行期插件表」一刀否掉(理由:间接跳转 + 丢 LTO)。**那是把两个轴混成了一个。**

| 绑定时机 | 用在哪 | 代价 | 契约形态 |
|---|---|---|---|
| **编译期** | arch(AAL)· 板级已知设备(HAL) | 零(LTO 后) | concept / 模板 |
| **链接期** | KAL 后端 · 单例能力 | 一次直接调用 | **C ABI** |
| **运行期** | 热插拔设备 · 插件 · 宿主上运行期发现的外设 | 一次间接跳转 + 丢 LTO | **C ABI + 函数表 / dlopen** |

⭐ **关键:C ABI 这一个契约形态,同时支持链接期和运行期绑定** —— `kal_write(handle, …)` 既可以链接期解析,也可以从函数表里取。这是 C ABI 作为契约的**额外好处**,初稿只讲了「跨包唯一可行」(H1),漏了这一条。

⇒ **运行期绑定不该被否掉,它有正当场景**(热插拔、插件式驱动)。而且它**不是构建期的事** —— mcpp 已有 `[runtime] dlopen_libs`(2026-07-24 决策 #7),又落回 §14.0c 的第 ③ 类。

### 14.0c ⭐ 责任划分:三类,判据不同

这是本节真正的组织原则 —— 上面两条撤回都是因为没先做这个划分。

| 类 | 判据 | 内容 | 何时做 |
|---|---|---|---|
| **① mcpp 必须做** | **不做则谁都做不了**(只有构建工具在这个位置) | 裸机 target 表达 · Freestanding 链接模型 · runner · **不静默错构** · BMI/指纹正确性 · 依赖解析与能力绑定(**已有**) | **先做** |
| **② mcpp 可以做** | 做了更好用,**不做也能用** | 契约层声明/证据/索引展示 · 能力差集的解析期诊断 · 符号审计 | **可后做** |
| **③ mcpp 不该做** | 生态 / SPEC 的领域 | AAL/KAL/HAL 的**接口内容** · **后端实现** · **后端选择策略** · 板子数据库 · libc 实现 · 运行期绑定机制 | **永不做** |

**② 为什么可以后做**:§7.5.4 已论证 —— **兜底是链接期硬错**(`-nostdlib` 下没有 libc 静默满足符号)。所以契约层只影响**错误发生的时机和可读性**,不影响正确性。

**③ 的判据**:如果一个东西**用户能用已有的 manifest 语法表达**(依赖、条件依赖、能力),它就是 ③。§14.0 的后端选择就是被这条判据踢出去的。

⇒ **按此重排 §14 的 39 项:**

| 类 | 项 | 数量 |
|---|---|---|
| **①必须** | A(6)· C(5)· D(3)· G(3)· H(1)· I(2)· J(1)· K(2) | **23** |
| **②工效** | B(2)· E(4)· F(6) | **12** |
| **③不该做 / 非引擎** | L 载荷(4) —— 是分发不是引擎 | 4 |

⚠️ **①里唯一的实施前置是 J(探测普查)**;K(harness)决定这条线是否真跑。**②的 12 项可以整体推迟到 ①跑通之后**,不影响正确性。

### 14.A 目标层### 14.A 目标层(`src/toolchain/triple.cppm`)

| # | 项 | 依据 |
|---|---|---|
| A1 | `parse()`:`none` 仅当整串无其它 OS token 时作 OS 段;否则维持 vendor 语义。**正反两侧单测** | §2.1 / `triple.cppm:299` |
| A2 | `TargetInfo` 增 `TargetSpec` 列(isa · abi · codeModel · linker · clib · **envCaps**) | §2.2 |
| A3 | `kKnownTargets` 加 `riscv64-none-elf` 行 | §2.2 |
| A4 | `artifact_naming` 处理 `os=none`(`.elf` + objcopy 产 `.bin`) | §2.2 |
| A5 | `family()` 对 `os=none` 返回空 —— **现状已正确,加测试钉住** | §2.1 |
| A6 | **单一取值口** `resolve_target_spec()`,消费方逐个核对(compile/link flags · cfg · 产物命名 · runner · BMI 键 · hermetic 白名单) | §2.2 ⚠️ #195 病灶 |

### 14.B cfg 谓词

| # | 项 | 依据 |
|---|---|---|
| B1 | `cfg(freestanding)` / `cfg(os = "none")` | §2.1 |
| B2 | **`cfg(capability = "heap")`** 类谓词 —— 条件编译需要它(能力检查在 resolver,条件编译在这里) | §7.5.6 |

### 14.C 链接层(`src/toolchain/linkmodel.cppm`)

| # | 项 | 依据 |
|---|---|---|
| C1 | **`CLibMode::Freestanding` 新态**(不复用 `None`) | §3.1 |
| C2 | 链接器按**载荷绝对路径**寻址 + 用前校验 `--version` 含 `LLD` | §3.3 ⚠️ E6-1 |
| C3 | `-T <script>` 通道(来自 provision 或 `[target]`) | §3.2 |
| C4 | hermetic 检查语义反转:`os=none` 下宿主路径**致命**,`allow_host_libs` **不生效** | §3.4 |
| C5 | ⚠️ LTO 是**兜底项**(§16.2 修正):真正的必需项是 SPEC 要求热路径原语标 `inline`;LTO 覆盖漏标与跨包复杂情况 | K3 / §16.2 |

### 14.D provisions(`src/build/provisions.cppm`)

| # | 项 | 依据 |
|---|---|---|
| D1 | `LinkerScript` kind —— **单值,冲突即报错** | §3.5 |
| D2 | `StartupObjects` kind —— **两个具名槽 `prologue`/`epilogue`,槽内数组序**(§19.3 定;提供者唯一 ⇒ 无跨包排序问题) | §3.5 / §19.3 |
| D3 | `Runner` kind | §4.1 |

### 14.E 能力模型(复用 `2026-06-29-feature-capability-model-design.md`)

| # | 项 | 依据 |
|---|---|---|
| E1 | 把 `provides`/`requires` 从**运行期宿主能力**扩到**构建/环境能力** | §7.5.6 |
| E2 | ⚠️ resolver 保证「**恰好一个提供者**」—— 链接器只在 1/3 情况会报错(两个 `.o` 响亮;`.o`+`.a` 与两个 `.a` **都静默**) | §11.2.9 |
| E3 | 能力集来源合并:`TargetSpec.envCaps ∪ BSP ∪ KAL 后端 ∪ HAL 提供者` | §6.0.3 |
| E4 | 差集非空 ⇒ **解析期**指名道姓诊断(而非链接期 mangled 符号) | §7.5.4 |

### 14.F 契约层

| # | 项 | 依据 |
|---|---|---|
| F1 | `[freestanding]` manifest 段(`tier` / `requires` / `example`)—— **可选,未声明 ≠ 不支持** | §5.3 |
| F2 | 一致性示例的构建 + 符号审计(`llvm-nm -u` 分类) | §5.3.5 / §5.4 |
| F3 | 证据写入 xpkg 描述符 `[freestanding.verified]` | §5.4 |
| F4 | ⚠️ **手写的证据字段在发布时被覆盖**(证据是机器输出不是人输入) | §5.4 |
| F5 | 索引展示三态(已验证 / 已声明未验证 / 未声明)**永不合并** | §5.6 |
| F6 | ⚠️ 新键名与 `kKnownXpkgKeys` 的 26 个键**编辑距离要足够远**(否则老客户端刷 did-you-mean 噪音) | §5.8 / C4 |

### 14.G run / test

| # | 项 | 依据 |
|---|---|---|
| G1 | `[target].runner` argv 模板 + **产物路径追加尾部**(Cargo 同款) | §4.1 |
| G2 | `mcpp test` 走 semihosting 退出码(picolibc `crt0-semihost.o` 现成) | §4.2 / X12 |
| G3 | BSP 提供 runner 的 provision 通道,`[target]` 可覆写 | §4.1 / D3 |

### 14.H import std

| # | 项 | 依据 |
|---|---|---|
| H1 | `os=none` ⇒ `hasImportStd = false`,诊断**说明为什么**并指向 `mcpplibs.std.freestanding` | §6.4 / E4 |

### 14.I BMI / 缓存(⚠️ 等级已被 ISO 抬高)

| # | 项 | 依据 |
|---|---|---|
| I1 | ⚠️ **freestanding 进 BMI 缓存键** —— 不一致的后果不是「结果不对」,是**编译器崩溃** | ISO 系列 |
| I2 | `TargetSpec` 全字段进指纹(isa/abi/codeModel 变了 BMI 必须失效) | §2.2 / I1 |

### 14.J ⚠️ 探测代码普查(实施前必做)

| # | 项 | 依据 |
|---|---|---|
| J1 | 所有「编个小程序链一下」的探测在 `os=none` 下会**假失败** —— doctor · 工具链能力探测 · `hasImportStd` 探测,**逐个普查** | §2.3(CMake `TRY_COMPILE_TARGET_TYPE` 教训) |

### 14.K e2e harness

| # | 项 | 依据 |
|---|---|---|
| K1 | **`# requires-hard: <cap>`** —— 缺能力 **FAIL 而非 SKIP**(现有 `# requires:` 保持 SKIP) | §13.6 |
| K2 | CI 汇总输出**实际执行条数**;判据是「裸机 e2e 执行条数 > 0」不是「全绿」 | §13.6 ⚠️ PR#451 |

### 14.L 载荷 / 分发

| # | 项 | 依据 |
|---|---|---|
| L1 | per-target compiler-rt builtins;⚠️ **验收用例必须含 64 位除法或软浮点** | E3 |
| L2 | picolibc 载荷;⚠️ 发行版包**无 rv64gc 变体**,须自建 multilib 或采纳 LLVM Embedded Toolchain | X11 |
| L3 | libc++ 每目标 freestanding `__config_site` | E5 |
| L4 | qemu 进 xim-pkgindex(xlings/mcpp 双生态复用) | §9.1 |

### 14.M 统计

| 组 | 项数 | 性质 |
|---|---|---|
| A 目标层 | 6 | 已有结构加列 |
| B cfg | 2 | 词表扩展 |
| C 链接层 | 5 | 已有结构加态 |
| D provisions | 3 | ⚠️ 含唯一的新语义(带序) |
| E 能力模型 | 4 | **复用现有 capability,不新造** |
| F 契约层 | 6 | manifest 可选段 + 索引侧 |
| G run/test | 3 | 纯新增,无兼容负担 |
| H import std | 1 | 挂现有门 |
| I BMI | 2 | ⚠️ 等级已抬高 |
| J 探测普查 | 1 | ⚠️ 实施前置 |
| K harness | 2 | ⚠️ 决定整条线是否真跑 |
| L 载荷 | 4 | 分发侧,非引擎 |
| **合计** | **39** | 其中引擎代码 ~29,载荷/CI ~10 |

⭐ **没有一项需要引擎理解 AAL/KAL/HAL** —— §14.0 的 (c) 让三层退化成普通的 capability provider。

---

## 15. 使用侧全景:九个场景,每个只多一件事

> 每个场景标注 **[实测]**(本轮真跑过)或 **[设计]**(形态已定,未实现)。
> 每个场景只比上一个多**一件**事 —— 这是检验「简单容易」的方式:如果某一步跳跃太大,说明抽象漏了。

### S0 — hosted 应用(对照组)**[实测:今天就是这样]**

```toml
[package]
name = "app"
version = "0.1.0"
```
```cpp
import std;
int main() { std::println("hi"); }
```
```console
$ mcpp build && mcpp run
```

⭐ **整套裸机/三层架构对这个场景的影响 = 0。** 不写 `[freestanding]`、不加 `--target`,一切照旧。**这是所有设计的基线约束。**

---

### S1 — 纯裸机内核,零依赖 **[实测:E2 / §7.3]**

**+1 件事:一个裸机 target 段。**

```toml
[package]
name = "toy_kernel"
version = "0.1.0"

[target.riscv64-none-elf]                    # ← 新增
toolchain     = "llvm@22.1.8"
linker-script = "link.ld"                    # 自己写
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-bios", "default", "-kernel"]
```
```cpp
export module toy_kernel;
namespace { volatile char* const kUart = (volatile char*)0x10000000; }
export extern "C" void kmain() {
    for (const char* s = "bare metal\n"; *s; ++s) *kUart = *s;
}
```
```console
$ mcpp build --target riscv64-none-elf     # → 136 字节裸镜像,零未定义符号
$ mcpp run   --target riscv64-none-elf
```

⚠️ 你还得自己写 `link.ld` 和 `start.S`。**没有 `import std`**(E4)。

---

### S2 — 裸机 + BSP **[设计;BSP 机制已验证]**

**+1 件事:一个依赖。−2 件事:`link.ld` 和 `start.S` 都删掉。**

```toml
[dependencies]
riscv-virt-rt = "1.0"                        # ← BSP:start.S + link.ld + runner + _init

[target.riscv64-none-elf]
toolchain = "llvm@22.1.8"                    # linker-script / runner 都由 BSP 提供
```
```cpp
export module toy_kernel;
export extern "C" void kmain() { /* 只有业务逻辑 */ }
```

⭐ **手写的裸机知识:0 行。** 换板子 = 换 BSP 包(`hifive-rt` / `stm32f4-rt`),`kmain` 一行不改。

---

### S3 — 裸机 + std 子集 **[实测:X10]**

**+1 件事:一个 import。**

```toml
[dependencies]
riscv-virt-rt         = "1.0"
mcpplibs.std.freestanding = "1.0"            # ← 新增
```
```cpp
import mcpplibs.std.freestanding;            // ← 新增

export extern "C" void kmain() {
    std::array<Task, 4> t{...};
    std::ranges::sort(t, {}, &Task::prio);   // ⭐ 名字就是 std::,不是影子命名空间
    std::optional<Task> top = ...;
    std::atomic<int> seen{0};
}
```

⭐ **代码搬回 hosted 一行不用改**(导出的是同一个实体)。
⚠️ `std::thread` / `std::mutex` **编译期不存在**(X4:`no type named 'mutex'`),不是运行期惊喜。

---

### S4 — 裸机 + 堆 **[实测:T3b]**

**+1 件事:换一个提供 heap 能力的 BSP。代码零改动。**

```toml
[dependencies]
riscv-virt-rt = { version = "1.0", features = ["heap"] }   # ← BSP 提供 sbrk/operator new
```
```cpp
export extern "C" void kmain() {
    std::vector<int> v;                      // ⭐ 突然可用了 —— 因为环境多了 heap 能力
    v.push_back(7);
}
```

⭐ **档位是环境的函数,不是代码的函数**(Y4)。你没改一行代码,只是环境提供了 `operator new`。

---

### S5 — 跨 hosted / 裸机的库(KAL)**[实测:KA1–KA3]**

**+1 件事:面向 KAL 接口写,而不是面向 OS 写。**

```toml
[dependencies]
"kal:io/streams" = "1.0"                     # ← 接口包(能力),不是具体后端

[target.'cfg(freestanding)'.dependencies]
openkal-bare  = "1.0"                        # ← 后端由条件依赖选,mcpp 不认识「后端」
[target.'cfg(unix)'.dependencies]
openkal-linux = "1.0"
```
```cpp
export module mylib;
import openkal.io;
export void greet() { kal::write(1, "hello", 5); }   // ⭐ 零 #if
```
```console
$ mcpp build                                  # → hosted:转发真 write(2),实际跑出 hello
$ mcpp build --target riscv64-none-elf        # → 裸机:转发 MMIO UART,157 字节
```

⭐ **应用对 KAL 的全部依赖面是一个可审计符号集**,两侧完全相同:`kal_write`。
⭐ 换后端**不重编应用**(C ABI 缝的性质)。

---

### S6 — 写外设驱动(HAL)**[实测:H3]**

**+1 件事:驱动对 concept 编程,不对板子编程。**

```cpp
export module mydriver;
import mcpplibs.hal;

export template <hal::Console C, hal::Clock K>       // ⭐ 两个能力组合
void banner(C& c, K& k) { c.write(...); k.ticks(); }
```
```cpp
// 用它的人
bsp::Uart u1; bsp::Uart2 u2; bsp::Timer t;
banner(u1, t);
banner(u2, t);          // ⭐ 同一驱动,两个提供者共存,各实例化一次,零成本
```

⭐ 同一个驱动包既跑裸机 MCU,又跑 Linux(`linux-embedded-hal` 已证明这个形状)。
⚠️ 但假设 µs 级 GPIO 的驱动在 Linux 上**类型全对、能跑、慢 1000 倍** ⇒ 时序是能力(`realtime`),不是能藏的差异。

---

### S7 — 写内核(AAL)**[实测:AAL-1 / K3]**

**+1 件事:对 arch concept 编程,并且必须开 LTO。**

```toml
[target.'cfg(arch = "riscv64")'.dependencies]
aal-riscv64 = "1.0"
[target.'cfg(arch = "x86_64")'.dependencies]
aal-x86_64  = "1.0"

[profile.release]
lto = true                                   # 兜底;热路径原语应已标 inline(§16.2)
```
```cpp
template <aal::Arch A>
void schedule(A& a, typename A::Context& cur, typename A::Context& next) {
    a.ctx_switch(cur, next);                 // 通用调度代码
}
```

⚠️ **不开 LTO,`local_irq_disable()` 会变成真实 `jal`**(K3)—— 内核热路径不可接受。
⚠️ Context 的**细粒度寄存器访问不进通用层**(AAL-2 B 类裂缝:`no member named 'a0'`)。

---

### S8 — RTOS:裸机上要线程 **[设计;契约面已实测 TH1/TH2]**

**+1 件事:换一个提供 threads 能力的 BSP(RTOS 作为 KAL 后端)。**

```toml
[dependencies]
freertos-rt = "1.0"                          # ← RTOS 作为 KAL 后端,提供调度器
```
```cpp
import mcpplibs.std.freestanding;
export extern "C" void kmain() {
    std::thread t{[]{ /* ... */ }};          // ⭐ 因为环境提供了 threads 能力
    t.join();
}
```

⭐ **KAL 转发调度器,不包含调度器** —— RTOS 是后端,不是 KAL 的内容。
⚠️ `thread_local` **不在 KAL 里**(TH6):它需要 BSP 初始化 `tp` + 链接脚本 `.tdata/.tbss`,否则**编过链过零诊断,运行期静默写坏内存**。

---

### S9 — 库作者的四级投入 **[设计;符号审计已实测 Y5/Z1]**

```toml
# L0:什么都不做 ——「未声明 ≠ 不支持」,hosted 用户零影响,裸机用户靠链接期兜底
[package]
name = "mcpplibs.tinyfsm"

# L1:跑一次审计(常见结果:发现自己其实已经 clean)
# L2:条件化
[target.'cfg(freestanding)'.build]
sources = ["!src/uses_heap.cppm"]

# L3:声明 + CI
[freestanding]
tier    = "core"
example = "examples/freestanding.cpp"        # ← CI 在裸机目标上编它 + 审计符号
```

⚠️ **L0 永远可用是硬承诺** —— 一旦「不标注就被排除」,生态立刻分裂。

---

### S-E — 错误场景:要了环境没有的能力 **[实测:ISO(flag 一致时)]**

```cpp
import mcpplibs.std.freestanding;
export extern "C" void kmain() { std::mutex m; }     // 环境无 threads
```

**解析期**(有声明时,②工效档):
```
error: 包 'mylib' 需要能力 'threads';目标 riscv64-none-elf + riscv-virt-rt 未提供
       提供 threads 的 BSP:freertos-rt, zephyr-rt
```

**编译期**(总是有效,①必须档):
```
error: no type named 'mutex' in namespace 'std'
```

**链接期兜底**(总是有效):
```
ld.lld: error: undefined symbol: __libcpp_thread_create
```

⭐ **三道防线由外向内收紧,而只有最内一道是必须的**(§14.0c)—— 这就是为什么②可以后做。

---

### 15.X 场景差异一览

| 场景 | 比上一个多了什么 | 用户手写的裸机知识 |
|---|---|---|
| S0 hosted | — | 0 |
| S1 纯裸机 | 一个 target 段 | `link.ld` + `start.S` |
| S2 + BSP | 一个依赖 | **0** |
| S3 + std 子集 | 一个 import | 0 |
| S4 + 堆 | **换 BSP,代码零改** | 0 |
| S5 跨 OS 库 | 面向 KAL 接口写 | 0(**零 `#if`**) |
| S6 写驱动 | 对 concept 编程 | 0 |
| S7 写内核 | arch concept + **必须 LTO** | 只剩 arch 相关 |
| S8 RTOS | **换 BSP,代码零改** | 0 |

⭐ **S2→S8 之间,用户手写的裸机知识始终是 0**;变化的是**依赖**和**环境能力**,不是代码。

---

## 16. 两处澄清:依赖形状,与 LTO 的真实必要性

### 16.1 接口与实现必须分离,而且接口要显式依赖

§15-S5 的写法给全了但没解释为什么。**规则:**

```toml
[dependencies]
openkal.io   = "1.0"          # ① 接口:编译依赖 —— 你 import 它,必须显式写

[target.'cfg(freestanding)'.dependencies]
openkal-bare = "1.0"          # ② 实现:链接依赖 —— 按 target 选
[target.'cfg(unix)'.dependencies]
openkal-linux = "1.0"
```

**为什么接口必须显式依赖(不能只依赖实现让它 re-export):**

你的代码写了 `import openkal.io` —— 那是**编译依赖**。若只依赖实现包、靠它把接口 re-export 出来,接口就成了**隐式依赖**:换一个实现,它 re-export 的接口版本可能不同,而你的 manifest 里**看不出来**。这与 mcpp 的 usage-requirements scope 模型(公开/私有边必须显式)是同一条原则。

**实现侧有两种写法,取舍不同:**

| 写法 | 今天可用 | 何时用 |
|---|---|---|
| **条件依赖**(上例) | ✅ **零机制,已实测**(§14.0) | 后端由 target 唯一决定 —— 覆盖绝大多数情况 |
| **能力 provider**(`provides` + resolver 绑定恰好一个) | ⚠️ **需要 §14.E1** —— 今天 `requires` 是 **feature 级**(`featureRequires`),包级/构建级的 require 还没有 | 同一 target 上有多个候选实现,要可替换/可 pin |

⚠️ **不建议的第三种:接口包自带默认实现**(如 hosted 上默认 fallback 到 libc)。它会造成「在一个平台上悄悄能用、换平台神秘失败」——正是本文档反复标记的那类静默失败。

### 16.2 ⚠️ 修正:AAL 不是「必须 LTO」,是「热路径原语必须显式 `inline`」

§15-S7 写的是「LTO 是必需项」。**实测把这条改了 —— 而且真因反直觉。**

同一个模块接口里四种写法,跨模块 `-O2`、**不开 LTO**:

| 写法 | 结果 |
|---|---|
| `void off() { asm(...); }` —— **类内定义(隐式 inline)** | ❌ `jal` = 1,**没内联** |
| **`inline void off()`** —— 显式 `inline` 关键字 | ✅ **`csrci` 内联进来** |
| `[[gnu::always_inline]] void off()` —— **只有 attribute** | ❌ `jal` = 1,**没内联** |
| **`[[gnu::always_inline]] inline void off()`** | ✅ **内联** |

**⇒ 决定内联的是 `inline` 关键字,不是 `always_inline` attribute。**

**为什么(机理):**

1. **内联需要消费者 TU 能看到函数体。** 同 TU 时定义就在眼前(K3-a 实测:`csrci` 直接嵌入,零调用)。
2. **非模块世界**里,类内定义是隐式 inline,而且**头文件把定义物理复制进了每个 TU** —— 所以「隐式 inline 能跨 TU 内联」是头文件模型的副产品,不是 inline 本身的功劳。
3. **模块世界**里没有这个复制。消费者只有 BMI。实测:clang 22 对**隐式** inline 的模块附着函数,不把可内联的定义暴露给消费者;**显式 `inline`** 才暴露。
4. `always_inline` 单独无效,因为它只说「**如果**要内联就必须内联」——**函数体压根不可见时,它没有作用对象**。
5. **LTO 修好它的原理不同**:LTO 把所有 TU 的 **IR** 推到链接期统一优化,那时优化器能看到 `off()` 的 IR 定义 ⇒ 内联。**它工作在 IR 层,与 BMI 里有没有 AST 层面的 body 无关。**

**⇒ 修正后的指导:**

| 层次 | 做法 |
|---|---|
| **首选(免费)** | **AAL / HAL 的热路径原语,在模块接口里显式写 `inline`** —— 这应写进 SPEC 纪律 |
| **兜底** | LTO —— 覆盖没标 `inline` 的、以及跨包的复杂情况 |
| **无效** | 只加 `always_inline` 而不加 `inline` |

⚠️ 这条实测于 clang 22;是 QoI 差异还是 bug 未定,但**显式 `inline` 在两种解释下都安全且免费**,所以是无条件推荐的写法。

⚠️ 同时 §14-C5 的措辞要改:LTO 从「必需项」降为「**兜底项**」;真正的必需项是 **SPEC 要求热路径原语标 `inline`**。

---

## 17. 仍待确认清单

### 17.1 ⚠️ 最大的一条:战略决策从未被确认

本文档从「issue #403 调研」一路推到「openkal / openhal / AAL 三层生态」。**但 §10 的 D1–D16 决策回执至今没有被 review 过。**

⚠️ 后续所有讨论**隐含选择了超出 Route C 的方向**(三层 SPEC + 官方维护 + 可组合内核愿景),而这从未被明说。⇒ **需要显式确认:**

| # | 待确认 | 为什么现在必须问 |
|---|---|---|
| 17.1a | **D1**:E1 静默失败**立刻修**(独立于路线) | 这是唯一「无论选什么都要做」的义务,不该被大方向讨论淹没 |
| 17.1b | **D2 的实际选择**:B(最小缝)/ C(全栈)/ **超出 C(三层 SPEC 生态)** | §14 的 39 项与 §11–16 的三层是**两个不同规模**的承诺 |
| 17.1c | **§14 的 39 项**(除已讨论的后端选择、LTO 两条外) | 输出过但未逐条表态 |

### 17.2 设计上确实还没讨论过的

| # | 项 | 现状(实测) |
|---|---|---|
| 17.2a | ⚠️ ~~workspace 多 target~~ **已由 §20 更正:今天就能用** | `cmd_build.cppm:110-122`:fan-out 把**同一个** `ov.target_triple` 传给每个成员 ⇒ 「hosted 成员 + 裸机成员」在**一条命令里表达不了**(`-p` 逐个构建可以)。而 **openkal SPEC 仓库天然是 workspace**(接口包 + N 个后端 + 测试),成员 target 不同 |
| 17.2b | **裸机 `mcpp test` 的粒度** | hosted 是每个 `tests/*.cpp` 一个 binary;裸机每个用例要**链一个完整镜像 + 跑一次 qemu**。要不要合并成「一个镜像多个用例」?这直接决定裸机测试能不能用 |
| 17.2c | **裸机产物的 `mcpp pack` / 分发** | `.elf` + `.bin`(+ `.hex`?)+ 符号表/map 文件。§14 完全没有这一项 |
| 17.2d | **IDE / `compile_commands.json`** | 裸机 target 的 flags 与 hosted 完全不同(`-ffreestanding -march=... -nostdinc++`)。mcpp 有 configure-only CDB,但**多 target 下 CDB 该出哪一份**未定 |

### 17.3 命名

| # | 项 |
|---|---|
| 17.3a | **AAL 这个名字**是本文引入的(原始表述是「KAL + 硬件 HAL」)。KAL=syscall 层已确认,**AAL 未确认**。⚠️ §11.3 提醒过「叫 KAL 会招来范围蔓延」,AAL 同理 —— 名字决定边界会不会被侵蚀 |

### 17.4 不是设计问题

| # | 项 |
|---|---|
| 17.4a | 谁写 SPEC、第一版时间线 —— §13.5 定了**治理形态**,没定**人和排期** |

### 17.5 已核实为「不需要」的(避免重复讨论)

| 项 | 核实结果 |
|---|---|
| `[profile.*] lto` | ✅ **已存在**(`types.cppm:796`)⇒ §16.2 的兜底零新增 |
| 后端选择需要新轴 | ✅ **不需要**(§14.0,条件依赖已实测) |
| 运行期绑定机制 | ✅ **不需要**(已有 `[runtime] dlopen_libs`) |
| 契约层影响正确性 | ✅ **不影响**(§7.5.4:兜底是链接期硬错)⇒ 12 项可后做 |

---

## 18. 仓库布局:接口与实现分仓(定)

### 18.1 决定

**接口仓与实现仓分离**,不走 Linux「所有实现收进内核树」的路子。官方只维护少数几个后端,其余社区。

```
mcpp-community/
├── openkal            SPEC:接口包           ← target 中立
├── openhal            SPEC:接口包           ← target 中立
├── openkal-conformance  一致性测试套件       ← §18.3,分仓的必需配套
├── openkal-linux      官方后端
├── openkal-windows    官方后端
├── openkal-bare       官方后端(通用裸机骨架)
└── openhal-linux      官方(对标 linux-embedded-hal)

社区/
├── openkal-freertos · openkal-zephyr · openhal-stm32f4 · …
```

### 18.2 ⭐ 这直接解掉了 §17.2a

**接口包是 target 中立的** —— 一份 concept / 一份 C ABI 声明,不针对任何 target。所以:

| 仓库 | 成员 target | 撞不撞 workspace 多 target 限制 |
|---|---|---|
| 接口仓(openkal / openhal) | **中立**(或统一 host) | ❌ 不撞 |
| 后端仓(openkal-bare-riscv) | **单一** target | ❌ 不撞 |

⚠️ 但要把一致性示例**放在后端仓**,不放接口仓 —— 接口本身没有实现,无从验证;**是后端证明自己满足接口**。放错地方就会把多 target 需求带回接口仓。

⚠️ **§17.2a 本身仍是 mcpp 的真实限制**,只是 openkal 生态绕开了它。一般用户仍会撞上 —— **「固件 + 上位机工具」在同一个产品仓库里是极常见的组合**。这条留在 §17。

### 18.3 ⚠️ 分仓的必需配套:一致性测试套件(新增,之前没有)

**分仓 = 没有任何一个地方能一次跑所有后端。** 那么「两个后端行为是否一致」谁来保证?

⇒ **接口仓必须提供一份 conformance suite**,每个后端仓引入并运行:

```toml
# openkal-freertos/mcpp.toml
[dev-dependencies]
openkal-conformance = "1.0"     # ← 引入官方一致性套件

[freestanding]
example = "examples/conformance.cpp"
```

- 这是 **WASI 的做法**(`wasi-testsuite`),不是新发明;
- 它同时是 §5.3 `example` 机制的自然归宿:**后端的一致性示例 = 跑 conformance suite**;
- ⚠️ **没有它,分仓会退化成「N 个各自解释接口的实现」** —— 那正是 POSIX 生态几十年的老问题。

### 18.4 官方维护哪几个后端:判据不是「用户多」

⭐ **判据是:能让 SPEC 的准入规则可执行。**

§11.2.12 定的准入判据是「每个原语必须能在 **Linux / Win32 / 裸机 / WASI** 四个后端自然实现,不需模拟层」。**要执行这条判据,官方就必须自己持有那几个后端** —— 否则每次提案都要求社区先实现一遍才能审。

| 后端 | 官方维护的理由 |
|---|---|
| `openkal-linux` | POSIX 世界的代表 |
| `openkal-windows` | **非 POSIX 世界的代表** —— 没有它,SPEC 会不知不觉长成 POSIX(§11.2.12) |
| `openkal-bare` | 无 OS 世界的代表 |
| `openkal-wasi` | 可选/参考(WASI 自身已是成熟 SPEC,可作对照而非必须) |

⇒ **官方维护这三个不是为了服务用户,是为了让准入判据可执行。** 其余(FreeRTOS / Zephyr / 各 MCU)交社区。

### 18.5 ⚠️ 分仓的代价与缓解

| 代价 | 缓解 |
|---|---|
| **改接口要跨仓协调**(加一个接口 = SPEC 仓 + N 个后端仓都要动) | §13.5 已定「**每个 interface 独立版本 + 只增不改**」⇒ 加接口时老后端**不用动**(它只是不提供新接口)。**这条现在有了更硬的理由** |
| 版本对齐:`openkal-linux 1.2` 实现的是接口的哪个版本 | 后端 manifest 显式声明依赖的接口版本;conformance suite 按接口版本分支 |
| 社区后端质量参差 | §5.6 的三态展示(已验证 / 已声明未验证 / 未声明)—— 索引照实显示,不背书 |

---

## 19. 八项决策的落定(2026-08-19 review)

### 19.1 A4 产物形态 —— **要**

`os=none` 目标的产物集(工具全部已在载荷内):

| 产物 | 默认 | 怎么来 | 理由 |
|---|---|---|---|
| `.elf` | ✅ 总是 | 链接产物 | 调试器 / qemu `-kernel` 都吃它 |
| `.bin` | ✅ 默认产 | `llvm-objcopy -O binary` | 多数 loader / 烧录器要裸镜像 |
| `.map` | ✅ 默认产 | `-Wl,-Map=` | **裸机最常见的问题是尺寸和段布局**,事后补不了 |
| `.hex` | ⬜ 可选 | `llvm-objcopy -O ihex` | MCU 烧录常用,非通用 |
| size 摘要 | ✅ 构建后打印 | `llvm-size` | text/data/bss 一行 —— 裸机第一关心项 |

⚠️ `.map` 与 size 摘要默认开的理由是**不可回溯**:出问题时再想要,得重新构建一次并且可能已经改了代码。

### 19.2 C4 hermetic 逃生舱 —— **`os=none` 下失效**

`allow_host_libs` / `MCPP_ALLOW_HOST_LIBS` 在 `os=none` 下**完全不生效**(不是降级为警告)。裸机链宿主库没有正当用例。

### 19.3 ⭐ D2 链接顺序 —— 简洁设计(依赖图**不适用**)

**先回答「依赖图 ok 吗」:❌ 不 ok。**

`crti → crtbegin → 用户对象 → crtend → crtn` **不是依赖关系** —— `crtn.o` 并不「依赖」用户对象,它只是必须排在后面。依赖图给出的是**包之间**的拓扑序,而这里要的是**同一条链接线内的位置**。两者不同构。

**简洁方案:两个具名槽,槽内保持数组序。**

```toml
# BSP 包的 mcpp.toml
[provides.startup-objects]
prologue = ["src/crti.S", "src/crtbegin_shim.S"]   # 排在用户对象之前
epilogue = ["src/crtend_shim.S", "src/crtn.S"]     # 排在用户对象之后
```

链接线 = `prologue(数组序) → 用户对象 → epilogue(数组序)`。

**为什么这样就够(而不需要序号):**

1. **语义上真的只有两个位置** —— 「用户代码之前」和「之后」。OSDev 的五段式正是这两组;
2. ⭐ **提供者唯一**(§3.5:`LinkerScript` 单值,`StartupObjects` 同规则)⇒ **跨包排序问题根本不存在**,顺序退化成「一个包内的数组顺序」;
3. 其余对象(如 `.init_array` 遍历器)**顺序无关**,当普通目标文件处理;段落布局由链接脚本管。

⚠️ **两个 BSP 同时提供 `startup-objects` = 硬错误**(与 `LinkerScript` 同规则)。

⇒ **新语义被压到最小:不是「带序的 provision」,而是「两个具名槽」。** 实现上是两个有序 `vector`,不是排序算法。

### 19.4 E1 `requires` 扩到包级 —— **ok**

从 `featureRequires`(feature 级)扩到包级 / 构建级。是**已有模型的作用域扩展**,不是新模型。

### 19.5 F1 `[freestanding]` manifest 段 —— **ok**

按 §5.3 落地(`tier` / `requires` / `example`)。⚠️ 保持②工效档定位:**未声明 ≠ 不支持**,正确性不依赖它。

### 19.6 G2 裸机 `mcpp test` 粒度 —— **可选,默认「一个镜像跑一次」**

| 模式 | 行为 | 何时 |
|---|---|---|
| **`batch`(默认)** | 所有用例链进**一个镜像**,**一次 qemu**,用例间用 semihosting 输出分隔 | qemu 启动开销大,默认要快 |
| `isolated`(可选) | 每个用例一个镜像、一次 qemu | 定位问题时 |

⚠️ **batch 的代价必须写进文档**:裸机上一个用例挂死(坏 MMIO 写、跑飞)会**带走整批**。缓解:
- 每个用例开始时通过 semihosting 打**用例名**,超时后能指出「卡在哪个用例」;
- 超时后**自动重跑一次 `isolated`** 以定位 —— 这让默认档的风险可控。

### 19.7 K1 `# requires-hard` —— **加**

- `# requires:` 保持 SKIP 语义(真正可选的能力);
- **`# requires-hard: <cap>`** —— 能力缺失时 **FAIL 而非 SKIP**;
- 裸机 e2e 一律用后者;
- CI 汇总输出**实际执行条数**,判据是「> 0」不是「全绿」。

### 19.8 ⭐ 17.2d 是什么:多 target 下的 `compile_commands.json`

**问题(实测)**:CDB 写到 `<projectRoot>/compile_commands.json` —— **一个文件,项目根,不按 target 区分**(`compile_commands.cppm:445`)。

于是在双 target 项目里:

```console
$ mcpp build                              # 写入 host flags 的 CDB
$ mcpp build --target riscv64-none-elf    # ⚠️ 覆盖成裸机 flags 的 CDB
```

clangd 只看得到**最后一次**。而两套 flags 差异巨大(`-ffreestanding -march=rv64gc -nostdinc++ -isystem <picolibc>` vs 宿主的一切)⇒ **在 IDE 里,固件代码和上位机工具必有一方满屏红波浪线。**

⚠️ 这不是裸机独有 —— 任何「固件 + 上位机工具」「主程序 + 交叉编译的插件」项目都会撞上,只是裸机把它放大了(flags 差异最大)。

**方案:**

| 层次 | 做法 |
|---|---|
| **默认(零变化)** | 跟最后一次 build 走 —— 单 target 项目完全无感 |
| ⭐ **多 target 项目** | **CDB 本来就是 per-file 的** ⇒ `mcpp build --configure-only` 遍历项目声明的 target,**按文件合并成一份 CDB**:每个文件用**它所属 target** 的 flags |

⭐ 关键洞察:**clangd 的 CDB 是逐文件的,所以根本不需要「选一个 IDE target」** —— 内核文件用裸机 flags、工具文件用 host flags,**同一份 CDB 里共存**即可。

⚠️ 前提:**文件到 target 的归属必须明确**。workspace 成员级最自然(成员 A 是固件、成员 B 是工具);单包内混合则需要 `[target.<triple>]` 声明它管哪些源。

⚠️ 而这条又依赖 §17.2a(workspace 多 target)—— **两条是同一个根**:mcpp 今天假设「一次构建 = 一个 target」。

---

## 20. ⚠️ §17.2a 更正:workspace 多 target 已经能用(WS1–WS3 实测)

### 20.1 我判错了

§17.2a 断言「fan-out 把同一个 target 传给每个成员 ⇒ 混合 workspace 表达不了」。**错。** 我读了 `cmd_build.cppm:110` 的 `mo.target_triple = ov.target_triple` 就下了结论,**漏了 `ov` 为空时成员 manifest 会填**(`prepare.cppm:1225-1226`)。

**WS1 实测** —— 一个 workspace,两个成员各自声明 `[build] target`:

```console
$ mcpp build --workspace          # 不带 --target
firmware/target/x86_64-linux-musl/…/firmware   → statically linked (musl)
hosttool/target/x86_64-linux-gnu/…/hosttool    → dynamically linked (glibc)
```

**一条命令,两个成员各自用了自己的 target。** ⇒ 「固件 + 上位机工具」在一个 workspace 里**今天就能表达**。

⚠️ 这是本轮第三次「读代码下结论、实测推翻」(前两次:ISO 的不合格对照、E1 的判断)。**代码读出的是意图,实测读出的是行为。**

### 20.2 剩下的两条(都比原判小得多)

#### (a) `--target X --workspace` 是全局覆盖 —— 需要一个决策

**WS3 实测**:显式 `--target x86_64-linux-gnu` **覆盖了** firmware 的 `[build] target = musl`,firmware 被编进了 `target/x86_64-linux-gnu/`。

对混合 workspace 这是错的语义:`mcpp build --workspace --target riscv64-none-elf` 会把 `hosttool` 也编成裸机。

| 选项 | 后果 |
|---|---|
| **(a) 冲突即报错**(推荐) | 「成员 `firmware` 声明了 target `A`,与 `--target B` 冲突」—— **说出来,不猜**,与 E1 的教训一致 |
| (b) `--target` 只填空,不覆盖显式声明 | 同构 workspace(无人声明)的主用例仍然可用;但「我就是要全部编到 X」失去表达 |
| (c) 保持现状(全局覆盖) | 混合 workspace 上静默做用户没要求的事 |

⇒ **建议 (a)**:把静默覆盖变成显式错误。要逐个覆盖用 `-p`。

#### (b) ⭐ CDB:形状变了,而且变简单了

**WS2 实测**:CDB 是**每成员一份**(`firmware/compile_commands.json`、`hosttool/compile_commands.json`),**workspace 根没有** ⇒ 在根目录打开编辑器时 clangd 找不到。

⚠️ 这是**与裸机无关的已有 gap**,裸机只是让它更痛(flags 差异最大)。

⭐ **但它比 §19.8 描述的情况简单**:不是「一个文件被两个 target 反复覆盖」,而是「每成员一份、根上没有」。⇒ **解法就是合并到 workspace 根**。

而且**每个成员已经有自己正确的 target flags** ⇒ 合并出来的 CDB **天然就是「每个文件用它所属 target 的 flags」** —— §19.8 提的「per-file 正确」**不需要新机制,合并即得**。

⇒ **§19.8 的方案简化为一句:`--workspace` 构建后,把各成员 CDB 合并到 workspace 根。**

### 20.3 更正后的 §17 状态

| 项 | 原判 | 现状 |
|---|---|---|
| 17.2a workspace 多 target | ❌ 表达不了 | ✅ **已能用**(WS1);剩 `--target` 覆盖语义一个小决策 |
| 17.2d 多 target CDB | 需要 per-file 合并机制 | ✅ **合并各成员 CDB 到根即可**,per-file 正确性免费 |

---

## 21. 剩余项:推荐方案(**2026-08-19 已全部批准**)

### 21.0 已定:`--target` + `--workspace` = 冲突即报错

成员声明了 `[build] target` 而命令行给了不同的 `--target` ⇒ **报错说明冲突**,不静默覆盖。要逐个覆盖用 `-p`。(§20.2a 选项 a)

### 21.1 ✅ 17.2c 裸机 `mcpp pack` —— **已定**:复用现有机制,只换产物集

**先划边界**:烧录 / OTA 是**设备侧**的事,mcpp 不碰(与 2026-07-24 决策 #1「不做发行版构建器」一致)。`pack` 的边界是**产出一个可分发的目录/压缩包**,不是把它送进设备。

| 场景 | 方案 |
|---|---|
| **固件镜像发布**(新) | 产物集(§19.1:`.elf` `.bin` `.map` + size)+ **sha256** + **构建元数据**(target · toolchain · 版本 · BSP 版本) |
| **裸机库分发** | ⚠️ **不是新东西** —— #433 / `2026-08-17-library-distribution-design.md` 已覆盖;只需确认 `kind` 轴接受 `os=none` |
| **BSP 分发** | 源码包,走普通包路径,零新增 |

⚠️ **构建元数据是刚需不是装饰**:现场排障时第一个问题永远是「这台设备上跑的是哪个构建」。镜像本身不带版本 ⇒ 无法回答。

⇒ **推荐:`mcpp pack` 在 `os=none` 下换一套产物集 + 一份 metadata,不发明新格式、不新增命令。**

### 21.2 ✅ D1 —— **已定**:立即修,作为独立 PR,不进任何路线 scope

三条理由:

1. 它是**当下唯一「已发布能力可达的错误行为」**(2026.8.15.3 上可复现);
2. **修法与路线无关** —— 无论做不做裸机,「`--target` 被静默丢弃、报告成功」都是缺陷;
3. ⚠️ **不先修会污染后面所有验收**:S1 的判据「产物 `file` 含 `UCB RISC-V`」在 E1 未修时同时在验两件事,红了分不清是谁的锅。

⇒ **独立 PR 先落,规模:`prepare.cppm` 一处 + 回归测试(直接断言 `file` 输出)。**

### 21.3 ✅ D2 路线 —— **已定**:承诺 B,C 按需,D 先做「可证伪探针」而非承诺

| | 推荐 | 理由 |
|---|---|---|
| **B(23 项)** | ✅ **现在承诺** | issue #403 闭环;全在已有结构上加列/加态;**D 完全依赖 B,B 不依赖 D** ⇒ 先做 B 零损失 |
| **C(载荷 + std 子集)** | ⏸ **按需** | 等有真实用户要 std 子集时再做。载荷是真工程量(builtins × target、picolibc multilib、per-target `__config_site`) |
| **D(三层 SPEC 生态)** | ⚠️ **不承诺,先做最小可证伪探针** | 见下 |

**为什么 D 不该现在承诺:**

- ⚠️ **D 是不同产品**:openkal SPEC 是**多年期、跨组织**的标准化工作;mcpp 是构建工具。用户群、节奏、成功判据都不同。
- ⚠️ **SPEC 类项目的失败模式是「没有实现者」**,而不是「设计不好」。WASI 背后是 Bytecode Alliance;embedded-hal 背后是 rust-embedded WG 加几年积累。**这个变量不能靠设计质量决定。**
- ⚠️ §18.4 已定:官方必须自持 **linux / windows / bare 三个后端**才能执行准入判据 —— **那个成本是硬的**,不是可选的。

**推荐的探针形态**(小到可以扔掉):

> B 落地后,用它做一个 `openkal` **最小 world**(`io/streams` + `clocks/monotonic` + `memory/alloc`)+ **两个后端**(linux / bare)+ conformance suite 骨架。
> **KA1 已经证明这个规模能跑通**(`kal_write` 一个函数撑起两个后端)。
> **判据不是「设计好不好」,而是「有没有第三方来实现第三个后端」** —— 那才是 D 能否成立的真变量。

⇒ **用最小成本先测那个变量,比先建完整 SPEC 再等人来,风险低一个数量级。**

### 21.4 ✅ AAL 命名 —— **已定**:`openarch`

| 候选 | 取舍 |
|---|---|
| `openaal` | 与 `openkal`/`openhal` 命名一致,但 "AAL" 三个字母本身不自解释 |
| **`openarch`**(推荐) | 打破 `-al` 的对称,但 **"arch" 明确排除了 "kernel" 的联想** |

⚠️ §11.3 的判据是「**名字决定边界会不会被侵蚀**」—— 叫 KAL 会把策略吸进来,叫 AAL 同理。**清晰性优先于对称性。**

---

## 22. 决策收尾(2026-08-19)

**全部决策已定,本设计文档进入实施阶段。**

| 决策 | 结论 |
|---|---|
| **D1** E1 静默失败 | ✅ **立即修,独立 PR**,不进任何路线 scope |
| **D2** 路线 | ✅ **承诺 B(23 项)· C 按需 · D 先做可证伪探针不承诺** |
| A4 产物形态 | ✅ `.elf` + `.bin` + `.map` + size 默认,`.hex` 可选 |
| C4 hermetic 逃生舱 | ✅ `os=none` 下 `allow_host_libs` 失效 |
| D2(provision)链接顺序 | ✅ **两个具名槽** `prologue`/`epilogue`,槽内数组序 |
| E1 `requires` 扩包级 | ✅ |
| F1 `[freestanding]` 段 | ✅(②工效档) |
| G2 裸机 test 粒度 | ✅ 默认 `batch`,`isolated` 可选 + 超时自动重跑 isolated |
| K1 `requires-hard` | ✅ 加 |
| `--target`+`--workspace` | ✅ **冲突即报错** |
| 17.2c 裸机 pack | ✅ 复用现有机制 + 产物集 + metadata |
| AAL 命名 | ✅ **`openarch`** |

**⇒ 实施计划独立成文:`.agents/docs/2026-08-19-freestanding-baremetal-implementation-plan.md`**
