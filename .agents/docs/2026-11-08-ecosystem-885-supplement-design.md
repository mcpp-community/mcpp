---
subject: ecosystem
status: active
---

# #885 收尾补充方案：目标角色、平台事实、系统接口与版本书写

**日期：** 2026-11-08
**状态：** 第三版，待审查（Q1–Q4、Q6 已确认；Q5 改为"并存"，见 S3）
**上游文档：** [2026-11-07-ecosystem-885-closeout-design.md](2026-11-07-ecosystem-885-closeout-design.md)（D1–D20）
**相关：** mcpp#903、mcpp#904、mcpp-language-server#80

本文记录 #885 收尾过程中出现的七项发现（F1–F7），给出补充决定（S1–S6），以多个视角审查这些决定（第 3 节），并列出归属、版本与待审查的问题。被考虑而未采用的方案见附录 A。

## 0. 决定摘要

| 编号 | 决定 | 归属 |
|---|---|---|
| S1 | openkal 与原生目标不分等级，分"发布形态"与"构建目标"两种角色 | mcpp 文档；mcpp tier 评估 |
| S2 | os 提供"编译期能否 + 运行时是否"的两层平台判断，以及只接受泛型 lambda 的 `on<P>` | mcpplibs.os |
| S3 | 平台模块在能回答的地方定义；映像中的平台事实由 os 经可移植通道实现，openkal 不变；`is_<平台>` 与两层判断并存，各有确切含义 | mcpplibs.os；mcppls 过渡日志 |
| S4 | 生态的源文件与测试不包含系统头文件；系统头文件只在 os 的平台实现中；本轮并入 #52、#5 | mcpp-plugins 0.21.0、mcpp-ide 1.2.0、mcpp |
| S5 | 依赖写在一张表中：身份键、精确版本、一种拼写；锁文件的 `targets` 只随所规划的配置变化 | 生态各仓库（已完成）；mcpp#903、#904 |
| S6 | 原生引擎一律链接 zlib；darwin 的比值超限先由探针定位，上限不放宽 | mcppls-clangd 0.0.21 |
| S7 | mcpp 定义测试协议（用例的列出、选择、报告与上下文），测试框架以适配方式实现；`mcpp.core` 提供测试用的上下文注入 | mcpp 下一版本（提案，待审查） |

## 1. 发现

### F1 三层对 openkal 目标的定位不一致

| 层 | 依据 | openkal 的位置 |
|---|---|---|
| mcpp 目标表 | docs/21：tier 表示产物经过构建与运行 | `x86_64-openkal`、`aarch64-openkal` 为 `preview`；主要原生行为 `verified` |
| 生态应用的发布 | D18：mcppls-clangd、mcppls、mcpp-ide 只发布 ok 映像，原生构建只在手动对比运行中出现 | 唯一发布形态 |
| mcpplibs.os 接口 | README R3：编译期平台常量为首要接口；清单：映像中不定义任何平台模块 | 次要形态 |

实际发布的形态在 mcpp 中为 `preview`，在 os 中缺少平台模块。F2 是这一错位的直接后果。

### F2 映像程序静默失去平台事实

ok 映像的 `PLATFORM` 为 `openkal`，`is_windows`、`is_linux` 恒为 `false`。mcppls 的两处平台判断因此在全部发布产物中被丢弃：

- `src/orchestrator/workspace.cpp:706`：`if constexpr (os::is_windows) … os::windows::on_dev_drive(…)`，即 0.0.14 E1 的 Dev Drive 提示；
- `modules/toolrun/src/toolrun.cpp:229`：`if constexpr (os::is_linux) … os::linux::translator()`，即 PRoot 识别。

编译器与测试均不报告这一丢弃。第二处不只影响日志：`toolrun::sandbox()` 决定 PRoot 下模块构建在进程内进行（`src/engine/clangd.cpp:1968`），映像在 PRoot 下因此按普通内核的方式使用 worker。原生构建的 mcpp 不受影响，其 10 处平台模块调用（`windows::code_page` ×4、`console_output_code_page`、`long_paths_enabled` ×2、`machine_registry_path`、`writable_fixed_drive`、`posix::set_creation_mask`）依赖编译期分派。

### F3 系统头文件

