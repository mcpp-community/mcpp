---
subject: cross-platform
status: active
---

# 生态方案：跨平台通用基础设施（mcpp + openkal + xim），以一个 C89 工程为验证

**日期：** 2026-11-10
**状态：** 第四版，mcpp 部分已在 2026.11.10.1 实施。Q1–Q6 已定稿（第 11.1 节）；O1–O4 已按推荐实施，实施中的调整见第 11.2 节；第 12 节为两项
风险的说明，第 13 节为多视角深度自审；第 15 节为实施记录，第 16 节为测试体系向 mtest 的迁移
**范围：** mcpp（引擎与内置数据）；openkal 家族（openkal 规范、openkal-linux/macos/windows、openkal-musl、
openkal-llvm-runtime、openkal-image、openkal-platform、openkal-conformance，新增 openkal-freebsd、openkal-posix）；
xim-pkgindex 与 xlings（目标系统的数据载荷）；mcpp-index（compat 描述符）；mcpplibs 的运行器包。
验证工程（一个 C89 命令解释器）只用于检验，不在上游范围内，本文不点名。
**输入：** mcpp#911 与其复测评论（2026.11.9.1 上的 N1–N8）；mcpp#913、#907、#902；
[2026-11-09-target-model-design.md](2026-11-09-target-model-design.md)；
[2026-11-08-ecosystem-top-level-design.md](2026-11-08-ecosystem-top-level-design.md)；
2026-11-08 至 2026-11-10 的本地实测（第 14 节）。

## 1. 目标

验证工程是一个 C89 命令解释器。它自己列出的平台有：Linux、macOS、Windows、MSYS2、FreeBSD、NetBSD、OpenBSD、
Haiku、PureDarwin、GNU/Hurd、System V。以它为验证工程，要回答两个问题：

1. 只靠 mcpp，能否把这样一个工程构建成**所有这些平台的原生程序**，以及**一份跨平台的 ok 映像**（oke/okb）？
   同时让工程能用 C++ Modules 与 `import std` 编写新代码。
2. 达成这件事时，需要的能力是否全部是**通用基础设施**？要求是：mcpp 与 openkal 不吸收验证工程的任何形状，它的特殊需求
   留在它自己的仓库内部。

## 2. 原则

- **P1 机制在引擎，事实在数据。** 沿用 target-model 设计第 2 节。引擎不出现系统名或实现名；行、提供者、sysroot、
  运行器都是数据或包。
- **P2 第二个使用者检验。** 一项改动只有在能说出一个与验证工程无关的使用者时，才进入上游，否则留在验证工程。
- **P3 默认 B 路线与 oke，A 路线可选。** 两条路线见第 4 节。选择的对象是**目标身份**（`x86_64-linux-musl` 还是
  `x86_64-linux-gnu`），所以用目标的机制表达（`[pack] targets`、目标集合、`[target.<sel>.priority]`），不用 feature。
  feature 选择的是同一目标上包的代码变体，两者不混用。
- **P4 程序语义优先于默认路线。** 一个程序在某个系统上的行为如果依赖该系统的原生接口（例如一个命令解释器在 Windows 上依赖
  Win32 的路径写法与程序查找），这个程序在该系统上就以 A 路线为默认。判断与声明都在程序自己的仓库里。
- **P5 与 openkal 规范的三个时机一致。** 能力在依赖解析时回答（`requires-interfaces` 与 `provides-interfaces`），
  接口的有无在链接时回答（SPEC 6.1），行为差异由属性字回答（SPEC 6.2）。加载时才绑定的消费者用 `kal_interfaces()`
  （`openkal/version.h`）。
- **P6 缺失就如实缺失。** 不提供的接口在链接时不存在，不写返回"不支持"的桩函数（SPEC 6.1）；不能如实实现的语义宁可
  拒绝，也不给半真的成功（openkal-musl#36 的先例）。
- **P7 数据载荷取自权威上游，只做可复现的简单变换。** 来源是项目官方发布物，或该领域公认维护者的发布物；变换只限于
  按清单解包、裁剪与加上一份清单（`mcpp.toml`），脚本随包记录。名称使用业界通用叫法。
- **P8 缺省值是数据，覆盖点在同一处。** 每个缺省值都写在数据里（内置数据、平台包、工程清单），并且在工程清单、本机
  配置或命令行里有对应的显式覆盖。不引入只有一种写法、无法覆盖的隐式行为（第 10 节列出全部缺省与覆盖点）。
- **P9 不依赖宿主环境。** 构建、测试与运行所需的一切取自 xim-pkgindex（xpkg）或 mcpp-index。无法避免的宿主依赖收敛到
  最小，并以 xim 的 xpkg 封装，限定其影响范围。运行器所需的 wine、qemu 与虚拟机镜像，目标系统的 sysroot，CI 用到的
  mtest，都是 xpkg；每个工作项写明它的外部程序由哪个 xpkg 提供。

## 3. 基线：2026.11.9.1 上已经成立和尚未成立的部分（实测）

| 能力 | 状态 |
|---|---|
| ok 映像：验证工程不改源码即可构建，okb 带两个架构、两个程序（`--all-programs`） | ✅ |
| 平台定义的行（`[[platform.define]]`）：`x86_64-freebsd` 原生程序（C、C++、验证工程本体，动态或静态）在 FreeBSD 14.5 虚拟机中运行 | ✅ |
| 运行器（G11）：`mcpp run --target x86_64-freebsd` 经虚拟机运行器在 FreeBSD 中执行，退出码如实返回 | ✅ |
| 能力的缺省提供者（G7）：openkal-run 0.5.4 直接使用依赖图中的实现 | ✅ |
| `i686-none-elf`（G12）：ELF32 i386 程序 | ✅ |
| **一台 Linux 主机经 B 路线产出多系统原生程序**：C++ Modules + `import std` 的程序编出 `x86_64-windows-musl`（PE，wine 下运行）、`aarch64-macos-musl`（Mach-O，已构建）、`x86_64-linux-musl`（运行），以及 oke | ✅ |
| **Linux 主机经 A 路线产出 Windows 原生程序**：验证工程不改源码、使用 Win32 层，`x86_64-windows-gnu`（llvm + mingw 载荷）构建并在 wine 下运行（需关闭 `[resources]`，见 E3） | ✅ |
| openkal 下 Ctrl+C：程序在 `main` 开头调用一次 `kal_process_stop_requested()`，即与原生版行为一致 | ✅ |
| FreeBSD 系统 sysroot 行上的 `import std` | ✗ base 的 libc++ 为 21.1.8，未附带 std.cppm；引擎用了 llvm 23 payload 的 std.cppm 与头文件，版本不符 |
| Windows 行编译 `.rc` 资源（gnu 风格，llvm） | ✗ E3 |
| windows-musl 程序按名字启动外部程序 | ✗ `PATH` 是 Windows 原生格式（`;` 分隔），openkal-musl 的 `posix_spawnp` 按 `:` 切分，连 `hostname.exe` 也找不到（K11） |
| 写在 libc 之上的实现被放到 openkal-musl 下面 | ✗ mcpp 不拒绝；运行时 `kal_time_wall → clock_gettime → kal_time_wall` 无限递归崩溃（E14） |

## 4. 目标架构

### 4.1 原生程序的两条路线，以及 oke

- **B 路线（默认）：openkal hosted。** C 库用 openkal-musl，C++ 运行时与 `import std` 用 openkal-llvm-runtime，二者只
  依赖 kal_* 接口，与系统无关。每个系统只需要**一个**写在其内核接口上的 openkal 实现。产物是静态程序，任何宿主都能编，
  不需要 sysroot。
- **A 路线（可选，或由 P4 选为默认）：系统自身的 ABI。** 链接系统的 C 库与 C++ 运行时，需要目标系统的数据载荷
  （sysroot、SDK、导入库）。
- **oke：** 同一份映像由各系统的 okrun 运行；okb 把多个架构打成一个包。

### 4.2 按系统的 ABI 性质分为两类

