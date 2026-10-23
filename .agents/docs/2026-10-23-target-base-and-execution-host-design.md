---
subject: toolchain
status: landed
---

# 目标基座与执行宿主：sysroot 的通用模型、运行器契约、自备输入，以及同批的发布产物路径（#832）

日期：2026-10-23。分析基线：`4c80028c`（mcpp 2026.10.22.1）。实现：mcpp 2026.10.23.1。

**修订记录**

- rev 1–4（评审前）：见附录 A。
- rev 5（实现）：按实现修订以下几处，理由写在各节。
  - §3.2：每行合法的写法由三元组的平台类别决定（`mcpp.toolchain.base::row_base`），没有给 `kKnownTargets` 加两列。同一类别的行接受同样的写法，按类别写一张表，目标表中没有的行也能得到回答。
  - §3.1：去掉 `[target.<triple>.sysroot.c-abi]`。一个写在 `*-linux-gnu` 行上的 sysroot 呈现的就是该行的 C 环境；呈现别的环境，就是另一个目标行。`[c-abi]` 块留给由依赖图构建的 C 库（§11）。
  - §4.1：`Toolchain` 的五组基座字段迁入成员 `tc.base`（类型 `base::Resolution`）。Apple 交叉行的 SDK 记在 `tc.base.apple`，原生 macOS 不陈述时仍取 `xcrun` 的回答，以保证默认构建不变。
  - §4.12：`fallback/sysroot_complete` 没有删除，改为只作用于生态自己的 subos 视图（与载荷同在一个 home 之下）。按路径命名的工具链报告的厂商 sysroot 不再被写入。
  - §3.3：`--sysroot` 是本次调用的 `MCPP_SYSROOT`，在 build、run、test、pack、why 与 emit 上声明，与 `--toolchain` 同形。
  - §15.4：`ndebug = false` 是唯一的改回方式，没有给 `defines` 引入 `!NAME` 语法。`defines` 是集合，增加取反语法会让这一节扩大成清单语法的修改。
  - §15.3：cl.exe 不做探测，直接如实报告"本次未裁剪"（`diag::note`，不会被 `--strict` 升级为错误，因为构建的行为没有改变），没有使用未文档化的开关。
  - §15.5 R4：检查门匹配构建机的检出目录（`GITHUB_WORKSPACE`），不用 `/(home|tmp)/`：二进制本身含有 `/tmp/`、`/home/` 字面量，动态链接的本地构建的 loader 与运行路径指向工具链存储（那是 pack 改写的对象，不是源码路径）。本地实测：旧发布的二进制命中 `/home/runner/work/mcpp/mcpp/modules/libs/src/json/json.hpp`，新引擎构建的 mcpp 不再含检出目录。它作用于 linux-x86_64（由本次发布的引擎在 pack 时重建）与 macOS（第二阶段自举）；Windows 与 aarch64 由引导用的旧版引擎构建，在引导版本前移后自然得到裁剪。
  - pack 的下限写为 `HOST-REQUIREMENTS` 中的 `capability=c-abi.glibc>=X discovery=PT_INTERP`，并打印 `Requires glibc >= X`。
  - 新增拒绝码 `target-base`。
  - §15.2：R3（e2e 932）在 CI 上实测出链路上的第二处缺陷，位于 xlings：脚本类型包的别名写作未加引号的 `xlings script <路径>`，shim 按空白切分它（`failed to load script: package file not found: /tmp/…/mc`）。处理分两处：xim-pkgindex#1035 让 gcc 的 hook 以 argv 形式直接运行 `xlings script <脚本>`，不经别名，对已发布的所有 xlings 都成立，本次发布因此不依赖 xlings 的发布；openxlings/xlings#699 给别名中的路径加引号，修正根因。"不需要改 xlings"这一判断在实测后改为"本次发布不依赖 xlings 的修复，但 xlings 有一个需要修的缺陷"。索引侧另需修复的调用有 53 处，多于 §15.2 原先估计的范围。
  - §15.2：R3 在 CI 上实测出链路上的另两处缺陷。其一在 mcpp：build.ninja 中的工具变量（`cxx`、`cc`、`ar`、`mcpp` 等）与工具链路径参数（`-B`、`-isysroot`、`-ffile-prefix-map` 等）只做了 ninja 转义、没有按宿主加引号，`/bin/sh` 在空格处切开（`/bin/sh: 1: /tmp/…/mc: not found`）；工具变量改为 `ninja_tool_word`，路径参数改由 `escape_path` 在需要时加引号，不需要引号的路径逐字节不变，`read_engine_binding` 同时读回带引号的写法。其二在 glibc 载荷：重定位把安装目录写进 `libc.so`、`libm.so`、`libm.a` 三个 GNU ld 脚本，ld 在空白处切分未加引号的文件名；xim-pkgindex#1044 在重定位之后给含空白的名字加双引号。
  - §15.5 R3：实测确认，空格路径可以构建；非 ASCII 字符（`家`）使 GCC 16 的 module mapper（libcody）在预编译 std 模块时失败（`malformed string … INCLUDE-TRANSLATE`）。这是上游限制：同一份 gcc 放在仅含空格的目录下可以构建。mcpp 在该错误之后点名原因与出路（ASCII 路径的 home，或 llvm）；R3 改为空格 home 必须能构建，非 ASCII home 要么构建、要么给出这条诊断。docs/20 记为已知工具链限制。
  - §15.3：`trim_paths` 不作用于 `mcpp test`（以及与之同面的 configure 过程）。CI 实测：mcpp 自己的单测在 release 默认档下借 `__FILE__` 定位仓库，被裁剪成 `workspace@0.0.0/src` 后全部找不到。测试二进制在构建处运行，不发布；`ndebug` 照常作用于测试（与 Cargo 的 release 关闭 `debug_assertions` 一致）。
  - §4.5：clean specs 不恢复 `%(sysroot_spec)`。回归 e2e 220 实测：链接器拿到 `--sysroot` 后，会把 sysroot 前缀加到每个输入共享库的 DT_RUNPATH 上，SubOS farm 中 `libEGL.so` 的 RUNPATH 以绝对路径指向载荷，于是 `libGLdispatch` 找不到。hello 程序逐字节相同的测量只覆盖了没有这类输入的链接。改为：被陈述的 gnu-sysroot 基座由渲染器给 gcc 显式加上 `-Wl,--sysroot=<root>`（clang 的驱动自己把 `--sysroot` 交给链接器），生态缺省视图不变。SPEC-006 §4.3 按此改写。

关联：mcpp#755（按路径命名的工具链，`sysroot` 键由此引入）、mcpp#378（clean link specs）、
mcpp#400（运行时闭包校验）、mcpp#392（运行时绑定先于工具链解析）、**mcpp#832**（home 路径含空格；
发布产物残留构建机路径，本批的 §15）；
[SPEC-002](../../docs/specs/target-side.md)、[SPEC-004](../../docs/specs/manifest-semantics.md)、
[SPEC-006](../../docs/specs/toolchain-management.md)。issue：待建（sysroot 部分）。

复现：[2026-10-23-target-base-and-execution-host-repro.sh](2026-10-23-target-base-and-execution-host-repro.sh)
（只用 mcpp home 中已有的载荷，场景 A–K；`FETCH_DEBIAN=1` 时另取一份 Debian 11 glibc 2.31）。

读者：评审与实现者。本文由一次讨论收敛而来，从"自定义 sysroot 带自己的 glibc"的实测开始，
扩展到全平台 sysroot 的含义、Linux 上的 Apple SDK 与模拟器、引擎与插件的边界。
问答与结论见附录 A。

**本设计的边界**：

- **核心知识判据（D18）**：引擎只持有两类知识。一是通用的抽象与契约；二是**核心内置目标行所在平台的官方约定**，例如 glibc 的 loader 名与 Debian multiarch、Apple 自己的 `SDKROOT` 与 `xcrun`、微软的 vswhere。第三方生态与工具的知识（Swift SDK 包、qemu、xtool、ssh、simctl……）一律不进核心，通过通用接口由包和插件接入。
- **用户首先面对的是 `mcpp.toml`**：所有新键都可以写在工程里，`config.toml` 只是默认值。路径或工具缺失时，给出友好、可操作的提示。
- mcpp 与官方索引不分发受厂商许可约束的内容，只提供让开发者自备的标准接口。

## 0. 结论

