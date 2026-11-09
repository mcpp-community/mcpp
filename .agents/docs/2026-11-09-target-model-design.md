---
subject: target-model
status: active
---

# 目标与提供者：统一的目标模型（结合 mcpp#911）

**日期：** 2026-11-09
**状态：** 定稿（2026-11-09）：Q1–Q7、R1–R8、第 15 节与第 17 节 T1–T6 已确认（2026-11-09）；第 16 节为综合自审，第 18 节为任务与依赖
**范围：** mcpp（目标的身份、行、提供者与选择；新 SPEC-016 及 SPEC-002、SPEC-013、SPEC-015 的修订）；mcpp-index 描述符的
匹配；openkal-image（openkal-run 的能力槽）
**输入：** 2026-11-09 关于交叉构建的讨论；mcpp#911（移植 ChenPi11/cmd 到 ok 映像、FreeBSD 与 i386 时测得的 G1–G12 与 B1–B3）

## 1. 问题

SPEC-013 §0 规定引擎不持有任何平台的知识，§14 又规定宿主目标的约定留在引擎中、无平台包。两种体系因此并存：openkal 的目标由
包定义，带版本、来源与覆盖；原生目标的事实写在引擎中：封闭的目标词汇（`triple.cppm`）、每行注入的参数
（`freestanding/target.cppm`）、以代码判定的 hosted（`-ffreestanding`）、固定的链接方式、以本机为中心的运行时校验、封闭的描述
符键。提供各层的来源与在来源之间的选择，散在三元组补全、目标表的 pin、平台列与 `toolchain_decision` 的拒绝中，彼此以代理条件
推断。#911 的 G1–G6、G10，与 `--target x86_64-windows --toolchain llvm` 被拒绝，都由此而来。

## 2. 原则

**机制在引擎，事实在数据。** 引擎持有：

- 轴：arch、os、env、对象格式、数据模型、C 环境（hosted 或 freestanding）、执行宿主；
- 层名的闭集（SPEC-002 §1.1）；
- 编译器族的驱动知识（SPEC-002 §1.2：编译器不由包提供）；
- 协议：合并、来源、校验、打包、运行器、选择规则。

哪些系统存在、某一行用何种代码模型、产物由哪个加载器启动、谁提供 C 库，皆为数据。原生约定与 openkal 平台以同一形式表达。

## 3. 概念

| 概念 | 定义 |
|---|---|
| 请求 | 用户写出的 `--target`、`[build] target` 或 `[target.X]` 的键：`arch-os` 或 `arch-os-env`，是一个模式 |
| 身份 | 产物是什么：由选择得到的完整三元组。决定输出目录、缓存键、`cfg(...)` 与 ABI 标签 |
| 行 | 一个身份的定义（第 4 节） |
| 提供者 | 一个身份的一份完整供给：工具链、各层的来源、链接模型、C 环境、执行环境、默认参数（第 5 节） |
| 选择 | 由请求得到身份与提供者的唯一规则（第 6 节） |

## 4. 身份与行

- **I1. env 即 C 库。** 一个系统可能用到两种以上 C 库时，身份含 env。
- **I2. 系统的原生身份沿用业界写法。** 业界原生标识无 env 段的系统（macOS、BSD），其原生身份为两段（`aarch64-macos`、
  `x86_64-freebsd`）；其余 C 库以 env 区分（`aarch64-macos-musl`）。`<arch>-openkal` 为与系统无关的映像身份，两段。
  其余系统的身份为三段（`x86_64-linux-gnu`、`x86_64-windows-msvc`）。
- **I3. 行是数据。** 行的字段：`triple`、`arch`、`os`、`env`、`standard`（是否为业界标准身份）、`clang-triple`、
  `object-format`、`data-model`、`exe-suffix`、`loader`、`code-model`、`link-emulation`、`execution`。内置行随 mcpp 发布；
  平台包以 `[[platform.define]]` 定义新行，同一 schema（G1）。解析器接受任何已解析平台定义的 os 与 env；`cfg(os = …)` 按行
  求值；32 位 x86 的内置行 `i686-none-elf`（空代码模型，`elf_i386`）随内置数据提供（G12）。
- **I4. 身份与宿主无关。** 同一身份在任何宿主上是同一输出目录、缓存键与 `cfg`。
- **I5. 身份如实。** 身份为选择的结果，含实际 C 库的 env；产物格式随目标系统：身份为 `<arch>-openkal` 时为 ok 映像，否则为
  该系统的原生格式（ELF、PE、Mach-O），即使各层由 openkal 平台提供。

## 5. 提供者

提供者以 SPEC-013 §1 的 `[platform]` schema 表达，增加：

| 键 | 语义 | 对应 |
|---|---|---|
| `serves` | 所服务的身份 | 取代目标表的 pin 与 `hosted.targets` |
| `priority` | 整数，越大越先；同一请求的候选按此排序（Q6） | 第 6 节的顺序 |
| `hosted` | C 环境是否 hosted；sysroot 或 c-abi 提供者亦可声明 `hosted-standard-library` | G3 |
| `[platform.link] driver` | 在每一行生效；`"compiler"` 交由编译器驱动加入启动文件与库 | G4 |
| `execution` | 产物的执行环境（系统、加载器） | G5、G11 |
| 默认参数 | 行的机器描述不可替换；机器不同的目标是另一行（G1、G12）；每个参数的来源由 `mcpp why flags` 写出 | G2 |
| `pairs` | 可与之配对的编译器族（如 mingw 提供者：gcc、llvm） | 第 7 节 |

