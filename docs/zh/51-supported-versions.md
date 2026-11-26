# 51 —— 受支持的版本与兼容性

**读者：**带着政策问题而来的人——一次评审流程、一次隔网构建、一个被钉住的版本。

**本章回答的那一个问题：**哪些发布受支持、支持多久，以及版本之间什么可以变、
什么不可以变。

**不在这里：**退出码表，那是 [SPEC-003](../specs/exit-codes.md)；机器输出的
schema，那是 [50 —— 机器可读输出](50-machine-output.md)。

本文陈述哪些发布受支持、支持多久，以及版本之间允许发生什么变化。它的存在是因为
一个采用 mcpp 的工程会被自身的评审流程问到这些问题，而只存在于维护者脑子里的
答案无法被引用。

## 版本号

发布以 `YYYY.M.D.N` 命名——发布日期，加上当天发布的序号。这套方案的数字不携带
任何兼容性承诺：`2026.9.4.1` 不是 `2026.9.3.2` 的「小版本」。什么可以变、什么
不可以变，由下文陈述，而不是编码在版本号里。

## 支持范围

| | |
|---|---|
| **受支持** | 最新的一个发布 |
| **安全修复** | 最新的发布，以及上一个自然月的最后一个发布 |
| **不受支持** | 更旧的一切 |

发布是被取代，不是被撤回。更旧版本的已发布资产与索引条目保持原位，因为某个工程
可能钉住了其中一个，移除会破坏一个本来能正常构建的工程。

## 版本之间允许的变化

引擎自身各个接口的稳定性并不相同，这个差别值得精确陈述。

| 面 | 稳定性 |
|---|---|
| `mcpp.toml` 的键 | 只增。既有键保持原有含义；无法识别的键会被报告，不会被静默忽略。例外是下文的弃用 |
| CLI 命令与旗标 | 只增。被替换的拼写保留为别名 |
| 机器输出（`--message-format json`） | 由 `schemaVersion` 版本化，见 [50](50-machine-output.md) |
| `build.mcpp` 指令协议 | 版本化，见 `kProtocolVersion`。引擎遇到声明版本**高于**自身的程序会拒绝，而不是猜测 |
| `mcpp.lock` 格式 | 由 `schemaVersion` 版本化；更旧的文件在读取时被迁移 |
| 目标表的行 | 只增。一行的档位可以上升；只要还有已发布的包以它为目标，该行就不会被移除 |
| 构建指纹、缓存布局、`target/` 的内容 | **不是接口。** 它们不经通知即变化，任何东西都不应解析它们 |

一个调用了自身引擎所带 `mcpp` 模块中不存在的函数的 `build.mcpp`，失败发生在
构建程序的**编译**阶段，而不是通过协议错误。协议号管的是线上的指令；类型化 API
由所装的引擎决定。两者都写在这里，是因为包作者看到的失败取决于他越过了哪一条边界。

## 弃用

清单中将被移除的写法或读法先被弃用；"已有的键保持其含义"的例外，恰好是本表的各行
（mcpp 2026.10.11.1+）。

- 被弃用的写法照常工作。构建可以编辑的清单（根、工作空间成员、工作空间内的 path 包）
  中的每次使用都给出警告，写出取代它的写法、弃用它的版本与移除它的版本；`--strict` 下拒绝。
  `mcpp why compat` 列出所有使用，包括依赖中的使用。
- 移除以 CalVer 月份表示。弃用的版本与该月份之间至少相隔两个完整的日历月，读法在该月的
  第一个版本或之后移除。
- 本表由引擎中的唯一登记表（`modules/manifest/src/compat/registry.cppm`）生成；某个版本
  到达某一行的月份而读法仍在时，引擎自己的测试失败。