| 类别 | 系统 | B 路线 | A 路线 | oke 的 okrun 跑在 |
|---|---|---|---|---|
| 内核 ABI 稳定 | Linux、FreeBSD、NetBSD；Windows（Win32 是稳定 ABI） | ✅ 默认 | 可选 | 内核层实现（openkal-linux/windows/freebsd…） |
| libc 才是稳定 ABI | OpenBSD（禁止 libc 之外发系统调用）、Haiku（libroot）、GNU/Hurd（glibc）、illumos | ✗：ELF 没有两级命名空间，两个 C 库会在同一进程里冲突 | ✅ 唯一选择 | libc 之上的实现 openkal-posix |
| 特殊 | macOS：B 产物动态链接 libSystem，但 openkal-macos 内部直接发 `svc #0x80`，而 Apple 只保证 libSystem 这一层的稳定 | ✅ 默认（风险与处置见第 12.1 节） | 可选 | openkal-macos |

openkal 规范第 2 条允许实现"建立在 C 库之上、之下或不依赖 C 库"。openkal-emscripten 已是官方家族中第一个写在 C 库
之上的实现，openkal-posix 与之同类。写在 C 库之上的实现只能用于"C 库来自系统"的组合（A 路线与 okrun），不能放到
openkal-musl 下面；这一点由 E14 保证。

### 4.3 分层

```
                       验证工程（命名、入口、平台层选择、移植步骤、发布矩阵）
                                    │ 只依赖下面的通用能力
 ┌──────────────────────────────────┴───────────────────────────────────────────┐
 │ mcpp 引擎（机制）：行/提供者/选择 · 能力层（含层依赖不成环）· 接口集合的暴露 ·   │
 │ 补丁 · 运行器协议 · 打包                                                         │
 │ mcpp 内置数据：主流系统的标准身份（A 与 B 两种形式）与内置提供者                  │
 └──────┬─────────────────────────┬───────────────────────────────┬──────────────┘
        │ 平台包（提供者）          │ 依赖图中的 openkal 家族           │ 运行器包
  freebsd-platform 等         规范 · 实现：                           wine / qemu-user /
  （A 路线：sysroot 基座包）     内核层：linux · macos · windows ·      系统虚拟机
  openkal-platform              freebsd（新）· opensbi                 （mcpplibs）
  （B 路线与 oke）              libc 之上：emscripten · posix（新）
  长尾系统：platform.define    openkal-musl · openkal-llvm-runtime
                            openkal-image（okld · okrun）
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ xim-pkgindex：工具链；目标系统数据载荷（基座包）：glibc · linux-headers ·         │
 │ iphoneos-sdk · windows-sdk；新增 mingw-w64-sysroot ·                             │
 │ x86_64-freebsd-sysroot · aarch64-freebsd-sysroot                                │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### 4.4 每一层各管什么

| 层 | 持有 | 不持有 |
|---|---|---|
| mcpp 引擎 | 选择规则、层的合并与来源、层依赖的一致性、补丁契约、运行器协议、打包流程、把解析结果暴露给构建程序 | 任何系统名、实现名、sysroot 路径 |
| mcpp 内置数据 | 主流系统的标准身份（如 `x86_64-freebsd` 与 `x86_64-freebsd-musl`）与内置提供者 | 某个工程的约定 |
| 平台包 | 为身份提供实现（工具链、sysroot 基座、层、运行器、命名）；长尾系统可以自己定义行 | 引擎机制 |
| openkal | kal_* 接口、实现、C/C++ 运行时、映像格式与加载器、一致性测试 | 窗口、图形等系统服务以外的接口（规范第 3 节接口清单） |
| xim-pkgindex | 与宿主无关的目标侧数据（附带声明其提供哪些层的清单）、工具链 | 构建规则 |
| 验证工程 | 产物命名、入口、平台层选择、移植步骤、发布矩阵 | — |

## 5. 已否定或降级的形状

| 原想法 | 结论 | 理由 |
|---|---|---|
| openkal-platform 内置 FreeBSD 行与 FreeBSD sysroot | 否定 | 把一个系统的数据推进了 openkal 的平台包；B 路线有了 openkal-freebsd 后不需要 sysroot |
| openkal-musl 把 `sigaction(SIGINT, SIG_IGN)` 映射为 stop word 接管 | 否定 | 接管是三个信号一起的，只忽略 SIGINT 会连带吞掉 SIGTERM、SIGHUP（P6） |
| openkal-run 按操作系统选择 space 绑定单元 | 由 K1 取代 | 按实现提供的接口决定 |
| K1 以弱引用为主方案 | 降为记录在案的备选 | 主方案在构建期按解析结果选择，不依赖工具链的弱符号语义 |
| K1 在 E13 发布前先用弱引用过渡 | 否定（第三版） | E13 很小且排在 P1，FreeBSD 的 B 路线（K8）晚于它；过渡代码之后要整体删除，是一次性工作 |
| E13 同时提供 cfg 选择器 | 推迟（第三版） | 目前唯一的使用者 openkal-run 在构建程序里读取即可；cfg 键等出现第二个清单层面的使用者再加 |
| E5 用 `${sysroot}` 记号加一个独立的数据层包 | 由"基座包"取代（第三版） | 让 sysroot 本身成为带清单的包，不引入新记号，也不多一个包 |
| FreeBSD 的标准身份由 freebsd-platform 用 `[[platform.define]]` 定义 | 改为内置数据（第三版） | 主流系统的身份放在一处；平台包只做提供者，避免两个平台各自定义同一系统的行 |
| 用 feature 切换 A/B 路线 | 否定 | A/B 是目标身份，不是代码变体（P3） |
| 基座包用专门的 `[base]` 表（`roots` 按架构映射、`include-dirs`、`libs = { dynamic, static }`、`compiler-runtime-dir`） | 由"基座包就是普通包"取代（第四版） | 按架构映射的表是第二种条件写法（SPEC-004 §3.1）；连字符键与既有的 `include_dirs` 是同一概念的两种拼法（§5.1）；`dynamic` 不是 mcpp 的链接形态词；角色标记与 c-abi 独占律重复。定稿只新增一个键 |
| X3 独立的 `libcxx-modules` 包加版本核对 | 取消（第四版） | std 模块源由同一版本的 libcxx 源码随基座生成，一致由构造保证 |
| 修改 SPEC-002 的"c-abi 独占律" | 否定 | 一个供给 c-abi 的基座包同时供给其它层，现有规则（一个包可供给多层）已允许 |
| 把 i386 只当作 SVR4 的需求 | 改为"32 位一类目标" | i686、armv7、riscv32 共用同一组组件 |

## 6. 工作项

每项写明：问题、第二个使用者、设计、改动量、接受度、依赖。改动量为估算；标"实测"的已有原型或测量。

### 6.1 mcpp（mcpp-community/mcpp）

| # | 问题 | 第二个使用者 | 设计 | 改动量 | 接受度 | 依赖 |
|---|---|---|---|---|---|---|
| **E1** | R1：`Triple::has_own_sysroot()`、`freestanding::resolve()` 等判断只查编译期内置表，不认运行时登记的行（N2、N3 链接侧、N7） | 任何平台定义的行 | 这些判断改为查询选中的行与提供者：是否声明了 sysroot、`execution` 是否为本机、行自身的机器描述 | 200–400 行 + 3–4 个端到端测试 | 高（SPEC-016 §2.1、§5 已这样要求） | — |
| **E2** | N1：两个程序共用同一入口文件时，只编一个入口对象，一个程序的 defines 悄悄带进另一个 | 任何多变体程序 | 入口参数不同就按程序分别编译（与 G9 的共享源码变体一致），或拒绝并指出两个程序 | 50–100 行 + 1 个测试 | 高 | — |
| **E3** | N9：Windows 行（gnu 风格）找不到资源编译器。引擎只在 clang++ 旁边找 windres；llvm payload 带有 `llvm-rc`，mingw 载荷带有 `x86_64-w64-mingw32-windres`，都没被用上 | 任何从非 Windows 主机编译带版本信息或图标的 PE | 查找范围包括配对提供者的载荷；或 lld 链接时直接用 `llvm-rc` 产出的 `.res` | 30–80 行 | 高 | — |
| **E4** | 主流系统缺标准身份 | 任何要发布 FreeBSD 原生程序的工程 | builtin.toml 增加 `x86_64/aarch64-freebsd`（A，系统 ABI）与 `x86_64/aarch64-freebsd-musl`（B）四行；B 由 openkal 提供者服务，A 由平台包提供（行本身没有内置提供者，未选平台时报告"不可得"及原因）。NetBSD 同理。真正的长尾系统（如 SVR4）仍由平台 `[[platform.define]]` | 约 40 行数据 + 测试 | 高 | K8（B 行可用） |
| **E5** | A 路线上系统自带的 C++ 运行时没有干净的表达（N3 编译侧、FreeBSD 的 `import std`） | FreeBSD、#913 的 Windows 系统侧、其它系统 sysroot | 基座包，见下文 | 150–250 行 + SPEC-002 §2.5 | 中高 | E1、X2 |
| E6 | N5：给 git 工作区成员打补丁时，变体只复制成员目录，它对兄弟包的 `path` 依赖失效 | 任何 git 来源的多包仓库 | 变体复制克隆的根目录，保持成员的相对位置 | 50–120 行 | 高 | — |
| E7 | N8：外系统的动态程序在默认打包时被本机 `ldd` 处理；`--mode static` 给动态程序贴上 static 标签 | A 路线的所有行 | 外系统的行改为按 `DT_NEEDED` 加 sysroot 计算依赖闭包，或明确拒绝；static 模式核实产物 | 150–250 行 | 中 | E1 |
| E8 | N6 与 docs/04 的矛盾 | — | `driver = "compiler"` 时 `link.flags` 给出警告；修正 docs/04 第 544–546 行 | <30 行 | 高 | — |
| **E9** | #913：mingw 提供者把 Win32 系统侧绑在宿主的交叉 GCC 上，macOS 主机因此没有 windows-gnu | 所有 GUI 或调用 Win32 的工程 | 新增提供者数据（如 `mingw-llvm`：`serves = ["x86_64-windows-gnu"]`、`pairs = ["llvm"]`、`hosts = ["*"]`，sysroot 为 X1 基座包）；修正"写 toolchain 路径"那句诊断 | 约 20 行数据 + 诊断文字 | 高 | X1、E5 |
| E10 | #902：scan_overrides 匹配到被 feature 或 target 排除的源文件时报错 | boost-m | 按 #902 方案 A：文件存在但本次未参与编译时跳过或只警告 | 30–60 行 | 高 | — |
| E11 | 没有任何 32 位 hosted 行 | i686/armv7 的 Linux 设备 | builtin.toml 增加 `i686-linux-musl` 等行；`i686-openkal` 等待 K7 | 数据为主 | 中 | K7 |
| E12 | #907：GCC 行上的共享库变成 DF_STATIC_TLS | Python 扩展等 | 按 #907 的方案处理；不在验证工程的路径上，只在此记录 | — | — | — |
| **E13** | 能力层供给者声明的接口集合（`provides-interfaces`）只用于集合差检查，依赖它的包看不到 | openkal-run（K1）；openkal-conformance 可据此自动选择检查项；按需启用网络等功能的库 | 只暴露给构建程序，拼写见第 11.2 节 O1 | 60–120 行 + 文档 | 高（docs/22 主张"在解析期回答"） | — |
| **E14** | 写在 C 库之上的实现被放到依赖它的 C 库之下，mcpp 不拒绝，运行时无限递归（实测） | openkal-emscripten、openkal-posix 及以后任何此类实现 | 规则：能力层之间的 `requires` 不得成环，成环时在解析期拒绝并写出环上的每个包。数据：openkal-musl 如实声明 `requires = ["mcpp:kernel-abi=openkal"]`；写在 C 库之上的实现声明 `requires = ["mcpp:c-abi"]` | 引擎 40–80 行 + 1 个测试；各包 1 行 | 高（把 SPEC-002 §3.2 规则二变成可检查的） | — |

**E5 的细节：基座包（第四版定稿）。**

目标：A 路线上"系统自带的 C 库、C++ 运行时、编译器运行时"由一个包表达。定稿的形态是**基座包就是普通的包**：不新增表，
只新增一个键；唯一的新约定是"包根即 sysroot，一个包服务一个身份"。

```toml
# xim:x86_64-freebsd-sysroot@14.5 —— 包根即 sysroot
[package]
name     = "x86_64-freebsd-sysroot"
version  = "14.5"
provides = ["mcpp:c-abi=freebsd-libc", "mcpp:c++-abi=libc++", "mcpp:compiler-runtime=libgcc",
            "hosted-standard-library"]