**内置提供者。** 原生约定改写为随引擎发布的提供者：glibc payload（`*-linux-gnu`，native）、musl-gcc（`*-linux-musl`，
native）、Apple SDK（`*-macos`，native）、MSVC（`*-windows-msvc`，native）、mingw（`*-windows-gnu`，convention）、Android NDK、
Emscripten SDK。openkal-platform 的 hosted 部分为提供者，服务 `*-linux-musl`、`*-windows-musl`、
`*-macos-musl`（待验证，第 11 节）。

**存放。** 内置行与提供者为 mcpp 源码树中的数据文件，由构建程序（`build.mcpp`）编入引擎，不提交生成物；运行时不联网、
不进入依赖解析，「每个 mcpp 固定一份」的可复现性不变。

## 6. 选择

1. **显式者优先。** `--platform`、`MCPP_TARGET_<T>_PLATFORM`、项目与 config 的 `platform`、显式工具链；依赖图提供的层亦属显式
   （层报告为 `graph`）。`--platform none` 只用 `native` 与 `convention` 提供者，不用平台包。
2. **请求是模式。** 写出 env 的请求只匹配该身份；只写 arch-os 的请求匹配该系统的全部身份。I2 中原生身份为两段的系统，请求
   `aarch64-macos` 同时是该身份的写法与模式：候选的第一个是原生身份。
3. **顺序是数据。** 每个提供者带一个整数 `priority`，同一请求的候选按它从大到小排序，相同者按行的顺序、再按提供者名。
   内置数据给出默认值：各系统的标准工具链与 C 库（glibc、musl-gcc、Apple SDK、MSVC、BSD 的系统 libc）较高，跨系统平台
   （openkal-platform）居中，其他约定（mingw）较低。引擎不含类别，也不含任何实现名（SPEC-002 §1.1）；所有提供者同一
   结构，只有数值不同。平台包为自己的提供者声明 `priority`；项目与 config 可按选择器改写：
   `[target.<selector>.priority] <provider> = <n>`，来源与其他值一样记录。优先级的调整（如 openkal-glibc 就绪后的
   `*-linux-gnu`）是一次数据改动，附 CHANGELOG。
4. **取第一个可服务者。** 提供者服务该身份，且其 payload 或包在本宿主可得；工具链族被显式指定时，只取与该族配对的提供者
   （`pairs`）。
5. **报告。** 状态行沿用现有格式：`Target <写法> → <身份>`，其下为各层的来源；发生回落（第一个候选在本宿主不可服务）时，`Target`
   行附一句原因（`<提供者> is not available here`）。
6. **皆不可服务时拒绝。** 诊断写出该身份所需、每个候选提供者不可用的原因、可直接执行的命令。
7. **原生构建**（不写 `--target`）取宿主的 `native` 提供者，结果与现在相同。

| 请求 | 候选（依次） | 典型结果 |
|---|---|---|
| `x86_64-linux` | `-linux-gnu`：glibc payload；`-linux-musl`：musl-gcc、openkal-musl | Linux：`-gnu`；无 glibc 与 musl-gcc payload 的宿主：`-musl`（openkal） |
| `x86_64-linux-musl` | musl-gcc、openkal-musl | Linux：musl-gcc；macOS：openkal |
| `x86_64-windows` | `-windows-msvc`：MSVC；`-windows-musl`：openkal；`-windows-gnu`：mingw | 有 MSVC 的 Windows 宿主：`-windows-msvc`；其余：`-windows-musl`（Q4） |
| `x86_64-windows-gnu` | mingw（gcc 或 llvm 配对） | mingw |
| `aarch64-macos` | `aarch64-macos`：Apple SDK；`aarch64-macos-musl`：openkal | macOS：原生；其他宿主：`-musl`（openkal） |
| `<arch>-openkal` | openkal-platform | ok 映像 |

## 7. llvm 与 mingw payload

本机实测（2026-11-09，clang 23.1.3，`xim-x-mingw-cross-gcc/16.1.0`）：C++ 的静态程序与 `import std`（clang 预编译 libstdc++ 的
`bits/std.cc`，`std::println`）均在 wine 下正常运行；导入表为 `KERNEL32.dll`、`msvcrt.dll`，与 mingw gcc 产物同一 ABI。参数：
`--no-default-config --target=x86_64-w64-mingw32 --sysroot=<mingw payload> -stdlib=libstdc++ -rtlib=libgcc -fuse-ld=lld`；静态
链接另加 `-pthread`（该 mingw 为 posix 线程模型，clang 驱动不自动加入 winpthread）；`std::print` 的终端函数在
`libstdc++exp.a`，与 gcc 一行的处理一致。

## 8. 与 #911 其余各项及分发的衔接

| 项 | 处置 |
|---|---|
| G6 描述符 | 源码制品一份、与目标无关；预编译 payload 按身份的轴匹配，含 `any` 与回落；`linux`、`macosx`、`windows`、`openkal`、`none` 块作为轴组合的别名读取，已有索引条目不变；`descriptor_keys` 照旧可读，不再必需 |
| G7 能力槽 | SPEC-002 的层机制推广到普通包：`requires` 加按系统的默认提供者；解析顺序为依赖图 > 平台 > 默认值。openkal-run 的清单随之改写 |
| G8 补丁根 | 包根（解析后的清单所在目录）或显式 `patch_root`；`mcpp why deps` 报告包根，补丁由 `git diff` 写出 |
| G9 按产物的编译变体 | `[targets.X.build] defines/cflags/cxxflags` 作用于该产物链接的本包翻译单元，在 `obj/_variant/<X>/` 编译变体；`[targets.X] defines/flags` 维持只作用于入口；模块接口每包一个，不做变体 |
| G10 载体与打包 | 任何已解析平台能构建的行，按平台声明作载体或打包的系统 |
| G11 执行宿主 | 运行器协议：传输、运行、输出流、退出码，由 runner 包实现（VM、ssh、设备）；无运行器的异系统产物为「已构建，未运行」 |
| B1 | 生成的清单以 TOML 引号书写含 `.` 的目标名 |
| B2 | 平台规定的产物后缀不被宿主后缀规则覆盖；冲突在规划期报告 |
| B3 | 多个程序可打包时照旧打包（以包命名者，否则按名字的第一个）并说明其余程序与两种选择写法（`mcpp pack <program>`、`--all-programs`） |
| 构建插件（SPEC-007） | SPEC-013 §1.2 的分工不变：行、提供者、层只能声明（解析工具链与依赖图时尚无程序可运行）；`build`、`link`、`runner` 可由插件陈述，来源记录；构建程序的 `artifact-name` 不改变平台的产物格式 |