| # | 决定 |
|---|---|
| D1 | **sysroot 是交付形式，不是层**：它是目标侧下层的**预制供给者**，称为**目标基座**，清单键仍叫 `sysroot`。它供给 `c-abi`，通常还有 `kernel-abi`，可能还有 `c++-abi`、`compiler-runtime` 与平台库 |
| D2 | **四个正交属性**：来源、地址、供给的层、运行时语义 |
| D3 | **全平台同一套写法**：`[target.<triple>].sysroot` 接受生态包、本机定位器、`{ path }`、`""`，或者不写。定位器族名用业界词汇：`glibc`、`musl`、`msvc`，以及 xcrun 的 SDK 名 `macosx`、`iphoneos`、`iphonesimulator`。某行接受哪些写法是目标表里的数据 |
| D4 | **一次解析，所有读者读同一个结果**（编译、扫描、std、驱动与 ld 的链接、产物运行时、闭包校验、hermetic、缓存键与指纹、报告、pack、运行器）。七处散落的决定合成一个 `BaseResolution` |
| D5 | **"工具链跑在什么上"（宿主）与"产物绑定什么"（目标）分开回答**。前者不变；#392 的时序约束只属于前者 |
| D6 | **c-abi 独占律**：一个目录作为 `--sysroot` 寻址，当且仅当它供给 c-abi。可叠加层（比如内核头文件）由引擎接线；基座的 c-abi 与依赖图的 c-abi 并存时，按规则一报错 |
| D7 | **不新增 `libc` / `c-abi` 键**，用已有的 `provides`、`requires`、`version-floor` |
| D8 | **规则二改为可测量**：编译器运行时与预制库要求的 C 库版本，不得高于基座的版本 |
| D9 | **运行时语义三种**：binding、floor、none。开发构建时 binding，pack 时改为 floor |
| D10 | **不写入、不修补外部树**。布局问驱动，实现与版本问预处理器；唯一的布局知识是 Debian multiarch（有规范定义，且实测需要） |
| D11 | **宿主基座 `glibc@system` / `musl@system`**（`{ path = "/" }` 归一到它）。用于为发行版打包、链接宿主的库 |
| D12 | **执行宿主**：工程写 `runner`（用什么跑），`run_under` 写"经过什么到达执行宿主"，执行时两者组合。本地、虚拟机、远程真机在引擎里是同一个抽象 |
| D13 | **运行器契约标准化**：一组环境变量、一份运行闭包清单、运行器可嵌套、可能被并发调用、退出状态透传、物理上无法执行时在执行前拒绝 |
| D14 | **值的层次**：**`mcpp.toml` 为主**（工程与团队的约定）；`~/.mcpp/config.toml` 与系统级 `/etc/mcpp/config.toml` 提供**默认值**；环境变量与命令行负责**覆盖**。顺序沿用 #755：覆盖 > 工程 > 机器默认 > 引擎默认。缺失时给出友好提示，列出三种让它存在的办法：xpkg 分发、仓库内相对路径、团队约定路径 |
| D15 | **自备输入**写成规范条款：只按公开标准定位、记录身份，从不获取、托管或分发 |
| D16 | **目标行与厂商的硬件路线无关**：x86_64 的 Apple 行由 SDK 里有没有对应架构切片来决定（测量），执行由执行宿主决定。`x86_64-ios-sim` 由 CI 验证：arm64 job 构建，Intel job 运行（§2.6） |
| D17 | 一并修正的缺陷（§1.2）：clean specs 丢了 ld 的 `--sysroot`；声明的 sysroot 没有 loader 与 rpath；rule B 拿生态 glibc 比对；`ensure_sysroot_complete` 写入外部树；基座身份不进指纹；工具链阶段静默丢弃键；非 Apple 宿主完全不能定位 Apple SDK |
| D18 | **核心知识判据**（见开头）。据此：Swift SDK 包（SE-0387）和 `QEMU_LD_PREFIX` **不进核心**；`SDKROOT` 进核心，并且在所有宿主上读取 |
| D19 | **同批修复 #832 中属于 mcpp 的部分**（§15）：建议命令中的路径加引号；release / dist 档默认做路径裁剪（`trim_paths`，根包与依赖都映射成 `<name>@<version>`），mcpp 自己的发布产物接入检查门；release / dist 档默认定义 `NDEBUG`（`ndebug` 键）；home 路径含空格由 xim-pkgindex 修复（不改 xlings），mcpp 加 e2e |

## 1. 问题与证据

### 1.1 实测（Linux x86_64，宿主 glibc 2.39，mcpp 2026.10.22.1）

| 场景 | 写法 | 结果 |
|---|---|---|
| A | `[toolchain.linux] { path }`，自定义 gcc 16.1.0 + binutils | ✅ 用生态 glibc 2.44.3 |
| B | A 加 `prefix` | ✅ 同 A。注释说"用自己携带的 sysroot"，实际不是 |
| C | A 加 `sysroot = <glibc 2.44 sysroot>` | ❌ `ld: cannot find /lib64/libm.so.6` |
| D | C 加 `-Wl,--sysroot` | ❌ PT_INTERP 是宿主的 loader，RPATH 末尾是 subos farm，运行时报 `symbol lookup error … GLIBC_PRIVATE`；rule B 拦下了，但建议的是"选一个 SubOS" |
| E | D 再补上 loader 与 rpath | 产物本身正确，**rule B 仍然拒绝** |
| F | 托管 gcc 加 `[target] sysroot = "<目录>"` | ❌ "is not an xpkg reference" |
| G、H | 交叉 `aarch64-linux-musl-gcc` | ✅ |
| I | 加 Debian 11 glibc 2.31 sysroot | ❌ 报错落在 `std.cc` 里；真正的原因是 libstdc++ 要求 GLIBC_2.38，没有说出来 |
| J | 生态 gcc 加 `--sysroot=/`（Ubuntu） | ❌ `cannot find crti.o`；补上 multiarch 目录后 ✅ |
| K | 生态 clang 加 `--sysroot=/` | ✅（clang 自己识别 multiarch） |

A、D、E 三个工程的输出目录指纹相同（`17bd18d28b33e658`）：基座不同，却没有被区分。

### 1.2 缺陷与设计

时间线：

- **#378**（08-08）引入 clean specs，`sysroot_mode` 显式补 loader 与 rpath。当时的前提是：GCC 的 sysroot 只可能是 subos，subos 链接脚本里写的是 payload 的绝对路径。
- **#400**（08-10）引入闭包校验 rule B。
- **#755**（10-02）复用 `sysroot_mode`，同时清空了 `payloadPaths`，没有写测试。

| 现象 | 位置 | 裁定 |
|---|---|---|
| ld 拿不到 `--sysroot`（gcc 在读 `-specs=` 之前插入的 `%(sysroot_spec)` 被覆盖掉） | `src/toolchain/post_install.cppm:375` | **缺陷**（字段注释写的是"for the compiler and the linker"；违反 SPEC-006 §4.3） |
| 声明的 sysroot 没有 loader 与 rpath | `local_toolchain.cpp:305`、`linkmodel.cppm:515` | **缺陷**（#378 的注释明言这是缺陷） |
| rule B 拿生态 glibc 比对 | `prepare/toolchain.cpp:151-165` | **缺陷**（违反 SPEC-006 §3.1） |
| 工具链阶段 `spec` 加其他键时静默丢弃 | `local_toolchain.cpp:140` | **缺陷** |
| 往外部 sysroot 里建符号链接 | `detect.cppm:139` → `fallback/sysroot_complete.cppm` | **缺陷**（违反 SPEC-006 §2.2.1） |
| 基座身份不进指纹 | `fingerprint.cppm` 第 10 字段 | **缺陷** |
| 非 Apple 宿主不能定位 Apple SDK | `macos.cppm` 的 `sdk_path()` 整段在 `#if defined(__APPLE__)` 里，`SDKROOT` 也读不到 | **缺陷**（对本设计而言） |
| `[target].sysroot` 只接受 xpkg 引用 | `toml.cppm:844` | 设计，本次扩展 |
| `{ path }` 不写 sysroot 时用生态 glibc | `local_toolchain.cpp:260` | 设计，保留 |
| 清单的 `cfg(...)` 只能判断目标，不能判断宿主 | SPEC-004 §3 | 设计，保留（影响 §3.7） |

### 1.3 根因

`Toolchain` 里装着五组属于基座的字段（`sysroot`、`payloadPaths`、`targetSysroot*`、`appleSdk*`、`msvc*` / `windowsSdk*`），
由七条代码路径各自填写：

- Linux 托管、Linux 声明、裸机的 multilib 块；
- Apple 的四处 `sdk_path()`；
- `bind_msvc_sysroot`；
- `has_own_sysroot`（9 个文件）；
- `bind_windows_ucrt` 的回流。

缺陷都出在这些路径的接缝上。

### 1.4 本轮不实测的部分

通过本机 macOS 客体运行 `x86_64-ios-sim` 这条路，本轮不测（附录 A.8）。核对过的前提：

- 满足：`/dev/kvm` 可读写；32 核 / 62G；系统 QEMU 8.2.2；本地配方 `localtest:osx-kvm` / `macos-vm` / `xcode-in-vm` 都在（2026-09-12，头注释写明"仅本地，永不发布"）。
- 不满足：**`/home` 只剩 7.1G**，需要 100G 以上；没有客体磁盘；没有 Xcode.xip。

它的验证由 CI 的 Intel runner 替代（§2.6）。

## 2. 模型

### 2.1 定义与四个属性

> **目标基座（sysroot）**：目标侧下层的预制供给者。它是一个按目标驱动约定组织的目录，供给 `c-abi`，通常还有 `kernel-abi`，可能还有 `c++-abi`、`compiler-runtime` 与平台库。

层是"供给了什么"，五层闭集不变；基座是"以什么形式预制供给"。SPEC-002 §1.3 的 `prebuilt`
放宽为"被点名、按内容识别的预制目录"。出处类别沿用 #755 的 `SourceClass`，**不新增层来源，也不新增出处类别**。