| 读法 | 弃用版本 | 移除版本 | 应写作 |
|---|---|---|---|
| `workspace-position`：工作空间根自己的 `[toolchain]`、`[indices]`、`[profile.<name>]`、`[target.<triple>]` 标量或 `[xlings]` 按位置到达成员 | 2026.10.10.1 | 2027.1 | `[workspace.toolchain]`、`[workspace.indices]`、`[workspace.profile.<name>]`、`[workspace.target.<selector>]`、`[workspace.xlings]` |
| `host-module-in-dependencies`：`[dependencies]` 中请求 `host-module = true` 或 `rule_module` 特性的条目，按构建期依赖读取 | 2026.10.11.1 | 2027.1 | `[build-dependencies]` 中的同一条目 |
| `abi-capability`：`abi:` 运行时能力（`abi:glibc`、`abi:libc=<c>`、`abi:cxxstdlib=<c++>`），按目标侧某一层的需求读取 | 2026.10.26.1 | 2027.1 | `requires = ["mcpp:c-abi=<impl>"]` 或 `["mcpp:c++-abi=<impl>"]` |
| `undeclared-cxx-runtime`：链接线点名、或链接线上的库需要的、本构建之外的另一套 C++ 运行时，且没有包声明 | 2026.10.26.1 | 2027.1 | 在需要它的包中写 `requires = [{ layer = "mcpp:c++-abi=<impl>", visibility = "private" }]` |
| `plugin-imports-from-build-dependencies`：构建插件的库导入其自身 `[build-dependencies]` 中以 `host-module = true` 写出的包 | 2026.10.11.1 | 2027.1 | 插件 `[dependencies]` 中的该包 |
| `pack-universal`：`[pack] universal`——`true` 被拒绝，`false` 被忽略；打包的目标即其腿 | 2026.10.30.1 | 2027.1 | `[pack] targets = ["openkal"]`（全部架构），或一个 `--target` |
| `platform-universal`：平台的 `[platform] universal`，被忽略；打包的目标即其腿 | 2026.10.30.1 | 2027.1 | 平台中不写；由打包命名目标（`--target openkal`） |
| `no-universal-flag`：`mcpp pack --no-universal`，单目标打包不再需要它（与目标集合同用时拒绝） | 2026.10.30.1 | 2027.1 | `--target <arch>-<os>`（一条腿） |
| `quoted-dotted-dependency-key`：带点的加引号依赖键（`"mcpplibs.cmdline" = "0.0.2"`），即一个 TOML 键，按其中的点所拼写的选择器读取；它是一个包与其父包或子包并列时的唯一写法时除外（docs/05） | 2026.11.8.1 | 2027.2 | 不加引号的选择器 `mcpplibs.cmdline = "0.0.2"` |
| `ambiguous-contract-name`：构建程序以一个名字提问（`dep_dir`、`dep_linkage`、`dep_bin`、`xpkg_dir`），该名字的契约变量由多个依赖或载荷回答，回答为其中的第一个 | 2026.11.21.1 | 2027.2 | 精确的拼写：限定名，或 `xpkg_dir("<ns>", "<name>")`；没有可区分的拼写时，改名其中一个包 |
| `reopened-dotted-table`：点号键定义了一个表之后，又由 `[表头]` 重新打开它（`[dependencies]` 下写了 `compat.libpng = ...` 之后又写 `[dependencies.compat]`），TOML 1.0 不允许 | 2026.11.27.1 | 2027.3 | 把表头下的条目写成它所扩展的表中的点号键，`compat.zlib = ...`；下一次对该表执行 `mcpp add` 时即如此改写 |
| `implicit-default-environment`：未陈述环境的工程使用机器缺省环境中已安装内容的并集：其库进入运行期搜索路径与链接期的 `--sysroot`，其 `bin` 进入测试子进程的 PATH（Windows 除外） | 2026.11.27.1 | 2027.3 | 依赖提供该库或工具的包，或写 `[xlings] subos = "default"` 陈述使用共享环境 |
| `unmatched-exact-export`：`exports` 中不含通配符、却指名共享镜像未定义之符号的条目，被无声地从发布集合中去掉 | 2026.11.27.1 | 2027.3 | 镜像所定义的名字（C++ 名字写修饰后的形式，或以 `extern "C"` 声明该函数），或把该条目写到定义它的 `[target.<selector>]` 下 |

## 复现一次构建

`mcpp.lock` 记录一次构建解析出的结果。`--locked` 断言当前解析与它一致，不一致
则失败，并点名发生变化的那个包：

```
error: --locked was given and this resolution differs from mcpp.lock:
         mcpplibs.cmdline 0.0.1 -> 0.0.2
```

发布构建、审计或 CI 作业应当传入 `--locked`。它会关闭构建快路径，因此该断言
总会执行。没有 `mcpp.lock` 时没有可供断言的记录：此时 `--locked` 失败并说明这一点，
而不是把每个包都报成发生了变化，也不写出 lock。