## 9. 多视角自审

| 视角 | 发现 | 处置 |
|---|---|---|
| 一致性 | 第四版把「类别」同时放在行与提供者上，含义不清 | 行只标 `standard`（是否业界标准身份）；排序只看提供者的 `priority`（Q6） |
| 一致性 | 「只有 openkal 是两段」与 macOS 原生无 env 并存 | I2：业界原生标识无 env 的系统，原生身份为两段；其余 C 库以 env 区分 |
| 一致性 | 依赖图提供 musl 而请求为 `x86_64-linux` 时，现有身份为 `-linux-gnu`，报告却写 `c-abi musl`（target_side.cpp 已记录此矛盾） | I5：身份取实际 C 库，为 `-linux-musl`。这是第二处默认行为变化（第 10 节），需确认（Q5） |
| 一致性 | 第四版把 MSVC 列为 Windows 的原生标准却未进入候选 | 候选按类别排序后 MSVC 在首位；无 MSVC 的宿主（Linux、macOS）自然落到 openkal，Windows 宿主的取向由 Q4 决定 |
| 兼容性 | 原生约定由代码移为数据，任何偏差都会改变产物 | 等价性测试：迁移前由现有代码表生成每行的编译与链接参数快照，迁移后由数据生成并逐行比对，二者相同才删除旧代码 |
| 兼容性 | 新清单键（`[[platform.define]]`、`serves`、`priority`、`execution`、`pairs`）在旧 mcpp 上 | 旧 mcpp 对未知键警告（SPEC-013 §1.1）；使用新键的平台包声明 `[package] mcpp` 下限 |
| 可复现性 | 同一模糊请求在不同宿主可能解析为不同身份 | 身份与宿主无关（I4）；需要确定 ABI 的场合写完整身份；构建报告与机器输出记录「请求 → 身份」；锁按身份记录目标（与 #903 一致） |
| 缓存与 ABI | `-linux-musl` 可由 musl-gcc 或 openkal-musl 提供；二者的预编译产物是否互换 | 缓存键已含工具链与平台的指纹，二者不共享缓存；索引中按身份匹配的预编译 payload 要求 C 层 ABI 兼容，openkal-musl 与 musl-gcc 的 musl 版本差异列为验证项 |
| 跨平台 | 从 macOS 构建 `-linux-gnu` 需要 glibc payload 能装在 macOS 宿主上 | glibc payload 是目标数据，属 G6 的按身份匹配；在其可得之前，该身份在 macOS 宿主上不可服务，请求 `x86_64-linux` 回落到 `-musl` |
| 跨平台 | macOS 无 SDK 的链接（dyld、libSystem 的存根） | 列为验证项（第 11 节）；验证前 `aarch64-macos-musl` 在非 macOS 宿主上标为 preview |
| 安全 | 平台包可以定义行 | 平台包本就能注入参数；定义的行不能覆盖内置行；`mcpp why flags` 写出每个参数的来源层 |
| 性能 | 选择不得拉长构建的准备阶段 | 内置行与提供者由构建程序编为引擎中的静态表，运行时不解析任何文件；平台包只在被选中或已解析时读取（与现在相同）；候选按优先级依次检查可得性（本地 store 的存在性），取到第一个即停；结果在一次构建内缓存；`--timings` 报告选择所用时间，e2e 断言其上界 |
| 用户体验 | 新增提示可能冗余 | 只在发生回落时附一句原因；其余沿用现有的「写法 → 身份」与层来源行 |
| 诊断 | 拒绝时用户不知道下一步 | 第 6 节第 6 条：所需、各候选的原因、可执行的命令 |

## 10. 行为变化

1. 只写 arch-os 的 Windows 请求：在无 MSVC 的宿主上由 `-windows-gnu`（mingw）改为 `-windows-musl`（openkal）。输出目录与
   `cfg(env)` 随之改变；CHANGELOG 记载，简写解析时提示一次。
2. 依赖图提供的 C 库与请求的默认 env 不同时，身份取实际 C 库（Q5）：例如依赖 openkal-musl 的项目以 `x86_64-linux` 构建，身份由
   `x86_64-linux-gnu` 改为 `x86_64-linux-musl`，输出目录随之改变。

其余：现有身份字符串、输出目录与 `cfg` 结果不变；`[target.X]`、`descriptor_keys`、`platform`、`[platform.hosted]` 的写法照旧
可读，`hosted.targets` 视为 `serves` 的别名。

## 11. 验证项

- `aarch64-macos-musl` 在非 macOS 宿主上的链接与运行（无 SDK）；
- openkal-musl 与 musl-gcc 的 musl 在 C 层 ABI 上的兼容范围（预编译 payload 是否可互用）；
- FreeBSD 平台定义的行：静态与动态链接、运行时校验、`cfg(os = "freebsd")`；
- i686 行：空代码模型与 `elf_i386`。

