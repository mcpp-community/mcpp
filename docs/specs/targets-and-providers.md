# SPEC-016：目标与提供者

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-016 |
| 标题 | 目标与提供者：行、提供者、选择、平台定义的行、报告与锁 |
| 状态 | 评审中 v1.2 |
| 版本 | 1.2 |
| 最后修改 | 2026-11-12 |
| 对应实现 | mcpp >= 2026.11.9.1；v1.1 条款 mcpp >= 2026.11.10.1；v1.2 条款 mcpp >= 2026.11.12.1 |
| 相关设计文档 | `.agents/docs/2026-11-09-target-model-design.md`、`.agents/docs/2026-11-10-cross-platform-ecosystem-design.md`、`.agents/docs/2026-11-12-ecosystem-architecture-optimization-design.md` |
| 相关 issue | mcpp#911、mcpp#913 |
| 相关规范 | [SPEC-001](package-identity.md)（引用文法）、[SPEC-002](target-side.md)（层与能力）、[SPEC-013](platforms.md)（平台的语句）、[SPEC-014](packaging.md)（打包）、[SPEC-015](target-sets.md)（目标集合） |
| 使用文档 | [21 —— 目标三元组](../zh/21-the-target-triple.md)、[35 —— 平台](../zh/35-platforms.md) |

用语按 RFC 2119：**必须 / 禁止**为强制，**应当**为建议，**可以**为可选。

## 0. 范围

本规范规定一次构建的目标如何确定：用户写出的**请求**、构建所用的**身份**、为身份提供工具链与目标侧各层的**提供者**，以及由
请求得到身份与提供者的唯一规则。它同时规定平台包如何定义新的身份与提供者，以及选择的结果如何报告、记录与锁定。

## 1. 术语

| 术语 | 定义 |
|---|---|
| 请求 | `--target`、`[build] target`、config 的缺省目标中写出的字符串 |
| 身份 | 一个行的三元组；决定输出目录、缓存键、`cfg` 求值与锁中的记录 |
| 行 | 身份的数据：段、档位、机器描述、对象格式、执行系统等（§2） |
| 系统 | 一个 os 段所指的操作系统；不属于任何单独一行的事实记在系统表（§2.3） |
| 提供者 | 一个身份的一份完整供给：工具链、各层、链接、C 环境（§3） |
| 选择 | 由请求得到身份与提供者的规则（§4） |

## 2. 行

### 2.1 字段

行的字段：`triple`（`<arch>-<os>[-<env>]`）、`tier`（verified、preview、planned）、`standard`、`exact`、`march`、`mabi`、
`code-model`、`multilib`、`flags`、`link-emulation`、`clang-triple`、`object-format`、`family`、`data-model`、`exe-suffix`、
`shared-suffix`、`loader`、`execution`、`descriptor-keys`。

- 一个系统可能用到两种以上 C 库时，身份**必须**含 env；env 即 C 库。业界原生标识无 env 段的系统，其原生身份为两段
  （`aarch64-macos`），其余 C 库以 env 区分（`aarch64-macos-musl`）。
- 身份与宿主无关：同一身份在任何宿主上是同一输出目录、缓存键与 `cfg`。
- `exact = true` 的行只由写出其全部段的请求得到（Android 的行）。
- `march`、`mabi`、`code-model`、`flags`、`link-emulation` 是行的机器描述，引擎原样使用，**不可**被替换；机器不同的目标以
  另一行表达（§5）。
- `clang-triple` 是交给 clang 的目标拼写，未写时由段推出；它只改变编译器所见的三元组，不改变身份（v1.1）。
- `loader` 是该行的动态程序所写的程序解释器：运行它的机器上的路径（`/lib/ld-linux-aarch64.so.1`）。内置行与平台定义的行
  同一读法；引擎不另持有按架构的加载器表（v1.2）。
- `execution` 是产物运行的系统，未写时为 os 段。内核相同而系统不同的行**必须**写出它（Android 的行为 `android`）（v1.2）。

