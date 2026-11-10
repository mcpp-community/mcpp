# SPEC-016：目标与提供者

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-016 |
| 标题 | 目标与提供者：行、提供者、选择、平台定义的行、报告与锁 |
| 状态 | 评审中 v1.1 |
| 版本 | 1.1 |
| 最后修改 | 2026-11-10 |
| 对应实现 | mcpp >= 2026.11.9.1；v1.1 条款 mcpp >= 2026.11.10.1 |
| 相关设计文档 | `.agents/docs/2026-11-09-target-model-design.md`、`.agents/docs/2026-11-10-cross-platform-ecosystem-design.md` |
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

### 2.2 内置行

内置行是 mcpp 源码树中的数据（`modules/toolchain-model/data/builtin.toml`），由该包的构建程序编入引擎；运行时不读取文件，
不联网。行的顺序是数据：同一系统中标准身份在前。

- 内置行的 os 与 env 是解析器接受的词，与平台定义的行相同（v1.1）。
- 主流系统的两种身份都是内置数据（v1.1）：系统自身 ABI 的行（`x86_64-freebsd`、`aarch64-freebsd`）没有内置提供者，由平台
  提供；未选平台时，请求被拒绝并指名 `[target.<身份>] platform`。openkal 的 hosted 行（`x86_64-freebsd-musl`、
  `aarch64-freebsd-musl`）由 openkal 提供者经 openkal-platform 0.26.0 服务（openkal-freebsd 0.1.0 发布后，mcpp >= 2026.11.10.2）。
- 32 位 hosted 行（`i686-linux-musl`、`i686-linux-gnu`、`i686-openkal`）为 planned（v1.1）：请求按 planned 拒绝，而非按
  未知目标；显式的 `[target.<身份>] toolchain` 可提前启用。

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

- 引擎不含提供者的类别，也不含实现名；所有提供者同一结构，只有数据不同。
- 内置提供者：glibc 载荷、musl-gcc、MinGW-w64（与 gcc、llvm 配对）、`mingw-llvm`（v1.1）、MSVC、Apple SDK、Android NDK、
  Emscripten SDK、裸机、openkal-platform（ok 映像，及 `*-linux-musl`、`*-windows-musl`、`*-macos-musl` 的 hosted 部分）。
- 与声明的编译器族配对的提供者提供系统：例如 llvm 与 MinGW-w64 配对时，该提供者的载荷是 clang 的 sysroot、C 库与 C++ 运行时
  （libstdc++）。
- **所选提供者的 sysroot 是该身份的基座陈述（v1.1）。** 行的基座由 sysroot 陈述决定的（gnu-sysroot、
  driver-internal），在项目与本机都未陈述时，先取所选提供者的 `sysroot`（包，或相对平台清单的 `{ path = "<目录>" }`），
  再取平台的 `sysroot`（SPEC-006 §3.9.2 的引擎默认档），按项目陈述的同一规则解析；是基座包时按 SPEC-002 §2.5 读取。
  平台由此为每个身份指名自己的基座。
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
- 行为变化：只写 `<arch>-<os>` 的请求在本宿主不可得其标准身份时取下一个可得的身份（无 MSVC 的宿主上 `x86_64-windows` 为
  `x86_64-windows-musl`；非 macOS 宿主上 `aarch64-macos` 为 `aarch64-macos-musl`；x86_64 宿主上 `aarch64-linux` 为
  `aarch64-linux-musl`）；依赖图提供 musl 而请求未写 env 时身份为 `-musl`。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-11-09 | 初版：行、提供者、选择、平台定义的行、C 环境、执行、报告与锁 |
| 1.1 | 2026-11-10 | `clang-triple`；内置行的系统词；FreeBSD 两种身份与 32 位 planned 行；提供者 sysroot 为基座陈述的最后一级；`mingw-llvm` 与 `aarch64-windows-gnu`；C 库由平台描述的行与内置行同一读法；打包读文件闭包；`link.flags` 的警告；构建程序的 `MCPP_KERNEL_ABI_INTERFACES` |