[build]                                   # 与依赖图中任何层供给者同一套词
include_dirs      = ["usr/include/c++/v1"]
ldflags           = ["-lc++", "-lcxxrt"]
std-module        = "usr/share/libc++/v1/std.cppm"
std-compat-module = "usr/share/libc++/v1/std.compat.cppm"
std-module-flags  = ["-D__fenv_static="]
```

```toml
# 平台包：每个身份由一个提供者服务，各指名自己的基座（SPEC-016 §3 的 sysroot 键）
[[platform.provider]]
serves  = ["x86_64-freebsd"]
sysroot = "xim:x86_64-freebsd-sysroot@14.5"

[[platform.provider]]
serves  = ["aarch64-freebsd"]
sysroot = "xim:aarch64-freebsd-sysroot@14.5"
```

规则：

1. **角色由位置决定。** 一个包在 sysroot 的位置被指名（项目、本机配置、平台或提供者的 `sysroot`），且其根带 `mcpp.toml`，
   它就是基座包；清单里没有、也不需要角色标记。SPEC-002 §3.1 的 c-abi 独占律已经规定"供给 c-abi 的目录作为
   `--sysroot` 寻址"。
2. **层由 `provides` 回答，层的事实用 `[build]` 的既有键。** 与依赖图中的层供给者（如 openkal-llvm-runtime）完全相同：
   `include_dirs` 是 C++ 库的头文件（规则二，`-nostdinc++` 之后加入）；`ldflags` 是链接的库，供给 c++-abi 时引擎加
   `-nostdlib++`；`std-module`、`std-compat-module`、`std-module-flags` 按 SPEC-002 §2.3。区别只在来源：`prebuilt`
   而非 `graph`。
3. **包根即 sysroot，一个包服务一个身份。** 路径照旧相对包根。身份是 mcpp 的通用键（输出目录 `target/<身份>/`、锁、
   缓存），sysroot 与身份一一对应，因此不需要架构映射，也不引入第二种条件写法（SPEC-004 §3.1）。驱动能在一个根内区分
   架构的系统（MinGW：`<sysroot>/<arch>-w64-mingw32/lib`），一个包可以服务该系统的全部身份。包名相应为
   `<身份>-sysroot`（`x86_64-freebsd-sysroot`，与 xim 的 `aarch64-linux-musl-gcc` 同一惯例）或 `<系统>-sysroot`
   （`mingw-w64-sysroot`）。
4. **唯一的新键：`[build] compiler_runtime_dir`。** 编译器运行时所在的目录，相对包根；clang 渲染为 `-resource-dir`，
   gcc 渲染为 `-B`。它是任何 compiler-runtime 供给者都可以陈述的事实，不属于 clang，也不限于基座（SPEC-004 §5.1 的
   下划线写法）。
5. **一个链接列表。** `ldflags` 不按链接形态区分：动态链接时多出的 `-lcxxrt` 无害（`libc++.so.1` 本来依赖它），静态
   链接需要它。静态链接时引擎加 `-static`，使导入库与共享库让位于归档。
6. **std 模块随基座。** 第三版设想的 X3（独立的 `libcxx-modules` 包加版本核对）取消：基座里的 std 模块源由与基座
   libc++ 同一版本的官方 libcxx 源码发布物生成（libc++ 自己的 `modules/CMakeLists.txt` 的同一变换），版本一致由构造
   保证。
7. **目标侧完整时不注入宿主的东西。** 基座供给 c++-abi 时，引擎不注入 payload 的 libc++ 头文件、本机库目录与为 payload
   宿主构建的 C++ 归档（与 E1 合作）。

渐进披露：

| 层级 | 写什么 | 适用 |
|---|---|---|
| L0 | `provides = ["mcpp:c-abi=…"]` | 只有 C 库的 sysroot；与 `{ path, provides }` 等价，但自描述 |
| L1 | + `[build] include_dirs`、`ldflags`、`std-module*` | C++ 运行时与 `import std` |
| L2 | + `[build] compiler_runtime_dir` | 自带编译器运行时（MinGW） |

多架构不是一个层级：它是多个包加多个提供者，对使用者不可见。

X1 的清单：`provides` 为 c-abi（UCRT 上的 MinGW-w64）、c++-abi（libc++）、compiler-runtime（compiler-rt）与
`hosted-standard-library`；`[build] include_dirs = ["include/c++/v1"]`、`ldflags = ["-lc++", "-lunwind"]`、
`std-module = "share/libc++/v1/std.cppm"`、`compiler_runtime_dir = "compiler-rt"`。在 driver-internal 的 MinGW 行上，
基座是 clang 的 sysroot。

### 6.2 openkal 家族

| # | 仓库 | 内容 | 改动量 | 接受度 | 依赖 |
|---|---|---|---|---|---|
| **K1** | openkal-image（openkal-run） | 按实现提供的接口绑定，见下文 | 约 40 行 + 删除 `no_space.cpp` 与按系统的源文件表 | 高 | E13 |
| **K8** | 新仓库 openkal-freebsd | 写在 FreeBSD 系统调用上的内核层实现，结构照 openkal-linux：系统调用层、启动与 `.note.tag` 品牌标记、线程指针（sysarch）、thr_new + _umtx_op、pdfork + wait6、ppoll/kqueue、TIOCGETA、sysctl。有了 K1 可以先提供部分接口 | 从 openkal-linux 派生，约 4–6k 行 | 高 | K1（部分接口时） |
| K10 | 新包（名称待定，如 `openkal-unix-core`） | 类 Unix 内核层实现之间可共用的机械部分，边界见第 11.2 节 O2 | 视抽取范围 | 中 | K8 之后 |
| **K11** | openkal-musl | 按名字查找程序（`execvp`、`execlp`、`posix_spawnp`）遵循所在系统的约定：目录列表的分隔符与程序后缀取自 `kal_env_system()`。Windows 上用 `;` 分隔并依次尝试 `PATHEXT` 中的后缀，其它系统行为不变 | 50–100 行 + 测试 | 高（规范第 30 条明确写了"程序的后缀、目录列表的分隔符"由此回答） | — |
| K6 | openkal 规范 + 三个实现 | stop word 由"置 1"改为"每次请求加 1"，"非 0 且永不清除"仍成立；加到回绕为 0 时跳过 0 | 规范一句，每个实现约 3 行，加一致性测试 | 中高 | — |
| K5 | openkal 规范 + 三个实现 + musl | 硬链接：openkal.fs 中加一个 linkat 风格的操作，加一个按卷查询的属性位，照软链接（规范第 7 条）的先例 | 250–350 行，跨 5–6 个仓库 | 中高 | — |
| K2 | 新包 openkal-posix | 写在 POSIX C 库之上的实现，服务第 4.2 节第二类系统上的 okrun。声明 `requires = ["mcpp:c-abi"]`（E14）；不写桩函数；先修完一致性测试 | 实测：一致性测试 148 项通过、8 项不通过、4 项无法观察，之后在 openkal.process 卡住；修复约 100–200 行 | 中高（openkal-emscripten 是先例） | K1、E14 |
| K9 | openkal-platform、openkal-run | K8 之后：`[defaults]` 增加 freebsd；运行器与载体的主机列表加入 freebsd | 约 10 行 | 高 | K8、E4 |
| K12 | openkal-macos | 讨论项：系统调用层改为调用 libSystem 导出的函数（Apple 保证的稳定层），kal 语义不变，见第 12.1 节 | 只改系统调用层 | 待维护者判断 | — |
| K7 | 规范 + openkal-image + musl + llvm-runtime + openkal-linux | 32 位一类目标：第 12 条补 32 位约定、libokld 支持 ELF32 与 R_386 重定位、image_abi、32 位系统调用、`i686-openkal` 行。先做 i686 | 2–3k 行以上，跨 6 个仓库 | 低到中，以设计讨论形式提出 | E11 |

**K1 的方案。**

- **问题：** openkal-run 0.5.4 按 Linux/macOS/Windows 选择 `bindings_space.cpp` 或 `no_space.cpp`，绑定表对其余所有
  接口都是强引用。结果是新系统要打补丁，只实现部分接口的实现要写违反 SPEC 6.1 的桩函数。
- **方案（构建期，依赖 E13）：**
  1. openkal-run 在清单里写 `[kernel-abi] requires-interfaces`，列出 runner 自身要用的接口。mcpp 现已在解析期按实现
     的 `provides-interfaces` 检查它。
  2. `build.mcpp` 已经在从 SURFACE.txt 生成绑定表，改为只生成"自身需要 ∪ 实现提供"的接口的名字。接口集合从 E13
     读取。
  3. space 与 prof/debug 的转发按同一集合编译或省略；删除 `no_space.cpp` 与按系统的源文件表。
  4. 加载时，runner 认定的"已提供"是 `kal_interfaces()` 与编进来的集合的**交集**；缺接口的映像被拒绝（`run.cpp`
     现有逻辑，SPEC 12.4）。实现的清单与运行时回答不一致时，结果仍是加载时的拒绝，而不是调用空指针。
  5. 实现未声明 `provides-interfaces` 时视为提供全部接口，即今天的行为。规则简单：要么声明，要么全部提供。
- **配套：** openkal-conformance 增加一项检查，核对实现清单中的 `provides-interfaces`、`kal_interfaces()` 的回答与
  实际导出的名字三者一致（`tools/check-surface.sh --complete` 的延伸）。
- **为什么是这个方案：**
  - 符合"在解析期回答"（docs/22，P5）；
  - 不依赖任何工具链的弱符号语义；
  - 同一数据还服务 openkal-conformance 与其它库（E13 的第二个使用者）。
- **备选方案记录：** 弱引用加 `kal_interfaces()` 已实测可行，但不作为过渡方案实施（第 5 节）。它的实测结果证明了
  "加载时拒绝"这一半是可靠的。

### 6.3 xim-pkgindex 与 xlings

| # | 包（业界通用名） | 权威来源 | 变换 | 许可档 |
|---|---|---|---|---|
| **X1** | `mingw-w64-sysroot` | mingw-w64 是 Win32 头文件与 crt 的权威上游，但只发布源码。用 clang 构建好的成品，以 **llvm-mingw**（Martin Storsjö，LLVM 中 MinGW 支持的维护者）为来源：20261104 版基于 LLVM 23.1.5，构建脚本开启了 `LIBCXX_INSTALL_MODULES`，自带 std 模块 | 包根即 sysroot，服务 `x86_64-windows-gnu` 与 `aarch64-windows-gnu`：`include/`（共用头文件，含 `c++/v1`）、`<arch>-w64-mingw32/{lib,bin}`、`compiler-rt/lib/windows/`、`share/libc++/v1/`；不含宿主程序，一个归档服务所有宿主；UCRT 变体；22 MB；以打包修订版 `20261104-2` 发布（包清单以 `mcpp:c-abi=ucrt` 陈述其 C 库，与 MinGW-w64 载荷同名） | mingw-w64（ZPL 等）与 LLVM（Apache-2.0 + 例外），可再分发 |
| X2 | `x86_64-freebsd-sysroot`、`aarch64-freebsd-sysroot` | FreeBSD 官方发布的 `base.txz`（download.freebsd.org，摘要与发布的 MANIFEST 一致） | 每个身份一个包，包根即 sysroot：`usr/include`、共享库、`ld-elf.so.1`、crt 与 C 库、C++ 运行时、编译器运行时及其所需库的静态归档；去掉 FreeBSD 自身的工具链（`usr/lib/clang`、`libprivate*`）；加上由 libcxx 21.1.8 官方源码生成的 std 模块源 | BSD，可再分发 |
| ~~X3~~ | ~~`libcxx-modules`~~ | — | 取消（第四版）：std 模块源随基座生成，见 E5 第 6 条 | — |
| X4 | `openbsd-sysroot` 等 | 各系统官方发布物 | 同 X2 | 各自评估 |
| X5 | xlings 的"任意宿主"写法 | — | 目标侧数据与宿主无关，目前只能像 `iphoneos-sdk` 那样在每个宿主块里重复同一个 URL | — |

现有包名中 `mingw-w64` 已用于 Windows 宿主上的 winlibs 工具链，所以 X1 带 `-sysroot` 后缀以示区别。X2 按身份命名，与 xim 的 `aarch64-linux-musl-gcc` 同一惯例。版本节奏见第
11.2 节 O4。

### 6.4 mcpp-index 与 mcpplibs

| # | 内容 |
|---|---|
| I1 | compat.glfw 描述符：Windows 行以 `requires = { "mcpp:c-abi=ucrt" }` 陈述 Win32 后端所写的 C 库（与 Linux 行陈述 glibc 同一形式）。MSVC 行、MinGW-w64 载荷与 X1 基座都回答它，X1 由 E9 的提供者在每个宿主上给出，不需要私有依赖；B 路线（openkal-musl）没有窗口系统，在解析期按该要求被拒绝（#913 的 note (b)） |
| RP | 运行器包：wine（Windows 行）、qemu-user（异架构 Linux 行）、系统虚拟机（FreeBSD 等）。按 docs/41 的运行协议实现，供所有工程的 `mcpp run` 与 `mcpp test` 使用 |

## 7. 验证工程内部（只为验证，不进入上游）

### 7.1 Windows 上用 POSIX 层还是 Win32 层：有区别，按 P4 默认 Win32

一个同时带 POSIX 层与 Win32 层的程序，在 wine 下对比两种构建（A：`x86_64-windows-gnu` + Win32 层；B：
`x86_64-windows-musl` + POSIX 层）：

| 方面 | A（Win32 层） | B（POSIX 层） | 依据 |
|---|---|---|---|
| 当前目录与路径写法 | `Z:\tmp\winrun` | `Z:/tmp/winrun` | 实测 |
| 外部程序查找（`hostname`、`findstr`） | 能找到并运行 | 找不到。原因已定位为 openkal-musl 的程序查找（K11），不在程序本身 | 实测 |
| 批处理文件 | 可运行 | 可运行 | 实测 |
| 子进程收到的命令行 | 原样传递 | 按 argv 重新拼接 | 读代码推断 |
| 窗口状态与优先级、控制台 API、文件属性、文件关联 | 有 | 无对应 | 读代码推断 |

K11 落地后外部程序查找的差异会消失，但路径写法、命令行的原样传递与 Windows 特有的内建命令仍然不同。因此这样的程序在
Windows 上默认使用 Win32 层（A 路线），B 路线的 Windows 构建作为可选项。这是程序自己的决定：对大多数 POSIX 程序，
B 路线在 Windows 上就是正确的默认。

### 7.2 其余内部改动（形状，不含名字）

1. **按 C 库选择平台层**：依据是 `cfg(c-abi = …)` 与 `__OPENKAL__`，而不是操作系统宏；实现不提供 `openkal.space` 时
   改用基于 `posix_spawn` 的路径；`__OPENKAL__` 下以 `kal_process_stop_requested()` 处理 Ctrl+C、以 `kal_env_image()`
   取自身路径。
2. **构建配置**：产物命名留在程序的 `build.mcpp`；共用入口的两个程序在 E2 之后不再需要入口包装文件；资源脚本在
   E3 之后不再按行限定。
3. **模块化**：C89 内核保持为 C 源码；新增 C++23 模块层，模块接口在全局模块片段中包含 C 头文件，新代码与入口用
   `import std`；不支持 `import std` 的行用 cfg 回落到 C 入口；不写条件 import。
4. **发布矩阵**：`[pack] targets` 写 B 路线各行、Windows 的 A 路线与 `openkal`（okb）；SPEC-015 的目标集合不接受
   `macos-<env>`，所以 macOS 写完整三元组。

## 8. 验证工程的平台覆盖矩阵与自然获得的新能力

### 8.1 覆盖矩阵

| 验证工程列出的平台 | 默认 | 可选 | 所需的通用工作项 | 状态 |
|---|---|---|---|---|
| Linux | B（`*-linux-musl`）+ oke | A（gnu） | — | ✅ 现在可用 |
| Windows | A（windows-gnu，Win32 层，按 P4）+ oke | A（msvc，Windows CI）；B（windows-musl，POSIX 层） | E3；从 macOS 主机构建 A：E5、E9、X1；B 的程序查找：K11 | ✅ Linux 主机现在可用（资源除外） |
| macOS | B（macos-musl）+ oke | A（原生 SDK，macOS 主机） | —（风险见第 12.1 节） | ✅；程序需改取自身路径 |
| FreeBSD | B + oke | A（`<身份>-freebsd-sysroot`） | B：K8、E4、K9（K1）；A 的完善：E1、E5、E7、X2 | A 已实测；B 待 K8 |
| NetBSD | B + oke | A | openkal-netbsd（K10 之后可共用部分）、对应的行 | 待做 |
| OpenBSD、Haiku、GNU/Hurd | A + oke（okrun 跑在 openkal-posix 上） | — | E1、E5、E7、E14、X4、K2 | 待做 |
| PureDarwin | macOS 的 B 产物或 oke | — | — | 推断可行，未实测 |
| MSYS2 | Windows 的产物直接可用 | MSYS2 自身 ABI（msys-2.0.dll）作为独立行不在本方案中 | — | 视为由 Windows 覆盖 |
| System V（SVR4 i386） | 链接式安排：openkal-svr4 放在 i386 的 musl 下面；oke 需要 32 位映像 | — | K7、E11、E1（N7） | 最后做 |

### 8.2 自然获得的新能力

这些不在验证工程的平台清单里，通用设施落地后无需额外工作即可获得（标"实测"者已验证，其余为推断）：

| 能力 | 来源 |
|---|---|
| 一台主机产出 Linux、Windows、macOS 原生程序，含 `import std`（实测） | B 路线的内置行；Windows 的 A 路线（Linux 主机，实测） |
| aarch64 全覆盖：aarch64 的 Linux/macOS 原生程序，以及 oke/okb | B 路线与 okb 的现有架构 |
| aarch64 的 Windows 原生程序 | X1 含 `aarch64-w64-mingw32/`；还需要对应的行 |
| riscv64 Linux 原生程序 | 内置 `riscv64-linux-musl` 行 |
| 一份 oke 在所有有 okrun 的系统上运行，包括 OpenBSD 等第二类系统 | K1、K2 |
| 每个系统的单文件可执行程序（`mcpp pack --mode self-contained` 的载体） | openkal-platform 的载体机制 + K9 |
| Android/Termux：`linux-musl` 静态程序；或 A 路线的 android 行 | 推断，未实测 |
| RISC-V 裸机上运行（经 openkal-opensbi，无操作系统） | 推断，需裸机下的终端与进程语义评估 |
| 跨系统的 `mcpp test`：同一组测试在 wine、qemu-user、FreeBSD 虚拟机上运行（FreeBSD 已实测） | RP |
| openkal-conformance 自动按实现的接口选择检查项 | E13 |
| 所有 windows-musl 程序可以按名字启动 Windows 程序 | K11 |
| 其它工程同样受益：#913 的 GUI 工程在三种主机上产出 Windows PE | E5、E9、X1、I1 |

## 9. 阶段与依赖

| 阶段 | 内容 | 依赖 |
|---|---|---|
| P0 | 不需要任何上游改动：验证工程内部改动（第 7 节），在 9.1 上跑出第一份覆盖报告：linux-musl、macos-musl（构建；macOS CI 运行）、windows-gnu（Win32 层，wine）、oke/okb、FreeBSD（A，静态） | — |
| P1 | mcpp：E1、E2、E3、E6、E8、E10、**E13**、**E14**；openkal：**K1**（随 E13 发布）、**K11**、K6 | — |
| P2 | E4 → K8 → K9：FreeBSD 的 B 路线、okrun、oke 一次到位；K5 | P1 的 K1 |
| P3 | A 路线的基座包：E5 → X1 → E9 → I1（解决 #913）；X2；E7 | P1 的 E1 |
| P4 | K2 与 X4：第二类系统；openkal-netbsd 与 K10 | P1 的 E14、P3 |
| P5 | 32 位一类目标：E11 → K7（先 i686）；之后 SVR4 | P2 |
| 讨论 | K12（openkal-macos 经 libSystem） | — |

## 10. 缺省值与覆盖点（P8）

| 决定 | 缺省值（在哪里） | 显式覆盖（在哪里） |
|---|---|---|
| 本机构建的目标 | 本机的标准身份（mcpp 内置，不变） | `--target`、`[build] target` |
| 只写 `<arch>-<os>` 的请求选哪个身份 | 按提供者优先级取第一个可得者（内置数据，SPEC-016） | `[target.<sel>.priority]`、显式三元组 |
| 工程发布哪些产物（B 优先） | 由工程的 `[pack] targets` 声明（工程数据） | `mcpp pack --target …` |
| 某系统用 A 还是 B（P4） | 工程的 `[pack] targets` 中写哪个身份 | 命令行三元组 |
| 平台层（POSIX 或 Win32）等代码变体 | `cfg(c-abi = …)` 由目标推出（工程清单） | 工程自己的 feature（如需要） |
| 身份由哪个平台提供 | 目标表的平台列与提供者数据 | `--platform`、`[target.<sel>] platform` |
| 基座包的版本（X1、X2） | 内置提供者或平台包固定的版本 | `[target.<sel>] sysroot`、平台包版本 |
| openkal-run 绑定哪些接口 | 实现声明的 `provides-interfaces`（未声明则全部） | 换实现（`[defaults]` 或依赖图） |
| 接口集合的暴露（E13） | 只给声明了 `requires` 该层的包 | — |

## 11. 决定与推荐

### 11.1 已定稿（2026-11-10 评审）

| 编号 | 决定 |
|---|---|
| Q1 | K1 为构建期按解析结果选择（依赖 E13）；弱引用只作为实测记录，不作过渡 |
| Q2 | openkal-freebsd 独立成包；只共用机械部分，按"三次规则"在 netbsd 时抽取 K10 |
| Q3 | 默认 B 路线，A 路线可选；用目标机制表达，不用 feature（P3、第 10 节）。macOS 也默认 B |
| Q4 | E5：基座包——sysroot 是带清单的包，声明它供给的层；不修改 c-abi 独占律 |
| Q5 | X1 `mingw-w64-sysroot`（来源 llvm-mingw）、X2 `<身份>-freebsd-sysroot`（来源 base.txz），均按 P7 处理；X3 在第四版取消（std 模块源随基座生成） |
| Q6 | 两者有实测的使用差异，带两种平台层的程序在 Windows 上默认 Win32 层（A 路线），POSIX 层作为可选 |

### 11.2 O1–O4 的推荐

**O1 E13 的拼写——推荐：**

- 只暴露给**构建程序**，不新增 cfg 键（第 5 节：等出现第二个清单层面的使用者再加）。
- 环境变量 `MCPP_KERNEL_ABI_INTERFACES`，值为以 `;` 分隔的接口名（与 `MCPP_PKG_AUTHORS` 的列表写法一致），可以为空。
- 访问函数 `mcpp::kernel_abi_interfaces()`。实施时改为返回 `const char*`（原样的 `;` 列表），与同一头文件里其它访问
  函数的形式一致；拆分由使用者完成。
- **"未知"用空值表示（实施时的调整）。** 构建程序协议的约定是契约变量总是设置（缺失的变量在构建程序里无法与"引擎较旧"
  以外的原因区分），所以变量总是设置，空值表示未知：供给者未声明列表，或包没有声明该层的需求。较旧的引擎不设置该变量，
  访问函数同样得到空串，结果一致。"声明了但一个都不提供"不是有效的实现清单（openkal 的每个实现至少提供核心接口），
  不单独表示。
- 只给声明了 `requires` 该层的包（openkal-run 已声明），值计入构建程序的重跑键；宿主图与目标图各有自己的值。
- 名字里的 `kernel-abi` 是引擎持有的层名闭集中的一员，不是实现名，不违反 P1。目前只有 kernel-abi 有接口列表，所以
  不做按层名参数化的通用形式。

| 视角 | 评估 |
|---|---|
| 规范一致性 | 沿用已有的环境变量与访问函数约定 |
| 兼容性 | 旧引擎没有该变量，使用者按"未知"回落 |
| 简洁 | 一个变量、一个函数，没有新语法 |
| 扩展性 | 以后要加 cfg 键或其它层，是增加而不是修改 |

**O2 K10 的抽取边界——推荐：**

- **时机：** 等第三个类 Unix 内核层实现（netbsd）开始时再抽取。
- **抽取：** 只抽**与系统无关的机械部分**——句柄表与权限记录、名字的校验、errno 到 kal 错误码的映射框架（映射表本身
  留在各实现）、时间换算、termios 模式字的翻译、启动参数的组装。
- **不抽：**
  - 系统调用层；
  - 任何接口的语义判断（fs、process 的行为）；
  - 启动与线程局部存储。
- **形式：** 源码级的内部包，不声明 `provides`，不是 kernel-abi 供给者，也不面向消费者（与 openkal-kit 区分）。
  通过 openkal-platform 的版本组与各实现一起发布。
- 每个实现单独通过一致性测试。

| 视角 | 评估 |
|---|---|
| openkal 生态 | 保住"独立实现验证规范"的价值 |
| 稳定性 | 共用部分不含语义，改动不会让两个实现同时出同一个语义错误 |
| 简洁 | 不提前抽象，按实测的重复来抽 |

**O3 B 路线在 Windows 上找不到外部程序——推荐：** 已定位，是 openkal-musl 的通用问题，按 K11 修复。

- **实测：** windows-musl 程序拿到原生的 `PATH`（`C:\windows\system32;C:\windows;…`）；`posix_spawnp("hostname")` 与
  `posix_spawnp("hostname.exe")` 都失败，写全路径则成功。
- **依据：** openkal 规范第 30 条：`kal_env_system()` 存在的目的就是让程序遵循所在系统的约定，原文举例"程序的后缀、
  目录列表的分隔符"。
- **修法：** 在 C 库的程序查找里做，不在 openkal 接口里加新操作，也不要求程序处理。

| 视角 | 评估 |
|---|---|
| 规范一致性 | 正是第 30 条的用途 |
| 通用性 | 所有 windows-musl 程序受益 |
| 不硬编码 | 约定按 `kal_env_system()` 的回答选择，不按编译期宏 |
| 语义清晰 | 其它系统上 POSIX 语义不变 |

**O4 X1 的版本节奏——推荐：**

- **随 mcpp 发布固定版本：** 由内置提供者数据（E9）写明 X1 的版本，选择"LLVM 主版本与 mcpp 内置 llvm 相同"的
  llvm-mingw 发布。当前即 20261104（LLVM 23.1.5，对应 mcpp 的 llvm 23.1.3）。
- **同步升级：** mcpp 升级内置 llvm 主版本时，同时升级 X1，由 CI（wine 运行）验证。
- **覆盖：** 工程可用 `[target.<sel>] sysroot` 选别的版本（P8）。
- **X2：** 跟随系统的版本，由平台包固定；其 std 模块源跟随基座 libc++ 的版本。

| 视角 | 评估 |
|---|---|
| 稳定性 | 同一 mcpp 版本得到同一系统侧，可复现 |
| 一致性 | libc++ 头文件、std 模块与编译器主版本一致，避免第 3 节 FreeBSD 那种版本不符 |
| 缺省 + 可配置 | 缺省固定，可以覆盖 |
| 运维 | 每年数次随 llvm 升级，节奏可控 |

## 12. 两项风险的含义与处置

### 12.1 "Apple 不保证系统调用 ABI"

- **含义：** macOS 上稳定、受保证的接口是 libSystem 导出的函数（`open`、`read` 等）。它下面的内核调用约定（`svc #0x80`
  的调用号、参数与结构布局）是 Apple 的内部细节，可以随系统版本改变。Go 在 2018 年前后正是因此改为经 libSystem 调用。