## 12. 决定

| 编号 | 事项 | 状态 |
|---|---|---|
| Q1 | 所有目标由提供者服务，原生约定改写为内置提供者 | 已确认 |
| Q2 | 新 SPEC-016「目标与提供者」；SPEC-013 §0、§3、§14，SPEC-002 §3.4、§3.5，SPEC-015 §1.2 改为引用之；docs/21、22、35 同步 | 已确认 |
| Q3 | mcpp 一个 release PR 实现第 4–8 节与第 9 节的处置；openkal-image（openkal-run 的能力槽）一个 PR，依赖 mcpp 的发布 | 已确认 |
| Q4 | 宿主有原生提供者时取原生；Windows 宿主有 MSVC 时只写 arch-os 的 Windows 请求取 MSVC，否则取 openkal 平台（优先级次之）。即第 6 节规则的直接结果，无 Windows 专门规则 | 已确认 |
| Q5 | 身份取实际 C 库（第 10 节第 2 条）。理由：预编译 payload 按身份的轴匹配（G6），身份不如实即取错预编译库；同一系统的 C 库增多（openkal-glibc、BSD）后 `cfg(env)` 与 ABI 标签须如实；现有报告的不一致由此消除 | 已确认 |
| Q6 | 提供者在结构上统一，只以整数 `priority` 排序；默认值是数据，平台包、项目与 config 可以改写；引擎不含类别与实现名 | 已确认 |
| Q7 | #906（2026.11.8.1）先合入并发布；本设计为其后的版本，单独一个 PR | 已确认；2211fb5c 合入，release 已触发 |

## 13. 实现与测试

| 部分 | 位置 |
|---|---|
| 行与内置提供者的数据、编入引擎的构建程序 | 新数据目录；`build.mcpp` |
| 三元组：开放的 os 与 env，按行求值 | `modules/toolchain-model/src/triple.cppm` |
| 选择 | `prepare/toolchain.cpp`（`step1_resolve_target_triple_request` 起）、`prepare/platform.cpp`、`platform.cppm` |
| 提供者的编译与链接 | `toolchain/hostflags`、`toolchain/linkmodel`、`build/flags.cppm`、`toolchain/stdmod.cppm`、`freestanding/*` |
| hosted、`driver = "compiler"`、执行环境与运行时校验 | `freestanding/target.cppm`、`build/ninja_backend.cppm`、`build/runtime_validation.cppm` |
| 描述符的轴匹配、补丁根、按产物的变体、载体与打包、运行器协议 | `pm/*`、`pm/patch.cppm`、`build/plan.cppm`、`pack/*`、`run/*` |
| 诊断与报告 | `prepare/toolchain_decision.cpp`、`ui`、机器输出、`mcpp why target` |
| 统一的包引用与 git 的缺省引用 | 依赖的引用解析器（`pm/*`）、平台与运行器的引用读取、命令行与环境变量的字符串形式 |
| B1–B3 | `pack/pack.cppm`、`pack/manifest_emit.cppm`、`pack/distribution.cppm`、`pack/pipeline.cppm` |
| 规范与文档 | SPEC-016（新）；SPEC-001、SPEC-002、SPEC-013、SPEC-015 的修订；docs/05、20、21、22、30、35（中英） |
| 测试 | 等价性测试（第 9 节）；e2e：只写 arch-os 的交叉构建（payload 可得与不可得）、`-windows-gnu` 配 llvm（含 `import std`，wine 运行）、显式身份不被替换、原生构建不变、显式平台与依赖图提供者、平台定义的 FreeBSD 行、i686 行、hosted 声明、`driver = "compiler"`、异系统产物的运行时校验、能力槽、补丁根、按产物的变体、B1–B3、拒绝时的诊断；引用：命令行的 `git+` 引用（缺省、`tag`、`branch`）、`path` 引用、`[indices]` 中未进索引的描述符、清单引用入锁与命令行引用不入锁、`--locked` 拒绝未锁定的分支、离线时的行为 |

## 14. 使用者视角的自审