| 仓库 | 状态 |
|---|---|
| mcppls | 0 处（`253aff8` 删除 test_archive 中针对已修复 Windows 映像故障的 `open`/`fstat` 探针；`devtools check os-surface` 报告任何 `#include`） |
| mcpp-plugins | 全局模块片段中包含 `<cstdio>`（`dist/*.cppm`、`rules/*.cppm` 共 39 处，用于 `popen`）、`<sys/wait.h>`、`<dlfcn.h>`；#52 的 `docs/plugin-development.md` §4 规定"只包含 ISO C 之外的平台函数头文件" |
| mcpp-ide | 测试使用本地头文件 `support.hpp` 及 `<cstdio>`、`<cstdlib>`、`<string>`、`<fstream>` |

mcpplibs.os 0.3.1 已提供对应能力：`process::run`（含输出捕获与有界读取）、`library::open`、`spawn_options` 中的子进程环境。mcpplibs.mwrap.libc 只覆盖 ISO C，不含 POSIX。tests 下的 Qt、Android 头文件属于测试用的 C/C++ 使用方工程，不在此列。

### F4 依赖的书写与精确性

本轮将 mcppls、mcpp-ide、mcpp-plugins、mcppls-clangd 的依赖改为单表、不带引号的身份键与精确版本。mcpp 解析中，不带运算符的版本即精确地址（`try_merge_semver` 将其规范为 `=<v>`），因此 `"0.3.1"` 与 `"=0.3.1"` 等价。mcpp 本体仍使用带引号的旧式键与按命名空间分立的子表，`os` 写作 `"=0.3.1"`（mcpp#904）。mcpplibs.os 自身以范围依赖 openkal 家族（`openkal = "^0.24.0"` 等），其精确性由平台版本组（D12）给出。

### F5 锁文件的 `targets` 随命令变化

`mcpp test --workspace --no-run` 删除未规划配置的目标，`mcpp test -p … --target …` 为原本无 `targets` 的条目写入目标。锁文件因此在不改变解析的命令下改写（mcpp#903）。

### F6 对比运行（mcppls-clangd run `42369457883`）

- **win32-x64 分片交换失败（image → native）。** 原生 Windows 引擎读取映像写出的分片时报告 `Compressed string table, but zlib is unavailable` 并重建全部 12 个单元。`ci/scripts/configure.sh` 在 Windows 上未提供 zlib，LLVM 的 `LLVM_ENABLE_ZLIB` 在找不到 zlib 时关闭。0.0.21 CHANGELOG 中"原生 clangd 同样链接 zlib"一句对 win32 不成立。
- **darwin-arm64 的比值超限。** 该测量有效（四对，一对在离散界外，政策允许一对）而未达限：冷启动 `ready_seconds` 的映像/原生中位比值为 1.204，上限 1.15。按 `ci/performance-policy.md`，有效而未达限的测量即为结果，不重复；其后进行的一次重跑违反此条，结果（冷启动 1.141，热启动 `ready_seconds` 1.177、`cpu_seconds` 1.244）只作诊断参考。0.0.20 的同一测量为冷启动 0.963、热启动 `cpu_seconds` 1.081。

### F7 复用答案时的编辑范围（mcppls#80，已修复）

clangd 的 include 补全替换光标后编辑器自动补出的右引号。mcppls 复用早先答案的三条路径（同词完整答案、迟到答案、答案层）均把编辑终点改为光标，导致 `"foo.h""`。修复保留越过旧光标的部分（在光标后的行文本不变时），且答案层不回答 include 行。

## 2. 补充决定

### S1 目标的角色

openkal 与原生目标之间不设等级，按角色区分（Q1 已确认）：

- **发布形态：** 生态中的应用以 ok 映像作为唯一发布物，一份产物覆盖全部系统，测试在每个真实系统上运行映像。适用于只需文件、进程、网络与终端的程序：mcppls-clangd、mcppls、mcpp-ide 的后端。
- **构建目标：** 原生目标（`*-linux-gnu`、`*-linux-musl`、`*-windows-msvc`、`aarch64-macos`、Android、iOS、`wasm32-emscripten`、`*-none-elf`）是完整支持的构建目标，mcpp 与 mcpplibs.os 对其提供与映像同等的保证。下列情形只能以原生目标构建：
  - mcpp 本体：按系统发布原生二进制，以编译期分派调用 `os::windows::*`、`os::posix::*`；
  - 库：mcpplibs.cmdline、tinyhttps、mcpplibs.os 本身，随使用者选择的目标编译进使用者的程序；
  - 链接系统或厂商库的程序：Qt、Vulkan/Metal、CUDA/SYCL、Android 的 `native_activity`、iOS 应用（mcpp-plugins 的测试工程即此类）；
  - 映像不覆盖的平台：Android、iOS、wasm、裸机。