| 属性 | 取值 | 由谁回答 |
|---|---|---|
| 来源 | 覆盖、工程、机器默认、引擎默认（§3.3） | 优先级 |
| 地址 | `gnu-sysroot`（驱动与 ld）、`apple-isysroot`、`msvc-roots`、`multilib`、`driver-internal` | 目标表一列 |
| 供给的层 | `{层 → 实现, 版本, 位置}`，加上事实（如 `glibc=2.31`、`iphoneos.sdk=18.5`、架构切片） | 测量（§4.3），必要时声明 |
| 运行时语义 | binding、floor、none | c-abi 的实现，加上"是否本机" |

### 2.2 全平台语义

| 目标行 | 基座实物 | 供给的层 | 地址 | 运行时语义 | 可用写法 | 不写时 |
|---|---|---|---|---|---|---|
| `*-linux-gnu` | glibc 加内核头文件（可能带库）；宿主的 `/` | kernel-abi、c-abi | gnu-sysroot（加 multiarch） | 本机 binding；交叉 floor | `xim:…`、`glibc@system`、`{path}`、`""` | 生态 glibc 绑定 |
| `*-linux-musl` | 工具链里的 `<triple>/`；Alpine 的 `/` | kernel-abi、c-abi | gnu-sysroot | 静态 none；动态本机 binding | `xim:…`、`musl@system`、`{path}`、`""` | 工具链携带 |
| `*-none-*` | picolibc / newlib 的 multilib 目录 | c-abi | multilib | none | `xim:…`、`{path}`、`""` | 行的 `sysroot` 列 |
| `*-macos` | MacOSX SDK | kernel-abi、c-abi（libSystem 桩）、C++ 头文件、frameworks | apple-isysroot | floor | `macosx@system`、`macosx@<版本>`、`{path}` | `macosx@system` |
| `*-ios` / `*-ios-sim` | iPhoneOS / iPhoneSimulator SDK | 同上 | apple-isysroot | floor | `iphoneos@…` / `iphonesimulator@…`、`{path}` | 对应的 `@system` |
| `*-windows-msvc` | VC toolset 加 Windows SDK | c++-abi、compiler-runtime、c-abi（UCRT）、kernel-abi | msvc-roots | UCRT 是 floor | `msvc@system`、`msvc@<版本>`、`xim:msvc@…`、`{path}`（仅 clang 行） | `msvc@system` |
| `*-windows-gnu`、`*-android`、`wasm32-emscripten` | SDK 工具链内建 | 视 SDK 而定 | driver-internal | floor 或 none | 不可写 | 工具链携带 |

定位器族名必须与目标行一致。`glibc@<版本>` 不带前缀时，等同于 `xim:glibc@<版本>`（SPEC-006 §2.3）。

### 2.3 两个运行时问题

| 问题 | 轴 | 回答者 |
|---|---|---|
| 编译器自己运行在什么 C 库上 | 宿主 | 提供者为工具链钉定（`pin_runtime`），不变 |
| 产物针对什么编译、运行时绑定什么 | 目标 | `BaseResolution` |

默认构建两问答案相同，所以输出不变。托管 gcc 配自定义基座时：编译器运行在生态 glibc 2.44 上，产物绑定基座的 2.31。

### 2.4 c-abi 独占律

- **独占**：c-abi 决定驱动在哪里找 `crt*.o`、`libc.so` 与 loader，不能从基座里拆出去。
- **可叠加**：内核头文件、第三方头文件和库，由引擎用 `-isystem` / `-L` 接线。
- 基座里自带的其他库，来源可以追溯，允许使用，并在链接闭包中标明来自基座。宿主基座下，宿主的库就是基座的库，不再需要 `allow_host_libs`。
- 不供给 c-abi 的目录是库前缀，按依赖处理，不能当 sysroot。

### 2.5 执行宿主

| 执行宿主 | 用途 | 引擎看到的 |
|---|---|---|
| 本地 | 本机目标 | 不包装 |
| qemu-user / binfmt | Linux 交叉目标 | 运行器（qemu 的包提供，它把 `MCPP_SYSROOT` 转成 `-L` 或 `QEMU_LD_PREFIX`） |
| 本机虚拟机的客体（如 macOS 客体加 simctl） | 在 Linux 上执行 x86_64 的 Apple 产物 | `run_under` 加运行器 |
| 远程真机（Mac、云 Mac、开发板） | arm64 的 Apple 行、开发板 | `run_under` 加运行器 |
| 设备 | iOS / Android 真机 | 运行器（插件提供） |

在本地构建，在执行宿主上执行；mcpp 本身不需要运行在执行宿主上。

### 2.6 目标行、硬件路线与 `x86_64-ios-sim`

- **目标行**能否构建，取决于有没有一份带对应架构切片的基座。切片可以测量（`libSystem.tbd` 的 `targets`），引擎里不写截止日期。新版 SDK 去掉 x86_64 后，用户可以用 `{ path }` 或 `macosx@<旧版本>` 固定一份旧 SDK。
- **执行**取决于执行宿主。GitHub 当前的 runner 镜像中有 x64 的 `macos-15-intel` 与 `macos-26-intel`，而 Xcode 27 的镜像只有 arm64。由此可以确认：
  - macOS 26 仍然支持 Intel。本地 `macos-vm` 配方头注释里"Tahoe 完全放弃 Intel"的说法有误。
  - Intel 的终点大约在 Xcode 27 这一代。
- **`x86_64-ios-sim` 的真实需求**：
  - 在 Linux 虚拟机里运行它的需求很低。
  - 构建它的需求真实存在：给 Intel Mac 或 Intel CI 的使用方提供 arm64 + x86_64 的模拟器通用切片（XCFramework 的标准形态），虽然 mcpp 目前还不产出 XCFramework。
  - 这一行已经在发布（preview），但**从没运行过**。
- **验证（纳入本 PR）**：arm64 的 job 用 mcpp 构建，把产物交给 Intel 的 job，后者只执行 `xcrun simctl spawn`。Intel 一侧不需要 mcpp（mcpp 也没有 Intel macOS 的二进制）。这是"在这里构建、在那里执行"的实证，传输方式是 CI 的 artifact。S15 通过后，按层级规则把该行升为 verified，同时更新 README 的目标表。

### 2.7 行业对照

| 系统 | sysroot | 运行器 / 执行 | 机器默认 |
|---|---|---|---|
| GCC / Clang | `--sysroot`；clang `/winsysroot` | — | — |
| CMake | `CMAKE_SYSROOT` / `CMAKE_OSX_SYSROOT`（可写 SDK 名） | `CMAKE_CROSSCOMPILING_EMULATOR` | 用户 presets |
| Cargo | — | `target.<triple>.runner` | `~/.cargo/config.toml`、`CARGO_TARGET_<TRIPLE>_*` |
| Bazel | `cc_toolchain` | `--run_under` | `.bazelrc` |
| Meson | `sys_root` | `exe_wrapper` | 机器文件 |
| Xcode | `SDKROOT`、`iphoneos18.2` | `simctl` / `devicectl` | — |

## 3. 清单与配置语义

### 3.1 写法

```toml
[target.<triple>]
sysroot = "xim:glibc@2.39"                     # 生态包；团队私有索引也行："acme:sysroot-rhel8@1.0"
sysroot = "glibc@system"                       # 本机定位器（glibc / musl / macosx / iphoneos / iphonesimulator / msvc）
sysroot = "iphoneos@18.5"                      # 定位器加版本匹配
sysroot = { path = "third_party/sysroot" }     # 按路径命名：相对路径相对于写它的那个文件
sysroot = ""                                   # 不用预制 C 库
sysroot = { path = "/opt/exotic", provides = ["mcpp:c-abi=exotic"] }
```

- 表只有 `path` 与 `provides` 两个键；`{ path = "/" }` 归一为该行的 `<libc>@system`；`~` 会展开。
- 陌生的 c-abi 用 `provides = ["mcpp:c-abi=<impl>"]` 声明；不支持 `[c-abi]` 块（rev 5，见修订记录）。
- **团队让基座"存在"的三种办法**（文档与诊断都按这个顺序推荐）：
  1. **xpkg 分发**（包括团队私有索引）：有版本、有身份、可复现，首选；
  2. **仓库内相对路径**（子模块或 LFS）；
  3. **团队约定的路径**（如 `/opt/acme/sysroot`）。

### 3.2 每行合法的写法是数据

每行接受的写法与地址由该行的平台类别决定：`mcpp.toolchain.base::row_base` 按三元组返回地址、接受的写法位集与定位器族名，`refusal` 据此拒绝并列出该行接受的写法。清单解析只做语法检查（是不是某种写法），因为清单模块不解析三元组；某行接受与否在目标已知处判定。

### 3.3 优先级：四档

| 档 | 来源 | 出处类别（#755） |
|---|---|---|
| 1 覆盖 | `--sysroot` / `MCPP_SYSROOT`；`MCPP_TARGET_<TRIPLE>_SYSROOT` | 按写法 |
| 2 **工程** | ① `mcpp.toml` 的 `[target.<triple>].sysroot`（含工作空间层）；② `[toolchain] { path, sysroot }`；③ 工具链阶段陈述的 `sysroot`；④ 工具链携带、经探测得到 | ①② 按写法；③ `program`；④ 跟随该工具链 |
| 3 机器默认 | `~/.mcpp/config.toml`，其次 `/etc/mcpp/config.toml` 的 `[target.<triple>].sysroot` | 按写法 |
| 4 引擎默认 | ① 目标表该行的 `sysroot` 列（仅当依赖图中没人供给 c-abi）；② 平台默认 | `managed`，默认输出不变 |