### 2.2 内置行

内置行是 mcpp 源码树中的数据（`modules/toolchain-model/data/builtin.toml`），由该包的构建程序编入引擎；运行时不读取文件，
不联网。行的顺序是数据：同一系统中标准身份在前。

- 内置行的 os 与 env 是解析器接受的词，与平台定义的行相同（v1.1）。
- 主流系统的两种身份都是内置数据（v1.1）：系统自身 ABI 的行（`x86_64-freebsd`、`aarch64-freebsd`）没有内置提供者，由平台
  提供；未选平台时，请求被拒绝并指名 `[target.<身份>] platform`。openkal 的 hosted 行（`x86_64-freebsd-musl`、
  `aarch64-freebsd-musl`）由 openkal 提供者经 openkal-platform 0.26.0 服务（openkal-freebsd 0.1.0 发布后，mcpp >= 2026.11.10.2）。
- 32 位 hosted 行（`i686-linux-musl`、`i686-linux-gnu`、`i686-openkal`）为 planned（v1.1）：请求按 planned 拒绝，而非按
  未知目标；显式的 `[target.<身份>] toolchain` 可提前启用。

### 2.3 系统表（v1.2）

`[[platform.system]]` 陈述一个系统不属于任何单独一行的事实，内置数据与平台包同一 schema。

| 键 | 语义 |
|---|---|
| `name` | 该系统各行的 os 段 |
| `uname` | `uname -s` 在该系统上的回答；每个系统一个程序的单文件按它选择程序 |

- 陈述了 `uname` 的系统可以进入多系统单文件；表中未陈述 `uname` 的系统，其程序是文件开头的 PE；表中没有的系统被拒绝，
  并指名 `[[platform.system]]`。
- `mcpp pack --systems all` 取表中的全部系统，加上平台定义了行的系统；其中没有程序可构建的对逐个写为未覆盖（SPEC-014 §4.5）。
- 同一系统陈述两次时取先陈述者；内置数据先于平台。
- 引擎源码中系统名的出现次数由检查持有（`.github/tools/check_system_names.py` 与其基线），只许减少。

## 3. 提供者

| 键 | 语义 |
|---|---|
| `name` | 提供者名；内置提供者以 `builtin:<name>` 指名 |
| `serves` | 所服务的身份；逐段匹配的模式，`*` 代表任一段（`*-linux-musl`） |
| `priority` | 整数，越大越先；内置数据给出缺省值 |
| `hosts` | 提供者在其上可得的宿主（`<os>-<arch>`，`*` 与 `{arch}` 为模式） |
| `pairs` | 可与之配对的编译器族；声明了编译器族的构建只取与之配对的提供者 |
| `toolchain`、`sysroot`、`linkage`、`layers`、`link` | 同 SPEC-013 §1 |
| `hosted` | C 环境是否 hosted |
| `execution` | 产物运行的系统 |
| `tier` | 这一安排的档位；未写时为行的档位（v1.2） |
| `system-headers` | 陈述系统自身接口的头文件所在的包；只由对它们编译的步骤读取（资源编译）（v1.2） |
| `runner` | 该安排的产物怎样启动：`{ cmd = [argv], hosts = [...], requires = [...] }`（§6）（v1.2） |

- 引擎不含提供者的类别，也不含实现名；所有提供者同一结构，只有数据不同。
- 内置提供者：glibc 载荷、musl-gcc、MinGW-w64（与 gcc、llvm 配对）、`mingw-llvm`（v1.1）、MSVC、Apple SDK、Android NDK、
  Emscripten SDK、裸机、openkal-platform（ok 映像，及 `*-linux-musl`、`*-windows-musl`、`*-macos-musl` 的 hosted 部分）。
- 与声明的编译器族配对的提供者提供系统：例如 llvm 与 MinGW-w64 配对时，该提供者的载荷是 clang 的 sysroot、C 库与 C++ 运行时
  （libstdc++）。