- **现状：** openkal-macos 的系统调用层直接发 `svc #0x80`（`src/sys.h`，实测）。macos-musl 程序虽然动态链接了 libSystem，
  但 kal_* 的实现绕过了它。
- **后果：** 不需要重复实现任何东西。可能出现的情况是：某个新版 macOS 改了某个内核调用，**已经发布的** macos-musl 程序
  与 okrun 在该系统上出错，需要 openkal-macos 出新版本并重新构建、重新发布。
- **处置：**
  1. **检测**：openkal-macos 的一致性测试在 macOS 的新测试版上运行，提前发现变化（无需改代码）；
  2. **可选**：工程可以为 macOS 发布 A 路线产物（原生 SDK），P8 的覆盖点已具备；
  3. **讨论项 K12**：openkal-macos 的系统调用层改为调用 libSystem。kal 语义不变，只换最底层。Mach-O 有两级命名空间，
     程序里 musl 的 `open` 与 libSystem 的 `open` 本来就能共存；但同一个映像内静态链接时，引用会先解析到 musl 自己的
     定义，所以需要按 libSystem 显式查找（例如在启动时从 libSystem 取得函数地址）。这会削弱 openkal-macos README
     所说的"写在内核自己的调用上、用来检验规范"的初衷，是否采纳由维护者权衡。