| 视角 | 关心 | 发现 | 处置 |
|---|---|---|---|
| 应用开发者 | 写一个 target 就能构建；结果可预期 | 同一模糊请求在不同宿主可能得到不同身份，团队协作时不易察觉 | R1：锁按请求记录解析出的身份；另一宿主解析不同时写出一行说明（请求、锁中的身份、本次的身份），`--locked` 下拒绝 |
| 应用开发者 | 出错时知道怎么办 | 拒绝的诊断已有原则（第 6 节第 6 条），但没有「先问后建」的入口 | R2：`mcpp why target [<请求>]`（与现有的 `why toolchain/runtime/deps` 同族）列出候选、各自的优先级与可得性、选中者与原因；`--format json` |
| 应用开发者 | 不必学新概念 | 「行」「提供者」「优先级」是实现与平台作者的概念 | R3：用户文档只用「请求的 target」「构建的 target」「提供者」三个词；「行」「priority」只出现在 SPEC-016 与平台作者文档 |
| 应用开发者、平台开发者 | 指定用谁 | 「选一个提供者」现在有 `platform`、`toolchain`、`--platform none` 三种写法，语义交叠 | R4：内置提供者有名字（如 `builtin:msvc`、`builtin:mingw`、`builtin:glibc`）；`platform` 选择一个提供者，名字或包引用皆可；`--platform none` 表示不用平台包、只用内置提供者，含义不变；`toolchain` 只选编译器族与版本，与提供者按 `pairs` 配对。不新增键或标志 |
| 库开发者 | 条件依赖与预编译 payload 按 target 匹配 | Q5 使依赖图供给 C 库的项目身份改变，以 `cfg(env = "gnu")` 写的条件可能不再成立 | R5：CHANGELOG 与 docs/22 写出迁移：按 C 库写的条件应以实际 C 库为准；`mcpp why target` 显示 `cfg` 求值所用的身份 |
| 平台开发者 | 用最少的键写出一个平台 | `[platform]` 与 `[platform.hosted]` 是两个特例的提供者，第三个提供者没有写法 | R6：一般形式 `[[platform.provider]]`（`serves`、`priority`、`toolchain`、`layers`、`link`、`hosted`、`execution`、`pairs`）；`[platform]` 与 `[platform.hosted]` 为其两种简写，照旧可读。`mcpp new --template platform` 生成一个最小平台；读取平台清单时校验行与提供者的一致与 `priority` 冲突；`mcpp why platform` 列出它服务的身份 |
| 构建插件开发者 | 构建程序读到的 target 与引擎一致 | `mcpp::target_os()` 等访问器的文档以封闭集合描述取值；新系统的 os 值须照样流过 | R7：访问器返回身份的轴（字符串，取值由行定义）；新增 `mcpp::target_provider()`（`MCPP_TARGET_PROVIDER`）；docs/30 的取值表改为「由行定义，内置行为……」 |
| agent | 可脚本化、确定、可解释 | 构建的机器输出只有身份，没有请求与提供者；拒绝没有稳定的代码 | R8：机器输出的 target 记录为 `{request, identity, provider, priority, fallback}`；选择的拒绝有稳定代码（`target-unserved`、`target-ambiguous` 等，进 SPEC-016 的拒绝码表）；文档建议自动化写完整身份 |
| mcpp 开发者 | 迁移不引入回归，代码不再散落 | 原生约定由代码移为数据是一次大改 | 第 9 节的等价性快照为合入条件；选择逻辑集中于一个模块，旧的代理条件（目标表 pin、`toolchain_decision` 的拒绝、三元组补全中的推断）随迁移删除，不保留双轨 |

**简洁性。** 概念：请求、身份、提供者（用户文档三个词）；数据：行、提供者（同一 schema）；规则：一条（显式者优先，其余按 priority 取第一个可服务者）。新增的用户面：`mcpp why target`、`[target.<selector>.priority]`、内置提供者的名字；其余为已有写法的推广。

## 15. 统一的包引用

凡是「指名一个要取得的东西」的位置，使用依赖的同一套引用文法与解析机制（SPEC-001）：

| 位置 | 现在 | 统一后 |
|---|---|---|
| `[dependencies]` | 版本、`path`、`git`（SPEC-001） | 不变，为文法的来源 |
| `platform`（含 `--platform`、环境变量） | 包引用（字符串或表，含 `path`、`git`）；命令行 `path:<dir>` | 同一文法；增加 `builtin:<name>`（R4）与命令行的 git 形式 |
| `[platform.layers]`、`tools`、`carrier` | 包引用 | 不变 |
| 运行器包（G11） | 无 | 同一文法 |
| `toolchain`（含 `--toolchain`） | `<族>@<版本>`、`xim:<包>@<版本>`、`path:<dir>` | 不变（编译器不由包提供，SPEC-002 §1.2）；`<族>@<版本>` 为 `xim:` 的简写 |

**文法。** TOML 中为字符串或表（`name`、`version`、`path`、`git` 与 `tag`/`rev`/`branch`、`features`），即现有的包引用；命令行与环境变量
为字符串：`[<ns>.]<name>@<约束>`、`path:<dir>`（或以 `/`、`./`、`../`、`~`、盘符开头的路径）、
`git+<url>[#tag=<t>|rev=<r>|branch=<b>]`、`builtin:<name>`。`git+` 之后接 `https://`、`ssh://` 或 scp 形式
（`git@host:owner/repo`）的地址。

**缺省的引用。** git 引用不写 `tag`、`rev`、`branch` 时，取远端的默认分支：由 `git ls-remote --symref <url> HEAD` 得到其名字（`main`、
`master` 或其他），不在引擎中写死分支名；远端不通告默认分支时，依次尝试 `main` 与 `master`，并在报告中写出所用的分支。缺省引用
按 `branch` 处理（锁记录解析得到的提交）。`[dependencies]` 的 git 依赖同样适用：SPEC-001 由「`tag`、`rev`、`branch` 恰好一个」改为
「至多一个，省略即默认分支」；原先被拒绝的写法由此成为合法，已有清单不受影响。

**未分发的仓库。** 符合 mcpp-index 分发模型、含 `mcpp.toml` 的仓库或目录，以 `path`、`git` 引用，身份取其清单（SPEC-001 §5.4），
与索引中的包同样解析、获取与锁定。只有 xpkg 描述符（`.lua`）而未进索引的，以项目的 `[indices]`（`path` 或 `git`）声明为一个索引，
其中的包以普通的 `<ns>.<name>@<版本>` 引用。不新增第三种来源。

**锁。** 清单中写出的引用记入 `mcpp.lock`（git 记录解析得到的提交）；只在命令行或环境变量中出现的引用是一次构建的选择，不改写锁，
记入构建报告与机器输出（R8）。

**跨平台。** 路径按 SPEC-013 §3.2 的规则识别（含 Windows 盘符）；git 取得与 `[dependencies]` 的 git 依赖同一实现与缓存。

**稳定性与安全。** `branch` 引用不可复现：报告写出解析得到的提交，`--locked` 下拒绝未锁定的 `branch`；引用带来的构建程序（平台插件）
与 git 依赖受同样的信任规则（`untrusted_because`）。