按具体系统构建时，os 按 `[target.<os>.build]` 只编译该系统的实现文件（`src/linux/*.cpp`、`src/posix/*.cpp`、`src/windows/*.cpp`），平台模块 `mcpplibs.os.<平台>` 只在其平台的构建中有定义；这一机制不因 S2、S3 改变。

mcpp 依 docs/21 的 tier 定义评估两行 openkal 的升级：两行已在四个系统上构建并运行，满足 `verified` 的条件。角色划分写入 mcpp docs/24（openkal）与 docs/35（平台）。

### S2 mcpplibs.os：两层平台判断

R5 依赖一条语言规则：被丢弃的语句（discarded statement）中出现的函数不构成 ODR 使用，因而 Linux 原生构建引用 `os::windows::*` 不要求其定义。普通的 `if` 不具备这一性质，即使条件是常量；映像中又只有运行时判断可用。os 因此提供两层判断：

```cpp
// 本构建能否运行在 p 上：原生构建为 PLATFORM == p；映像为 p 属于 {linux, macos, windows}
constexpr bool can_run_on(platform p) noexcept;

if constexpr (os::can_run_on(os::platform::windows))   // 原生的其他平台：整段丢弃，零开销
    if (os::host() == os::platform::windows)           // 原生为常量，映像在加载时判断
        cacheOnDevDrive = os::windows::on_dev_drive(dir);
```

映像中，运行时判断是主要机制：`os::host()` 返回加载映像的系统，`os::conventions()`（可执行文件后缀、路径列表分隔符、名称是否大小写无关）与 `os::dirs` 已在运行时作答。两层判断使同一源码在原生构建与映像构建中都正确。

简写为 `os::on<P>(f)`，`f` 必须是泛型 lambda：

```cpp
os::on<os::platform::windows>([&](auto) { cacheOnDevDrive = os::windows::on_dev_drive(dir); });
```

泛型 lambda 的函数体是模板，未实例化时不构成 ODR 使用；普通 lambda 的函数体不是被丢弃的语句，引用其他平台的符号属于"不合规、无须诊断"，因此 `on<P>` 以约束拒绝非泛型的可调用对象。os 的文档给出常见写法（两层判断、`on<P>`、`conventions()`、`dirs`）及其适用场景；语法糖只以合乎标准 C++ 的库接口提供，不提供构建插件（Q4 已确认）。

### S3 mcpplibs.os：映像中的平台事实