### 12.2 "E13 发布前 K1 没法用主方案落地"

- **含义：** K1 的主方案要读 E13 提供的接口集合，所以 openkal-run 的那次改动要等某个包含 E13 的 mcpp 发布之后，并把
  openkal-run 的 `mcpp` 下限提到该版本。
- **不需要重复实现：** 第二版提到的"先用弱引用过渡"会让 openkal-run 先写一套弱引用代码、之后再删掉换成主方案，属于一次性
  工作。第三版不采纳：
  - E13 很小（60–120 行），排在 P1；
  - 真正需要 K1 的 FreeBSD B 路线（K8）规模大得多，排在 P2，届时 E13 早已发布；
  - E13 发布前，新系统的 okrun 仍可以像今天一样用补丁过渡，不影响 Linux、macOS、Windows。

## 13. 多视角深度自审

### 13.1 自审中发现并已修改的问题

| # | 视角 | 发现 | 修改 |
|---|---|---|---|
| 1 | 简洁 / 不过度设计 | E13 同时提供 cfg 键，但目前没有清单层面的使用者 | 只暴露给构建程序（第 5 节、O1） |
| 2 | 简洁 / 一致性 | E5 原设计需要 `${sysroot}` 新记号加一个独立层包 | 改为基座包：sysroot 本身带清单，复用"一个包供给多层" |
| 3 | 一致性 / 不硬编码 | FreeBSD 的 A 身份由 freebsd-platform 定义、B 身份由内置数据定义，两处来源 | 主流系统的 A、B 身份都放内置数据，平台包只做提供者（E4） |
| 4 | 语义清晰 / 稳定性 | 写在 C 库之上的实现可以被放到 openkal-musl 下面，运行时递归崩溃（实测） | 新增 E14：层依赖不得成环，并让相关包如实声明 `requires` |
| 5 | 规范一致性 | K6 的计数在回绕为 0 时违反"非 0 且永不清除" | 回绕时跳过 0 |
| 6 | 稳定性 | K1 中实现清单与 `kal_interfaces()` 不一致时可能调用空指针 | 取交集；一致性测试增加三方核对 |
| 7 | 通用性 | O3 曾被当作验证工程的问题 | 实测定位为 openkal-musl，新增 K11 |
| 8 | 缺省 + 可配置 | 缺省值与覆盖点散在各节 | 新增 P8 与第 10 节总表 |
| 9 | 工程节奏 | K1 的弱引用过渡会产生一次性代码 | 取消过渡（第 12.2 节） |