**性能。** git 引用按（仓库、提交）缓存于 store，浅克隆；`path` 引用不复制；同一来源在一次构建中只解析一次。

## 16. 综合自审（全文）

| 视角 | 检查 | 结论或修订 |
|---|---|---|
| 一致性 | 引用文法是否处处同一 | 依赖、平台、层、工具、载体、运行器包同一文法与解析器；`builtin:` 只用于提供者；`toolchain` 保留自己的写法，理由在 SPEC-002 §1.2 |
| 一致性 | 第 5 节的提供者 schema 与 R6 的 `[[platform.provider]]` | 第 5 节的键即 `[[platform.provider]]` 的键；`[platform]`、`[platform.hosted]` 为其两种简写（R6） |
| 一致性 | git 的缺省引用与依赖 | 两处同一规则（第 15 节），SPEC-001 随之修订 |
| 可复现性 | 缺省引用与 `branch` 会移动 | 清单中的引用由锁固定到提交；命令行的引用每次按远端解析，报告写出提交；`--locked` 下拒绝未锁定者 |
| 离线 | 无网络时 | 锁中已有提交且 store 中已取得：直接使用；否则以「需要网络解析 <url>」拒绝，并给出 `--offline` 的含义（只用已取得的） |
| 安全 | 命令行的 git 引用带来构建程序 | 运行前写出来源与提交；信任规则同 git 依赖（`untrusted_because`） |
| 跨平台 | 路径、URL 形式 | 路径识别含 Windows 盘符（SPEC-013 §3.2）；git 地址的三种形式在各系统一致 |
| 性能 | 选择与引用不得拉长准备阶段 | 内置数据为静态表；可得性检查只读本地 store；git 按（仓库、提交）缓存、浅克隆，缺省引用每次一次 `ls-remote`；一次构建内每个来源只解析一次；`--timings` 报告选择与引用解析的时间 |
| 用户体验 | 新增的写法是否必要 | 新增的用户面：`mcpp why target`、`[target.<selector>.priority]`、`builtin:<name>`、命令行的 `git+` 形式；其余为已有写法的推广 |
| agent | 可脚本化 | 机器输出的 target 记录（R8）与引用记录（来源、解析得到的提交、是否来自命令行）；拒绝码稳定 |
| 可维护性 | 实现集中 | 选择一个模块、引用一个解析器（依赖的那一个）；旧的代理条件随迁移删除；等价性快照为合入条件 |
| 规范 | 受影响的规范 | 新 SPEC-016；修订 SPEC-001（git 的缺省引用；引用文法的适用范围）、SPEC-002（§3.4、§3.5、hosted 声明、能力槽）、SPEC-013（§0、§1 的 provider 一般形式、§3、§14）、SPEC-015（§1.2）；docs/05、20、21、22、30、35 |


## 17. 测试与构建基础设施（2026-11-09 补充）

同一 mcpp 发布包含以下几项。它们与目标模型同属引擎的契约，不另开 PR。

### 17.1 单元测试迁移到 mtest

mcpp 的单元测试（`tests/unit/` 166 个文件，以及各成员的 `modules/<name>/tests/`）由 GoogleTest 改写为 mtest 的用例对象：
- 每个 `TEST` 改写为一个 `UTest` 对象，标识为 `Suite/Case`，文件中不写 `main`；
- 夹具改写为派生自它的用例；
- 只在失败时才有效的消息改为惰性求值；
- 启动自身副本的测试以用例标识选择副本，并清除副本中的协议变量；
- 标准流捕获使用 mcpplibs.os 0.4.1 的 `stdio::capture`。

已实现，全部用例通过（根 166、成员 10）。

### 17.2 构建缓存的分级

实测（mcpp 仓库，一个配置）：
- 配置目录共 5.5G，其中 `bin/` 占 5.1G：166 个测试程序，每个约 31MB，因为每个都链接了全部 483 个目标文件；
- `obj` 为 164M，`gcm` 为 154M；
- `mcpp test -p <成员>` 另开一个配置目录，重编译成员；
- 全局缓存中的对象以复制方式进入配置目录。

分级与规则：

| 层 | 位置 | 键 | 内容 |
|---|---|---|---|
| global | `~/.mcpp/build-cache/v1` | 包身份与包配置指纹 | 索引包、std 模块、宿主工具；以硬链接（或 reflink）物化，不复制 |
| workspace | `target/.cache/pkg/<包>@<指纹>` | 成员自身的配置指纹 | 工作区成员（path 包）的对象、BMI 与静态库形态；根构建、`-p`、build 与 test 共用 |
| package | 同上一条目 | — | 一个包的编译结果 |
| target | `target/<triple>/<配置指纹>/` | 消费者配置 | 仅链接产物 |

测试程序链接各包的静态库形态，加 `--gc-sections`，只拉入被引用的部分。
- 实测 test_diag：由 32.1MB 降至 14.7MB，代码段由 17.5MB 降至 2.7MB。
- 支持时使用 `-gsplit-dwarf`，调试信息按对象只存一份。
- 依靠静态构造自注册、无符号被引用的对象，以 whole-archive 链入。
- `[test] link = "objects"` 保留逐对象链接作为退路。

配置目录与 workspace 缓存按指纹失效和 LRU 回收，由 `mcpp cache gc` 与 `mcpp clean --stale` 执行。

### 17.3 测试程序的组成与用例级调度

- 一个测试目录中只声明对象、编译与链接配置相同的文件，链接为一个测试程序（每包每目录一个）。缺省即此。
- 自带 `main` 或有特殊参数的文件单独成程序；`[test] program = "file"` 恢复逐文件成程序。
- 调度粒度为用例：引擎以协议列出用例，以同一程序的 N 个进程执行，每个进程由 `MTEST_SELECT` 选取用例。
  - 顺序按历史耗时，长者在前；
  - 遵守 `machine`、`serial:` 与内存容量（17.5）。