- **R5 修订：** 平台模块的函数在能回答的地方定义——自身平台的原生构建，以及映像。其他平台的原生构建仍不定义，误用仍为链接错误。现有签名不变，mcpp 的调用不受影响。
- **映像能做与不能做的：** 映像只经由 openkal 的 `kal_*` 接口与宿主交互。它不能调用 Win32、sysctl 等原生接口，也不能加载宿主的库（openkal SPEC 第 12 条；os 0.3.1 的 `src/openkal/library.cpp` 对 `library::open` 返回 `not_supported`）；一个映像服务全部系统，因而没有编译期的平台分支。它能做的是运行时判断（`host()`）与经由可移植通道取得事实：读文件、读环境、启动宿主程序并解析其输出（`process::run`）。
- **事实的三类与各自的实现：**

  | 类 | 函数 | 映像中的实现 |
  |---|---|---|
  | okrun 自身的事务 | `windows::code_page`、`console_output_code_page`、`long_paths_enabled`、`machine_registry_path`、`writable_fixed_drive` | 不提供（误用为链接错误）。代码页与长路径约束原生 Win32 程序；映像的输入输出全部经由 okrun，由 okrun 处理。后两者服务 mcpp 自身的安装与工具链定位，不属于映像程序 |
  | 经由可移植通道可得 | `linux::translator()` | 读取 `/proc/self/status`、追踪者的 `/proc/<pid>/comm` 与环境，复用原生实现的 `classify_translator` |
  | | `macos::translated()` | 恒为 `false`，不启动进程：macOS 上运行映像的 okrun 只有 aarch64 版本（`x86_64-macos` 在目标表中为 `planned`），映像因此总以本机架构运行。日后有 x86-64 的 okrun 时，判断为"映像为 x86-64 且 `hw.optional.arm64` 为 1"；不读 `sysctl.proc_translated`，它回答的是被启动的 sysctl 进程自己 |
  | | `windows::elevated()` | `%SystemRoot%\System32\whoami.exe /groups` 的输出中含 SID `S-1-16-12288`（高完整性级别）。以 SID 而非组名判断，与系统语言无关；子进程继承 okrun 的令牌 |
  | | `windows::on_dev_drive()` | 经由 `fsutil` 查询所在卷，返回值已是 `std::optional<bool>`。是否需要管理员权限、输出是否随语言变化，实施前实测；不能可靠回答时返回空 |
  | | `posix::effective_user()` | `/usr/bin/id -u`（Linux 也可读 `/proc/self/status` 的 `Uid:`） |
  | 只有原生接口能回答、无宿主程序可用 | — | 目前未见实例 |

  这些实现位于 os 的 `src/openkal/` 下，由 os 清单的 `cfg(os = "openkal")` 选择，与原生实现并列。宿主程序以绝对路径启动（不经 PATH 搜索），以有界读取与超时运行；失败时返回空（`optional`）或保守值（`elevated()` 为 `false`）。结果在进程内缓存，每个会话只付一次进程启动的代价，且只在调用时发生。全部工作在 mcpplibs.os 内完成，openkal 不变（Q3）。
- **`is_<平台>` 与两层判断并存（Q5）。** 二者回答不同的问题，都保留，都不标 deprecated：
  - `is_windows`、`is_linux`、`is_macos`、`is_posix`：本构建是否为该平台的原生构建。编译期常量，映像中为 `false`。它是"只属于原生构建"的代码的正确写法，例如设置控制台代码页：映像的输入输出由 okrun 处理，这段代码在映像中本就不应执行。
  - `can_run_on(p)` 与 `host()`：本构建能否运行在、是否正运行在该平台上。一段代码要在映像中也按平台生效时用它们。

  第一版拟在映像构建中把 `is_<平台>` 标为 deprecated。审查发现这会误报上面那类合法用法，而 deprecated 警告只能以编译器的诊断指令（宏形式）逐处抑制，与项目规则相悖；因此不采用（附录 A.5）。F2 这类错误由两处防止：os 的文档按"原生专属 / 随平台生效"说明两种写法的选择；以映像为发布形态的应用在自身的检查中禁止 `is_<平台>`（mcppls 的 `devtools check os-surface`、mcpp-ide 的等价检查），因为它们的每一段平台代码都应在映像中生效。

mcppls 在 os 发布该版本后改用 S2 的写法，两项功能随之恢复；在此之前，映像在 Windows 与 Linux 上运行时于日志中说明不检测 Dev Drive 与 PRoot（mcppls 0.0.21 `e34cca0`，docs/50-troubleshooting.md），不再静默跳过。

### S4 系统接口的书写规则

生态中的源文件与测试不包含系统头文件：操作系统经由 mcpplibs.os，C 库经由 `std`（宏经由 mcpplibs.mwrap.libc）。系统头文件只出现在 os 自身按平台选择的实现文件中，这是 os 的边界。原始 POSIX 或 Win32 接口不作为 os 的模块提供；确有需要的项目以独立的 mwrap 包（例如 `mcpplibs.mwrap.posix`）承载，os 保持语义接口。

此规则对原生构建同样成立：插件在 Linux 上原生构建时，`os::process::run` 编译为 os 的 `src/posix/exec.cpp`，在 Windows 上编译为 `src/windows/exec.cpp`；插件源码不再随平台变化。

本轮并入 mcpp-plugins #52（0.21.0）与 mcpp-ide #5（1.2.0）（Q2）。