### 13.2 各视角的结论

| 视角 | 结论 |
|---|---|
| mcpp 规范 | E1 落实 SPEC-016 §2.1、§5；E5 只扩展 SPEC-002 §1.3、§2.3、§2.4 的写法，不改 §3.1 规则；E13 沿用构建程序协议的变量约定；E14 让 §3.2 规则二变成可检查的 |
| openkal 生态 | 规范层面只有 K5、K6、K7；K11 是规范第 30 条的落实；K1 只改 openkal-run；openkal-platform 不收任何系统 sysroot；独立实现的价值由 K8 独立成包与 O2 的边界保住 |
| 架构 | 身份（内置数据）、提供者（平台包）、基座（xim 包）、实现（openkal）各在一处，彼此通过数据引用 |
| 稳定性 | 所有新行为都有"缺失即旧行为"的回落（E13、K1 的未声明列表）；A 路线的基座版本随 mcpp 固定；macOS 的风险有检测手段与可选路线 |
| 通用性 | 每个上游工作项都写明了与验证工程无关的使用者（第 6 节"第二个使用者"一列） |
| 不硬编码 | 引擎里不新增系统名或实现名；E4、E9、E11 是数据；K11 按 `kal_env_system()` 的回答选择约定；K1 删除 openkal-run 里的系统名 |
| 语义清晰 | A/B 是身份、平台层是代码变体，二者的机制分开（P3）；"未知"与"空"在 E13 中有不同的表示 |
| 简洁 | 没有新语法；E5 不新增概念；E13 一个变量；被否定的形状都写明了原因（第 5 节） |
| 一致性 | 同类事物同一种表达：系统侧数据都是基座包；主流系统身份都在内置数据；列表变量都用 `;` |
| 跨平台 | B 路线任何宿主可编；X1 使 Windows A 路线在三种宿主可编；运行验证由运行器包覆盖 |
| 扩展性 | 新的内核 ABI 稳定系统：一个 openkal 实现 + 两行身份数据 + 一行 `[defaults]`；新的 libc-ABI 系统：一个基座包；长尾系统：平台 `[[platform.define]]`；新的 32 位架构：K7 之后是数据 |
| 缺省 + 可配置 | 见第 10 节，每个缺省都有显式覆盖 |
| 测试与验证 | 一致性测试（含 K1 的三方核对）、运行器包、各阶段的覆盖报告 |
| 供应链 | 基座包记录上游归档的 sha256 与官方校验文件，变换脚本随包 |