- **所选提供者的 sysroot 是该身份的基座陈述（v1.1）。** 行的基座由 sysroot 陈述决定的（gnu-sysroot、
  driver-internal），在项目与本机都未陈述时，先取所选提供者的 `sysroot`（包，或相对平台清单的 `{ path = "<目录>" }`），
  再取平台的 `sysroot`（SPEC-006 §3.9.2 的引擎默认档），按项目陈述的同一规则解析；是基座包时按 SPEC-002 §2.5 读取。
  平台由此为每个身份指名自己的基座。
- **一条条目陈述一种载荷安排（v1.2）。** 原生安排与交叉安排是两条条目，各有自己的 `hosts`、`sysroot`、`linkage` 与
  优先级；其事实随所在宿主而变的条目**必须**拆开。推论：一个身份在其原生宿主上，写与不写 `--target` 得到同一条链接行。
- **`glibc-sysroot`（v1.2）。** 以基座包 `xim:aarch64-linux-gnu-sysroot` 与 llvm 服务 `aarch64-linux-gnu` 的交叉安排，在
  `linux-x86_64` 可得，优先级 40，动态链接，档位 preview；缺省运行器为 `qemu-aarch64-static -L {sysroot} {}`。原生安排仍是
  `glibc`（优先级 100，`linux-{arch}`）。
- **`system-headers`（v1.2）。** 资源脚本由 llvm-rc 编译、而工具链未陈述任何头文件来源的行，取所选提供者的 `system-headers`
  所指的包的 `include`；已陈述来源的行（MSVC 的 SDK、配对载荷）不解析它，也不安装该包。该包无法安装时构建被拒绝并指名它。
- **`mingw-llvm`（v1.1）。** 以基座包 `xim:mingw-w64-sysroot`（llvm-mingw 的目标侧：UCRT 上的 MinGW-w64、libc++、libunwind、
  compiler-rt）与 llvm 服务 `x86_64-windows-gnu` 与 `aarch64-windows-gnu`，在任一宿主可得，优先级 5，低于有 gcc 载荷之处的
  MinGW-w64 提供者。基座是 clang 的 sysroot；其 libc++ 的模块源码是 std 模块；编译器运行时取自基座所指的目录；静态链接取基座的
  归档而非其导入库。

## 4. 选择

1. **显式者优先。** `--platform`、`MCPP_TARGET_<T>_PLATFORM`、项目与 config 的 `platform`（含 `builtin:<name>`）只保留所指的
   提供者；`--platform none` 不用平台包实现的提供者；声明的编译器族只保留与之配对的提供者。
2. **请求是模式。** 写出 env 的请求只匹配该行；只写 `<arch>-<os>` 的请求匹配该系统的全部行（`exact` 的行除外）。
3. **顺序是数据。** 候选（行与服务它的提供者）按 `priority` 从大到小，相同者按行的顺序，再按提供者名排序。
   `[target.<selector>.priority] <provider> = <n>` 改写所匹配行上的优先级；项目先于 config。
4. **取第一个可得者。** 提供者服务该行、在本宿主可得（`hosts`）、探测成立（MSVC）、与声明的编译器族配对，即为可得。皆不可得
   时取第一个候选，由缺少之处拒绝，拒绝写出各候选及其不可得的原因（`host-cannot-serve`）。
5. **身份随 C 库。** 依赖图提供 C 库（`mcpp:c-abi`）而请求未写 env 时，身份取实际 C 库的行，重新准备一次。写出的 env 是项目的
   拼写，不被替换；与图不一致时报告（目标报告的 `asks for the … C ABI`），不拒绝。

6. **请求的回答是契约（v1.2）。** 新增的提供者条目**禁止**改变任一宿主上既有请求的回答：它以低于现有回答的优先级进入，
   只由写全身份的请求或显式选择取得。改变一个请求的缺省回答是一次单独的决定，记入 CHANGELOG 与 §8。行表快照
   （`tests/fixtures/target-rows.snapshot`）记录每个请求在每种宿主上的回答。