- `mcpp test` 将每次运行写入项目的 mtest store（`[test] store`，缺省 `.mtest`），带每个程序的构建区间。由此 `mtest report|analyze|graph|trend` 读得到 `mcpp test` 的运行。

### 17.4 运行是一张有向无环图；报告

- 一次测试运行是一张 DAG：
  - 节点为编译、链接、程序启动、用例、run 作用域夹具的建立与回收；
  - 边为构建依赖、"程序构建 → 其用例"、同一串行资源的先后持有者、worker 槽位的释放与占用。
- 关键路径是该 DAG 的最长加权路径（权为实测墙钟）。`mtest analyze path` 由"最长的程序"改为这一定义。
- `mtest report --format md`：
  - 章节可选：summary、kinds、slowest、failures、metrics、flaky、trend、graph、path、gantt、resources；
  - 范围可选：run、set、program、kind；
  - 选择方式为参数或 `mtest.toml` 的 `[report]`；
  - 图为 mermaid：DAG 中标出关键路径，并给出按 worker 与资源分行的 gantt。
- `mtest run --report md:FILE` 在运行结束时写出报告；`mtest ci annotate` 将其写入 job summary。
- 各 job 上传本地报告与 store；汇总 job 以 `mtest ci collect` 合并后，生成整体报告。
- `mtest ci` 可读取 GitHub 的 job 时间线，给出流水线层的 gantt 与关键路径。

### 17.5 资源监控

| 级别 | 内容 | 开销 |
|---|---|---|
| `off` | 无 | — |
| `basic`（缺省） | 进程级：墙钟、user/sys CPU、峰值内存；用例级：墙钟、CPU | 进程与用例边界各一次系统调用 |
| `detailed` | 另加用例级内存（峰值、增量）、IO 字节、分配次数与字节（可选模块） | 小 |
| `trace` | 另加 CPU% 与内存的时间序列，采样间隔可设 | 随采样间隔 |

- 写法有三种：用例属性 `monitor{.memory = true, .sample = 10ms}`、`mtest.toml` 的 `[monitor]`、`--monitor <级别>`。代码优先。
- 数据以标准指标名进入协议 P6，每项带精度（exact、process、approx）：`cpu.user`、`cpu.sys`、`mem.peak`、`mem.delta`、`io.read`、`io.write`、`net.sent`、`net.recv`、`alloc.count`、`alloc.bytes`、`proc.spawned`。
- 网络：经 mcpplibs.os 的套接字为 exact，其余为 approx。
- 现有的上下界（`at_most{"mem.peak", 64_MiB}`）与趋势分析直接适用。调度以历史峰值内存作为容量约束。
- 进程资源用量由 mcpplibs.os 提供：子进程与自身的 CPU、峰值内存、IO 计数。

### 17.6 可复用测试组件

- 组件是一个包，声明 `provides = ["mtest:component=<name>"]`，向框架注册：
  - 夹具及其作用域（case、suite、program、run）；
  - 它管理的资源与容量；
  - 新的用例类型与属性；
  - 它贡献的指标与报告章节。
- 注册方式与用例相同（静态初始化）。`list --static` 与协议的 `components` 事件列出组件。
- run 作用域的夹具由引擎在首个使用者之前启动、在最后一个使用者之后回收，端点以 `MTEST_FIXTURE_<NAME>` 交给用例；建立与回收是 DAG 的节点。
- 组件一律经进程与协议交互，与 adapters 相同，与语言无关。
- mcpp 的组件：沙箱 home、假 xpkg 索引、git 服务、工程树与 golden、标准流捕获、假工具链载荷。mcppls 的组件：一致性夹具。

### 17.7 决定

| 编号 | 事项 | 状态 |
|---|---|---|
| T1 | 17.2 缓存分级与测试的静态库链接进入本 PR | 已确认 |
| T2 | 17.3 按目录组成程序（缺省）与用例级调度；mtest 提供多种方式供开发者选择 | 已确认 |
| T3 | mcppls #79 的 CI 编排（已发布的 mcpp、按用例分片、store 与报告） | 已确认 |
| T4 | 17.4 的 DAG、关键路径、Markdown 报告与汇总在 mtest 0.5.0；`mcpp test` 写 mtest store | 已确认 |
| T5 | 17.5 资源监控：缺省 `basic`；网络经 mcpplibs.os 为 exact，其余为 approx | 已确认 |
| T6 | 17.6 组件机制在 mtest 0.5.0；mcpp 自身测试的共用部分在本 PR 改为组件 | 已确认 |

## 18. 任务与依赖