### 13.3 仍保留的风险

- macOS 的 B 产物依赖不受保证的内核调用约定（第 12.1 节）。
- B 产物是静态 musl 程序，不能加载系统动态库，没有 GUI；需要这些的程序按 P4 走 A。
- K8 的规模（4–6k 行）与维护承诺需要 openkal 维护者认可。
- K7（32 位）跨仓库多、需求小众，只作为设计讨论。

## 14. 证据（本地实测，mcpp 2026.11.9.1，Linux x86_64 主机，FreeBSD 14.5 虚拟机，wine）

- mcpp#911 及其复测评论：N1–N8 的复现与源码位置。
- K1 弱引用原型：openkal-image 0.5.4 上的改动；openkal-posix 去掉 net/datagram/prof/debug 的桩函数后仍能链接（28 个弱
  引用）；验证工程的 oke 输出不变；需要 openkal.net 的映像在加载时被拒绝；无 space 变体可运行 hello 与不 fork 的验证工程；
  默认实现 openkal-linux 下全部为强解析；FreeBSD 虚拟机中零补丁运行；弱引用在 ELF、COFF（gnu 与 msvc，clang + lld）
  上可链接。
- 一致性测试：openkal-conformance 0.10.0 对 openkal-posix（Linux）148 项通过、8 项不通过、4 项无法观察，之后卡住。
- 一主机多系统：C++ Modules + `import std` 的程序编出 windows-musl（wine 运行）、macos-musl（构建）、linux-musl
  （运行）与 oke；`i686-none-elf` 拒绝 `import std`；FreeBSD 系统 sysroot 行的 `import std` 失败（版本不符）。