写法与出处类别的对应：

| 写法 | 出处类别 |
|---|---|
| 生态包 | `managed`；工程写了版本时为 `pinned` |
| `{ path }` | `custom` |
| `*@system` | `host` |
| `*@<版本>` | `pinned` |
| 工具链阶段 | `program` |

- 环境变量里的 `none` 表示"不用预制 C 库"（对应清单里的 `""`）；空字符串等于没设置。
- 同一档内，更具体的陈述胜出，并报告覆盖。
- `--managed-only` 拒绝一切非默认的类别。
- 与 `[xlings] subos` 同时出现时，subos 只供给宿主工具，并报告一行。

### 3.4 `provides`、`requires` 与版本下限

- 测量能回答时，`provides` 用于核对，不一致就拒绝；测量回答不了时，`provides` 就是声明；两者都没有就拒绝。
- 断言实现：`requires = ["mcpp:c-abi=glibc"]`。
- 版本下限：`[[runtime.requirements]] kind = "version-floor"`。基座把测量到的事实以 `runtime.provides` 的同形发布。

### 3.5 工具链陈述与工具链阶段

- `[toolchain] { path, sysroot }` 的含义是"这套工具链携带的基座"。
- **工具链阶段可以同时陈述 `spec` 与 `sysroot`**，表示由程序陈述目标行的基座（类别 `program`）。这是插件定位基座的接口。
- `spec` 加 `prefix` / `launcher` / `tool.*` / `family` 时按名拒绝，修正现在静默丢弃的缺陷。

### 3.6 依赖、工作空间与宿主图

- 键类别不变（Configuration，从根读取）。
- 依赖用 `requires` 与 `version-floor` 表达要求。
- **宿主图永远不读目标基座，也不读 `run_under`。**

### 3.7 `run_under`：写在工程里为主，机器默认为辅

```toml
# mcpp.toml —— 团队约定，所有人都经同一台共享 Mac 执行
[target.aarch64-ios-sim]
runner    = ["simctl-run"]
run_under = ["remote-run", "team-mac", "--"]      # team-mac 是团队 ssh 配置里的主机别名
```

```toml
# ~/.mcpp/config.toml —— 只在这台机器上需要（混合团队：Mac 上的同事不需要它）
[target.x86_64-ios-sim]
run_under = ["remote-run", "macvm", "--"]
```

- **层次与 `sysroot` 相同**：覆盖（`MCPP_TARGET_<TRIPLE>_RUN_UNDER`，值为 `none` 时清除）> `mcpp.toml` > `config.toml` > 无。
- 执行形如 `<run_under…> <runner…> <产物> <参数>`；`--no-runner` 表示"这个三元组在本机是原生的"，所以 `runner` 与 `run_under` 都去掉。
- **团队怎么选**：全员同一种执行环境，写在 `mcpp.toml`；混合环境（有人在 Mac 上、有人在 Linux 上），写在各自的 `config.toml`；个别机器的例外，用环境变量清除或替换。
- 我们**不**给清单的 `cfg` 加宿主谓词：清单的条件只判断目标（SPEC-004 §3），三层配置已经够用（§11）。
- 机器默认不能放宽工程的约束：`requires` / `version-floor` 照样核对。

### 3.8 缺失时的友好提示

| 缺什么 | 提示里给出 |
|---|---|
| `{ path }` 不存在 | 文件与行号、解析后的绝对路径；三种让它存在的办法（§3.1）；如何在本机覆盖（`MCPP_TARGET_…_SYSROOT`、`config.toml`） |
| 定位器找不到 | 查过哪些位置；生成方法（例如 Apple SDK 在 Linux 上可以用 xtool 从自己的 Xcode.xip 生成，然后设 `SDKROOT` 或写 `{ path }`） |
| `run_under` / `runner` 的程序找不到 | 现有的"程序、查过的目录、errno"；怎么通过 `[xlings.workspace]` 装上它；本机覆盖或 `--no-runner` |
| 物理上无法执行（如 Linux 上的 iOS 模拟器产物，又没有 `run_under`） | 执行前就拒绝，说明原因，并给出 `run_under` 的写法 |

### 3.9 拒绝的写法

| 写法 | 理由 |
|---|---|
| `sysroot = { path, libc = … }` | 第二种语法；用 `provides` |
| 基座的 c-abi 与依赖图的 c-abi 并存 | 规则一 |
| `{ path }` 下既测量不到 c-abi，也没有声明 | 是库前缀，不是基座 |
| SDK 工具链行写基座 | 基座就是那个 SDK |
| cl.exe 行写了不同的 toolset，或写 `{ path }` | SPEC-006 §3.5 |
| 定位器族名与目标行不符 | 会描述另一个平台 |
| `xim:` 前缀加 Apple 定位器 | 没有生态包 |
| 工具链阶段 `spec` 加路径专属的键 | §3.5 |

### 3.10 场景

| 场景 | 之后的写法 |
|---|---|
| 用旧 glibc 构建可分发的二进制 | `sysroot = "acme:sysroot-glibc228@1"`（团队索引），或 `{ path }`；`pack` 时报"需要 glibc ≥ 2.28" |
| 为发行版打包；链接宿主的库 | `glibc@system` |
| 厂商交叉 SDK | `[toolchain] { path, prefix, sysroot }` |
| 托管编译器配公司的 sysroot | 目标行写 `{ path }` 或团队 xpkg |
| Yocto / Buildroot SDK | 同上 |
| 固定一份 Apple SDK | `iphoneos@18.5` 或 `{ path }` |
| Linux 上构建 iOS / macOS | 自备 SDK：`SDKROOT`，或 `{ path }` |
| Linux 上运行 x86_64 的 iOS 模拟器或 macOS 产物 | `run_under = ["remote-run", "<客体或远程 Mac>", "--"]`，写在 `mcpp.toml` 或 `config.toml` |
| 解包的 VS 或 winsysroot 目录 | clang 行写 `{ path }`，两种布局自动识别 |
| CI 或发行版打包时换基座 | `MCPP_SYSROOT=path:/sysroots/rhel8` |

## 4. 引擎架构

### 4.1 一个解析结果

纯数据部分放在 `modules/toolchain-model/src/base.cppm`，定位、测量与安装放在 `src/toolchain/base.cppm`。

```cpp
struct BaseStatement { Form form; std::string value; std::vector<std::string> provides; SourceDecision origin; };
struct BaseResolution {
    SourceClass source;  AddressKind address;  std::filesystem::path root;
    std::map<Layer, BaseLayer> layers;  std::vector<std::pair<std::string,std::string>> facts;
    bool multiarch = false;  RuntimeSemantics runtime;
    std::filesystem::path loader;  std::vector<std::filesystem::path> runtimeDirs;
    std::string identity;  MsvcRoots msvc;  AppleSdk apple;
};
```

`Toolchain` 收缩为编译器本身，五组字段迁入 `BaseResolution`。cl.exe 行的 toolset 记为 `via toolchain`。

### 4.2 解析时序

| 步 | 内容 |
|---|---|
| 1 | 由覆盖、工程、机器默认、目标表得出 `BaseStatement`（工具链阶段的陈述在第二遍 prepare 时进入） |
| 2 | 定位（Package 走 fetcher；Locator 走 §4.4；Path 检查存在；Default 走现有的 `resolve_runtime_binding`） |
| 3 | 宿主侧：工具链运行库的钉定（不变，#392 的时序在这里） |
| 4 | 工具链解析（不变） |
| 5 | 测量（§4.3） |
| 6 | 产物运行时由基座推导（`bind_windows_ucrt` 的回流并入这一步） |
| 7 | 层间下限检查 |
| 8 | 依赖图解析之后，目标侧读取基座的层（检查规则一、规则三） |

### 4.3 测量

- **布局问驱动**：`-print-file-name=crt1.o / libc.so / libc.a`、`-print-search-dirs`。
- **Debian multiarch**：驱动找不到 `crt1.o`，且存在 `<root>/usr/lib/<tuple>`（tuple 取自 Debian MultiarchTuples 数据表）时，补上 `-B`、`-L`、`-isystem` 与 `-Wl,-rpath-link`，再问驱动一次，并记录 `multiarch = true`。驱动自己能处理（clang）时不补（实测 J、K）。
- **实现与版本**：驱动 `-E -dM` 预处理一个探针翻译单元，对照数据表：

| 实现 | 判据 | 版本 | 运行时语义 | 事实名 |
|---|---|---|---|---|
| glibc | `__GLIBC__` | `__GLIBC__.__GLIBC_MINOR__` | 本机 binding / 交叉 floor | `glibc` |
| bionic | `__BIONIC__` | `__ANDROID_API__` | floor | `android.api` |
| picolibc | `__PICOLIBC__` | `__PICOLIBC_VERSION__` | none | `picolibc` |
| newlib | `_NEWLIB_VERSION` | 同名宏 | none | `newlib` |
| emscripten | `__EMSCRIPTEN__` | `__EMSCRIPTEN_major__…` | none | `emscripten` |
| musl | 以上都不是，且能找到 `ld-musl-*` 或 `libc.a` | 读不出 | 静态 none / 动态 binding | `musl` |
| ucrt | 定位器 | SDK 版本 | floor | `ucrt` |
| libSystem | 定位器 | `SDKSettings.json`；切片取自 `libSystem.tbd` 的 `targets` | floor | `<sdk名>.sdk` |