锁尚不约束解析——它只记录并校验解析结果。把锁作为解析的输入来钉住结果，是对
解析器的另一项改动。

## 物料清单

`mcpp emit sbom` 写出一份描述**已记录**解析结果的 CycloneDX 1.5 文档：

```bash
mcpp emit sbom -o sbom.json
```

组件的 `name`、`group` 与 `purl` 陈述的是包本身，与声明它的清单如何拼写无关：`cmdline` 与
`mcpplibs.cmdline` 都是 `pkg:mcpp/mcpplibs/cmdline@<版本>`。变体在名字中保留其摘要；只被构建程序
持有的版本就是该版本的这个包，并为每个构建程序带一条 `mcpp:build-program` 属性。purl 对各段做
百分号编码，因此 `+` 写作 `%2B`。

它读取 `mcpp.lock` 而不是重新解析，因为一份描述了与实际构建不同的依赖图的
文档，比没有文档更糟。许可证 mcpp 无法识别的组件会被写成 `NOASSERTION` 而不是
省略：缺失的字段读起来像「未检查」。没有 `mcpp.lock` 时，文档只列出根包，并在标准
错误上提示没有任何记录：这对没有依赖的工程是全部事实，对从未构建过的工程则不是。

## 缺省版本（2026.11.21.1 起）

缺省 —— 首次运行安装的工具链、引导工具（ninja、patchelf）—— 取 mcpp 发布时其上游
最新的正式发布，经 mcpp 的 CI 与 canary 验证。更新的版本与实验版本由用户陈述
（`[toolchain]`、xlings 的包）；mcpp 不以它们为缺省。缺省是内置表
（`modules/toolchain-model/data/builtin.toml`）的 `[[default]]` 条目，发版时逐条与索引
比对（`.github/tools/check_defaults.sh`）。一个包的缺省取其所服务的每个宿主都已发布的最新正式发布：
llvm-project 的 23.1.6 没有发布 macOS 的二进制，而 llvm 的缺省包括 macOS，所以保持 23.1.3。内置表的
一行取它所服务的每个宿主都能安装的最新正式发布：musl 的行的交叉载荷（自 x86_64 的交叉，与 Windows
上的 Canadian cross）只发布了 16.1.0，所以保持 16.1.0。工具链的配套载荷（binutils、Linux 头文件）
取其载荷声明的版本（20 —— 工具链，「配套载荷」）。

xim-pkgindex 中一个包的版本是其上游的版本；同一上游版本的重新打包是同一个键的
`revision`，mcpp 对某个上游正式发布的修正写作 `<上游>-mcpp.<n>`（`ninja@1.13.2-mcpp.1`）。
已发布的键不改名：`binutils@2.42.1` 指 binutils 2.42。

缺省改变时，以先前缺省构建过的工程各提示一次（20 —— 工具链，「解析与锁」）；陈述了
工具链的工程保持不变。

## 离线与隔网使用

`--offline`（或 `MCPP_OFFLINE=1`，或 `[build] offline = true`）会阻止一切网络访问：
索引刷新、包下载与工具链安装。home 缺少的载荷取自写明的归档目录；否则需要网络的构建
会点名自己所需要的东西并失败，而不是尝试联网。`mcpp fetch --to` 与 `--from` 把工程的
输入带到没有网络的机器上（05 —— 依赖，「离线构建与 mcpp fetch」）。

## 报告问题

缺陷与安全报告提交到拥有该组件的仓库的 issue tracker——引擎、包索引，或者
包本身。一份点明了版本、宿主、目标与命令的报告是可处理的；缺了这四项的报告
通常会换来一次索要它们的回复。

## 当前边界

- **`mcpp.lock` 目前尚不约束解析。** 它记录一次构建解析出的结果，`--locked`
  核验新的解析与它一致；把锁作为解析的**输入**来钉住结果，是对解析器的另一项
  改动。
- SBOM 描述的是**已记录**的那次解析，而不是重新解析一次，因此在修改 manifest
  之后、执行 `mcpp update` 之前写出的文档，描述的是更早的那张依赖图。
- 许可证 mcpp 无法识别的组件会被写成 `NOASSERTION` 而不是省略，因为缺失的
  字段读起来像「未检查」。