- Windows A 路线：验证工程不改源码，`x86_64-windows-gnu` + `llvm@23.1.3`（mingw 载荷配对）在 Linux 主机构建，wine 下运行；
  `[resources]` 需关闭（E3）。A/B 的使用差异见第 7.1 节。
- windows-musl 的程序查找：`PATH` 为 Windows 原生格式；`posix_spawnp` 对 `hostname` 与 `hostname.exe` 均失败，全路径
  成功。
- E14：openkal-posix 作为 kernel-abi 放在 openkal-musl 下面（x86_64-openkal）可以构建，运行时在 `kal_time_wall` 与
  `clock_gettime` 之间无限递归崩溃；openkal-musl 只声明了 `requires mcpp:compiler-runtime`。
- openkal-macos 的 `src/sys.h` 直接发 `svc #0x80`（x86_64 为 `syscall`）；macos-musl 产物动态链接
  `/usr/lib/libSystem.B.dylib`。
- openkal-linux 共 8,680 行，几乎不包含系统头文件（自有系统调用层 719 行）。
- FreeBSD sysroot：完整解压 458 MB；核心部分 xz 压缩 9.4 MB。
- 9.1 内置的 mingw 提供者：`hosts = ["windows-*", "linux-x86_64"]`，`pairs = ["gcc", "llvm"]`。
- mcpp 没有把 `provides-interfaces` 暴露给 cfg 或构建程序；openkal-conformance 以手写 feature 选择接口。
- llvm-mingw 20261104：基于 LLVM 23.1.5；发布物有 macOS、Linux、Windows 三种宿主版本；`build-libcxx.sh` 开启
  `LIBCXX_INSTALL_MODULES`。
- openkal 规范第 3 节接口清单不含窗口与图形；第 2 条允许实现建立在 C 库之上（openkal-emscripten）；第 30 条说明
  `kal_env_system()` 用于遵循系统约定（程序后缀、目录列表分隔符）。

## 15. 实施记录（mcpp 2026.11.10.1）

| 项 | 实施 | 验证 |
|---|---|---|
| E1 | 平台定义的行，以及没有内置提供者的内置行，由同一谓词读取：机器描述进入编译与 ld.lld 链接；payload 为其宿主构建的目标侧不进入；本机加载器的检查只用于在本机运行的行 | e2e 1057；FreeBSD 虚拟机运行 |
| E2 | 共用入口的程序按各自的参数分别编译入口（`obj/_entry/<名>/`） | e2e 1054 |
| E3 | GNU ABI 的 Windows 行在配对载荷中找 windres，或用 `llvm-rc`；带引号的工具链头文件不当作生成输入 | e2e 1058 |
| E4 | FreeBSD 两种身份为内置数据；行的 `clang-triple` 为数据；行的系统词是语言的词 | 单元测试（三元组词汇） |
| E5 | 基座包（本文第 6.1 节定稿） | e2e 1059、1063；FreeBSD 虚拟机：`import std`、异常、动态与静态 |
| E6 | 给 git 工作区成员打补丁时复制整个仓库 | e2e 1060 |
| E7 | 另一系统的产物从 `DT_NEEDED` 读闭包；`--mode static` 链接并核对静态程序 | e2e 1061 |
| E8 | `driver = "compiler"` 时 `[platform.link] flags` 给出警告 | e2e 1062 |
| E9 | `mingw-llvm` 提供者与 `aarch64-windows-gnu` 行；提供者的 sysroot 包是基座陈述的最后一级；未服务目标的诊断写出可被解析的拼写 | e2e 1063（wine 运行） |
| E10 | 文件存在但本次不参与构建的 `scan_overrides` 不再报错 | e2e 1064 |
| E11 | 32 位 hosted 行为 planned | 单元测试 |
| E12 | 只记录 | — |
| E13 | `MCPP_KERNEL_ABI_INTERFACES` / `mcpp::kernel_abi_interfaces()` | e2e 1056 |
| E14 | 层供给者的需求不成环（`layer-cycle`） | e2e 1055、单元测试 |
| 规范 | SPEC-002 v1.6、SPEC-006 v0.11、SPEC-013 v1.7、SPEC-014 v2.2、SPEC-016 v1.1；docs 04、10、21、22、30、35、50（中英文） | 文档检查 |
| X1、X2 | xlings-res 的 GitHub 发布与 GitCode 镜像（下载回验 sha256）；xim-pkgindex 收录，CI 在三种宿主上安装、检查、卸载；X1 的修订版 `20261104-2` 以 `mcpp:c-abi=ucrt` 陈述其 C 库，基座的 C 库按其陈述的名字报告 | xim-pkgindex CI；e2e 1063 |
| I1 | compat.glfw 的 Windows 行要求 `mcpp:c-abi=ucrt` | gnu 行构建、windows-musl 行在解析期拒绝（实测） |
| CI 中暴露并修复 | 提供者的基座在依赖图载入前被记住（改为图载入后才保留）；没有 cfg 的 Windows llvm 载荷编译时不陈述配对的 sysroot；同一 home 的两个进程并发安装同一个包（安装次序锁 `registry/.install.lock`） | target-matrix、openkal 交叉构建、各宿主 e2e |
| 实施中发现并修复 | 工具链为另一目标构建的 C++ 归档（其父目录的三元组与目标的架构或系统不同）不链接，并给出降级诊断；ok 映像的启动器依赖 mcpplibs.os 0.4.4（openkal 0.24 与 0.25 两族均可） | 单元测试 `Distribution/ForeignArchivesAreNotLinked`；mingw 交叉打包 |

openkal 家族（K5、K6，openkal 0.25.0）、openkal-freebsd 0.1.0（hosted 与 standalone 两种形态，x86_64 与 arm64）、
openkal-musl 0.29.1 与 openkal-llvm-runtime 0.20.1（FreeBSD 的 standalone C 与 C++ 运行时）以及 mcpplibs/os 的实施记录
在各自仓库的 PR 与 CHANGELOG 中。测试与依赖解析的实施记录见补充方案第 9 节。

## 16. 测试体系向 mtest 的迁移

- **顺序。** 每个仓库先以原有测试集验证功能改动（CI 记录运行号），再追加迁移到 mtest 的提交，以新测试集再次验证；
  PR 描述同时引用两次运行。迁移保留每一条断言，不能迁移的测试在 PR 中点名。
- **mcpp 自身。** e2e 已经经 `tests/e2e/mtest.toml` 作为 mtest 的 level 0 程序运行。功能提交在 CI 通过后：
  1. 新增的 e2e（1054–1064）与其余脚本一样由 mtest 发现与报告；
  2. bench 的引擎比较仍是手动运行的标准集；mcpp 自身的开销（无事构建、`why flags`、改一个接口后的重建）
     是 `bench/tests/engine.cpp` 的 mtest Bench 用例，由 Linux 单元测试 job 运行，指标进入 mtest 的存储；
  3. CI 以 mtest 优化：按存储中历史耗时分片（`--shard`）、按失败历史排序、每个 job 写 `mtest report --format md` 到摘要、
     一次 `mtest ci report` 汇总全部 job。
  4. CI 的组合 action：`mtest`（按 `.github/versions.env` 的 `MTEST_VERSION` 安装并导出 `MTEST`）、
     `e2e-mtest`（e2e 的存储与分片）、`test-report`（把一个 job 中 `mcpp test` 的存储合并、写摘要、上传）；
     ci.yml 的 `test-report` job 以运行号为组生成整个工作流运行的报告。
- **openkal 家族。** 实现的测试改写为 mtest 的 UTest；一致性套件以 mtest 的测试协议报告；CI 用组合 action `mtest` 与
  `test-report`，`MTEST_VERSION` 固定在 `.github/versions.env`。
- **宿主依赖（P9）。** mtest、wine、qemu 与虚拟机镜像都取自 xpkg。


## 17. 补充方案

测试与依赖解析的补充决定（T1–T18：被替换版本撤回请求、根上的 `override`、测试图与快路径、构建锁、失败的呈现、mcpp test spec 的基础与可选能力、会话、mcpp-ide 的对接）与两份方案合并的全局自审，见
[2026-11-10-test-and-resolution-supplement-design.md](2026-11-10-test-and-resolution-supplement-design.md)。