- 读不出的版本不参与比较。
- 结果按"驱动身份 × 基座身份"缓存；生态默认基座不执行探针。

### 4.4 定位器（只用平台的官方约定，D18）

| 族 | 查找顺序（报告用了哪个） |
|---|---|
| `glibc@system` / `musl@system` | `/`；在 Luban 系统上，`/` 按已有的 `declared_system_root` 规则处理 |
| `macosx` / `iphoneos` / `iphonesimulator` `@system` | ① `SDKROOT`（**所有宿主**；仅当它指向该平台的 SDK，沿用现有的 `sdkroot_answers`）；② `xcrun --sdk <名>`；③ `xcode-select` 与固定位置（②③ 只在 Apple 宿主上）。`sdk_path()` 不再整段包在 `__APPLE__` 里 |
| `…@<版本>` | 同上，只接受版本匹配的那一个 |
| `msvc@…` | 现有规则，不变 |
| MSVC 行的 `{ path }` | toolset 取 `<root>/VC/Tools/MSVC/<ver>`；SDK 在 `<root>/Windows Kits/10` 存在时用它（clang `/winsysroot` 布局），否则走机器的 SDK 查找（VS / `xim:msvc` 布局），并报告用了哪个 |

**Swift SDK 包（SE-0387）不在核心里**：Linux 用户用 xtool 等工具生成之后，设一次 `SDKROOT`，或者写 `{ path }`。
插件或包可以提供一个辅助命令打印路径（§6）。

### 4.5 地址的渲染

每种地址一个渲染器，取代 `ToolchainLinkModel` 中描述 C 库的部分与 `hostflags` 片段。

- **gnu-sysroot**：驱动**与 ld** 都拿 `--sysroot`（被陈述的基座由 gcc 的 `-Wl,--sysroot=<root>` 给出，见 rev 5）；binding 时加 loader 与 RUNPATH；需要时加 multiarch。
- **apple-isysroot**：`-isysroot` 加 deployment target。
- **msvc-roots**：`-Xmicrosoft-*` 与 `-fms-compatibility-version`；cl.exe 用 INCLUDE / LIB。
- **multilib**：`-isystem` / `-L` 指向 `<root>/{include,lib}/<libdir>`。
- **driver-internal**：只给 `--target`。

### 4.6 产物运行时

- **binding**：PT_INTERP 与 RUNPATH 指向基座；契约哈希包含基座身份；subos farm 只在生态默认基座时追加；rule B 以基座为参照，按来源给出修复建议。
- **floor**：loader 用目标的标准路径；`runtimeId` 作为下限。
- **none**：没有运行时。

### 4.7 运行器契约

执行形如 `<run_under…> <runner…> <产物> <参数>`。引擎不认识任何运行器的名字。

| 项 | 规定 |
|---|---|
| 定位 | 先在声明的载荷 `bin/` 里找，再到 `PATH` 上找；`run_under` 的第一个元素同样处理 |
| 环境变量 | `MCPP_RUN_TRIPLE`、`MCPP_RUN_KIND`（`exe` / `bundle`）、`MCPP_RUN_MODE`（`run` / `test`）、`MCPP_RUN_ID`、`MCPP_RUN_CLOSURE`、`MCPP_SYSROOT` |
| 运行闭包清单 | 一份带 schema 版本的 JSON：产物、它运行时需要的文件（来自 `runtime_placement`）、资源目录、基座根 |
| 嵌套 | 运行器可以包装运行器，argv 原样传递 |
| 并发 | 可能被并发调用，用 `MCPP_RUN_ID` 区分 |
| 退出状态 | 运行器的退出码即程序的退出码；拿不到程序自己的状态的运行器，必须在运行前说明 |
| 物理边界 | 没有任何执行路径时，执行前拒绝（§3.8） |

`QEMU_LD_PREFIX` 这类工具专属的变量**不由引擎设置**（D18），由对应运行器的包从 `MCPP_SYSROOT` 转换。

### 4.8 层间下限检查

- 编译器载荷的 `libstdc++.so.6` / `libgcc_s.so.1` 的 verneed 中最高的 `GLIBC_x.y`，与基座事实比较，编译前拒绝。
- 预制库走同一个比较器。
- Apple：deployment target ≤ SDK 版本，且 SDK 带目标架构的切片。

### 4.9 身份、指纹与快速路径

- **身份**：生态包取包身份；定位器取版本加根目录；路径与宿主基座取根目录加测量所依据文件的路径、大小、修改时间。
- 身份进入契约哈希，因此进入指纹第 10 字段；同时写入 stamp；跨工程缓存键也加上它。
- 生态默认基座的身份就是现在的契约哈希，升级不会让缓存失效。

### 4.10 报告

- 非默认的基座与 `run_under` 各打印一行：`Using sysroot glibc 2.31 ← third_party/sysroot [custom · mcpp.toml:12]`、`Running via remote-run team-mac [mcpp.toml:15]`。
- 默认情况下输出逐字节不变。
- 目标侧报告中，基座供给的层显示为 `prebuilt`。
- `resolution.json` 新增 `base` 与 `execution`；`sources` 新增 `sysroot`、`run_under`；新增 `mcpp why sysroot`。

### 4.11 pack

- `vendored` / `system` 模式：binding 改成发行版路径，并写入基座下限。
- `self-contained` 模式：随包带的是基座的 loader 与 C 库。
- §15 的路径裁剪默认作用于 pack 所用的 dist 档。

### 4.12 删除与收敛

| 删除或改写 | 去向 |
|---|---|
| `Toolchain` 的五组基座字段 | `BaseResolution` |
| `fallback/sysroot_complete.cppm` | 保留，只作用于与载荷同在一个 home 下的 subos 视图（rev 5） |
| `payloadPaths.reset()` 一带 | 改为陈述基座 |
| 裸机 multilib 块 | Package 加 multilib |
| 四处 `sdk_path()` 与 `__APPLE__` 的整段包裹 | Apple 定位器 |
| `bind_msvc_sysroot` / `check_cl_row_sysroot` | MSVC 定位器 |
| `bind_windows_ucrt` 回流 | §4.2 第 6 步 |
| `has_own_sysroot()` 的 9 处读取 | `address == driver-internal` |
| `sysroot_refusal` 的拼写分支 | 目标表 |

## 5. 自备输入（BYO）

### 5.1 条款

对受厂商许可约束、或者属于某台机器某个人的输入，引擎：

1. 只按公开标准定位（`SDKROOT`、xcrun、`/winsysroot` 布局、`{ path }`、`config.toml`）；
2. 记录身份；
3. 从不获取、托管或分发，官方索引不收录；
4. 缺失时给出友好提示（§3.8）；
5. 生成它们的工具在许可证与服务条款允许时可以是 xpkg；
6. 不接触任何凭据。

### 5.2 范围

| 输入 | 谁生成 | 怎么接入 |
|---|---|---|
| Apple SDK | Xcode；在 Linux 上由 xtool / xcross 用用户自己的 Xcode.xip 生成 | `SDKROOT`、`{ path }` |
| macOS 客体、远程 Mac | 用户 | `run_under` |
| 签名证书、描述文件、账号 | 用户 | 插件 |
| 非生态的 VS / Windows SDK | 用户 | MSVC 定位器 |

### 5.3 许可证

mcpp 只为统一平台提供中性机制，不规避许可证。文档以远程 Apple 执行宿主为主要例子；
CI 不运行 macOS 虚拟机，Intel 验证用 GitHub 的 Intel runner（§2.6）。

## 6. 生态侧配套（引擎之外）

引擎 PR 不依赖这些；它的 e2e 用测试目录里的假运行器验证契约。

| 项 | 归属 | 说明 |
|---|---|---|
| `remote-run` | xim-pkgindex（可公开） | ssh 传输：读闭包清单、rsync、执行、带回退出状态、按 `MCPP_RUN_ID` 清理 |
| qemu 的运行器包装 | xim-pkgindex（`qemu-user-*` 的配方） | 把 `MCPP_SYSROOT` 转成 `-L` 或 `QEMU_LD_PREFIX` |
| Swift SDK 包路径辅助 | 包或插件 | 读 `swift-sdk.json` 并打印 `sdkRootPath`，供用户设 `SDKROOT` 或写 `{ path }` |
| `simctl-run` | 不需要改 | 由 `remote-run` 远程调用 |
| `apple-device-tools` 的 Linux 后端 | xim-pkgindex | pymobiledevice3 或 xtool；退出状态参考 xcross 的 gdb-remote 附加做法 |
| `dist-apple` 在 Linux 上签名 | mcpp-plugins | rcodesign |
| `xim:xtool` | xim-pkgindex | MIT；SDK 生成工具与设备后端（生成 SDK 是否需要 Swift，未实测） |
| xcross | 不联动 | AGPL-3.0、没有通用入口、凭据风险；借鉴它的思路 |
| 虚拟机配方 | 只在本地或用户自己的索引 | 永不进官方索引 |

## 7. 诊断（示例）