## 5. 平台定义的行与提供者

平台包以 `[[platform.define]]` 定义行（§2.1 的字段），以 `[[platform.provider]]` 声明提供者（§3 的键）；`[platform]` 与
`[platform.hosted]` 是两种简写。

- 已解析平台定义的 os 与 env 为解析器接受的词；`cfg(os = …)` 按定义的行求值。
- 读取平台清单时按行检查其语句：与内置行同名的定义、同一行的两次定义**必须**被拒绝；不服务任何行的提供者、以同一优先级
  服务同一身份的两个提供者为警告，按清单警告的规则只在平台自身的构建中给出。
- 平台定义的行不取宿主载荷的 C 库：除非平台或项目声明 sysroot，该行没有 C 库。
- **C 库由平台描述的行与内置行由同一谓词读取（v1.1）。** 平台定义的行，以及没有内置提供者的内置行，其 `march`、
  `link-emulation` 等机器描述进入编译与 ld.lld 链接（`mcpp why flags` 标为 `row`）；工具链载荷为其自身宿主构建的目标侧——
  libc++ 头文件、运行时目录、编译器运行时目录、宿主的 `-L` 与 RPATH——不进入该行的编译与链接；本机加载器的闭包与符号检查
  只在该行的执行系统为本机系统时进行。
- 首次遇到只在索引中的平台时，请求以临时行进入第一次准备，平台取得后重新准备。

## 6. C 环境、链接与执行

- **hosted。** 声明 `hosted-standard-library` 的 sysroot，或 `hosted = true` 的提供者，使裸机行不加 `-ffreestanding`。
- **链接驱动。** 平台的 `link.driver = "compiler"` 在每一行生效，包括行本身以 `link-emulation` 直接调用 ld.lld 的行。
- **执行。** 行的 `execution` 不是本机系统时：构建不以本机加载器检查产物的运行时闭包；`mcpp run` 与 `mcpp test` 在无运行器、
  无执行主机时报告「已构建，未运行」。运行器包是目标的工具（`[target.<sel>.tools]`），以 docs/41 的运行协议启动。打包从产物
  文件读取其闭包，不运行本机加载器（SPEC-014，v1.1）。
- **一个谓词（v1.2）。** 「产物是否由本机运行」只有两个问法，各在一处回答，内置行与平台定义的行相同：执行系统是否为本机
  系统（启动之前问）；执行系统与架构是否都是本机的（检查加载器与运行时闭包之前问）。
- **不启动另一系统的程序（v1.2）。** 执行系统不是本机系统、又没有运行器与执行主机时，`mcpp run` 与 `mcpp test` 在启动任何
  进程之前回答「已构建，未运行」，写出两个系统与可写的声明；退出码为 2。本机内核不读取程序所属的系统，另一系统的系统调用
  号在本机另有含义，所以启动不是观察。`--no-runner` 陈述该身份在本机可直接执行，照此执行。同一系统、另一架构的产物照旧
  启动：内核拒绝它，或由已登记的模拟器运行它。
- **程序解释器（v1.2）。** 由本机运行的动态产物写基座自己的加载器所在的路径；不由本机运行的动态产物写行的 `loader`，且
  不带任何本机目录的 RUNPATH。构建机的安装路径**禁止**进入后一类产物。
- **基座包只有一种读法（v1.2）。** 根目录带清单的基座是包根即 sysroot（SPEC-002 §2.5），不论其目录怎样命名；目录布局只对
  不带清单的目录探测。另一台机器的基座不取工具链载荷为其自身宿主构建的运行时目录。
- **提供者的运行器（v1.2）。** 提供者条目的 `runner` 是运行器陈述的最低一级：其上依次是平台、构建程序、本机配置、项目与
  环境变量（SPEC-013 §4.5）。`hosts` 列出该运行器所在的宿主，空为任一宿主。模板中 `{}` 是产物，`{sysroot}` 是该安排的
  基座，`{pkg:<name>}` 是 `requires` 中该名字的包的根；模板的程序先在 `requires` 的包中查找，再查 PATH。`requires` 的包
  在第一次 `mcpp run` 或 `mcpp test` 时按工具链的同一方式安装；构建不安装它们。