- **mcpp-plugins：**
  - 七个模块（`dist/apk`、`rules/cuda`、`metal`、`slang`、`spirv`、`swift`、`sycl`）各自以 `popen`/`_popen` 与 `#if defined(_WIN32)` 取一条命令的输出。改为 `mcpp.plugins.tool` 中的一个函数：以 argv 调用 `os::process::run`（不经 shell，因而不再拼接 `2>/dev/null`、`2>nul` 与引号），返回退出状态与输出；七处调用改用它，预处理平台分支随之消失。
  - `rules/cuda` 以 `dlopen`/`dlsym` 读取驱动版本，改为 `os::library::open` 与 `handle::function<>`。
  - `src/testing.cppm` 本就为每个用例启动一次自身（`--mcpp-plugins-testing-case=<n>`），此前以 `setenv`/`_putenv_s` 改写本进程的环境、经 `std::system` 与 shell 重定向把上下文传给子进程，并以 `environ`/`_NSGetEnviron` 枚举环境。改为以 `os::process::run` 启动，用例的上下文写在 `spawn_options.env` 中（本进程的环境不再被改写），环境由 `os::env::variables()` 枚举。设计不变，平台分支与头文件随之消失（Q7 因此不再成立）。
  - os 由 `plugins-progress` 的依赖变为插件的基础依赖，`docs/plugin-development.md` §4 改为"不包含系统头文件"，CI 增加与 mcppls `os-surface` 等价的检查（报告 `#include` 与平台预处理分支；`tests/` 下的使用方工程除外）。os 成为基础依赖对构建程序首次构建时间的影响，实施时测量并记录在 PR 中。
  - 实施中发现 GCC 16 的模块缺陷（mcpp#905）：内联函数体经由 mcpplibs.os 时，导入方读不回模块（"Bad file data"）或链接时缺 `std::optional<std::vector<grant>>` 的成员。规避为"经由 os 的辅助函数不内联"，写入 `docs/plugin-development.md` §4（`e77b920`）。`capture` 的标准错误以独立管道读取后追加（`stdio::output` 不是合并到子进程的标准输出管道，`6305d89`）。
- **mcpp-ide：** `tests/support.hpp` 改为测试模块 `mcppide.testsupport`（`testsupport/`，`mcppide.testing` 已是后端的测试框架模块），各测试改为 `import std` 与该模块；`mcpp-ide-dev check-sources` 在 CI 中报告头文件、预处理分支与 `os::is_<平台>`（`f5686ac`）。
- **mcpp：** CI 增加等价检查，与 mcpp#904 一并处理。

### S5 依赖书写与锁文件

- 依赖写在一张表中，键为不带引号的身份（命名空间在前），版本为精确版本，精确版本只用一种拼写（不带运算符）。`[workspace.package] mcpp = ">=…"` 声明构建所需的最低 mcpp，不属于依赖，保留范围。
- mcpp 本体按此改写清单与 `examples/02-with-deps`，并对带引号的点分键给出带改写提示的警告（mcpp#904）。
- 锁条目的 `targets` 为解析到达该包的配置集合，命令只更新其规划的配置，不删除也不新增其他配置（mcpp#903）。

### S6 mcppls-clangd 0.0.21 的对比运行

- **原生引擎一律链接 zlib**（`80daf27`）。`ci/scripts/configure.sh` 对全部原生平台设 `-DLLVM_ENABLE_ZLIB=FORCE_ON`，找不到 zlib 即停止；Windows 取 vcpkg 的 `x64-windows-static`（与 `/MT` 一致）。原生与映像双向读取对方的分片，CHANGELOG 的 zlib 陈述随之成立。
- **darwin 的比值超限先定位，上限不放宽。** 0.0.21 的映像相对 0.0.20 新增 zlib；Linux 两行（原生使用发行版的普通 zlib）通过，只有 macOS（原生使用系统优化的 libz）未达限，热启动读取分片时的解压是首要嫌疑。探针分支 `probe/darwin-zlib-off` 以 `LLVM_ENABLE_ZLIB=OFF` 构建映像，只对 darwin-arm64 做对比运行（run `42386660489`）。比值回到 0.0.20 的水平即确认，处理为映像使用带 NEON 与 CRC 指令的 deflate 实现（Q6）：aarch64 基线即有 NEON，分片格式与行为不变；实现以 mcpp-index 的包提供，原生构建不受影响。另一可选项"映像写出不压缩的分片"不采用（附录 A.6）。比值不回落则 zlib 被排除，按装载与页错误继续定位（冷启动 `page_faults` 比值 2.74–2.87）。0.0.21 在 darwin 达限之前不发布；mcppls 0.0.21 锁定该引擎，随之等待。
- 新提交上的完整对比运行（run `42388978321`；Windows 的 zlib 以头文件目录与库文件指定）通过后，CHANGELOG 记录 `Comparison run: <url>`，按 D18 发布。