```
error: mcpp.toml:12: [target.x86_64-linux-gnu].sysroot = { path = "third_party/sysroot" }
       resolves to /work/proj/third_party/sysroot, which does not exist.
       A sysroot a project names is the project's to provide. Common ways:
         · publish it as a package in your index:  sysroot = "acme:sysroot@1.0"
         · keep it in the repository (submodule/LFS) at that relative path
         · agree on a path for the team (e.g. /opt/acme/sysroot)
       To use another one on this machine only: MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT=path:<dir>

error: [target.x86_64-linux-gnu].sysroot: gcc 16.1.0's runtime requires glibc >= 2.38
       (libstdc++.so.6: GLIBC_2.38), and this sysroot provides glibc 2.31.

error: target aarch64-ios needs the iphoneos SDK, which this machine does not provide.
       Looked in: SDKROOT (unset).  It is not redistributable, so mcpp locates it:
       on macOS install Xcode; elsewhere produce one from your own Xcode.xip (e.g. with xtool)
       and set SDKROOT, or write [target.aarch64-ios] sysroot = { path = "…" }.

error: cannot run x86_64-ios-sim artifacts on this host: the iOS Simulator runs only on macOS.
       Name an execution host: [target.x86_64-ios-sim] run_under = ["remote-run", "<host>", "--"]
       (in mcpp.toml for the team, or ~/.mcpp/config.toml for this machine).

error: the C library is supplied twice: sysroot glibc 2.44 ← … and graph musl ← openkal-musl@0.3.3.
```

## 8. 规范修订（条文草案）

**SPEC-006**

- §1 "sysroot" 改为 §2.1 的定义，附规范性附录（§2.2 的表）。
- 新 §3.5 "目标基座"，共五款：一次解析；写法与优先级；测量与声明（布局知识仅限 multiarch）；不写入；运行时语义。
- 新 §3.6 "自备输入"。
- 新 §3.7 "核心知识判据"（D18）。
- §2.2.1：`sysroot` 是工具链携带的基座；工具链阶段可以陈述 `spec` 加 `sysroot`。
- §3.1 补全读者名单；§3.3 增加主体 `sysroot` / `run_under`。
- §4.3 增加：gcc 的 `-specs=` **必须**保留 `%(sysroot_spec)`。

**SPEC-002**

- §1.3 放宽 `prebuilt`；§2.4 标为已实现。
- 规则一、二、三各增一款（基座是一个供给者；可测量的下限；可叠加层）。
- 新 §6 "目标侧执行"：运行器契约、`run_under` 的组合、物理边界。

**SPEC-004**

- §2 平面：值的来源写明四档"覆盖 > 工程 > 机器默认（用户级、系统级 `config.toml`）> 引擎默认"；`sysroot` 与 `run_under` 两个键都可以出现在工程与机器默认中。
- §4.2：写法与表写法。
- 键登记表：`target.<sel>.sysroot`（允许表值）、`target.<sel>.sysroot.c-abi`、`target.<sel>.run_under`。

**构建档案（docs/04 §2.9，以及它对应的规范条款）**：`trim_paths`（§15.3）与 `ndebug`（§15.4）两个档案键，以及它们在内置档案中的默认值。

**用户文档**：docs/20、04、22、10、09、50、25，中英文同步。

## 9. 兼容与无感升级

- 现有拼写的含义全部不变。
- 生态默认构建的命令行、产物、输出、指纹逐字节不变。例外有两个：
  - gcc 的 ld 新拿到 `--sysroot=<subos>`，需要用 CI 逐项比对产物来确认；
  - **release / dist 档默认开启 `trim_paths`**，产物里的源文件路径与调试路径会变（§15.3）；需要绝对路径的人可以写 `trim_paths = false`。
  - **release / dist 档默认定义 `NDEBUG`**，这些档里的 `assert` 会消失（§15.4）；需要保留的人可以写 `ndebug = false`。
  - 这两项都会让 release / dist 档在升级后重建一次，dev 档不受影响；两项都写进 changelog，并在升级后第一次构建时提示一次。
- 运行器多出来的环境变量，读不懂的运行器会忽略。
- 新增的拒绝：工具链阶段 `spec` 加路径专属的键（发布前在 mcpp-plugins 与 mcpplibs 里检索用法）。

## 10. 多视角自审

第一轮与第二轮审查的结论保留在下表的前半部分；第三轮（本次修订）新增的放在后半部分。

| 视角 | 发现 | 修订 |
|---|---|---|
| 规范一致 | 新造了出处类别 `carried` / `system` | 沿用 #755 的 `SourceClass` |
| 简洁 | 优先级平铺六级 | 收为四档 |
| 跨平台 | Linux 上 Apple 行被整段编译掉 | 列为缺陷；定位器跨宿主 |
| 跨平台 | 宿主基座下生态 gcc 直接失败（J） | multiarch 一条 |
| 场景 | 宿主基座可有可无？ | 生态 glibc 高于多数发行版，所以正式支持 |
| 插件边界 | 工具链阶段拒绝 `spec` 加 `sysroot`，堵住了插件入口 | 改为允许 |
| 正确性 | 远程运行器不知道要传什么；会被并发调用 | 闭包清单与 `MCPP_RUN_ID` |
| **用户心智（第三轮）** | 上一稿把 `run_under` 限定在机器层，用户要学两个文件，团队的约定又写不进工程 | **工程为主，机器为默认，环境变量为覆盖**，两个键同一套层次（D14、§3.7） |
| 用户心智 | 混合团队里，工程写了 `run_under`，Mac 上的同事怎么办？ | 混合团队写在各自的 `config.toml`；个别例外用 `…_RUN_UNDER=none`；不给 `cfg` 加宿主谓词（§11） |
| 一致性 | 环境变量里"空"是什么意思，`sysroot` 与 `run_under` 不一致 | 统一：空字符串等于没设置，`none` 表示显式清除 |
| 可用性 | 路径不存在时只报"不存在"，开发者不知道怎么办 | §3.8 提示表：三种让它存在的办法，加本机覆盖 |
| 核心边界 | SE-0387 与 `QEMU_LD_PREFIX` 是第三方工具的知识，却放进了核心 | 按 D18 移出，由包提供；`SDKROOT` 是平台约定，保留，并在所有宿主上读取 |
| 事实核对 | 本地配方说"Tahoe 放弃了 Intel" | GitHub 有 `macos-26-intel` 镜像，说明这个说法有误；时间线改为以 Xcode 27 为界（§2.6） |
| 需求真实性 | `x86_64-ios-sim` 值不值得验证？ | 构建有真实需求；运行验证可以用 CI 的 Intel job 低成本完成，纳入本 PR；本地虚拟机不做 |
| 范围 | #832 只记在附录，与发布流程脱节 | 纳入同批（§15），有自己的 checkpoint 与验收；它与本设计共享"路径占位符"这套机制（§15.3 与缓存键的相对化同源） |
| 跨仓库 | 上一稿把 xlings 的 argv 形式 exec 当成前置依赖，多了一个仓库的发布 | 索引自己就有 `libs/` 共享库机制，修复只需在 xim-pkgindex 里完成；xlings 的那一项降为可选的优化（§15.2） |
| 测试 | R2 原本用 `assert` 验证路径裁剪，而 release 默认定义 `NDEBUG` 后 `assert` 不再展开 | R2 改用 `__FILE__` 与 `std::source_location`，`NDEBUG` 单独验收（R2b） |
| 风险 | PR 改动面更大了 | 十三个 checkpoint；§15 的几个 checkpoint 与基座部分互相独立，可以单独回滚 |
| 安全 | `run_under` 写进工程，就能执行任意程序 | 与工程的 `runner`、构建程序同等可信，没有新增攻击面；`--no-runner` 可以去掉它 |
| 可测试 | Linux CI 上怎么测 Apple 定位器？ | 用假 SDK 目录加 `SDKROOT` 做 plan-only 解析 |
| 遗留 | 生态侧的外部工具都未实测 | 不在引擎 PR 的验收里，列为附录 A 的开放问题 |

## 11. 不采纳的方案

| 方案 | 理由 |
|---|---|
| 只修 §1.2 的缺陷 | 不解决目标轴、托管编译器配自定义基座、多处解析这几个问题 |
| `{ libc = …, path }` | 把形式与层揉在一起；会构成规则一冲突 |
| 新增层来源或出处类别 | 已有的维度够用 |
| 规范化任意布局 | 写入外部树；与驱动行为分叉 |
| 本机自定义基座默认 floor | 本机能否运行就取决于宿主 |
| 废弃 `[toolchain] { sysroot }` | 厂商成对交付是真实存在的形态 |
| 定位器族名叫 `apple-sdk` | 不是业界词汇 |
| **`run_under` 只允许写在机器层**（上一稿） | 工程才是开发者首先面对的地方；团队约定需要写进工程 |
| 机器默认单独放一个文件 | `config.toml` 本来就是机器层，并且分为用户级与系统级 |
| 给清单的 `cfg` 加宿主谓词 | 扩大了条件语法（SPEC-004 §3 "条件化的唯一形状"）；三层配置已经能表达 |
| **核心读取 Swift SDK 包（SE-0387）** | 第三方生态的格式（D18） |
| **核心设置 `QEMU_LD_PREFIX`** | 工具专属（D18） |
| `[target.<triple>.sysroot.c-abi]` 块 | sysroot 写在哪一行，呈现的就是那一行的 C 环境；`[c-abi]` 块属于由依赖图构建、呈现非默认环境的 C 库（rev 5） |
| 给 `defines` 加 `!NAME` 取反语法 | 只为改回 `NDEBUG` 扩大清单语法；`ndebug = false` 足够（rev 5） |
| 定位器插件机制（`sysroot = { command = … }`） | 每次构建都要执行外部程序，身份与缓存都会变复杂；自备 SDK 本来就是一次性的手工步骤 |
| 引擎内建虚拟机或 ssh 支持 | 具体绑定 |
| 在 macOS 客体里运行 mcpp | 需要 Intel macOS 的 mcpp 二进制，而且没有必要 |
| 与 xcross 做 xpkg 联动 | AGPL、没有通用入口、凭据风险 |
| 通过 xim 分发 Apple SDK 或 macOS 镜像 | 违反许可证 |
| 用本地虚拟机验证 `x86_64-ios-sim` | 要 100G 磁盘、手工安装、许可证灰区；CI 的 Intel job 更便宜也更干净 |