| 编号 | 仓库 | 任务 | 依赖 |
|---|---|---|---|
| A1 | mcpp | 行与内置提供者为数据（build.mcpp 编入静态表）、等价性快照 | — （已完成） |
| A2 | mcpp | 选择：行的登记、开放的 os 与 env、`select`、prepare 接入（pin、平台、可服务判断、报告） | A1（进行中） |
| A3 | mcpp | 身份取实际 C 库（重新准备）、显式身份与图冲突的拒绝 | A2 |
| A4 | mcpp | `[[platform.define]]`、`[[platform.provider]]` 的读取与登记；`[platform]`、`[platform.hosted]` 为简写；清单读取时的语句校验、`mcpp why platform`、`mcpp new --template platform` | A2 |
| A5 | mcpp | llvm 配 mingw 载荷 | A2 |
| A6 | mcpp | #911 G2–G11、B1–B3 | A4 |
| A7 | mcpp | 统一的包引用（`git+`、缺省分支、`builtin:`）；SPEC-001 修订 | — |
| A8 | mcpp | 锁记录"请求 → 身份"、`--locked`；`mcpp why target`；机器输出；`mcpp::target_provider()`；拒绝码 | A2 |
| B1 | mcpplibs/os | 0.4.1：`stdio::capture`、进程资源用量 | — （#20 进行中） |
| B2 | mcpp | 单元测试迁移到 mtest | B1 （已完成，待 os 0.4.1 发布后改锁） |
| C1 | mtest | 0.5.0：DAG、关键路径、Markdown 报告、汇总、监控、组件、多进程用例调度所需的协议增补 | B1 |
| C2 | mcpp | 缓存分级、静态库链接、按目录组成程序、用例级调度、写 mtest store、资源采集 | C1 |
| C3 | mcpp | e2e 由 mtest 引擎作为唯一入口；共用部分为组件 | C1 |
| D1 | openkal-platform | hosted 部分的服务改写为 musl 身份 | A4 |
| D2 | openkal-image | openkal-run 的能力槽（G7） | A6、mcpp 发布 |
| E1 | mcppls | #79：已发布的 mcpp 2026.11.8.1、CI 编排、mtest 0.5.0 报告 | C1 |
| F1 | 全部 | 规范：SPEC-016（新），SPEC-001、002、013、015 的修订；docs 05、08、20、21、22、30、35、90（中英） | A2–C3 |
| G1 | 全部 | CI 全绿、生态级自审、合入与发布、xim-pkgindex、clean-room 验证、关闭 issue | 以上全部 |

可以并行的部分：B1、C1 的 CI 运行期间推进 A2–A8；mcpp 的 CI 运行期间推进 D1、E1。发布顺序为 os → mtest → mcpp → openkal-platform 与 openkal-image → mcppls。


## 19. 实现中的取舍（2026-11-09，按“通用、必要、符合规范”评估）

| 项 | 结论 | 理由 |
|---|---|---|
| G2 `replace` | 撤回 | 有了 G1 与 G12，机器不同的目标即另一行；追加参数时编译器取后者。替换机制是多余的核心复杂度 |
| G2 解释 | `mcpp why flags`，不新增 `explain flags` | `why` 是来源解释的既有命令族；`explain` 只解释错误码 |
| 显式身份与图冲突 | 报告，不拒绝（SPEC-002 §3.4 规则四） | 拒绝曾实测打破把宿主身份写作 `x86_64-linux-gnu` 的配置；只有未写 env 的请求随 C 库 |
| 拒绝码 | 沿用 `host-cannot-serve`、`unknown-target`、`tier-planned` | 不新增与既有码同义的 `target-unserved` 等 |
| B2 | 只约束构建程序的 `artifact-name` | 项目与本机对产物名的陈述是显式选择，照常生效 |
| G9 | 新表 `[targets.X.build]` | 现有键的作用范围不变，升级无感 |
| G8 `mcpp patch edit/save` | 不采纳 | 核心保留的是补丁契约（`patches`、包根、`patch_root`）；编辑与生成是通用工具的事：`mcpp why deps` 报告包根，`git diff` 写出补丁，无功能缺口 |
| `mcpp check platform` | 不采纳，并入清单读取与 `mcpp why platform` | 一级子命令须有独立的场景；平台语句的校验在读取清单时进行，作者与使用者在构建中即得到；所服务的身份属来源解释，与 `why target`、`why flags` 同族 |
| 测试程序 | 按目录组成为可选（`[test] programs`）；`[test] split` 按文件分进程为缺省；测试只保留可达部分（`--gc-sections`） | 缺省不改变既有工程的程序划分与命名；按文件分进程同时保留隔离与并行 |
| 归档的测试 | 由 `mcpp test` 自身的运行器运行（协议、按文件分进程、工作线程、存储） | 一份归档与一次构建的测试是同一批程序，报告不应因运行方式而异；清单记录构建时判定的协议 |
| 测试的模式 | 匹配程序名，或运行所示的部分名（`unit/test_home`） | 用户看到什么名字就能用什么名字选择；规则只有一处（`test_matches`） |
| 链接丢掉的协议标记 | 从链接所用的目标文件读取 | 0.5.1 之前的 mtest 与 gtest 适配器的标记无人引用；默认链接方式改变后既有工程照常被识别（无感升级） |
| 被构建的包出现在自身测试图中 | 由该包自身代表（仅经 `[dev-dependencies]` 到达的边） | 测试框架本身使用被测库（mtest 与 mcpplibs.os）时，第二份副本会让每个模块在一个程序中出现两次；库自身依赖图中的回边仍是环并被拒绝 |
| 版本要求与根的路径钉 | 维持拒绝 | 版本要求是对图的承诺；改为由 mtest 声明 `^0.4.1`、os 以 0.4.2 发布（增量 API 在 0.4 内兼容） |
| 交叉目标的 `[target.<t>] toolchain` | 构建程序取被它替换的规格 | 与行的 pin 同一规则；该段名的是目标的编译器 |
| mcppls 的自启动测试 | mtest `child` 对象；需独立缓存的两个测试放在 `tests/isolated/` | mcpplibs.os 不修改进程自身环境，隔离缓存只能由新进程获得 |
| openkal-run 的实现 | 能力槽加按系统默认（D2，openkal-image 0.5.4） | 图或平台已有的实现优先，FreeBSD 等新系统无需修补该包 |
| B3 无同名程序 | 照旧打包按名字的第一个并说明，不拒绝 | 拒绝曾打破既有工程的 `mcpp pack`（mcppls）；问题在于不说明，不在于选择本身 |