### S7 mcpp 的测试协议（已实现于 mcpp 2026.11.8.1，见顶层方案 §9.2）

**现状。** 生态中有四种互不相通的测试方式：mcpp 本体用 GoogleTest；mcppls 有自己的模块 `mcppls.testing`（无宏，`"名称"_test = [] {}`）；mcpp-plugins 有 `mcpp.plugins.testing`（插件逻辑的用例，经改写进程环境注入上下文）；mcpp-ide 有 `mcppide.testsupport`。mcpp 对测试的契约只有"一个 `.cpp` 一个程序，以退出码判定"（docs/08），用例是框架内部的事；mcpp-ide 因此在 `spec/test-frameworks.json` 与 `mcppide.testing` 中逐个框架编码列出用例的参数、报告格式与宏名，才能给出用例树、单个用例的运行与失败位置。每增加一个框架或一个消费方（IDE、CI、agent），这份知识就要再写一遍。

**提案。** mcpp 以规范定义测试程序与运行器之间的协议，mcpp 只实现协议的运行器一侧，框架以适配实现另一侧，开发者自选框架。这与构建插件的关系相同：mcpp 给出契约，具体实现属于包。

- **能力声明。** 提供测试入口的框架包已以 `kind = "entry"` 声明入口、由 `[test] main = { from = "fw/main" }` 选用（docs/08）；入口再声明它实现的协议版本（例如 `test-protocol = 1`）。未声明的程序照旧是一个以退出码判定的用例，现有测试无须改动。
- **列出。** 运行器以协议规定的参数启动程序，程序输出用例的结构化清单：标识、显示名、源位置、标签。
- **选择。** 运行器以用例标识选择要运行的用例；转义与通配由协议规定，不再是各框架的命令行语法。
- **报告。** 程序把每个用例的开始、结束、状态（通过、失败、跳过）、耗时、失败位置与输出写到运行器给出的通道（NDJSON），mcpp 汇总到 `mcpp test --message-format json`。mcpp-ide、CI 与 agent 只读这一种格式。
- **上下文。** 协议允许运行器为单个用例给出上下文（环境与工作目录），并规定用例在同一进程或各自的进程中运行由运行器决定。`mcpp.core` 的访问器经由一个可在测试中按作用域替换的上下文读取引擎的值，替代对 `std::getenv` 的直接读取；插件逻辑的测试因此在进程内设置上下文，S4 中保留的 `setenv` 随之删除。

**框架与头文件。** GoogleTest 成为协议的一种实现：一个适配包以 GoogleTest 的事件监听接口实现列出、选择与报告，使用者仍写 `TEST`、`EXPECT_EQ`，因而仍包含其头文件——宏无法经由模块导出，这是 GoogleTest 自身的形态。消除头文件的途径是一个模块原生的框架：以 `std::source_location` 记录位置、以用户定义字面量或函数注册用例、不定义宏（`mcppls.testing` 已是此形态）。协议使它与 GoogleTest 处于同等地位；生态的四种测试方式收敛为这一个框架包，项目仍可选择 GoogleTest、doctest、Catch2 的适配。