## 12. 验收

### 12.1 不变量

- 三个平台的默认构建：命令行、`resolution.json`（除新增对象外）、输出、指纹逐字节不变（`trim_paths` 只影响 release / dist，单独验收）。
- gcc 加上 `%(sysroot_spec)` 前后，产物的 DT_NEEDED / PT_INTERP / RUNPATH 逐项相同。

### 12.2 单测

- 写法与每行拒绝；四档优先级（`sysroot` 与 `run_under` 各一套）；环境变量的空值与 `none`。
- 测量数据表；multiarch；运行时语义。
- Apple 定位器（假 SDK 目录加 `SDKROOT`，非 Apple 宿主上也能运行）；MSVC `{ path }` 的两种布局。
- 身份与指纹；源码门禁（`src/` 中不出现针对外部树的 `create_*symlink`）。
- 运行器 argv 的组合；`--no-runner`。
- §15：前缀映射表的生成与排序；建议命令的引号。

### 12.3 e2e

| 编号 | 内容 |
|---|---|
| S1 | 自定义 gcc 加 custom glibc 基座：构建、运行（用的是基座的 glibc）、pack 下限 |
| S2 | 托管 gcc 加 custom 基座（相对路径写法） |
| S3 | 下限拒绝 |
| S4 | 原地改写基座后重建；默认快速路径不变 |
| S5 | 基座与依赖图的 c-abi 冲突 |
| S6 | 非基座目录被拒；路径不存在时的友好提示 |
| S7 | 覆盖（`MCPP_SYSROOT`、`…_SYSROOT=none`）、`config.toml` 默认、`--managed-only` |
| S8 | 交叉 musl 携带的基座 |
| S9 | 工具链阶段：`spec` 加 `sysroot` 生效；`spec` 加 `prefix` 被拒 |
| S10 | `glibc@system`：gcc（走 multiarch）与 clang |
| S11 | 运行器契约：假运行器经 `run_under`（分别写在 `mcpp.toml` 与 `config.toml`）加 `runner` 嵌套调用；argv、环境变量、闭包清单、退出码；`…_RUN_UNDER=none`；`--no-runner` |
| S12 | Linux 上的 Apple 定位器：假 SDK 加 `SDKROOT` 做 plan-only；缺失时的提示 |
| S13（macOS CI） | `macosx@system`、`@<版本>`、`{ path }` 一致；`iphonesimulator@system` |
| S14（Windows CI） | `msvc@system` 与 `{ path }`（clang 行）一致；cl.exe 行 `{ path }` 被拒 |
| **S15（CI，跨 job）** | macOS arm64 job 用 mcpp 构建 `x86_64-ios-sim` 的测试程序（以 artifact 交出）→ `macos-15-intel` job 用 `xcrun simctl spawn` 运行，断言程序自己的退出码与输出；arm64 job 同时断言 SDK 测量出 x86_64 切片 |
| R1–R4 | §15.5 |

另外，挑出与基座有关的既有用例（134、179、201、219、295、296、778、875、889、891 等），在新旧二进制上比对。

### 12.4 本地

用复现脚本跑 A–K。本地虚拟机路线不测（§1.4）。

## 13. 单 PR 与 commit checkpoint

1. 数据：测量表、multiarch 表、目标表的两列、基座数据类型，加单测。
2. 解析、优先级、定位、测量、缓存；`Toolchain` 字段迁移（行为不变）。
3. 渲染器、`%(sysroot_spec)`、multiarch。
4. 产物运行时、rule B、farm 规则。
5. 下限检查。
6. 清单与 `config.toml`：写法、`provides`、`c-abi` 块、覆盖、`run_under` 的层次、缺失提示；工具链阶段的修订；拒绝表。
7. Apple 与 MSVC 定位器，删除 `__APPLE__` 的整段包裹。
8. 运行器契约（环境变量、闭包清单、组合、物理边界）。
9. 删除 `sysroot_complete` 与回流特例；报告、`resolution.json`、`mcpp why`、`--managed-only`、pack。
10. CI：S15 的跨 job 验证。
11. **§15.1**：建议命令的引号。
12. **§15.3、§15.4**：`trim_paths`（档案键、映射、渲染、调试映射文件、缓存键）与 `ndebug`；mcpp 自身发布的检查门。
13. 规范与文档（中英文），e2e S1–S14、R1–R4；**§15.2** 的 e2e 在索引版本跟进之后打开。

## 14. 评审决议

前几轮的决策点都已经定下，记录如下：

| # | 问题 | 决议 |
|---|---|---|
| 1 | 机器层包装前缀的键名 | `run_under`（沿用 Bazel 的语义） |
| 2 | 机器默认放在哪里 | 现有的 `config.toml`（用户级与系统级），不另开文件；**`mcpp.toml` 为主** |
| 3 | Swift SDK 包是否进核心 | 不进（D18） |
| 4 | `NDEBUG` | 跟随业界惯例：release / dist 默认定义，`ndebug` 键可以改回（§15.4） |
| 5 | `trim_paths` 中根包的映射 | `<name>@<version>`，与依赖同形（§15.3） |
| 6 | `x86_64-ios-sim` 的层级 | S15 通过后，按层级规则升为 verified |
| 7 | #832 中 home 路径含空格 | 只改 xim-pkgindex（`libs/shell.lua` 加 lint）；xlings 的 argv 形式 exec 属于优化，不是依赖（§15.2） |

目前没有待决的事项。

## 15. 同批：mcpp#832 的 mcpp 侧

#832 报告了两个问题（发布产物残留 CI 路径；home 路径含空格时失败）。下面是分析后属于 mcpp 的部分，
它们与本设计在同一个发布里交付。

### 15.1 建议命令里的路径没加引号

`src/home.cppm:273` 的提示把未加引号的路径拼进了建议命令（`mcpp self init --force --home /tmp/mc probe/home`）。

- 修正：所有**给用户照抄**的命令都经过同一个辅助函数生成，按平台的 shell 规则（`mcpp::platform::shell::quote`）加引号。
- 审计：扫一遍 `src/` 中拼进诊断里的命令行。mcpp 自己执行的命令大多已经加了引号，例如 `probe.cppm`。

### 15.2 home 路径含空格时，工具链的 hook 失败

- **根因不在 mcpp**：xim-pkgindex 的 `pkgs/g/gcc.lua:662` 用 `system.exec(string.format("%s %s --dynamic-linker %s …"))` 拼接字符串，路径没加引号。索引中有 48 个配方使用 `system.exec(string.format(…))`，共 86 处插值，需要逐个审计其中插入路径的那些。
- **只改 xim-pkgindex，不需要改 xlings**：
  - 索引已经有共享库机制：`libs/*.lua`，以 `xim.pkgindex.<名>` 导入，例如 `sysroot` 被 102 个配方使用。`glibc.lua` 里也已经有一个私有的 `__sh_quote`。
  - 新增 `libs/shell.lua`（`xim.pkgindex.shell`），提供 `quote(s)` 与 `exec(argv)`，按宿主的 shell 规则加引号后调用 `system.exec`。
  - 迁移 `gcc.lua` 以及其他插入路径的调用点；`glibc.lua` 的私有实现改为使用它。
  - 新增 `tools/lint_exec_quoting.sh`：`system.exec` / `os.exec` 的格式串中，`%s` 必须经过 `quote()`。
- **xlings 的 argv 形式 exec 属于优化，不是依赖**：它能彻底绕过 shell，不必再操心各平台的引号规则，可以在 xlings 的路线里单独做；本批不依赖它。
- **发布顺序**：xim-pkgindex 合入并发布 → mcpp 跟进索引版本 → 打开 R3。
- **mcpp 侧**：现有 e2e 179 只测了"项目路径带空格"。新增 R3：**全新 home，路径同时含空格与非 ASCII 字符**，三个平台各跑一次默认构建。由于本地沙箱的限制，它只在 CI 上运行。

### 15.3 发布产物里的路径：`trim_paths`

mcpp 自己的发布产物 `strings` 之后能看到 `/home/runner/work/mcpp/mcpp/modules/libs/src/json/json.hpp`，
来源是 json.hpp 中 `assert` 展开的 `__FILE__`。这不只是 mcpp 自己发布配置的问题：**mcpp 作为构建工具，
release / dist 档没有任何路径裁剪**，每个用户的发布产物都会带上他们机器上的绝对路径。
2026-07-30 的缓存设计里把它列为 S8，后来没有实施，当时的顾虑是调试器需要 `substitute-path`。

