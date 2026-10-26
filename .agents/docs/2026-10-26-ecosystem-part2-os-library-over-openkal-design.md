---
subject: ecosystem
status: accepted
---

# 生态设计 part 2：openkal 的最小原子与二进制映像、其上的 `mcpplibs.os`，以及 part 1 的收尾与回归记录

日期：2026-10-26。前文：[part 1](2026-10-26-control-contract-runtime-and-ide-ecosystem-design.md)（控制契约、并发、单一 C++ 运行时、
IDE 生态，已随 mcpp 2026.10.26.1 发布）。分析基线：mcpp `bfa9b8fe`（release/2026.10.26.2）；openkal `355b01a`（0.16.0）；
openkal-linux 0.17.0、openkal-macos 0.14.1、openkal-windows 0.12.0（index 当前最新的一组，三者都依赖 openkal 0.16.0）；
openkal-musl 0.20.1；speak-agent 的 openkal 系 fork（openkal#1、openkal-musl#1：动态加载）；xlings `f437e780`；
mcppls 0.0.17（`Sunrisepeak/mcpp-language-server`）；mcpp-ide 本地仓库 `6ea6178` 加未提交的工作。

**修订记录**

- rev 1：由讨论收敛，含 openkal 能力的实测（临时 PR [mcpp#843](https://github.com/mcpp-community/mcpp/pull/843)，run
  40274573232、40275769974、40276337637、40277053969；后两次修正了探测程序自身的两处错误，见 E15、E18）。
- rev 2（第一轮评审）：分层原则（内核层能力拆成 openkal 原子，`os` 只做组合与约定）；新增原子 G1–G6 与 K10；G7 不进 openkal；
  O2 / O3 / O6 / O8 定案；L1（实现的共享形态）、L2（openkal 之上的加载）进本批。
- **rev 3（第二轮评审，本稿）**：二进制跨平台进本批——openkal 双模式（原生格式 / ok 映像），映像约定进 SPEC，参考实现
  `libokld`、`libokrun`、`okrun`、`okld` 放在新仓库 `mcpplibs/openkal-image`，mcpp 增加 `<arch>-openkal` 目标；`<arch>-openkal` 只固定
  `kernel-abi`，`c-abi` 与 `c++-abi` 由依赖图决定（openkal-musl、libc++ 不是唯一实现）；mcppls 与 mcpp-ide 为第一方使用者；
  xlings 本批不改。正文按 rev 3 重写，rev 1、rev 2 的结论已并入；rev 3 的生态级自审见 §15，其结论已并入正文。
- rev 6（第五轮评审，本稿）：探测 P 完成（[mcpp#843](https://github.com/mcpp-community/mcpp/pull/843)，run 40295631789、
  40296883212），结果见 §3.2（E22–E29）：同一份 ok 映像字节在 linux-x86_64、windows-x64、linux-aarch64、macos-arm64 上运行；
  TLS 定为 emulated TLS（O10 关闭）；`os::host()` 在 ok 目标上采用"普通声明"形态（MSVC 不接受 `if consteval` 标记形态）。
  rev 6 的自审见 §15 末节。
- rev 5（第四轮评审）：命名定案——**ok 格式**（openkal image format）；可执行映像 `.oke`、可加载库 `.okso`（目标文件 `.o`、
  静态库 `.a` 不变）；库 `libokld`（加载器与格式定义的唯一实现）、`libokrun`（运行器核心）；程序 `okrun`、`okld`；仓库
  `mcpplibs/openkal-image` 为三个包的工作空间（`openkal-ld`、`openkal-run`、`openkal-tools`）。"kal 模式"改称"ok 模式"。M3 不再需要
  单独的嵌入库，由 libokrun、映像段与 `mcpp pack` 生成的入口组成。rev 5 的自审见 §15 末节。
- rev 4（第三轮评审）：O10、O11 定案（映像签名进约定，§8.3）；`os` 保留编译期平台常量为一等接口（`os::PLATFORM`、
  `os::is_windows` 等，供 `if constexpr` 使用），并区分"编译目标"与"运行宿主"（§6.1 R3、§8.6）；`os` 不限于 openkal 体系的项目；
  [mcpp#844](https://github.com/mcpp-community/mcpp/issues/844)（release 的 `trim_paths` 使切换成员时公共依赖反复重编）确认为
  mcpp 缺陷，并入 #842（§10.4，D13）；O9 的候选名 `krunner` 与 KDE 的 KRunner（`/usr/bin/krunner`）重名，待定。

**读者**：评审与实现者。涉及的仓库（每个一个 PR，§11）：mcpplibs/openkal、openkal-linux、openkal-macos、openkal-windows、
openkal-musl、openkal-llvm-runtime、**新建** mcpplibs/openkal-image、**新建** mcpplibs/os、mcpp-community/mcpp（#842）、
**新建** mcpp-community/mcpp-ide、Sunrisepeak/mcpp-language-server、mcpplibs/mcpp-index、mcpp-community/mcpp-plugins（#51）。
openxlings/xlings 本批不改。

## 0. 结论

| # | 决定 |
|---|---|
| D1 | **三层分工**：openkal 是与 OS、内核无关的最小内核 ABI（C ABI；面向写 C 库、运行时、裸机程序的开发者），只提供**最小、可组合的原子能力**；`mcpplibs.os` 是其上的 C++ OS 库，面向一般开发者，提供好用、更强的通用 API 与少量平台专属能力；各项目只保留领域逻辑（§1） |
| D2 | **分层原则**：一项需求拆成"openkal 原子 + 上层组合"。内核层能力一律以原子进 openkal（语义只用 openkal 自己的词汇；环境间的差异用按资源查询或不提供接口表达）；`os` 只做组合与约定；`os` 中的原生代码只用于非内核的环境服务（提权、Dev Drive）与平台专属模块（§1、§4） |
| D3 | **openkal 是 `os` 唯一的内核层后端**，不设两套后端。实测（§3）：openkal 的三个实现在默认安排（借用宿主运行时）下与 glibc、musl、libSystem、UCRT、libstdc++、libc++、MSVC STL 同进程工作，`cl.exe` 与 llvm（MSVC ABI）都能构建 |
| D4 | **新增原子**（S1，§7.2）：G1 rename 不覆盖；G2 自身映像路径；G3 目录句柄下按名字会合的连接；G4 按请求结束；G5 进程自身、按编号打开、进程信息；G6 终端输出解释 ECMA-48；G8 宿主环境查询（仅供约定使用）；L2 部分发布与保护粒度。实现缺陷 K1–K10 在各实现修（§7.1） |
| D5 | **openkal 双模式**（§8）：同一份源码，**原生模式**产出各 OS 的原生格式（今天的做法）；**ok 模式**产出 `<arch>-openkal` 映像，同一份字节在该架构的各 OS 上运行。映像约定作为 SPEC 的规范性一章；参考实现 `libokld`（加载器与格式）、`libokrun`（运行器核心）、`okrun`（启动器）、`okld`（映像工具）在新仓库 `mcpplibs/openkal-image`；配一致性套件 |
| D6 | **一个进程只有一份 openkal 实现**（L1）：规范写明，并给出共享形态的条件；ok 模式下这一份是 okrun 的；`os` 的资源类型在 openkal 非共享形态时不跨共享对象边界（R6） |
| D7 | **映像约定只约束边界**：只导入 kal_* 与其他映像、调用约定、TLS 访问方式、展开信息不出映像；不规定映像内的 C 库与 C++ 运行时。mcpp 的 `<arch>-openkal` 目标只固定 `kernel-abi = openkal`，`c-abi`、`c++-abi` 由依赖图决定，openkal-musl 与 openkal-llvm-runtime 只是默认提供者 |
| D8 | **`os` 的接口规则** R0–R6；`.cppm` 声明 / `.cpp` 实现；Windows 实现单元自己声明所用的 Win32 函数；名称 `mcpplibs:os` / `mcpplibs.os.*` / `mcpplibs::os`（§6） |
| D9 | **交叉验证**：上游 PR 以 git 分支引用（`{ git = …, branch = "<PR 分支>" }`），消费方 PR 的 CI 引用 os 与 openkal 系的 **PR 分支**；全部合入后经 mcpp-index 发布，消费方改为版本依赖后合入。第一方使用者：mcpp、mcpp-ide、mcppls（§11） |
| D10 | **mcpp 只有一个 PR**：#842 保持版本 2026.10.26.2，标题追加新增范围；含 wasm 回归修复、GCC 模块映射修复（D11）、`os` 接入、`<arch>-openkal` 目标（§10） |
| D11 | **GCC 模块映射以 BMI 目录为仓库根（`$root gcm.cache`）**，全局缓存键对含模块的包增加"BMI 路径约定"一轴：只有含模块的依赖重建一次（§10.2） |
| D13 | **`trim_paths` 的映射按包给出**（mcpp#844）：一个编译单元只带固定根（home、store、工具链、sysroot、构建目录）与**它自己的包及其依赖闭包**的映射，按长度与字典序排序；不再把整个计划的包映射放进全局参数。公共依赖的命令行因此与所选成员无关（§10.4） |
| D12 | **探测已完成**（§3.2）：同一份映像字节在 x86_64（Linux、Windows）与 aarch64（Linux、macOS）上运行；TLS 为 emulated TLS；x86_64 Windows 以导出入口与回调跳板转接约定；`os::host()` 在 ok 目标上为普通声明 |

## 1. 定位与分层

```
各项目的领域逻辑     mcpp：目标轴、runtime_search、脚手架；xlings：subos、隔离策略；mcppls：沙箱判定
mcpplibs.os          面向一般开发者：通用 API + 少量平台专属能力；组合 openkal 原子，加上约定
openkal-image        映像约定的参考实现：libokld（加载器与格式）、libokrun（运行器核心）、okrun（启动器）、okld（映像工具）
openkal-kit          只由 kal_* 组合出来的设施（channel、endpoint），不属于规范
openkal              与 OS、内核无关的运行时级 ABI：最小可组合原子、能力查询、映像约定、一致性套件
openkal-<os>         实现：原生模式下随程序链接；ok 模式下随 okrun
```

| | openkal | `mcpplibs.os` |
|---|---|---|
| 读者 | 写 C 库、运行时、加载器、裸机程序的人 | 写工具、服务、IDE 后端、CLI 的一般开发者 |
| 形式 | C ABI（头文件为正本，模块为同一声明的另一种入口） | C++ 模块：RAII、`std::expected`、`std::filesystem::path`、UTF-8 |
| 范围 | 最小、可组合的原子；不能由已有接口组合出来 | 组合与约定：PATH 查找、原子发布、进程树、目录约定、颜色、路径映射、本地 IPC |
| 名字 | 相对目录句柄（preopen），无当前目录，无 `..` | 绝对与相对路径都可用，内部映射到 preopen |
| 差异 | 能力字与按资源查询（SPEC §6.2） | 同样用查询表达；平台专属能力单列，不推荐使用 |
| 变化 | 极严格：规范、SURFACE、所有实现、一致性套件一起变；只增不改（SPEC §8） | 按 semver |

**判断一项能力放在哪一层**（D2）：

1. 是内核层的能力吗？是——拆出最小原子：只用 openkal 的词汇定义，三实现可实现或以查询 / 不提供表达，不能由已有接口组合。
2. 原子之上的丰富功能（命名、默认目录、清理、帧、重试、策略）在 `os` 组合。
3. 不在内核层的环境服务（UAC 同意界面、Dev Drive）与没有可移植含义的机制（Landlock、seccomp、`SCM_RIGHTS`）在 `os` 的按 OS
   实现单元或平台模块。
4. std 已做对的，`os` 不包装。

## 2. 需求清单：四个项目的平台层

| 项目 | 位置与规模 | 底座 | 已有的绕行 |
|---|---|---|---|
| mcpp | `modules/platform`，20 个模块约 6.9k 行 | libc / Win32，约 100 处 `#if` | — |
| xlings | `modules/platform`（partition），约 9.5k 行 | libc / Win32，`platform.cpp` 单文件 33 处 `#if` | — |
| mcppls | `modules/platform` 约 3.1k 行 + `modules/os/{linux,macos,windows}` | openkal（openkal-musl + llvm-runtime 全栈） | WA-PLATFORM-001（macOS SIGPIPE）；PRoot 下禁用 `execveat`；手工声明 7 个 Win32 函数取进程时间；Linux 读 `/proc` |
| mcpp-ide | `src/platform`，约 0.6k 行 | libc / Win32，已按 D8 的形态重构 | — |

| 能力 | 来源 | 需要的语义 |
|---|---|---|
| OS 常量（可执行后缀、PATH 分隔符） | ×4 | 原生模式下编译期已知；ok 模式下运行时得知（§8.6） |
| 启动子进程：argv、环境、工作目录、三条管道 | ×4 | 参数原样到达；子进程只继承指定的句柄 |
| PATH / PATHEXT 查找 | ×4 | 与 shell 一致 |
| 进程树：请求结束、强制结束、孙进程一并结束 | mcpp、xlings、mcpp-ide、mcppls | 进程组 / Job |
| 子进程随父进程结束 | mcpp、xlings、mcpp-ide | — |
| 非阻塞与带时限等待 | ×4 | — |
| 本进程被请求结束的通知与按请求结束 | mcpp（`cancelled` 记录，SPEC-003 的 130/143）、mcpp-ide、xlings | 普通上下文里清理，再以"被请求结束"的状态终止 |
| 自身可执行文件路径 | ×4 | 定位 home、载荷、相邻程序 |
| 原子写、写穿符号链接、不覆盖的发布、文件系统能力 | mcpp、xlings、mcppls | NFSv3 等不支持 rename-noreplace 的卷 |
| 文件锁 | mcpp、xlings | 可等待 |
| 链接：符号链接、目录联接 | xlings、mcpp | Windows 上不需要特权 |
| 环境变量：读、枚举、子进程的环境 | ×4 | — |
| 修改本进程环境变量 | xlings（85 处，多为给 git 子进程设变量） | 不提供（§6.4） |
| 目录约定 | ×4 | XDG、`APPDATA`/`LOCALAPPDATA`、`~/Library` |
| 终端：交互、尺寸、VT、颜色、UTF-8 | mcpp、xlings、mcpp-ide | `NO_COLOR`、`CLICOLOR_FORCE` |
| 标准流的二进制模式与整条写入 | mcpp-ide、mcppls | Content-Length 帧 |
| 本地连接与回环 TCP | xlings（AF_UNIX）、mcppls（回环 TCP） | 访问控制、单实例会合 |
| 进程身份、存活、CPU 时间（按编号） | mcppls（租约、LSP `processId`、预算） | 编号会被复用，需与启动时刻比对 |
| 线程级 CPU（另一个进程） | mcppls（clangd 诊断） | — |
| 机器真实架构 | xlings、mcpp | Rosetta、WoW64 |
| Linux 隔离、提权、Dev Drive | xlings、mcppls | 平台专属 |
| **动态加载**（插件） | MC++ 插件（fork 的动机）、L2 | 一个进程一份运行时 |
| **同架构二进制跨平台** | mcppls、mcpp-ide 的载荷 | 每种架构一份应用逻辑 |

## 3. 实测：openkal 作为唯一底座

探测程序：[mcpp#843](https://github.com/mcpp-community/mcpp/pull/843) 的 `.github/probe/okprobe`——普通宿主程序（`import std`、
`std::jthread`、`std::filesystem`）依赖 openkal 0.16.0 与本平台实现（借用安排），子进程是它自己。9 个组合：linux x86_64（gcc
dev、gcc release、llvm、musl static）、linux aarch64、macos arm64（llvm dev、release）、windows x64（llvm（MSVC ABI）、`cl.exe`）。

| # | 事实 | 范围 |
|---|---|---|
| E1 | `kernel-abi` 解析为 openkal，`c-abi` 保持 glibc / musl；构建、链接、运行正常 | Linux |
| E2 | `cl.exe` 与 llvm（MSVC ABI）都能构建运行；preopen 为当前目录与每个盘符；绝对路径映射为"盘符 + 相对名"后可打开 | Windows |
| E3 | 启动子进程：argv 中的空格、引号、反斜杠原样到达；完整环境（Windows 156 条）、环境覆盖、工作目录、三条管道、退出码正确；4 线程并发启动 20 个子进程，无一读端被推迟 EOF | 9 组合 |
| E4 | `kal_process_channel` 单向：`mine` 为读端；子进程 stdin 需把一对反过来用；规范文字未写方向 | 源码 |
| E5 | `kal_process_job_terminate` 结束子进程及其孙进程 | 9 组合 |
| E6 | `kal_timeout_wait_process` 以 1 ns 为时限即非阻塞等待 | 9 组合 |
| E7 | **openkal-linux 0.17.0**：stop word 的信号返回 trampoline 是无段指令的顶层 `asm`，GCC `-O0` 把它放进 `.bss`，SIGTERM 后 SIGSEGV；GCC `-O2`、llvm、musl `-O2` 正常 | 本机与 CI |
| E8 | stop 处理器用原始 `rt_sigaction` 安装，覆盖宿主处理器，并覆盖启动方继承下来的 `SIG_IGN` | 源码 |
| E9 | 文件：`create|exclusive`、flush（`fsync`）、替换 rename、文件锁、容量、`info` 正确；`..` 被拒绝；APFS 卷报告不区分大小写 | 9 组合 |
| E10 | **openkal-windows 0.12.0 不创建、不读取链接**；`set_executable_at` 为 `not_supported`（符合规范） | Windows |
| E11 | 无"不覆盖的 rename" | 规范 |
| E12 | 无自身映像路径、无本地端点、无本进程环境修改（后者为规范有意） | 规范 |
| E13 | `kal_task_wait/wake` 在宿主 `std::thread` 之间可用；回环 TCP、`kal_timeout_accept` 正确 | 9 组合 |
| E14 | mcpp：已推送的 `$root` 修复让旧版本写入全局缓存的依赖 BMI 不可读（`gcm.cache/gcm.cache/…`） | 本机 |
| E15 | `kal_env_var_at` 返回名字；openkal-windows 拒绝不含 `=` 的环境条目，openkal-linux 接受并传给 `execve` | Windows / Linux |
| E16 | openkal-windows：stop word 返回 word 但 props 不声明；控制台处理器返回 `FALSE`，进程随即被结束；`kal_process_terminate` 为强制结束 | Windows |
| E17 | stop word 在 macOS 只有 arm64；`BOUND_LIFETIME` 只在 Linux | 源码 |
| E18 | macOS arm64：spawn、管道、完整环境、进程树、链接、锁、stop word 正确；不声明 `BOUND_LIFETIME` | macOS |
| E19 | mcpp 2026.10.26.1：任何有依赖的工程中，带映射的单元 `import std.compat` 失败 | 本机 |
| E20 | openkal-linux 在构造函数中忽略 SIGPIPE（借用安排下改变宿主处置）；openkal-macos 未做，写入关闭的管道会结束程序 | 源码 |
| E21 | openkal-windows 以字节写控制台（不经 `WriteConsoleW`），由控制台代码页解释 | 源码 |

### 3.2 实测：ok 映像与 `os::host()`（探测 P）

探测程序：#843 的 `.github/probe/okimage`。`image/app.c` 以 llvm 23.1.3 编译为无 OS 的 ELF（`--target=<arch>-unknown-none-elf
-ffreestanding -fPIC -femulated-tls`，aarch64 另加 `-ffixed-x18`），`ld.lld -shared -z max-page-size=65536` 链接，只导入 8 个 kal_*；
映像在 Linux 上构建一次、作为工件分发。`runner/` 是原型运行器（mcpp + llvm 构建，链接本平台 openkal 实现），映射映像、做
`RELATIVE` / `GLOB_DAT` / `JUMP_SLOT` / `ABS64` 重定位、把 kal_* 绑定到以映像约定导出的入口后调用 `ok_main`。`hostconst/` 在五种
编译器组合上检验 `os::host()` 的两种形态。

| # | 事实 | 范围 |
|---|---|---|
| E22 | **同一份字节**：`app-x86_64.oke`（sha256 `86a8756c…`）在 linux-x86_64 与 windows-x64 上运行，`app-aarch64.oke`（`38b8ef4a…`）在 linux-aarch64 与 macos-arm64 上运行，四处输出一致、退出 0。本机构建得到相同的 sha256：映像构建是可复现的 | 4 个运行点 |
| E23 | **x86_64 Windows 的约定转接**：运行器为每个 kal_* 提供 SysV 入口（clang `sysv_abi`），转调 MS 约定的实现；`kal_task_start` 的回调方向由一个 MS 约定的跳板进入映像的 SysV 入口。8 个参数的混合宽度调用、按值传递的 `kal_stream`、两个上下文的启动与等待都正确 | windows-x64 |
| E24 | **TLS**：映像以 `-femulated-tls` 编译，`__emutls_get_address` 在映像内只用 `kal_task_current` 与 `kal_alloc` 实现；主上下文与两个新上下文各自的 `thread_local` 值互不干扰（1 / 10 / 20），新上下文从初始值开始 | 4 个运行点 |
| E25 | aarch64 映像以 `-ffixed-x18` 编译（Darwin 与 Windows 保留 x18），在 Linux 与 macOS 上运行；Windows ARM64 未测 | aarch64 |
| E26 | macOS arm64：运行器（ad-hoc 签名、未启用 hardened runtime）对匿名映射 `mprotect` 为 RX 被允许；启用 hardened runtime 并公证的 okrun 所需的 entitlement 未测 | macos-arm64 |
| E27 | 64 KiB 段对齐（`-z max-page-size=65536`）使每个段独占保护单位，满足 Linux 4K、macOS arm64 16K、Windows 64K 分配粒度 | 4 个运行点 |
| E28 | `os::host()` 形态 A（`constexpr` 函数，`if consteval` 一支调用非 `constexpr` 标记）：GCC 16、clang 23（Linux、macOS、Windows）接受且在常量表达式中使用时报错；**MSVC 19.44 拒绝函数定义本身**（C3615，未实现 P2448），即使只在运行时调用 | 5 个编译器组合 |
| E29 | `os::host()` 形态 B（ok 目标上为普通声明，原生目标上为 `constexpr` 函数）：五个组合都接受运行时调用，都拒绝常量表达式中的使用并点名 `host`；原生目标上 `static_assert(host() == PLATFORM)` 与 `if constexpr (host() == …)` 在五个组合上成立 | 5 个编译器组合 |

## 4. 覆盖矩阵（rev 3）

**A** openkal 已有、三实现可用；**A\*** 已有、某实现缺（§7.1）；**N** 新原子（§7.2）；**O** `os` 组合；**E** `os` 的按 OS 实现
（非内核环境服务）；**P** 平台专属模块；**S** std 已有；**X** 留在项目。

| 能力 | openkal | `os` |
|---|---|---|
| 启动子进程、管道、环境、工作目录 | A（E3、E4） | O：`os::process::spawn`（stdin 按 E4 反用 channel） |
| PATH / PATHEXT | A：`kal_env_var` + `kal_fs_info` | O |
| 进程树 | A（E5） | O |
| 随父结束 | A\*：Windows K5；macOS 无原语、不声明 | O：查询报告 |
| 等待 | A（E6） | O |
| 结束通知与按请求结束 | A\*（K1–K3、K6）+ N：G4 | O：`os::interrupt` |
| 自身映像路径 | N：G2 | O |
| 原子写、不覆盖发布、卷能力 | A + N：G1 | O：发布策略（含 NFSv3 回退） |
| 文件锁 | A | O |
| 链接 | A\*：Windows K4 | O |
| 环境、子进程环境 | A（E15） | O；本进程修改不提供 |
| 目录约定 | A：`kal_env_var` | O |
| 终端交互、尺寸、输入模式 | A | O |
| 终端输出解释 ECMA-48 | N：G6 | O：颜色与样式 |
| 控制台 UTF-8 输出 | A\*：K10（E21） | — |
| 本地连接 | N：G3 | O：`os::ipc::local`（运行目录、命名、清理、帧、单实例） |
| 回环 TCP | A | O |
| 进程编号、身份、存活、CPU 时间 | N：G5 | O：租约、客户端存活、CPU 预算 |
| 线程级 CPU（另一个进程） | — | E |
| 机器真实架构 | —（不进 openkal） | E |
| 宿主环境（ok 模式下的约定） | N：G8 | O：运行时约定（§8.6） |
| 动态加载 | N：L2（`publish_part`、`granularity`） | — （由 libokld 承担） |
| 提权、Dev Drive | — | E |
| Landlock、seccomp、namespaces、`SCM_RIGHTS` | — | P |
| 遍历、创建、删除 | — | S |
| 目标轴、subos、沙箱判定 | — | X |

## 5. 结论：openkal 足以支撑，不需要第二个后端

1. **共存成立**（E1、E2）：一份 `os` 实现服务 glibc / musl / libSystem / UCRT 宿主与 openkal-musl 全栈。
2. **缺口的性质**：没有一项需要第二个后端——A\* 是实现缺陷（§7.1），N 是满足接纳准则的原子（§7.2），E / P 本不属于内核层。
3. **第二个后端的代价**：每项能力在 POSIX 与 Win32 上再写一遍（四个项目今天的状态），并掩盖 openkal 的缺陷（E7 这类问题只在
   真实消费者上暴露）。
4. **前提**：`os` 发布之前，A\* 与 N 在 openkal 及其实现中发布；`os` 不写临时绕行；mcppls 的绕行随之删除。

## 6. `mcpplibs.os`

### 6.1 接口规则

- **R0** 不重复 std。
- **R1** 能通用就通用：可移植的能力只出现在 `os::xxx`，按平台的实现私有，由 `os::xxx` 转发；平台模块不重复导出。
- **R2** 平台模块只放没有可移植含义的东西，不推荐使用；每个导出写明为什么没有通用形式；CI 检查平台模块与通用 API 无同名。
- **R3** **编译期平台常量是一等接口**：`os::PLATFORM`（枚举：`linux`、`macos`、`windows`、`openkal`……，即编译目标）、
  `os::is_linux` / `is_macos` / `is_windows` / `is_posix`、`os::exe_suffix`、`os::path_list_separator`，供 `if constexpr` 分派；
  `os` 被任何多平台项目使用，不限于 openkal 体系。**`os::host()` 是 `constexpr` 函数**（运行宿主）：原生模式下宿主就是编译目标，
  这是编译时已知的信息，`host()` 可用于常量表达式（`static_assert`、`if constexpr (os::host() == os::platform::windows)`）；ok 模式下
  宿主在编译时未知，`host()` 是普通声明——在常量表达式中使用即为编译错误（点名 `os::host`），运行时由 G8 回答（§8.6，E28、E29）。按资源变化的差异（卷是否区分大小写、能否建链接）用查询
  （`os::fs::caps(path)`）。使用准则：只在原生模式构建的代码可用 `if constexpr (os::is_windows)` 或 `if constexpr (os::host() == …)`；
  也要构建为映像的代码在运行时调用 `os::host()`，原生构建中它被折叠为常量，零开销。
- **R4** 原生句柄不进通用类型。
- **R5** 平台模块处处声明、只在本平台定义：弃置语句中的 odr-use 不要求定义（[basic.def.odr]），非弃置的误用是链接错误。
- **R6** openkal 不是共享形态时，`os` 的资源类型不跨共享对象边界（D6）。

### 6.2 构建形态

```
mcpplibs/os
  mcpp.toml                 [dependencies] openkal；[target.<os>.dependencies] openkal-<os>
                            [build] sources = src/*.cppm src/*.cpp；[target.<os>.build] sources = src/<os>/*
  src/os.cppm               export module mcpplibs.os;
  src/<os>/target.cppm      原生模式下的编译期常量（每个 OS 一份）；openkal 目标下没有这一分区
  src/{process,fs,env,dirs,terminal,stdio,interrupt,net,ipc}.cppm / .cpp   接口 / 基于 kal_* 的实现
  src/<os>/*.cpp            E 类实现与平台模块的定义
  src/{linux,macos,windows,posix}.cppm                                        平台模块接口（R5）
  tests/                    行为测试 + 平台模块误用的链接期失败用例
```

- 接口单元 `import std;`，不含系统头、不用宏；基于 openkal 的实现只 `import openkal.*`。
- Windows 实现单元不包含 `<windows.h>`，自己声明所用函数（`dllimport`、`__stdcall`），因此在 MSVC ABI 与 openkal-musl 两种 C
  环境下都能编译；POSIX 实现单元的系统头只在全局模块片段。
- 实现单元的变化不改变 BMI（mcpp 的 `bmi-equal` restat）。

### 6.3 名称

包 `mcpplibs:os`，仓库 `mcpplibs/os`，模块 `mcpplibs.os`、`mcpplibs.os.<能力>`、`mcpplibs.os.<平台>`，命名空间 `mcpplibs::os`。
形态对应 Python `os`、Go `os` + `x/sys/{unix,windows}`；`mcpplibs.` 前缀避免与用户模块 `os` 冲突（mcpp#732）。

### 6.4 语义决定

- **路径**：接受 `std::filesystem::path`；词法规范化后映射为"最长前缀的 preopen + 正斜杠相对名"（mcppls `resolve_name` 为种子）；
  另提供按链接解析的 `os::fs::resolve`。
- **本进程环境不可修改**（与 openkal 一致）；子进程环境由 `spawn` 指定。
- **结束**：`os::interrupt::requested()` / `wait()` 基于 stop word，处理在普通线程；`os::interrupt::end_as_requested()` 基于 G4。
  mcpp 的取消：看到 word → 清理、写 `cancelled` 记录 → 按请求结束；POSIX 上父进程看到信号状态（shell 得到 130/143），Windows
  上为 `STATUS_CONTROL_C_EXIT`（part 1 rev 4）。
- **EPIPE**：openkal 让写入关闭的管道得到错误而非被结束（E20、K8）；消费方的输出层把它视为正常结束（mcpp 的 `| head`）。
- **错误**：`std::expected<T, os::error>`，可移植分类由 `kal_err_*` 映射，保留原生码。

## 7. openkal 的修改

### 7.1 实现缺陷（规范不变）

| # | 仓库 | 缺陷 | 修复 | 证据 |
|---|---|---|---|---|
| K1 | openkal-linux | 顶层 `asm` 无段指令 | `.pushsection .text,"ax",@progbits` … `.popsection`；CI 增加 GCC `-O0` 的 stop word 用例 | E7 |
| K2 | openkal-linux、openkal-macos | arm 覆盖启动方继承的 `SIG_IGN` | 按 §7.2 的接管规则：armed 即接管，但启动方安排不接收的请求不恢复接收 | E8 |
| K3 | openkal-windows | stop word：props 未声明；处理器返回 `FALSE` | props 声明；`CTRL_C` / `CTRL_BREAK` 返回 `TRUE`（进程继续，由程序处理）；`CLOSE` / `LOGOFF` / `SHUTDOWN` 只置 word，系统随后结束进程，规范写明 | E16 |
| K4 | openkal-windows | 链接不创建、不读取 | 目录用 junction（不需要特权）；文件用 `ALLOW_UNPRIVILEGED_CREATE`，否则 `not_supported`；读取用 `FSCTL_GET_REPARSE_POINT`；props 按实际声明 | E10 |
| K5 | openkal-windows | `BOUND_LIFETIME` 未实现 | 每个子进程一个私有 `KILL_ON_JOB_CLOSE` Job（嵌套 Job），与调用方的 unit 无关 | E17 |
| K6 | openkal-macos | stop word 只在 arm64 | x86_64 待 mcpp 的 macOS x86_64 发布后在 CI 运行（O5） | E17 |
| K7 | openkal（文字）、openkal-linux | channel 方向未写；环境条目格式两实现不一致 | 规范写明 `mine` 为读端、环境条目为 `NAME=value`；两实现一致地拒绝非法条目 | E4、E15 |
| K8 | openkal-macos | SIGPIPE 结束程序 | 以已实测的 `sigaction` 结构忽略 SIGPIPE；mcppls 删除 WA-PLATFORM-001 | E20 |
| K9 | openkal-windows | UNC 路径与启动后挂载的卷不可达 | 增加 UNC 根的 preopen；名字未命中时重新枚举卷；实现前实测 | O7 |
| K10 | openkal-windows | 控制台按代码页解释字节 | 交互式控制台流转为 UTF-16 用 `WriteConsoleW`；实现前实测 | E21 |
| K11 | openkal-linux | PRoot 下 `execveat` | 实现内已有 ENOSYS 回退；补"已知在翻译器下运行时从第一次起用 `execve`"，mcppls 删除其提前设置 | mcppls |

### 7.2 新增原子（S1，各实现随之）

接纳准则：(1) 目标内核都有的原语，或可由实现以查询 / 不提供表达；(2) 不能由已有接口组合而不丢失原子性或引入竞争；(3) 上层确有
需要；(4) 语义只用 openkal 自己的词汇，不带任何 OS 的形状。

| # | 原子 | 中性定义 | 各实现 | 上层需要 |
|---|---|---|---|---|
| G1 | `kal_fs_rename` 的新版本，带 `KAL_RENAME_NO_REPLACE`；`KAL_FS_PROP_RENAME_NO_REPLACE` 按卷 | 目标存在则失败，原子 | `renameat2`、`renamex_np(RENAME_EXCL)`、`ReplaceIfExists = 0` | 不覆盖的发布 |
| G2 | `kal_env_image(char*, cap)` | 环境在启动时告诉程序的、本映像的名字 | `AT_EXECFN` / `/proc/self/exe`、apple[] `executable_path=`、PEB `ImagePathName`；ok 模式下由 okrun 给出映像路径 | 定位 home、载荷 |
| G3 | `kal_net_listen_at(kal_dir, name, len, …)`、`kal_net_connect_at(…)` | 在目录句柄下按名字会合的连接；返回已有的连接与监听句柄；`KAL_FS_PROP_LOCAL_ENDPOINT` 按卷 | macOS `bindat`/`connectat`；Linux 经 `/proc/self/fd/<dir>/name`；Windows AF_UNIX（10 1803+） | `os::ipc::local` |
| G4 | `kal_process_end_as_requested(void)` | 以"未 arm 时该请求本会造成的方式"结束本程序；等待者经 `kal_process_wait` 看到 `terminated`；尚无请求时返回 `kal_err_invalid` | POSIX 恢复默认处置并重发；Windows `STATUS_CONTROL_C_EXIT` | SPEC-003 的 130/143；shell 的循环中断 |
| G4′ | stop word 的接管规则（文字） | arm 后结束请求只经 word 送达；启动方安排不接收的请求不恢复接收 | — | 一个所有者 |
| G5 | `kal_process_self`、`kal_process_open_id(kal_u64, …)`、`kal_process_info(kal_process, wanted, kal_process_info*)` | 编号为环境给出的不透明值，**可能被复用**；信息含编号、启动时刻、处理器时间，`present` 位标明可分辨者（形状同 `kal_fs_file_info`） | pidfd / `/proc`、`proc_pidinfo` / kqueue、`OpenProcess` / `GetProcessTimes` | 租约、客户端存活、CPU 预算 |
| G6 | 终端输出位置 `KAL_TERM_OUT_SEQUENCES` | 环境解释写入的 ECMA-48 控制序列（ECMA-48 为国际标准，非某个 OS） | POSIX 终端恒为真；Windows `ENABLE_VIRTUAL_TERMINAL_PROCESSING` | 颜色与样式 |
| G8 | `kal_env_system(char*, cap)` | 宿主环境的名字（开放集合：`linux`、`macos`、`windows`……）；**规范限定只用于选择约定**，不得用于改变 kal_* 的行为或推断 kal_* 的能力（能力只经查询） | 实现给出常量；ok 模式下为 okrun 所在的系统 | ok 模式下 `os` 的运行时约定 |
| L1 | 文字：一个进程只有一份实现；共享形态的条件（soname 按大版本，导出面即 SURFACE） | — | 各实现增加共享形态构建 | 插件、ok 模式 |
| L2 | `kal_exec_publish_part(region, offset, size)`、`kal_exec_granularity()` | 区域的一部分可执行、其余可写；保护粒度 | `mprotect`、`VirtualProtect`（speak-agent/openkal#1，rebase 到 0.16） | 加载器 |

不进 openkal：机器真实架构（G7，`os` 实现）、线程级 CPU、修改本进程环境、结束请求的原因（SPEC 的预留不变）。

每条原子按 openkal 的既有流程：SPEC 条款与修订说明（§11 记录理由）、头文件与模块、`SURFACE.txt`、`tools/gen-macros.sh` 重新生成、
三个实现、一致性用例；只增不改（SPEC §8）；openkal 升小版本。

## 8. 二进制跨平台：ok 模式

### 8.1 能做到的与做不到的

- **能**：同一 CPU 架构下，一份映像字节在 Linux、macOS、Windows 上运行。映像只含 kal_* 以上的代码（应用、`os`、C 库、C++
  运行时、展开器）；与 OS 相关的全部在宿主侧的 okrun。
- **不能**：一个文件被三个内核直接执行。初始映像的映射由内核完成且只认原生格式（Linux `binfmt_elf`；XNU 的 Mach-O 加载并在
  arm64 上强制代码签名；NT 以 `SEC_IMAGE` 映射 PE），三种格式的魔数都在文件开头、互相冲突。动态链接（`ld.so`、dyld、ntdll Ldr）
  在内核之上，这一层的格式可以自选——ok 模式工作在这一层。

### 8.2 模式

| 模式 | 产物 | 运行 | 用途 | 结论 |
|---|---|---|---|---|
| M1 原生 | 每个 OS 一份原生二进制 | 直接 | 默认；mcpp 自身 | 保留 |
| M2 映像 | 每种架构一份 `app.oke`（可加载库 `libx.okso`；目标文件 `.o`、静态库 `.a` 不变） | `okrun app.oke …`；Linux 可注册 `binfmt_misc` | 开发、IDE 载荷 | 采纳 |
| M3 自包含 | 每个 OS 一个原生可执行文件：libokrun + **以段嵌入的同一份映像** | 直接；可签名、公证 | 面向最终用户 | 采纳 |
| M4 多重格式单文件（APE 式） | 一个文件 | 依赖 shell 兜底 | — | 不采纳：macOS arm64 不能直接运行未签名的加载器；无法 Authenticode 与公证；杀毒误报 |

M3 的映像必须作为段（Mach-O `-sectcreate __OKE __image`、PE `.oke` 节、ELF `.oke` 段）而非追加在文件尾：签名只覆盖签名前的内容。
M3 由链接 libokrun、映像段与一个极小的入口（由 `mcpp pack` 生成）得到，**应用不重新编译**；各 OS 的文件内映像字节相同，可逐字节比对。okrun 不依赖厂商 SDK，
因此一台 Linux 主机即可用 lld 交叉产出三个 OS 的 M3（签名仍在各自平台）。

### 8.3 映像约定（SPEC 新章，规范性）

| 项 | 规定 |
|---|---|
| 调用约定 | 按架构固定：x86_64 为 SysV，aarch64 为 AAPCS64；实现按约定导出 kal_*（Windows x64 由工具从 SURFACE 与头文件生成转接，clang `sysv_abi`；Darwin 可变参数与 Windows x18 由实现在边界处理） |
| 映像 | ELF 子集：`ET_DYN`（PIE）；允许的重定位类型表（含 `DT_RELR`）；无 TEXTREL；段对齐 ≥ 64 KiB；只导入 kal_* 与其他映像 |
| `.note.openkal` | 约定版本、架构、**所需接口**（由导入的 kal_* 推导）；加载器据此在加载时拒绝并说明缺哪个接口（SPEC §6.1 在映像上的对应） |
| TLS | **emulated TLS**（E24，O10 关闭）：映像以 `-femulated-tls` 编译，不访问线程指针（Windows x64 用户态不能设置 `%fs` 基址，macOS arm64 的线程指针对用户态只读）；`__emutls_get_address` 由映像内的 C 运行时在 kal_* 之上实现（以 `kal_task_current` 区分上下文；`kal_task_current` 在上下文结束后可复用，C 运行时在它启动的上下文结束时释放该上下文的槽）。不新增原子 |
| 平台寄存器 | aarch64 映像不得使用 x18（`-ffixed-x18`，E25） |
| 入口 | okrun 以何种状态把控制交给映像入口；`kal_env_*` 由 okrun 的实现回答；G2 回答映像路径 |
| 回调 | 实现调用映像的函数指针（`kal_task_start` 的 `entry`、`kal_space_start`）按映像约定进入；在约定不同的平台（x86_64 Windows）由实现以跳板转接（E23） |
| 接口的有无 | 由加载器在装载时按 `.note.openkal` 的所需接口检查，不满足即拒绝装载：加载器在装载时承担链接器在链接时的职责，SPEC §6.1（"接口缺失由链接器报告"）与 §3.2、§6.2 不需要为 ok 模式另立运行时协商操作。这回应了 openkal 自己的 `2026-08-28-beyond-static-linking-abi-review.md` §9.3 与 §9.5（产物须携带自己的下限） |
| 展开 | 展开信息与异常不出映像；kal_* 边界为 C ABI |
| 版本 | 映像声明约定版本；加载器拒绝更高的主版本；次版本只增 |
| 签名 | `.note.openkal` 可带签名（对映像内容摘要的签名，算法与编码由约定规定，如 Ed25519）；信任根与"是否要求签名"是加载器的策略（okrun 提供要求签名的选项与信任根配置）；`okld sign` 生成签名，`okld check` 验证格式 |
| 双模式 | 原生模式（现有规范）与 ok 模式并列；同一源码 |

参考实现（非规范性，SPEC 与 README 列出）：libokld、libokrun、okrun、okld；一致性套件增加"一致性映像"与"加载器检查"。README 的"程序可以
依赖什么"表增加 ok 模式一行。

### 8.4 `mcpplibs/openkal-image`（新仓库，一个 PR）

仓库为 mcpp 工作空间，含三个包（index 中保持 openkal 家族前缀）：`openkal-ld`（lib 目标 `okld` → `libokld`）、`openkal-run`
（lib 目标 `okrun` → `libokrun`）、`openkal-tools`（bin `okrun`、`okld`）。分成三个包的原因：mcpp 的 lib 目标 `X` 产出 `libX`，同一个包
里 bin 与 lib 不能同名。一个仓库、一个 PR、一个版本号。命名：格式为 **ok 格式**（openkal image format）；`okrun` 读作 openkal run，
`okld` 读作 openkal ld；库与程序一一对应（`libokld` / `okld`，`libokrun` / `okrun`）。`libokld` 是运行时加载器而 `okld` 是构建机工具，
同词根、不同角色，与 GNU 的 `ld` / `ld.so` / `libdl` 相同，文档写明。

| 目标 | 性质 | 内容 |
|---|---|---|
| `libokld`（包 `openkal-ld`，lib 目标 `okld`） | 加载器，且是**映像格式定义的唯一实现**（freestanding C++，不用异常与 RTTI；对外 C ABI 头文件，前缀 `okld_*`，另有同一声明的模块形式） | 格式（ELF 子集、重定位表、`.note.openkal`、签名）、映射、重定位（x86_64、aarch64；riscv64 随一致性套件）、绑定、TLS、`DT_NEEDED`、`dl_iterate_phdr` 支持、GDB JIT 登记；使用者：libokrun、`okld`、openkal-musl 的 `dlopen`。源自 speak-agent/openkal-musl 的 `okm_dl*` |
| `libokrun`（包 `openkal-run`，lib 目标 `okrun`） | 运行器核心（C ABI 前缀 `okrun_*`，另有模块形式） | 以 libokld 装载映像，把映像导入的 kal_* 绑定到本进程那一份 openkal 实现，交出入口、处理退出；依赖 openkal、openkal-ld、`[target.<os>.dependencies]` openkal-<os>（`standalone`）；源码无平台分支；使用者：`okrun`、M3、在进程内承载 ok 映像的其他宿主 |
| `okrun`（包 `openkal-tools`） | bin（mcpp + C++ Modules） | libokrun 之上的薄 `main`；不链接宿主 C 库，进程内只有映像里的一套 C / C++ 运行时 |
| `okld`（包 `openkal-tools`） | bin（构建机工具） | `okld check`（约定检查）、`okld note`（推导所需接口写入 note）、`okld sign`（签名）、`okld inspect`；复用 libokld 的格式定义；不替代链接器；非 mcpp 的工具链也能用 |

发布：okrun 的 macOS 版签名并带可执行内存 entitlement、公证；Windows 版 Authenticode；okrun 构建**不启用 CFG**，或在映射映像时登记
有效调用目标（否则 CFG 拦截进入映像的间接调用）。

### 8.5 mcpp 的 `<arch>-openkal` 目标（#842）

| 项 | 内容 |
|---|---|
| 目标 | OS 值 `openkal`（`<arch>-openkal`、`cfg(os = "openkal")`）；映射到编译器三元组 `<arch>-unknown-none-elf` 与参数（`-fPIC -femulated-tls`，aarch64 加 `-ffixed-x18`；链接 `-shared -z max-page-size=65536 -z now`，E22–E27） |
| 目标侧 | 只固定 `kernel-abi = openkal`；`c-abi`、`c++-abi` 由依赖图的 `provides` 决定；目标表给出默认提供者（openkal-musl、openkal-llvm-runtime），可替换；不带 C 库的映像合法 |
| 产物 | bin → `.oke`；shared → `.okso`；static → `.a`；链接后调用 `okld check` 与 `okld note`，失败即构建失败并指出违反的条款 |
| 运行 | `run` / `test` 经已有的 `[target.<triple>].runner`，使用依赖图中宿主平台的 okrun（从源码构建） |
| 打包 | `pack` 增加 M2（映像）与 M3（每个 OS：libokrun + 映像段）格式 |
| 其他 | 编译数据库与 clangd 的目标参数；文档 21、22、24、50；e2e：同一映像字节在各 OS 运行 |

### 8.6 `os` 在 ok 模式下

`<arch>-openkal` 目标上 `os::PLATFORM == os::platform::openkal`，`is_linux` / `is_macos` / `is_windows` 均为 `false`（编译目标
不是任何一个 OS），`os::host()` 是**普通声明**（形态 B，E29；不用 `if consteval` 形态，因为 MSVC 拒绝，E28），在常量表达式中使用
即编译错误并点名 `os::host`，运行时由 G8 回答；原生目标上 `os::host()` 是返回 `PLATFORM` 的 `constexpr` 函数。`exe_suffix` 等按宿主而变的常量在该目标上不提供（误用即编译错误），由 `os::host()` 回答；约定（PATH 分隔符、
PATHEXT、配置目录、可执行后缀）由 G8 与按资源查询决定；平台模块只有声明（R5）。原生模式下一切不变。
消费方中以 `if constexpr (os::is_windows)` 写的代码，凡也要构建为映像的，改用 `os::host()`（mcpp-ide 的平台层即在此列）。

### 8.7 第一方使用者

- **mcppls**：已在 openkal 栈；同时产出 M1 与 M2 / M3；VSIX 载荷为一份映像 + 各 OS 的 okrun（或 M3）；不依赖 xlings。
- **mcpp-ide 后端**：映像构建切到 openkal-musl + openkal-llvm-runtime（它的选择，非约定的要求）；保留 M1，新增 M2 / M3；VS Code
  与 JetBrains 适配层按"原生后端"或"okrun + 映像"启动；调试配置对 `.oke` 目标生成 okrun + lldb（JIT 接口）。
- **mcpp 自身**：保持原生（工具链探测、PE / Mach-O 产物、xlings 集成）。

## 9. openkal-musl 与 openkal-llvm-runtime

- **openkal-musl**（一个 PR）：`dlopen`/`dlsym`/`dladdr`/`dl_iterate_phdr` 改用 libokld（包 `openkal-ld`；上游化 fork 的成果，不再自带加载器）；
  映像形态（无按 OS 的移植层，入口由 okrun 给出，TLS 按约定）；`examples/dlopen`（11 项检查）、`examples/early-constructor` 进 CI。
- **openkal-llvm-runtime**（一个 PR）：为 `<arch>-openkal` 构建 libc++、libunwind、compiler-rt。
- 二者是默认提供者，不是约定的一部分（D7）。

## 10. mcpp（#842）

### 10.1 接入 `os`

`modules/platform` 中 A / A\* / N / O 的部分改为 `mcpplibs.os`；领域部分保留；`bounded_process` 由 `os::process` 替代；取消路径按
§6.4；EPIPE 视为正常结束；`axis` 的架构宏改用 `os`；`modules/platform` 中 `#if` 归零。

### 10.2 GCC 模块映射（D11，替换 `9016c0b1`）

缺陷（2026.10.1.2 起）：根包中只导入根包模块的单元不给映射（GCC 默认映射器，仓库根 `gcm.cache`），带映射的单元仓库根为空；GCC 在
导入方的仓库根下按 BMI 记录的路径查找**间接**导入，两种写法相遇即失败（e2e 940；E19 的 `std.compat` 同因）。

`9016c0b1` 的 `$root gcm.cache` 统一了构建内的约定，但全局缓存里旧约定写入的依赖 BMI 被复用而读不出（E14）。"有映射就全部给映射、
不写 `$root`"不修 E19（`std`、`std.compat` 由默认映射器构建）。**仓库根只能统一为 `gcm.cache`**。

方案：保留 `$root`；`cache_key.cppm` 的 E 轴对 GCC 下**提供模块的包**加入 `bmi_path_convention = "$root=bmi-dir"`——含模块的依赖
重建一次，对象文件包的键不变；不提升 `kCacheEpoch`。测试：940 增加 C（带映射的单元 `import std.compat`）；升级通道（`ci-upgrade.yml`）
增加 `modules` 变体：上一个发布用 GCC 构建依赖 openkal 的工程（`openkal.timeout` 间接导入 `openkal.datagram`）填充缓存，新版本
第一次构建成功且不取旧 BMI、第二次从缓存取；`test_module_address` 断言 `$root`；`cache_key` 单测。本机实测：只有 `$root` 而无此轴的
构建在 2026.10.26.1 填充的缓存上失败（`openkal.types: failed to read compiled module`），加轴后通过。

### 10.3 范围

wasm 回归、§10.1、§10.2、§10.4、§8.5；版本 2026.10.26.2，标题追加范围。代价：2026.10.26.1 的 wasm 回归（mcpp-plugins `dist-web`）在发布前
持续存在。

### 10.4 `trim_paths` 的映射按包给出（mcpp#844，D13）

**缺陷**（2026.10.24.1 起）：`trim_map` 由整个计划的包集合生成，`flags.cppm` 把整张表追加到全局编译参数。工作空间中所选成员
变化时（`-p cli` / `-p gui`），公共依赖的命令行随之变化，ninja 判其过期而重编，两个成员在同一个配置目录里来回覆盖公共对象。
这违背 #751 与 e2e 872 的不变量（"成员无论如何选择只编译一次"）；872 只在 dev profile（`trim_paths` 默认关闭）下运行，因此没有
发现。本机复现（2026.10.26.1，gcc，三成员工作空间）：`--release` 下 `-p cli`、`-p gui`、`-p cli` 每次都重编 `core`；dev 下只编
第一次。

**修复**：一个编译单元的映射只含固定根与**它所属包及其依赖闭包**的包根——这些是它的输出中可能出现的全部路径（自身源码、依赖的
头文件与模块、生成文件、工具链与 store）。集合只取决于包的闭包，与所选成员无关；按路径长度、再按字典序排序（保持"最长前缀
优先"的既有约定）。映射进入按包的编译参数（与模块映射同一处），不再进入全局参数。全局缓存键已含依赖闭包（Merkle 的 F 轴），
不需要新轴。先例：Cargo 的 `trim-paths` 按 crate 给出 `--remap-path-prefix`。

**测试**：e2e 872 增加 F（`--release`，`trim_paths` 开启）：交替选择成员时公共依赖不重编——即 issue 的 CLI → GUI → CLI，在
2026.10.26.1 上失败（`-p app` 重编 3 个单元）、本版通过；`ninja_backend` 单测断言共享参数不含映射、一个单元只带它所属包的映射、
映射集合等于固定根加闭包且按前缀长度排序。

## 11. 交付

### 11.1 每个仓库一个 PR

| 仓库 | 内容 |
|---|---|
| mcpplibs/openkal | S1：G1–G6、G4′、G8、L1、L2、K7 文字；映像约定章；一致性用例（原子、共享形态、一致性映像、加载器检查）；README ok 模式一行 |
| mcpplibs/openkal-linux | K1、K2、K7、K11；G1–G6、G8、L2；共享形态；按约定导出 |
| mcpplibs/openkal-macos | K2、K8；G1–G6、G8、L2；共享形态；按约定导出 |
| mcpplibs/openkal-windows | K3、K4、K5、K9、K10；G1–G6、G8、L2；共享形态；按约定导出（SysV 转接） |
| mcpplibs/openkal-musl | §9 |
| mcpplibs/openkal-llvm-runtime | §9 |
| **mcpplibs/openkal-image（新建）** | §8.4 |
| **mcpplibs/os（新建）** | §6；CI：原生 9 组合 + `<arch>-openkal` |
| mcpp-community/mcpp（#842） | §10 |
| **mcpp-community/mcpp-ide（新建）** | part 1 §14 的全部 + `os` + M1 / M2 / M3 + 调试配置 |
| Sunrisepeak/mcpp-language-server | #71 + `os` + 删除 WA-PLATFORM-001、PRoot 提前设置、vendored openkal-linux + M2 / M3 载荷 |
| mcpplibs/mcpp-index | openkal 系新版本、openkal-image、os、mcpp、`mcpp.plugins` 0.20.0 |
| mcpp-community/mcpp-plugins（#51） | 引擎 pin 到新 mcpp |

### 11.2 依赖链

```
P  探测 PR（D12）：已完成（§3.2）
S1 openkal ─┬─→ 三个实现（K*、N、共享形态、按约定导出）
            ├─→ openkal-musl、openkal-llvm-runtime（映像形态、<arch>-openkal）
            └─→ openkal-image：libokld → okld → libokrun → okrun（libokrun 需三个实现的 standalone）
实现 + musl + runtime + image ─→ os（原生 9 组合 + <arch>-openkal）
os + image ─┬─→ mcpp #842：D11（可在 S1 之前单独完成并跑 CI）→ os 接入 → 取消路径 → <arch>-openkal 目标 → 文档
            ├─→ mcpp-ide：os → 会话测试 → 协议 schema → 生成器 → webui → vscode → jetbrains → agents → M2/M3 → CI
            └─→ mcppls：#71 → os → 删除绕行 → M2/M3
发布：S1 → 实现 / musl / runtime（tag）→ image（tag）→ index → os（tag、index）→ 消费方改为版本依赖、CI 再绿 → 合入
      → mcpp 发布（取消 main CI、release.yml、gtc 补 GitCode、重跑 main CI、合入 index bump）→ mcppls / mcpp-ide 发布
      → plugins #51 → index → 净室生态验证
```

### 11.3 分支引用

上游 PR 之间、消费方对上游，CI 中以 `{ git = …, branch = "<PR 分支>" }` 引用；合入前改为版本，`mcpp.lock` 中不留 git 依赖（CI 检查）。

## 12. 验收

1. **同一映像字节**：每种架构构建一次 `.oke`，CI 记录 sha256，在 x86_64（linux、windows）与 aarch64（linux-arm、macos-arm64、
   windows-11-arm）上经 okrun 与 M3 运行 `os` 的行为测试与 mcpp-ide 后端会话测试；M3 内的映像字节与 `.oke` 相同。
2. `os` 原生 9 组合全绿；平台模块误用的链接期失败用例；GCC `-O0` 的 stop word 用例（K1）。
3. openkal 一致性套件：新原子、共享形态（两个映像共用一份实现的句柄与 stop word）、一致性映像、加载器检查。
4. openkal-musl：`examples/dlopen`、`examples/early-constructor`。
5. mcpp：940（A、B、C）、941 与 872 的 release 一遍（#844）、升级 e2e、全量 e2e；`modules/platform` 中 `#if` 为 0；mcpp-plugins 的 `ci.yml -f mcpp_source_ref`。
6. mcppls 删除绕行后 conformance 与 nightly 不回退。
7. 发布后净室生态验证。

## 13. part 1 的收尾

| 项 | 状态 | 去向 |
|---|---|---|
| mcpp / xlings 2026.10.26.1、index #568、xim-pkgindex #1081/#1082 | 已发布 | — |
| wasm 回归 | 修复在 #842 | §10.3 |
| plugins #51 | 待新 mcpp | §11 |
| mcppls #71 | 未开始 | §11 |
| mcpp-ide | 后端本地实现并对真实 mcpp 冒烟通过；平台层已重构；其余未完成；仓库未建 | §11 |
| mcpp-vscode | 不动（mcpp-vscode#23） | 不变 |
| 临时 PR #840、#843 | 未关闭 | #840 随本设计关闭；#843 在探测 P 合并入后关闭 |
| 净室验证 | 未做 | §12 第 7 条 |

## 14. 回归与缺陷记录（本轮）

| # | 现象 | 根因 | 处理 |
|---|---|---|---|
| B1 | 2026.10.26.1：Emscripten 不写 `.wasm` | 临时名链接 | wasm 原地链接（#842） |
| B2 | 间接导入找不到 BMI | §10.2 | D11 |
| B3 | B2 的首个修复让旧缓存不可读（已推送，未合入） | 缓存键不含映射内容 | D11 + 升级 e2e |
| B3a | `import std.compat` 在带映射的单元失败 | 同 B2 | D11 |
| B4 | openkal-linux stop word 在 GCC `-O0` 下 SIGSEGV | E7 | K1 |
| B4a | openkal-macos 写入关闭的流时进程被结束 | E20 | K8 |
| B5 | openkal-windows 不支持链接；stop word 声明与语义 | E10、E16 | K3、K4 |
| B6 | part 1 实施中修复的问题 | 见 part 1 | 已随 2026.10.26.1 发布 |
| B7 | release 下切换工作空间成员时公共依赖反复重编（mcpp#844） | 全局参数携带整个计划的 `trim_paths` 映射 | D13 |

## 15. 生态级多视角自审（rev 3）

**架构**

- 分层一致：每项需求都落到"openkal 原子 / `os` 组合 / E / P / X"之一（§4），无一项以第二个后端实现。
- 映像约定是 openkal 的定义扩展（从"声明级可移植"到"架构级二进制约定"），按 README 首句"portable kernel application binary
  interface"仍在其职责内；它不涉及内核修改，全部在用户态。
- 参考实现与规范分离（openkal-image 不进 SPEC 正文）；加载器从 openkal-musl 移出，使 okrun 不依赖 C 库。
- **发现**：最初的名称 `krun` 与 containers/crun 构建 libkrun 支持时安装的 `/usr/bin/krun`（Fedora / RHEL 的 `crun-krun` 包）
  重名，`krunner` 与 KDE KRunner 重名；改为 `okrun` / `okld`（O9）。

**稳定性**

- B3 在自审中由实测发现（E14），D11 以升级 e2e 守护；E7 只在 GCC `-O0` 出现，各 CI 矩阵增加 gcc dev。
- ok 模式的两个硬点（TLS、Windows 转接）先探测再定案（D12），探测失败只修订 §8，不影响 §6、§7、§10 的其余部分——它们在各 PR
  中是独立的检查点。
- **发现**：okrun 若启用 CFG，会拦截进入映像的间接调用；已写入 §8.4（不启用或登记有效调用目标）。
- **发现**：Windows 上映像内的硬件异常没有 SEH 展开表，WER 的崩溃栈不完整；okrun 的崩溃处理打印映像相对地址，`okld inspect` 可
  符号化；C++ 异常走映像内的 DWARF，不受影响。

**优雅简洁**

- 新原子都复用已有类型（G3 返回已有连接句柄，G5 照搬 `kal_fs_file_info` 的形状，G4 以 `kal_process_wait` 的 `terminated` 定义）。
- G4 不引入原因词汇；G8 以规范限定用途，避免它成为"按 OS 分支"的入口。

**用户体验**

- 最终用户拿到的是 M3：普通的、已签名的原生可执行文件；开发与 IDE 用 M2。
- 调试：gdb、lldb（含 lldb-dap）经 JIT 接口看到映像符号；WinDbg 与 Visual Studio 调试器看不到，文档写明，mcpp-ide 对 `.oke` 目标
  生成 lldb 配置。
- `| head` 下 mcpp 不再被 SIGPIPE 静默结束：输出层把 EPIPE 视为正常结束。

**兼容与无感升级**

- 原生模式行为不变；ok 模式是新增目标，不影响已有构建。
- D11 只重建含模块的依赖一次。
- **发现**：mcppls 以 `=` 精确 pin openkal-llvm-runtime 0.15.1、openkal-musl 0.19.1，并 vendored openkal-linux 0.15.1；本批的
  openkal 系新版本要求 mcppls 一并更新 pin 并删除 vendored 副本（已列入 §11）。
- 映像约定带版本，加载器拒绝更高的主版本，次版本只增。

**跨平台**

- 原生 9 组合已实测；ok 模式的五个运行点（§12 第 1 条）由探测与 CI 覆盖。
- **发现**：G3 在 Windows 需要 10 1803+ 的 AF_UNIX；更早的系统以 `KAL_FS_PROP_LOCAL_ENDPOINT` 不声明报告。G5 的 `open_id` 在
  Linux 需要 pidfd（5.3+），更早的内核以查询报告。
- macOS 上 okrun 需要可执行内存 entitlement 与公证；Linux 的 SELinux 默认策略允许匿名可执行映射（`execmem` 在受限域中可能被拒，
  okrun 以错误报告）。

**一致性**

- K7 统一 channel 方向与环境条目；G8 的值集合与 mcpp 目标词汇（`linux`、`macos`、`windows`）一致。
- `os` 的错误分类由 `kal_err_*` 映射，三平台一致。

**安全**

- 映像相当于解释器的输入，不受 Gatekeeper 与 SmartScreen 检查；签名与公证只覆盖 okrun。面向最终用户的分发用 M3（签名覆盖嵌入的
  映像段）；M2 用于开发与本身已签名的载荷（VSIX、IDE 插件）。
- 映射可执行内存的加载器可能触发杀毒启发式；M3 以签名缓解。

**测试覆盖**

- 规范：一致性套件新增四类（§12 第 3 条）；实现：各自 CI；`os`：行为测试 + 链接期失败用例；消费方：交叉验证 + 同一映像字节运行。

**发布与依赖链**

- 链条长：S1 → 三实现 → musl / runtime → image → os → 三消费方。D11 不依赖 openkal，可先行完成并跑 CI。
- wasm 回归在 #842 发布前持续存在（O1 已定，接受）。
- GCC 16 在模块边界上的问题（mcpp `unix_terminal` 的注释）：`os` 接口使用 std 类型；mcpp-ide 已无问题，mcpp 由交叉验证确认，
  若复现在 `os` 一侧解决。

**rev 5 的补充自审**

- **libokrun 被原生宿主程序嵌入时的运行时边界**：宿主进程有自己的 C / C++ 运行时（如 glibc + libstdc++），映像带自己的一套
  （如 openkal-musl + libc++）。openkal README 规则 3 的单位是"映像"而非进程；part 1 D9 允许"孤岛"，前提是声明且可机械验证——
  ok 格式"只导入 kal_* 与其他映像"正是 `okld check` 可机械验证的条件，因此映像在宿主内是一个合格的孤岛：跨边界的只有 kal_* 的 C
  ABI，不传递任何运行时对象。`okrun` 程序自身以 standalone 构建，进程内只有映像那一套。
- **嵌入宿主中的进程级状态**：映像与宿主共用一份 openkal 实现（L1），stop word 也只有一份；映像内的 `os::interrupt` arm 即接管
  （G4′）。libokrun 的宿主 API 写明这一点，由宿主决定是否允许映像 arm。
- **libokld 的分层**：格式解析（只处理字节，不调用 kal_*）与装载（`kal_exec`、`kal_fs`）分开，使 `okld` 作为普通构建机程序在任何
  宿主上解析与检查映像，装载部分只在需要时链接。
- **版本**：三个包同一版本号；libokld 声明它支持的映像约定最高版本，`okld check` 与 libokrun 都据此拒绝更高主版本的映像。
- **M3 的交叉产出**：libokrun 以 standalone 方式构建，不依赖厂商 SDK；mcpp 已有 openkal 交叉构建（三目标 × 四主机）的 CI，
  在一台主机上产出三个 OS 的 M3 是同一机制。
- **文件关联**：本批不改 xlings，`.oke` 不注册系统关联；M2 只以显式的 `okrun app.oke` 用于 IDE 载荷与开发，面向最终用户的是 M3。
- **符号前缀**：`okld_*`、`okrun_*` 不使用 openkal 保留的 `kal_` 前缀。

**rev 6 的补充自审（探测之后）**

- **探测覆盖的范围与没有覆盖的**：覆盖四个运行点、两种约定、回调、emulated TLS、可复现构建。未覆盖：Windows ARM64（GitHub 的
  `windows-11-arm` 上 mcpp 无发布）、macOS x86_64、hardened runtime 下的可执行内存 entitlement（E26）、映像之间的 `DT_NEEDED`
  （`.okso`）、C++ 运行时（libc++、libunwind）在映像内的异常与展开、`dl_iterate_phdr`。这些进入 openkal-image 与 openkal-musl
  的 PR 的 CI（§12 第 1、4 条），不再单独探测；其中 Windows ARM64 记为 O12。
- **emulated TLS 的代价**：探测中的实现用全局锁与线性查找，只为证明正确性；C 运行时的实现须为每上下文无锁访问（按
  `kal_task_current` 的哈希槽，或在 C 运行时自己启动上下文时预先登记）。`thread_local` 密集的代码（libc++ 的异常全局量）在
  ok 映像中比原生模式慢，记入文档；性能验收以 mcpp-ide 后端的会话测试耗时对比原生模式给出上限。
- **`kal_task_current` 的复用**：规范只保证"同一时刻唯一"；C 运行时必须在它启动的上下文结束时释放槽，否则复用的编号会读到前一个
  上下文的 TLS。主上下文与非 C 运行时启动的上下文不受影响。已写入 §8.3。
- **约定转接的生成**：探测中的 8 个入口是手写的；实现中由工具从 `SURFACE.txt` 与头文件生成，并对含函数指针参数的操作
  （`kal_task_start`、`kal_task_start_sized`、`kal_space_start`）生成跳板。生成器与其产物的一致性纳入 openkal-windows 的 CI。
- **openkal 自己的先前结论**：openkal `2026-08-28` 评审 §9.2 指出"同一二进制遇到第二个实现"的判据在生态中无法构造；E22 正是这个
  判据——同一份字节遇到四个实现。§9.5（产物携带下限）由 `.note.openkal` 回答，§9.6（映射粒度）由 L2 的 `kal_exec_granularity` 回答。
- **`os::host()` 两种形态**：只采用形态 B；`if consteval` 形态在 MSVC 修复 P2448 之前不用。

## 16. 开放问题

| # | 问题 | 处理 |
|---|---|---|
| O1 | #842 的版本号 | 已定：2026.10.26.2，标题追加范围 |
| O4 | 环境条目的统一拒绝写入规范（K7） | 建议采纳 |
| O5 | macOS x86_64 的 stop word 在 CI 上运行 | 依赖 mcpp 的 macOS x86_64 发布；不阻塞 arm64 |
| O7 | Windows UNC 与后挂载卷 | 已定：K9，实现前实测 |
| O9 | 名称 | 已定：ok 格式；`.oke` / `.okso`；`libokld` / `libokrun`；`okrun` / `okld`（§8.4）。查重：`krun` 与 crun 的 libkrun 启动器重名、`krunner` 与 KDE KRunner 重名、`.ok` 为 Oak 语言源文件与测试期望输出的惯用扩展名，均未采用；`okld` 与 OKWS 的守护进程同名、`.okso` 为 okso.app 的文档格式，影响小，记录在此 |
| O10 | TLS 模型、编译器三元组 | 已定（E24）：emulated TLS；映像编译三元组 `<arch>-unknown-none-elf`，aarch64 加 `-ffixed-x18` |
| O12 | Windows ARM64 的 ok 映像运行（x18、AAPCS64 与 Windows ARM64 约定的差异） | 进 openkal-windows 与 openkal-image 的 CI；mcpp 在 windows-11-arm 上的发布是前提 |
| O11 | 映像签名 | 已定：进映像约定（§8.3），`okld sign`，okrun 的签名策略 |