**归属。** 规范与运行器属于 mcpp 的下一版本（新 SPEC 与 docs/08）；测试库、引擎与适配为通用测试系统 mtest（`mcpp-community/mtest`，设计见 [mtest .agents/docs/2026-11-08-mtest-design.md](https://github.com/mcpp-community/mtest/blob/main/.agents/docs/2026-11-08-mtest-design.md)）；mcpp-ide 改为读取协议的报告并以 `spec/test-frameworks.json` 作为未声明协议的程序的后备。不并入 #885 本轮。

## 3. 多视角自我审查

| 视角 | 审查所见 | 处理 |
|---|---|---|
| 架构 | 第一版的 S3 以 openkal 的"宿主事实查询"为前提，把一系统的产品特性压到内核抽象层 | 改为 os 经可移植通道实现；openkal 不变，`openkal.host` 只作为热路径需求出现时的扩展（附录 A.3） |
| 架构 | `can_run_on` 若在映像中恒为 `true`，会把 `emscripten` 也算作映像可运行的平台 | 定义为映像中 `p ∈ {linux, macos, windows}` |
| 稳定性 | 从映像启动宿主程序：PATH 可被替换、输出随语言变化、程序可能挂起 | 绝对路径启动；以 SID、数值等与语言无关的字段判断；有界读取与超时；失败返回空或保守值 |
| 稳定性 | `linux::translator()` 在映像中读取 `/proc`，取决于 openkal 在 Linux 上是否能访问 `/proc` | 实施前在 Linux 的映像中实测；不可读时返回空并在日志中说明 |
| 性能 | 进程启动在 Windows 上约数十毫秒，若在启动路径上会拖慢 mcppls 的就绪 | 结果缓存、只在调用时发生；mcppls 的 Dev Drive 检查移出启动的关键路径 |
| 性能 | darwin 的比值超限（F6） | 探针定位，上限不放宽（S6） |
| 用户体验 | 在 os 发布之前，映像用户失去 Dev Drive 提示与 PRoot 识别 | 0.0.21 的日志与故障排查文档明确说明（`e34cca0`）；os 发布后恢复 |
| 兼容性 | 第二版拟在映像构建中把 `is_<平台>` 标为 deprecated：会误报"只属于原生构建"的合法用法，抑制只能用宏形式的诊断指令；已有的映像构建在 `--strict` 下转为错误 | 改为并存：`is_<平台>` 表示原生构建，两层判断表示运行平台；防止 F2 的检查放在以映像为发布形态的应用中（S3）；os 0.4.0 只增加接口，不改变已有接口的行为 |
| 兼容性 | plugins 改用 argv 调用后，原先依赖 shell 的命令（重定向、管道）行为可能不同 | 七处命令逐一核对，只取标准输出、丢弃标准错误由 `process::run` 的选项表达；插件逻辑用例与各 rules 的使用方工程在三个系统上验证 |
| 跨平台 | 不经 shell 后，Windows 上以 `.bat`/`.cmd` 提供的工具（例如 Android SDK 的若干脚本）不能直接作为程序启动 | 实施时核对七处命令所启动的程序；脚本形式的工具经 `cmd.exe /d /c` 显式启动，并在该函数中注明 |
| 架构 | 第三、四版对 plugins 测试的分析有误：测试工具本已每个用例一个子进程，`setenv` 只是传递上下文的手段 | 改为在 `spawn_options.env` 中陈述上下文；`mcpp.core` 的上下文注入仍属 S7，用于进程内测试 |
| 兼容性 | R5 修订只增加定义，不删减；签名不变 | mcpp 的原生调用不受影响 |
| 跨平台 | `macos::translated()` 读 `sysctl.proc_translated` 会得到 sysctl 进程自己的答案；且 macOS 上只有 aarch64 的 okrun | 映像中恒为 `false`；有 x86-64 的 okrun 时改用"映像架构 + `hw.optional.arm64`" |
| 一致性 | 精确版本有 `"0.3.1"` 与 `"=0.3.1"` 两种拼写（mcpp 本体与生态） | 统一为不带运算符（mcpp#904） |
| 一致性 | 生态的头文件规则只在 mcppls 有检查 | plugins、ide、mcpp 各自增加等价检查（S4） |
| 无感升级 | 依赖精确固定后，os 的新版本不会被静默带入 | 升级随各仓库的版本 PR 显式进行，CI 在升级时报告 deprecated |
| 测试覆盖 | 映像中的平台事实若只在原生 CI 测试，会重演 F2 | os 的 CI 以映像在四个真实系统上运行这些函数：Windows 运行器为管理员，`elevated()` 断言为真；Linux 运行器在 PRoot 下运行映像，断言 `translator()` 为 `proot`；macOS 断言 `translated()` 为假 |
| 测试覆盖 | 非管理员的 Windows 路径在 CI 中不易得到 | 以受限令牌启动（`runas /trustlevel` 或等价方式）的测试在实施时评估；不可得时在文档中记录覆盖缺口 |
| 过程 | 对 darwin 有效测量的重跑违反性能政策 | 在 F6 记录；只作诊断参考，结论以第一次测量为准 |

## 4. 归属与版本

| 项 | 仓库 | 版本 | 状态 |
|---|---|---|---|
| F7 修复 | mcppls | 0.0.21（#79） | 已提交 `aaf7174` |
| F3 mcppls 头文件、`os-surface` 检查 | mcppls | 0.0.21（#79） | 已提交 `253aff8` |
| S3 末段：mcppls 过渡日志 | mcppls | 0.0.21（#79） | 已提交 `e34cca0` |
| S5 生态清单改写 | mcppls、mcpp-ide、mcpp-plugins、mcppls-clangd | 各自本轮 PR | 已提交 |
| S6 zlib | mcppls-clangd | 0.0.21（#18） | 已提交 `80daf27`、`becb991`，对比运行进行中 |
| S6 darwin | mcppls-clangd | 0.0.21（#18） | 探针进行中 |
| S4 mcpp-plugins | mcpp-plugins | 0.21.0（#52） | 已提交 `8d0afd7`（52 个插件逻辑用例、all-rules-compile 本地通过） |
| S4 mcpp-ide | mcpp-ide | 1.2.0（#5） | 已提交 `f5686ac`（`testsupport/` 模块、`check-sources`） |
| S7 | mcpp、mcpplibs、mcpp-ide | mcpp 下一版本 | 提案，独立设计 |
| S1 文档与 tier 评估 | mcpp | 下一版本 | 新 issue |
| S5 mcpp 本体 | mcpp | 下一版本 | mcpp#903、mcpp#904 |
| S2、S3 | mcpplibs.os | 0.4.0（只增加接口） | 新 issue；mcppls、mcpp-ide 随后迁移 |
| openkal | — | — | 不变 |

## 5. 待审查的问题

- **Q1、Q3、Q4、Q6** 已确认。
- **Q2** 已确认：S4 并入 #52、#5。
- **Q5** 已确认：并存（S3），`is_<平台>` 不标 deprecated；防止 F2 的检查放在以映像为发布形态的应用中。
- **Q7** 不再成立：plugins 测试工具本已每个用例一个子进程，改为以 `spawn_options.env` 陈述上下文（`8d0afd7`）。
- **Q8** S7（mcpp 测试协议、模块原生框架、GoogleTest 适配、`mcpp.core` 上下文注入）作为 mcpp 下一版本的独立设计推进，是否同意。

## 附录 A 被考虑而未采用的方案

- **A.1 构建插件提供平台语法糖。** 插件在构建时同样不知道映像的加载系统；改写源码形成 clangd 与 IDE 无法解析的方言；宏形式与项目规则相悖。
- **A.2 os 提供原始系统接口的映射（`os.unix`、`os.windows` 下的 POSIX/Win32 镜像）。** 生态中唯一的直接 POSIX 调用是已删除的诊断探针，未见用例；原始接口是一部分系统的表面，与 os 的语义接口和 R5 相悖。确有需要时以独立的 mwrap 包承载。
- **A.3 openkal 收录平台事实。** 逐项进入 ABI 会使 ABI 的版本随各系统的新特性变化。热路径上确需零开销时，合乎现有机制的形式是可选接口 `openkal.host`：okrun 作为原生程序在宿主侧作答，映像以弱导入使用（`examples/weak`），调用代价与其他 `kal_*` 调用相同；未提供该接口的运行器使弱导入为空，os 回退到可移植通道。目前 libokrun 只绑定规范的接口面（`run/src/bindings.cpp` 的静态表），不提供登记额外名字的接口；libokld 不在运行时提供服务。本轮所涉事实均为每会话一次的查询，不引入该接口。
- **A.4 映像中无法回答的事实定义为返回空值。** 会改变 `bool`、`unsigned` 等现有签名，影响 mcpp 的原生调用；不提供（链接错误）使误用在构建时可见。
- **A.5 映像构建中把 `is_<平台>` 标为 deprecated。** 误报"只属于原生构建"的合法用法，逐处抑制需要宏形式的诊断指令；以并存与应用侧的检查代替（S3）。
- **A.6 映像写出不压缩的分片。** 分片体积增大，映像与原生写出的分片在格式上不同；以更快的 deflate 实现代替（S6）。