- **运行器的身份（v1.2）。** 平台的 `runner` 可陈述 `built-as = { "<os>-<arch>" = "<身份>" }`：为该宿主对构建的运行器程序
  是哪个身份。自带运行器的包按它为各系统构建运行器；引擎只对它一向认识的三个系统保留约定，其余系统无此陈述即无程序。
- **`link.flags` 与链接驱动（v1.1）。** `[platform.link] flags` 只交给 ld.lld；`driver = "compiler"` 的平台陈述它时，构建
  给出警告并指名 `[platform.build] ldflags`。

## 7. 报告、机器输出与锁

- 状态行：`Target <请求> → <身份>`；回落时附原因。
- `mcpp why target [--format json]` 写出请求、身份、提供者、优先级、回落原因与按序的候选；`mcpp why flags [<unit>]` 写出每个
  参数及其来源层（行、平台、工具链、包、引擎）；`mcpp why platform` 写出平台定义的行与所服务的身份，各附提供者与优先级
  （根包是平台包时为其自身，否则为目标所选的平台）。
- 构建记录与 `plan` 事件的 `configuration.target` 为 `{request, identity, provider, priority, fallback}`；本机构建为 `null`。
- 构建程序读 `MCPP_TARGET_PROVIDER`（`mcpp::target_provider()`）。
- 声明 `requires = ["mcpp:kernel-abi=…"]` 的包，其构建程序读 `MCPP_KERNEL_ABI_INTERFACES`（`mcpp::kernel_abi_interfaces()`）：
  所解析的 kernel-abi 提供者的 `provides-interfaces`，以 `;` 连接；总是设置，空为未知（v1.1）。
- `mcpp.lock` 以 `[target."<request>"] identity, provider` 记录每个请求得到的身份；`--locked` 下选择另一身份的构建被拒绝，
  并写出锁中的与本次的身份。

## 8. 兼容性

- 已有身份字符串、输出目录与 `cfg` 结果不变；`[target.X]`、`descriptor_keys`、`[platform.hosted]` 照旧可读。
- 行为变化（v1.2）：执行系统不是本机系统的产物，在没有运行器时不再被启动（此前 `mcpp run` 启动它并报告内核的回答）；
  经 binfmt 登记了另一系统解释器的宿主以 `--no-runner` 或一条 `runner` 声明照旧运行。
- 行为变化：只写 `<arch>-<os>` 的请求在本宿主不可得其标准身份时取下一个可得的身份（无 MSVC 的宿主上 `x86_64-windows` 为
  `x86_64-windows-musl`；非 macOS 宿主上 `aarch64-macos` 为 `aarch64-macos-musl`；x86_64 宿主上 `aarch64-linux` 为
  `aarch64-linux-musl`）；依赖图提供 musl 而请求未写 env 时身份为 `-musl`。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-11-09 | 初版：行、提供者、选择、平台定义的行、C 环境、执行、报告与锁 |
| 1.1 | 2026-11-10 | `clang-triple`；内置行的系统词；FreeBSD 两种身份与 32 位 planned 行；提供者 sysroot 为基座陈述的最后一级；`mingw-llvm` 与 `aarch64-windows-gnu`；C 库由平台描述的行与内置行同一读法；打包读文件闭包；`link.flags` 的警告；构建程序的 `MCPP_KERNEL_ABI_INTERFACES` |
| 1.2 | 2026-11-12 | 行的 `loader` 与 `execution` 为内置行所读；系统表（`uname`）；提供者的 `tier`、`system-headers`、`runner`；一条条目一种安排；请求的回答是契约；`glibc-sysroot`；一个执行谓词，另一系统的程序不被启动；程序解释器与基座包的单一读法；运行器的占位、`requires` 与 `built-as` |