- **档案键** `trim_paths = true | false`。默认值：`dev` / `debug` 为 false，`release` / `dist` 为 true。对照：Go 的 `-trimpath`、Cargo 的 `trim-paths`（RFC 3127）。
- **映射表**，与缓存键的 `<store>` / `<home>` 相对化同源（`cache_key.cppm`），从同一处生成：

| 源前缀 | 映射为 |
|---|---|
| 依赖图中每个包的根 | `<name>@<version>` |
| 根包的根 | `<name>@<version>`（与依赖同形，见下文） |
| 存储根 | `/mcpp/store` |
| home | `/mcpp/home` |
| 构建输出目录 | `/mcpp/build` |
| 工具链根 | `/mcpp/toolchain` |
| **目标基座根** | `/mcpp/sysroot` |

  规则按最长前缀优先排列。

- **渲染**：
  - GCC / Clang 用 `-ffile-prefix-map=<from>=<to>`，它同时覆盖 `__FILE__`、`std::source_location` 与调试信息；
  - clang-cl 用 `/clang:-ffile-prefix-map=`；
  - cl.exe 先用探针确认可用的开关（例如 `/d1trimfile:`），没有可用开关时**报告一次"本次未裁剪"**，不能悄悄假装裁剪过。
- **调试**：开启裁剪、同时带调试信息时，在构建目录写出 `source-map.gdbinit` 与 `source-map.lldbinit`，把映射反过来，消除当初 S8 的顾虑。
- **缓存键**：映射后的旗标进入缓存键。占位符与机器无关，跨 home 的缓存共享因此更好。
- **mcpp 自己的发布**：mcpp 的发布用 dist / release 档，自动得到裁剪；发布 CI 增加检查门 `strings -a bin/mcpp | grep -E '/(home|Users|runner|tmp)/'`，有输出就失败。

- **根包的映射（已定）**：根包的源码同样映射成 `<name>@<version>`，与依赖同形。`__FILE__`、`assert` 的消息、`std::source_location`、调试信息里的路径都会变成这个样子：

  | 位置 | 不裁剪（dev） | 裁剪后（release / dist） |
  |---|---|---|
  | 根包的源码 | `/home/alice/work/app/src/main.cpp` | `app@1.2.0/src/main.cpp` |
  | 依赖的源码 | `/home/alice/.mcpp/registry/…/fmt/11.0.2/src/format.cc` | `fmt@11.0.2/src/format.cc` |
  | sysroot 的头文件 | `/opt/acme-sysroot/usr/include/stdio.h` | `/mcpp/sysroot/usr/include/stdio.h` |

  这样有三点好处：
  - 产物不暴露开发者的目录与用户名；
  - 同一份源码在不同目录检出、在不同机器上构建，嵌入的路径完全相同，这是可复现构建的前提；
  - 用户看到的崩溃信息或断言里，`app@1.2.0/…` 直接指明是哪个包、哪个版本、哪个文件，比 Cargo 那种相对路径 `./src/main.cpp` 更能分辨根包与依赖。

  调试器要找回源码时，加载生成的 `source-map.gdbinit` / `.lldbinit` 即可（每一行就是上表的反向映射）。工作空间的每个成员都是一个包，按各自的 `<name>@<version>` 映射。

### 15.4 `NDEBUG`（已定：跟随业界惯例）

json.hpp 的路径之所以出现，是因为 release 档没有定义 `NDEBUG`，`assert` 还在。路径裁剪能独立解决泄漏问题；
`NDEBUG` 另行决定，结论是跟随业界惯例：

- 档案键 `ndebug = true | false`。默认值：`dev` / `debug` 为 false；`release` / `dist` 为 true，即定义 `-DNDEBUG`（cl.exe 上是 `/DNDEBUG`）。对照：CMake 的 Release、RelWithDebInfo、MinSizeRel 都定义它；Meson 的 `b_ndebug=if-release`；Cargo 的 release 关闭 `debug_assertions`。
- 在档里写 `ndebug = false` 即可取消（rev 5：不为 `defines` 引入取反语法）。
- 这是行为改变：现有 release 构建里的 `assert` 会消失。写进 changelog，并在升级后第一次 release 构建时提示一次（"release 档现在定义 NDEBUG；要保留 assert，在 `[profile.release]` 里写 `ndebug = false`"）。
- 它会让 release / dist 档的编译旗标变化，因而指纹变化，升级后这两个档会重建一次。dev 档不受影响。
- mcpp 自己的发布产物同样受这条规则约束（mcpp.toml 的 `default-profile = "release"`）。

### 15.5 验收

| 编号 | 内容 |
|---|---|
| R1 | 建议命令：home 路径含空格时，提示中的命令照抄执行能成功（单测加 e2e） |
| R2 | `trim_paths`：release 构建一个用了 `__FILE__`、`std::source_location` 的程序，产物中不出现工程、存储、home、基座的绝对路径，根包显示为 `<name>@<version>/…`；同一份源码在两个不同目录构建，嵌入的路径相同；dev 构建中保留绝对路径；gdb 加载生成的 `source-map.gdbinit` 后能找到源文件；cl.exe 上如实报告 |
| R2b | `ndebug`：release / dist 中 `assert(false)` 不触发、dev 中触发；`ndebug = false` 能改回；升级后的提示只出现一次 |
| R3 | 全新 home 路径含空格与非 ASCII（CI，三平台），在索引版本跟进之后打开 |
| R4 | mcpp 自身发布产物的路径检查门（`.github/tools/check_release_paths.sh`，linux-x86_64 与 macOS） |

## 附录 A. 讨论记录与结论

| # | 问题 | 结论 |
|---|---|---|
| A.1 | sysroot 带自己的 glibc 时能不能用？ | 不能：链接失败；补齐后 loader 与 rpath 错误，rule B 误拒（§1） |
| A.2 | 是设计还是 bug？ | bug：#755 复用了前提已不成立的 `sysroot_mode`，没有测试 |
| A.3 | sysroot 一定带 libc 吗？要不要 `{ libc, path }`？ | 几乎都带，但不只有 libc；不加新键（D1、D7） |
| A.4 | 定位器族名 | 业界词汇：xcrun 的 SDK 名加 `glibc` / `musl` / `msvc`（D3） |
| A.5 | MSVC `{ path }` 的布局 | 两种都支持，按标志目录自动识别（§4.4） |
| A.6 | 运行器的环境变量 | 引擎只给 `MCPP_SYSROOT`；`QEMU_LD_PREFIX` 由 qemu 的包转换（D18） |
| A.7 | `{ path = "/" }` | 支持，写作 `glibc@system`；需要 multiarch（D11） |
| A.8 | Linux 上的 macOS / iOS 模拟器 | 经 macOS 客体或远程 Mac 可以运行；mcpp 只做支持、不分发；本地本轮不测（§1.4） |
| A.9 | x86 是否随 Apple 一起到期 | 不会：目标行看 SDK 切片，执行看执行宿主（D16） |
| A.10 | xtool | 可以做 xpkg，作为 SDK 生成工具与设备后端（§6） |
| A.11 | xcross | 借鉴思路，不联动（§6） |
| A.12 | 引擎与插件的边界 | 核心知识判据（D18） |
| A.13 | 许可证 | 中性机制，不规避（§5.3） |
| A.14 | `config.toml` 还是 `mcpp.toml`？ | **`mcpp.toml` 为主**，`config.toml`（用户级与系统级）为默认，环境变量为覆盖，不另开文件；缺失时友好提示（D14、§3.7、§3.8） |
| A.15 | Swift SDK 包要不要进核心？ | 不进；`SDKROOT` 跨宿主读取，`{ path }` 兜底，路径辅助由包提供（D18） |
| A.16 | `x86_64-ios-sim` 有没有真实需求？ | 运行需求低，构建需求真实；用 CI 的 Intel job 低成本验证，纳入本 PR（§2.6、S15） |
| A.17 | #832 的发布问题有没有放进方案？ | 放进同批（§15）：引号、`trim_paths`、`ndebug`、home 含空格的 e2e |
| A.18 | 必须改 xlings 吗？ | 本次发布不依赖（rev 5：实测发现 xlings 脚本别名未加引号的缺陷，由 xlings#699 修正，索引侧 #1035 让本次链路不经别名）。原结论：不必。`config.toml` 的 `[target.<triple>]` 由 mcpp 自己的 `src/config.cppm` 读取，与 xlings 无关；home 路径含空格只需改 xim-pkgindex（索引已有 `libs/` 共享库机制）；xlings 的 argv 形式 exec 是可以单独做的优化（§15.2） |
| A.19 | `NDEBUG` | 跟随业界惯例（§15.4） |
| A.20 | 根包映射成 `<name>@<version>` 有什么用？ | 产物不暴露开发者的目录；不同目录、不同机器构建出的路径相同（可复现的前提）；崩溃与断言信息直接指明包和版本；调试时由生成的 source-map 映射回来（§15.3） |

**开放问题**（生态侧，不阻塞引擎 PR）：

- xtool 生成 SDK 是否需要 Swift；
- pymobiledevice3 建隧道是否需要 root；
- 用 gdb-remote 附加能否在真机上拿到退出状态；
- 本地虚拟机路线的实际可行性。
