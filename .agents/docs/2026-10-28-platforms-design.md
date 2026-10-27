---
subject: platform
status: accepted
---

# 平台：目标体系的通用扩展；openkal 作为第一个平台包；IDE 一线与 mcppls 的同批升级

日期：2026-10-28。前文：[part 1](2026-10-26-control-contract-runtime-and-ide-ecosystem-design.md)（控制契约、IDE 生态规划 §13–§14）、
[part 2](2026-10-26-ecosystem-part2-os-library-over-openkal-design.md)（ok 映像与 `<arch>-openkal`）。
基线：mcpp `7d704820`（2026.10.26.2）+ release PR [#847](https://github.com/mcpp-community/mcpp/pull/847)（含 #846）；openkal 0.17.1 轮
（openkal-linux 0.18.2、openkal-macos 0.15.1、openkal-windows 0.13.3、openkal-musl 0.22.3、openkal-llvm-runtime 0.16.3、openkal-image 0.1.1）；
mcpplibs.os 0.2.0；mcppls 0.0.18（[#72](https://github.com/Sunrisepeak/mcpp-language-server/pull/72)，未合入）；mcpp-ide 本地仓库 `90ae383`；
mcpp-vscode 0.8.0；mcpp-plugins [#51](https://github.com/mcpp-community/mcpp-plugins/pull/51)（0.20.0，未合入）。
相关规范：SPEC-002、004、006、007、011、012。

**修订记录**

- rev 1（2026-10-27）：openkal 内置目标栈（`[openkal]` 表、`src/openkal/`）。
- rev 2（2026-10-28）：通用平台机制与 SPEC-013；openkal 是第一个平台包；板级支持包与目标表一并迁移。
- rev 3：第二轮评审——键表精简为七组（§4）；宿主图与目标图分别选择平台，构建程序可以作为 ok 映像由 `okrun` 运行（§6）；
  mcppls（含语义高亮的时延，§11.1）、mcpp-ide、mcpp-clion、mcpp-vscode、mcpp-plugins 纳入同批，作为生态级方案（§11）；
  #846 并入 #847；每仓库一个 PR、commit 作检查点的交付计划（§13）；多视角自审（§15）。
- **rev 4（本稿）**：第三轮评审——§16 的 1–7 定案；mcpp-clion 以插件依赖的形式依赖 mcppls 的 CLion 插件，语言功能只由 mcppls 提供
  （§11.2）；VS Code 与 JetBrains Marketplace 的发布由维护者上传，交付物为产物路径（§13.3）。§15 按定案重审，§16 改为本轮待确认项。
- **rev 6**：补充设计 part 2（打包与分发、okb、`.okso` 本机绑定、体积、openkal 0.17.2 轮）定案，以 mcppls 与 mcpp-ide 交叉验证（§17）。
- **rev 5**：§16 定案（VS Code 以 `extensionPack`、CLion 以 `<depends>` 引入 mcppls；上传交付物为本机验证后的产物目录）；开始实施。
  实施中发现两处缺口，列入同批：os 0.2.0 在宿主三元组上以 openkal-musl 为 C 库时不能编译（L9，os 0.2.1）；M3 载体的链接取到宿主 GCC 的
  `crtbeginT.o`（L10，§4.7）。

## 0. 摘要

**平台**是一类目标的一份可复用、带版本、可发布的配置：工具链、目标侧各层的默认提供者、本机工具、编译与链接约定、
产物命名与封装、运行器。平台包在自己的清单中以 `[platform]` 段声明；使用者在 `[target.<selector>]`（目标图）或
`[host]`（宿主图）中以同名键覆盖；构建程序在平台阶段以 SPEC-007 指令陈述的内容与之同义。引擎只提供机制：选择、
四层合并、把默认提供者放进依赖图、执行封装、报告来源。

openkal 由数据包 `openkal-platform` 描述；目标表为 `<arch>-openkal` 钉住它的一个版本，即"内置支持"。`mcpp build|run|test|pack
--target x86_64-openkal` 不写任何声明即可工作；引擎中不再有 openkal 专属代码。宿主图同样可以选择平台：选择 openkal 平台时，
构建程序编译为本机架构的 ok 映像，由 `okrun` 运行，在三个系统上行为与产物一致。

同批升级 IDE 一线：mcppls 修正大文件语义高亮远慢于补全的问题并迁移到平台；新仓库 mcpp-ide 交付后端、VS Code 适配层
（沿用扩展 ID `mcpp-vscode`）与 JetBrains 适配层（`mcpp-clion`）；mcpp-vscode 转为指向 mcpp-ide。

## 1. 问题（实测）

| 编号 | 观察 | 出处 |
|---|---|---|
| P1 | 净室中 `mcpp new hello && mcpp run --target x86_64-openkal` 需手写 `openkal-tools` 与 `openkal-llvm-runtime` 四行。 | 2026-10-27 净室，2026.10.26.2 |
| P2 | 缺少 `okld` 的提示写死 `openkal-tools = "0.1.0"`。 | `src/okimage/okimage.cppm` |
| P3 | 两个 openkal 版本同图时，多版本改名只改 C++ 模块名，不改 C 头文件与链接符号，故障出现在无关位置。 | part 1 §25.8 |
| P4 | 精确版本在本地索引中缺失时安装失败不刷新，并尝试 `compat:` 拼写。 | 用户报告；#847 |
| P5 | os、mcppls 在清单中重复声明运行时与工具，CI 从源码自建 `okrun`。 | os、mcppls |
| P6 | 承载 openkal 行为的引擎代码有六处：`flags`、`ninja_backend`、`prepare/plan`、`execute`、`pack`（按包名找 `openkal-tools`）、`triple`。 | `grep` |
| P7 | 报告把目标默认的运行时标为 `graph`。 | targetside 报告 |
| P8 | 体系知识分布在五种互不相通的机制：目标表行、`sysroot` 覆盖、payload 默认运行器、板级支持包的构建程序指令、`[target.<selector>]` 键。 | docs/04、22、32、34、41 |
| P9 | 构建程序由宿主工具链按宿主约定编译：同一 `build.mcpp` 在 Windows 上由 MSVC、在 Linux 上由 GCC 编译，行为与诊断随系统不同，缓存不能跨系统复用。 | SPEC-007 §10 |
| P10 | mcppls 中，大文件的补全很快生效，语义高亮往往晚很多（§11.1 分析）。 | 用户报告 |
| P11 | IDE 一线（part 1 §14）尚未交付：mcpp-ide 只有本地后端，无远端仓库；VS Code 与 JetBrains 适配层为空目录；mcpp-vscode 仍是独立实现。 | mcpp-ide `90ae383` |

## 2. 原则

1. **目标的属性归平台，平台归生态。** 引擎不持有体系知识。
2. **引擎只提供机制与规范。**
3. **一套词汇，三种写法同义**：平台包声明约定，使用者声明覆盖，构建程序陈述需要计算的部分。先例：R9.9。
4. **声明优先，代码兜底。**
5. **内置即钉住**：内置支持是目标表中的一个版本号。
6. **宿主与目标对称**：两张图各自选择平台，机制相同；宿主图的平台默认是宿主约定。
7. **一致、稳定、可观察**：同一份报告、同一个锁、同一组优先级；每个值可追溯来源；拒绝时给出键、来源与出路。

## 3. 词汇

| 词 | 含义 |
|---|---|
| 目标（target） | 产物运行的机器与格式，以三元组命名 |
| 平台（platform） | 一类目标的配置包（`openkal-platform`、一块开发板的支持包）；区别于构建配置 `--profile` |
| 目标图 / 宿主图 | 为目标构建的包；为构建机器构建的程序（构建程序、工具） |
| 层（layer） | `kernel-abi`、`c-abi`、`c++-abi`、`compiler-runtime`（SPEC-002） |
| 工具（tool） | 在构建机器上运行的程序（`okld`）；区别于随产物发布的 `artifacts`（R6.4） |
| 封装（seal） | 链接之后、发布之前对产物本身的处理与检查；区别于 `artifact` 角色（生成另一个文件） |
| 运行器（runner） | `mcpp run` / `mcpp test` 执行产物的方式；`runners.<name>` 为命名的设备动作 |
| 载体（carrier） | 把依赖加载器的产物与加载器合成一个本机程序的宿主程序包（M3） |

## 4. 平台包规范（SPEC-013 要点，精简后的七组键）

```toml
[platform]
targets   = ["<triple>", …]                  # 服务的目标
toolchain = "<spec>"                         # 约定的工具链
sysroot   = …                                # 预构建的目标基（可选；同 [target].sysroot）
runner    = ["<prog>", "{}"]                 # 或 { cmd = […], host = "same-arch" | "any", exclusive = bool }
runners   = { <name> = [ … ] }               # 命名的设备动作（flash、monitor…）
carrier   = { package = "<spec>", section = "<节名>", main = "<入口源码>" }   # M3 载体（可选）
plugin    = "build.mcpp"                     # 平台包的构建程序，在平台阶段运行（可选）

[platform.layers]   <layer> = <依赖规格> | false
[platform.tools]    <name>  = <依赖规格> | { program = "<名字>" }
[platform.build]    cflags / cxxflags / asmflags / ldflags = [ … ]
[platform.build.'cfg(<谓词>)']   同上，条件化（沿用 cfg 语法，例如 arch = "aarch64"）
[platform.link]     driver = "compiler" | "lld";  flags = [ … ]（driver = "lld" 时直接给 ld.lld 的参数）
                    start = "<符号>";  script = "<包内路径>"
                    inputs = [ { file = "<包内路径>" } | { from = [argv…], as = "<名字>" } ]
                    exe = "{name}.<ext>";  shared = "lib{name}.<ext>"
                    seal = [ [argv…], … ]
[platform.versions] <包> = "<版本>"          # 一组相互一致的版本（§8.1）
[platform.hosted]   targets、toolchain、layers   # 宿主三元组选用本平台时适用的子集（§5.4）；targets 限定可用的宿主三元组
```

相对 rev 2 的精简：`platform-spec`、`requires-mcpp` 删去，改用 `[package] mcpp`（R9.8，平台规范版本随引擎版本）；`round` /
`round-members` 合为 `versions`（平台包自身的版本就是版本组的名字）；`artifact.*` 与 `seal` 并入 `link`（"产物怎样生成"是一件事）；
`build.<arch>` 改用已有的 cfg 子表；`run.host`、`run.exclusive` 并入 `runner` 的表形式；`pack.carrier` 改为顶层 `carrier`。
功能不变：rev 2 的每个键都有对应。

约定沿用已有写法：argv 中 `{}` 为产物；程序以名字书写，按"图中为本机构建的程序 → 声明的载荷 → `PATH`"查找。

### 4.1 三种写法同义

平台包写约定（`[platform]`），使用者写覆盖（`[target.<selector>]`、`[host]`，键名相同），构建程序在平台阶段以指令陈述可变部分
（新增指令 `link_driver`、`link_input`、`artifact_name`、`seal`、`run_host`，与已有的 `cxxflag`、`link_flag`、`link_start`、`link_script`、
`runner`、`run_exclusive` 同列）。三者解析为同一结构 `PlatformStatement`，每个字段带来源。

### 4.2 声明与代码的分界

- **只能声明**：`targets`、`toolchain`、`sysroot`、`layers`、`tools`、`runner`、`runners`、`carrier`、`versions`、`hosted`——解析工具链与依赖图
  时需要，那时没有构建程序可运行；`self env`、IDE、报告不经构建即可读取。
- **首选声明**：`build`、`link`。
- **插件代码**（`plugin`）：依使用者的图、特性或机型而变的部分；在 `platform` 阶段运行（与 R9.9 的 `toolchain` 阶段并列），只能陈述
  `build`、`link`、`runner`、`runners` 的指令，其他指令被拒绝并点名；遵守 SPEC-007 的义务；结果按身份缓存。

### 4.3 层的默认提供者与让位

`layers.<layer>` 作为根的依赖边在依赖遍历前加入，来源 `platform`。图中已有包以 `provides` 提供该层时不加入；一个包提供多层只加入一次；
`false` 表示不提供。判断读 `provides`，不比较包名。

### 4.4 工具

依赖规格：为本机构建（等价于带 `tools = [...]` 的构建依赖，同包的工具只构建一次）；`{ program }`：由载荷或 `PATH` 提供，引擎报告所用的
程序（承担 R6.2 的报告义务）。图中已有为本机构建的同名程序时让位。

### 4.5 合并

标志按"平台 → 本机配置 → 项目"拼接，约定在前，使用者只能追加（需要不同约定时换平台）；`link.seal`、`link.inputs` 整体替换；其余按字段替换。

### 4.6 封装

`link.seal` 的步骤在链接器写出的临时产物上依次执行（`{}` 为它）；全部成功后原子发布（SPEC-011），任一步失败则不发布、保留上一次发布的
产物，错误给出该步 argv 与输出；步骤属于链接边的命令行。

### 4.7 载体（M3）

`mcpp pack --mode self-contained` 为本机构建 `carrier.package`，以 `.incbin` 把产物放入其 `section` 节（ELF、Mach-O、COFF 三种汇编由引擎生成），
并给出符号 `mcpp_carried_begin`、`mcpp_carried_end`、`mcpp_carried_name`；程序入口是平台包的 `carrier.main`（引擎不含任何加载器的调用）。
取代 `pack.cppm` 中按包名找 `openkal-tools` 的代码。载体按宿主约定构建（自举边界）：Linux 上为 `<arch>-linux-musl` 行并显式写出该行的工具链
（载体的图提供 kernel-abi，否则会取消行的约定，L10）。

## 5. 目标图：选择与覆盖

### 5.1 选择

第一个给出答案的胜出：① `--platform <规格>` / `MCPP_TARGET_<T>_PLATFORM`（`none` 不用平台）；② `[target.<selector>] platform`（工作空间根被成员继承）；
③ `config.toml` 同键；④ 目标表的 `platform` 列；⑤ 宿主目标的内置约定。`platform` 不能写在 `cfg(platform = …)` 选择器下。

### 5.2 覆盖

```toml
[target.'cfg(platform = "openkal")']          # 两个架构共用一份
layers.c-abi = { path = "../openkal-musl" }
tools.okld   = { path = "../openkal-image/tools" }
runner       = ["my-okrun", "{}"]
```

四层优先级与 `sysroot` 相同：`MCPP_TARGET_<T>_<KEY>` > 项目 > 本机 > 平台包。

### 5.3 平台作为依赖

被选中的平台包是该配置下根的依赖：其库目标可被 `import`（板级支持包选为平台即可用其模块）。旧写法（板级支持包作为普通依赖、
由构建程序发出 `runner`、`link_script`）继续有效，按 §4.1 合并。

### 5.4 宿主三元组选用平台

宿主三元组（`x86_64-linux`、`x86_64-windows-gnu`）只适用平台 `hosted` 子表中的 `toolchain` 与 `layers`；无 `hosted` 的平台被拒绝并说明它只服务其
`targets`。docs/24 的 "openkal 交叉" 因此写作 `[target.'cfg(any(linux, macos, windows))'] platform = "openkal-platform@0.17.1"`；手写
`openkal-llvm-runtime` 的旧写法由让位规则保持有效。

## 6. 宿主图：选择平台，构建程序作为 ok 映像

### 6.1 模型

一次构建有两张图：目标图为 `--target` 构建；宿主图为构建机器构建构建程序（`build.mcpp`、平台插件）与本机工具。两张图各自有一个平台，
机制相同（选择、合并、注入、报告）：

```toml
[host]                                   # mcpp.toml 或 config.toml
platform = "openkal-platform@0.17.1"     # 宿主图的程序编译为 <本机架构>-openkal，由 okrun 运行
```

选择顺序：`--host-platform` / `MCPP_HOST_PLATFORM` > 项目 `[host]` > 本机 `config.toml [host]` > 默认（宿主约定，即今天的行为）。

### 6.2 自举边界

宿主图选用平台 P 时，P 自己的工具与运行器（`okld`、`okrun`）总是按宿主约定原生构建：运行器不能由它自己运行。规则：**一个平台的
`tools` 与 `runner` 所需程序，在任何图中都按宿主约定构建**。没有循环：P 的工具属于原生宿主图，构建程序属于 P 的宿主图。

### 6.3 意义（为何做）

- **一致**：构建程序在 Linux、macOS、Windows 上由同一工具链（llvm）以同一约定编译，读同一个 openkal，行为、诊断与产物字节一致（P9）。
  Windows 上编译构建程序不再需要 MSVC。
- **缓存跨系统**：同一架构的构建程序映像与系统无关，身份不含宿主系统；全局缓存与 CI 缓存可在三个系统间共享。
- **对称**：宿主图与目标图是同一机制的两个实例，没有为构建程序另设的通道。

### 6.4 约束与默认值

- 构建程序的协议（标准输出上的指令、`MCPP_*` 环境、文件读写、查询用的进程启动）全部经 openkal 提供，映像内可用；依赖宿主原生库的
  构建程序不能编译为映像，此时报错点名该依赖与 `[host] platform` 键。
- 本批交付机制、验收与测量：e2e 覆盖构建程序、`build.mcpp` 的库、平台插件三类宿主程序作为映像运行；mcpp-index 的全量构建以
  `MCPP_HOST_PLATFORM=openkal-platform@0.17.1` 运行一次并报告结果（§13 mcpp-index）。默认值保持宿主约定；当测量显示索引中的构建程序全部
  通过时，默认值改为 openkal 平台，那是一行数据（目标表的宿主平台列），不需要新的设计。

## 7. 内置平台

- **宿主目标**（Linux、macOS、Windows、Android、iOS、Emscripten）：约定留在引擎中（链接 ELF、Mach-O、PE 所必需），以平台的形状报告
  （`Platform host conventions (built-in)`），其 `pin`、`sysroot` 列即内置平台的 `toolchain`、`sysroot`，使用者以同名键覆盖。
- **钉住的平台包**：目标表增加 `platform` 列，`<arch>-openkal` 写 `"mcpplibs.openkal-platform@0.17.1"`；增加宿主平台列，默认空（宿主约定）。
- **获取**：平台包按普通包解析、下载、缓存、锁定，在工具链解析前读取清单；无 `plugin` 的平台包不编译。`mcpp toolchain install --target <t>`
  （及 `--host`）预取一个配置的全部内容，之后可 `--offline` 构建。

## 8. 一致性与稳定性

### 8.1 版本组

图中属于平台 `versions` 的包被解析为不同版本时拒绝，消息列出双方请求链与两条出路：换平台版本，或固定造成冲突的包。

### 8.2 多版本改名的边界（一般规则）

改名只改 C++ 模块名，只对接口全由具名模块构成的包成立。导出头文件目录的包、提供目标侧层的包及其依赖不改名，两版本并存时拒绝并列出请求者。

### 8.3 稳定性的其他保证

- 平台包的身份（名字、版本、来源）进入构建配置的指纹：换平台即换构建目录，不复用不相容的产物（与 2026.10.26.2 的暂存修正一起，
  杜绝升级后残留旧产物）。
- 平台的约定只能追加，不能被使用者删除；需要不同约定时换平台，变化因此总是显式的、可在报告中看到的。
- 锁记录平台与其注入的包（`origin = "platform"`）；引擎升级不悄然替换已锁定的版本，`mcpp update` 才前移。
- 平台包获取、工具构建、封装、平台插件的失败都在发生处报告，带键与来源；网络问题遵循 #847 的刷新策略。

## 9. 命令、输出与诊断（可观察性）

- 报告（所有目标同构）：

```
      Target x86_64-openkal → x86_64-unknown-none-elf
    Platform openkal-platform 0.17.1 (built-in)
             kernel-abi        openkal        (the loader, okrun)
             c-abi             musl           (openkal-musl@0.22.3, platform)
             c++-abi           libc++         (openkal-llvm-runtime@0.16.3, platform)
             tools             okld, okrun    (openkal-tools@0.1.1, platform)
             runner            okrun          (platform)
```

  宿主图选用了平台时多一行 `Host platform openkal-platform 0.17.1 (config.toml)`。来源取值：`built-in`、`platform`、`config.toml`、`mcpp.toml`、
  `MCPP_*`、`--platform`、`graph`。
- `--message-format json`：`plan` 事件增加 `platform` 与 `hostPlatform` 对象（名字、版本、来源、`fields` 中每个字段的值与来源）。
- `mcpp self env --target <t> [--host] [--format json]`；`mcpp toolchain list` 增加平台列；`mcpp toolchain install --target <t>`；`mcpp self doctor`
  检查平台就绪与版本组；`-v` 下每个字段的决定以 `decision` 记录（R6.5 的同一机制）。
- 诊断：每条拒绝给出键、来源与出路，不写死版本号；新增错误码进入 `mcpp self explain`；格式遵守 SPEC-012。首次使用平台时的获取在
  状态行中可见（`Fetching platform openkal-platform 0.17.1`）。

## 10. openkal-platform

### 10.1 它做什么

一个**只含数据、不编译**的包，内容是 openkal 生态对 `<arch>-openkal` 的全部约定（映像的编译与链接标志、入口、注释节、`okld note/check`、
`.oke`/`.okso`、`okrun`、M3 载体）与默认版本（运行时、工具、实现）。今天前者写在引擎里，后者要求每个项目手写。它跟随 openkal 轮次发布
（0.17.1），只改约定时发 `0.17.1.1`；带验收测试（两个目标各构建一个最小程序，在本机架构运行）。mcpp 只钉住它的版本；项目也可以不等
mcpp，自己写 `platform = "openkal-platform@0.18.0"`。

### 10.2 内容

```toml
[package]
namespace = "mcpplibs"
name      = "openkal-platform"
version   = "0.17.1"
mcpp      = ">=<本批 mcpp 版本>"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch" }
carrier   = { package = "openkal-run@0.1.1", section = ".oke", main = "carrier/main.cpp" }

[platform.layers]
compiler-runtime = "openkal-llvm-runtime@0.16.3"
c++-abi          = "openkal-llvm-runtime@0.16.3"
c-abi            = { name = "openkal-musl", version = "0.22.3", default-features = false, features = ["posix"] }

[platform.tools]
okld  = "openkal-tools@0.1.1"
okrun = "openkal-tools@0.1.1"

[platform.build]
cflags   = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables", "-Xclang", "-fstack-clash-protection"]
cxxflags = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables", "-Xclang", "-fstack-clash-protection"]
[platform.build.'cfg(arch = "aarch64")']
cflags   = ["-ffixed-x18", "-mno-outline-atomics"]
cxxflags = ["-ffixed-x18", "-mno-outline-atomics"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared", "-z", "max-page-size=65536", "-z", "now", "-z", "noexecstack", "--hash-style=both", "--gc-sections", "--eh-frame-hdr"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]

[platform.hosted]
targets   = ["x86_64-linux-gnu", "aarch64-linux-gnu", "x86_64-linux-musl", "aarch64-linux-musl",
             "x86_64-macos", "aarch64-macos", "x86_64-windows-gnu", "x86_64-windows-musl"]
toolchain = "llvm@23.1.3"
[platform.hosted.layers]   # 与 [platform.layers] 相同

[platform.versions]
openkal = "0.17.1"
openkal-linux = "0.18.2"
openkal-macos = "0.15.1"
openkal-windows = "0.13.3"
openkal-musl = "0.22.3"
openkal-llvm-runtime = "0.16.3"
openkal-ld = "0.1.1"
openkal-run = "0.1.1"
openkal-tools = "0.1.1"
```

`link.flags` 是 `build.ldflags` 之外、只给 `driver = "lld"` 的链接器参数（`ld.lld` 直接调用时不经编译器驱动的 `-Wl,` 转写）。每个值取自今天的引擎
或已发布版本，无新约定。`-ffreestanding`（图中无 C 库）、`__OPENKAL__`（由 `kernel-abi` 取值定义）、无 C++ 运行时时拒绝 `import std`，本来就是按层
判断的通用规则，留在引擎。

### 10.3 仓库与发布顺序

新仓库 `mcpplibs/openkal-platform`（GitCode 镜像 `mcpp-res/openkal-platform`）。它引用 openkal-llvm-runtime，后者经 openkal-musl 依赖 openkal-image
的 openkal-ld，同仓会形成发布环，因此独立，是一轮中最后发布的包：实现 → openkal-image → openkal-musl → openkal-llvm-runtime → openkal-platform → 索引 →
mcpp 前移钉住。

## 11. 生态（同批，每仓库一个 PR）

### 11.1 mcppls：语义高亮的时延（P10）、平台迁移与 0.0.18 内容

**分析（代码事实）。**

| 环节 | 补全（快） | 语义高亮（慢） |
|---|---|---|
| 是否等待打字停顿 | 否 | 是：`textDocument/semanticTokens/*` 在 `builds_main_file_` 列表中（`src/engine/clangd.cpp:2261`），打字期间等待停顿 `typing_quiet_`（上次构建时长 ×1.2，夹在 300–1000 ms） |
| clangd 所需 | 前导（preamble）加当前文本，不建主文件 AST | 主文件的完整 AST；大文件的 AST 构建是秒级 |
| mcppls 的快速路径 | 有：`answered_from_word`（同一词内复用上一答案并重定位）、`answered_late`（迟到答案在同词内立即给出，C-2） | 无：没有上一结果的缓存，打开文件或 AST 重建期间什么也不返回 |
| 增量 | 不适用 | 无：声明 `full: true`，不声明 `delta`；客户端发来的 `full/delta` 被改写成 `full`（`workspace.cpp:1102`），每次传输整份 token |
| mcppls 自身的处理 | 轻 | `remap_core_tokens` 与 `merge` 各做一次 decode → vector → nlohmann JSON 数组的 encode；`merge` 对每个原生 token 扫描全部 core token（O(N×M)） |

结论：高亮慢于补全的主因是**高亮必须等主文件 AST**，而补全不需要；mcppls 又没有为高亮做补全那样的上一结果复用，也没有增量。约束：不动 clangd。

**方案（mcppls 内）。**

- **H1 上一结果即时返回。** 每个打开文档保存最近一次 clangd 的 token（已映射到本服务器的图例）与它所对应的文本版本。请求到达而 AST 未就绪时，
  以上一结果立即答复，按两版本之间的编辑重定位（行列平移；被编辑的行内 token 丢弃），然后在 AST 就绪后发 `workspace/semanticTokens/refresh`
  （引擎已有合并后的 refresh 机制，`engine.cppm:145`）。客户端不支持 refresh 时退回等待。
- **H2 持久缓存。** 以（文档内容摘要、编译命令摘要、mcppls 与 clangd 的版本、模块 BMI 的身份）为键，把 token 写入工作区缓存目录（遵守 C-8
  的预算）；打开文件时键命中即立即答复，再走 H1 的刷新。键中任何一项不同即视为未命中，保证不显示错误高亮。
- **H3 增量。** 声明 `full: { delta: true }`，为每次答复分配 `resultId`，对 `full/delta` 以上一结果计算编辑（LSP 的 SemanticTokensEdit），大文件
  只传变化部分。
- **H4 处理成本。** `merge` 改为两路有序合并 O(N+M)；remap、merge、encode 在一次遍历中完成，直接写出整数数组，不经中间 JSON。
- **H5 首屏。** 客户端发 `range` 请求时（VS Code 对可见区域先发），H1/H2 的结果按范围裁剪即时答复；mcppls 自身的模块语法 token（无需 AST）
  总是立即可用。
- **测量与判据。** devtools bench 增加任务：一个 1 万行、导入多个模块的文件，记录 didOpen 到首个补全、首个高亮（H1/H2 前后）、编辑后高亮更新
  的时延，以及每次答复的字节数；CI 的 bench-validate 校验基线与参考对。判据：打开已缓存文件时首个高亮不晚于首个补全；编辑后高亮答复的
  字节数与编辑规模成比例（H3）。

**操作系统层只来自 mcpplibs.os。** 与 mcpp 2026.10.26.2 删除 `modules/platform` 相同，mcppls 删除自己的 `modules/os` 与 `modules/platform`，直接使用
mcpplibs.os；只在确属 mcppls 自身需要处保留基于 mcpplibs.os 的最小封装。通用且适合进入 mcpplibs.os 的接口提交到 os 本批的 PR（mcpplibs/os#7，0.2.1），
带测试，供其他程序复用。

**同一 PR 的其他内容。** #72（0.0.18：服务器建于 mcpplibs.os、每架构一个 ok 映像、读取 mcpp 的结果记录）的内容；删除只为目标服务的 openkal
声明（运行时、工具、ld），映像以零声明的 `--target <arch>-openkal` 构建；CI 改用新 mcpp、`mcpp toolchain install --target`；CLion 插件 e2e 断言
mcpp 工程不被 Compilation Database 接管（issue #71 的第三项）。版本 0.0.18，发布于 mcpp 之后。

### 11.2 mcpp-ide（新仓库 `mcpp-community/mcpp-ide`）

按 part 1 §14 的结构与边界交付全部内容，并加入平台：

| 部分 | 内容 |
|---|---|
| 后端（已有，补全） | 会话、监听（结果记录、manifest）、编排（同工程串行、取消、新取代旧）、视图模型、设置校验与迁移、自检、i18n、能力协商；新增：目标与平台视图（来自 `mcpp toolchain list --format json` 与 `plan` 事件的 `platform`），`self env --target` 定位运行器 |
| `protocol/` | JSON Schema 与取自本批 mcpp 真实输出的 golden fixture（含 `platform` 对象） |
| `spec/` | 命令表、设置、菜单、i18n；由 `build.mcpp` 生成 VS Code `package.json` 声明部分与 `plugin.xml`（不提交生成物） |
| `webui/` | 依赖图、库、缓存、设置、打包、目标与平台面板；VS Code webview 与 JetBrains JCEF 加载同一份 |
| `vscode/` | 由 mcpp-vscode 0.8.0 迁移：Task（PTY）、Testing API、调试（cppdbg / CodeLLDB / lldb-dap；ok 映像经 `okrun` 的 GDB JIT 接口调试）、诊断来自事件流、manifest 诊断来自 mcpp；以扩展 ID `mcpp-community.mcpp-vscode` 发布 1.0.0 |
| `jetbrains/` | Kotlin，CLion 2025.2+（`sinceBuild = 252`，与 mcppls 的 CLion 插件相同）：mcpp 运行配置类型（PTY 控制台、`main()` 旁 ▶）、测试树、调试、console filter、面板；以 `mcpp-clion` 发布。**语言功能只由 mcppls 提供**：`plugin.xml` 以 `<depends>io.github.sunrisepeak.mcppls</depends>` 依赖 mcppls 的 CLion 插件（Marketplace 安装 mcpp-clion 时一并安装它），mcpp-clion 不注册任何语言服务、不启用 CLion 的 Compilation Database 接管 mcpp 工程；两个插件的分工：mcppls = 编辑（补全、高亮、诊断、导航），mcpp-clion = 工程（构建、运行、测试、调试、面板） |
| `agents/` | 面向 agent 的说明与 skill |
| 分发 | 后端以 `--target <arch>-openkal` 构建为 ok 映像，经 `mcpp pack --mode self-contained` 成为各系统程序（与 mcppls 0.0.18 同法），随 vsix 与 CLion 插件分发；同时以 xlings 包 `mcpp-ide` 发布。GitHub Release 附 `mcpp-vscode-<v>.vsix` 与 `mcpp-clion-<v>.zip`；两个 Marketplace 由维护者上传（§13.3） |
| CI | 后端三系统测试（先 `mcpp build` 再 `mcpp test`，或使用 L5 的 `MCPP_BIN_<TARGET>`）；VS Code 扩展测试；JetBrains plugin verifier；README 与 README.zh-CN 按 mcppls 的结构 |

VS Code 一端分工相同，依赖的强度不同：`vscode/` 的 `package.json` 以 `extensionPack: ["sunrisepeak.mcpp-language-server"]` 引入 mcppls 的 VS Code
扩展（`engines.vscode` 与之相同，`^1.91.0`）——一并安装，但用户可以卸载而保留 mcpp-vscode。理由：VS Code 用户可能已有自己的 C++ 语言服务，mcpp-vscode
0.7.0 起就提供"一键关闭其他 C++ 语言服务"而不是强制替换；CLion 的原生引擎不理解 mcpp 工程，那里没有可以保留的替代，因此用强依赖。两端都只经 mcppls
提供 mcpp 工程的语言功能，mcpp-ide 本身不注册语言服务。

### 11.3 mcpp-vscode

mcpp-ide 首个发布之后，一个 PR：README 指向 mcpp-ide，说明扩展 ID 不变、用户无感升级；仓库随后归档（part 1 的 O9）。

### 11.4 mcpp-plugins

#51（0.20.0：SYCL 一个 C++ 运行时）为本批的唯一 PR，追加：L2 通用库 `mcpp.plugins.platform`（平台插件构造平台陈述的函数，同 `mcpp.plugins.toolchain`），
docs/rules.md 增加平台插件一节；CI 使用本批 mcpp。

### 11.5 其他仓库

| 仓库 | PR 内容 |
|---|---|
| mcpplibs/openkal-platform（新） | §10；验收测试；CI 以 mcpp 的 release 分支构建（发布前）/ 发布版（发布后）；发布 0.17.1 与 GitCode 镜像 |
| mcpplibs/mcpp-index | 收录 openkal-platform；测试成员 `openkal-platform`（零声明的 `--target x86_64-openkal` 构建与运行）；全量构建以宿主平台 openkal 运行一次的测量（§6.4，报告进 PR） |
| mcpplibs/os | 0.2.1：L9；删除 `[target.'cfg(os = "openkal")']` 下的运行时与工具声明；CI 的映像任务改用 `mcpp toolchain install --target` 与 `self env --target` |
| mcpplibs/openkal-image | README：工具由平台提供；`[target.'cfg(platform = "openkal")'] tools.okld = { path }` 用于开发工具本身 |
| openxlings/xim-pkgindex | `mcpp-ide` 包（后端，预编译）；mcpp 的版本前移由机器人 PR 完成 |

## 12. mcpp 侧：迁移、遗留与代码组织

### 12.1 迁移

| 对象 | 变化 |
|---|---|
| 引擎的 openkal 代码 | 删除 `src/okimage/`；六处分支改为平台机制的通用调用 |
| 目标表 | 增加目标平台列与宿主平台列；`toolchain list` 显示 |
| 板级支持包 | docs/34 改写为"编写平台包"（板级支持包即平台包，指令写法同义保留）；e2e 952 以一个与 openkal 无关的本地平台包覆盖标志、链接脚本、命名、封装与运行器；332、336、338（目标行的启动）与 333（指令写法）不变，作兼容判据 |
| 清单 | `[target.<selector>]` 增加 `platform`、`layers.*`、`tools.*`、`link.*`、`carrier`；`[host]`；`cfg(platform = …)`；SPEC-004 已知键表同步 |
| 引擎接口 | `mcpp.core` 协议 17：`platform` 阶段与 §4.1 的新指令 |
| 文档 | 新增 docs/35「平台」与 SPEC-013；改写 docs/24、34；增补 docs/04、22、30、41、50；中英文同步 |

### 12.2 遗留问题

| 编号 | 问题 | 处理 |
|---|---|---|
| L1 | 精确版本缺失时安装失败不刷新（P4） | 已在 #847：`E_NOT_FOUND` 交给刷新策略后再请求一次；e2e 949 |
| L2 | 已读描述符的包仍尝试 `compat:` 拼写 | 已在 #847 |
| L3 | 多版本改名对带 C 接口的包静默生效（P3） | §8.2 |
| L4 | 安装失败的诊断转述 xlings 的 "the index is current"，而 mcpplibs 索引是陈旧工件 | mcpp 自己陈述：哪个索引、哪个修订、多久前同步、刷新是否进行及原因；xlings 原文降为 `-v` |
| L5 | `mcpp test` 不构建包的可执行目标 | 先构建，测试环境给出 `MCPP_BIN_<TARGET>`（对标 Cargo `CARGO_BIN_EXE_<name>`） |
| L6 | 工作空间中同一依赖的缓存行打印两次 | 每包每次构建报告一次 |
| L7 | os、mcppls 的 CI 自建 `okrun` | §11 |
| L8 | kernel-abi 包在宿主目标上不进入共享映像（#846） | 已并入 #847（a336ef01，e2e 950） |
| L9 | os 0.2.0 在宿主三元组上以 openkal-musl 与 openkal-llvm-runtime 为运行时（mcppls 的形态，即 §5.4 的 `hosted`）时不能编译：macOS 源文件包含 SDK 头（`sys/sysctl.h`、`mach/mach.h`），Windows 源文件把 `path::c_str()`（此时为 `char`）传给宽字符接口 | os 0.2.1：按 Darwin ABI 自行声明所用的 libSystem 接口，Windows 显式构造宽字符串；CI 增加三系统的该形态测试 |
| L10 | `mcpp pack --mode self-contained` 为 M3 构建载体时，静态链接取到宿主 GCC 的 `crtbeginT.o`，被封闭性检查拒绝 | `:carrier` 以平台工具链为本机构建载体，C 运行时对象来自工具链载荷；e2e 覆盖 |

### 12.3 代码组织

`src/platform/`，模块 `mcpp.platform`：`:model`（`PlatformStatement`，纯数据）、`:select`（目标图与宿主图的选择）、`:merge`、`:inject`（层、工具、
载体、让位）、`:link`（driver、inputs、命名、封装）、`:carrier`、`:versions`（版本组与改名边界）、`:report`。清单解析在 `modules/manifest`；新指令在
`hostprogram.cppm` 与 `build_program`。`:model`、`:select`、`:merge` 为纯函数，以表格测试锁定。

## 13. 交付：每仓库一个 PR，commit 作检查点

### 13.1 顺序

```
openkal-platform(新) ─┐
                      ├─> mcpp-index ─> mcpp #847 CI 绿 ─> 合入/发布/索引前移 ─┬─> os、openkal-image、mcppls #72、mcpp-plugins #51
                      │                                                     └─> mcpp-ide 首发 ─> xim-pkgindex(mcpp-ide) ─> mcpp-vscode
```

- openkal-platform 先于 mcpp-index：索引收录需要它的发布。
- mcpp-index 的成员需要本批 mcpp（零声明构建）。解决：该成员的 `[package] mcpp` 写本批版本；索引 CI 增加与 os 相同的 `MCPP_SOURCE_REF`
  能力（成员的下限高于 CI 所用 mcpp 时，以该 ref 构建 mcpp 运行它；无 ref 时报告为跳过并点名下限）；收录条目在 mcpp 的最终 CI 之前合入，使 mcpp 的 e2e 读到索引中的平台包。
- mcpp 的最终 commit 把目标表的钉住从开发期的 `{ git, tag }` 换成索引版本（与 os 0.2.0 的切换相同）。
- 等待 CI 期间并行开发 mcppls、mcpp-ide、mcpp-plugins（不等 mcpp 发布即可开发，以 release 分支构建验证）。

### 13.2 各仓库的检查点

**mcpp-community/mcpp — #847（标题改为本批发布，版本号在发布当日确定）**

1. L1/L2（已有）；L8 #846（已有）
2. SPEC-013、docs/35 初稿
3. 清单：`[platform]`、`[target]` 新键、`[host]`、`cfg(platform)`；单元测试
4. `src/platform` 的 `:model :select :merge`；表格测试
5. `:inject`、锁的 `origin = "platform"`
6. `:link`（driver、inputs、命名、封装）接入 flags / ninja / plan；`:carrier` 从 pack 移出
7. 运行器表形式与 `host`；execute
8. `:report`、JSON、`self env --target/--host`、`toolchain install --target`、`toolchain list` 平台列、doctor、explain 码
9. 删除 `src/okimage`；目标表平台列（开发期钉 git tag）
10. 宿主图平台与自举边界；构建程序作为映像的 e2e
11. `:versions` 与改名边界
12. L4、L5、L6
13. 板级支持包迁移（examples/13、docs/34、e2e 332/336/338）
14. 新 e2e；942、947 改为零声明
15. 文档（04、22、24、30、34、35、41、50，中英）、CHANGELOG、specs README
16. 钉住切换为索引版本

**mcpplibs/openkal-platform（新）**：初始 commit（README、LICENSE）→ PR：平台清单 → 验收测试 → CI → 发布 0.17.1、GitCode 镜像。

**mcpplibs/mcpp-index**：收录描述符 → 测试成员 → 宿主平台测量（报告）→ CHANGELOG。

**Sunrisepeak/mcpp-language-server — #72**：bench 任务（先测基线）→ H4 → H1 → H2 → H3 → H5 → bench 判据 → 平台迁移 → CI 与 CLion 断言 → CHANGELOG 与版本。

**mcpp-community/mcpp-ide（新）**：初始 commit（现有后端）→ PR：protocol fixture 更新 → 后端补全与平台视图 → spec 生成器 → webui → vscode 适配层（迁移）→
jetbrains 适配层 → agents → 分发与 CI → README 中英 → 发布 0.1.0。

**mcpp-community/mcpp-plugins — #51**：`mcpp.plugins.platform` → 文档 → CI 前移到本批 mcpp。

**mcpplibs/os、openkal-image、openxlings/xim-pkgindex、mcpp-vscode**：各一个 PR，内容见 §11。

### 13.3 维护者的上传

mcpp-ide 发布时，交付本机验证后的产物目录（含两份文件与 `SHA256SUMS`）：

- `mcpp-vscode-1.0.0.vsix`：上传到 VS Code Marketplace（publisher `mcpp-community`，扩展 ID `mcpp-community.mcpp-vscode`），可同时上传 Open VSX；
- `mcpp-clion-0.1.0.zip`：上传到 JetBrains Marketplace（插件 ID `io.github.mcpp-community.mcpp-clion`，依赖 `io.github.sunrisepeak.mcppls`）。

两份文件在交付前通过：vsix 的扩展测试与 `vsce ls` 清单检查；zip 的 plugin verifier（最低与当前 CLion）。mcpp-vscode 的归档在两份上传完成之后。

## 14. 测试

单元：`test_platform_merge`（四层、拼接、整体替换、继承、`cfg(platform)` 下写 `platform` 被拒）；`test_platform_select`（目标图五级、宿主图四级）；
`test_platform_inject`（让位、多层一次、`false`、工具让位、自举边界）；`test_platform_manifest`（每个键与错误位置、`hosted` 的键限制）；
`test_platform_versions`；`test_mangle_boundary`；锁的 `origin`。

e2e：零声明 openkal 的 build / run / test / pack（含 aarch64 构建）；942、947 零声明后照常通过；覆盖（`layers.c-abi = false`、`tools.okld = { path }`、
`config.toml`、`MCPP_TARGET_*_PLATFORM`、`--platform`）；让位（声明 openkal-musl 时来源为 `graph`）；版本组冲突；一个与 openkal 无关的本地平台包
（标志、封装、命名、运行器）；板级支持包两种写法；宿主三元组选用平台与被拒；宿主图平台（构建程序、库、平台插件三类作为映像运行；自举边界下
`okrun` 原生构建）；输出与 JSON；`toolchain install --target` 后 `--offline`；L4、L5、L6；950。

mcppls：bench 判据（§11.1）；高亮缓存命中、未命中与键变化的单元测试；delta 的往返测试；VS Code e2e 断言首个高亮时延。
mcpp-ide：后端单元测试与 golden fixture；两端适配层的冒烟测试。

## 15. 多视角自审

1. **兼容性。** 旧写法全部保留：手写 `openkal-tools` / `openkal-llvm-runtime`（让位）、板级支持包的指令写法（同义合并）、`[target]` 的已有键、旧锁（无
   `origin` 视为项目声明）；宿主图默认不变。破坏性只有一处：`src/okimage` 的提示文字与 `.oke` 生成由平台提供，行为不变（942、947 判据）。mcppls 的
   高亮缓存键包含版本与编译命令，升级后不显示陈旧结果。mcpp-vscode 的用户经同一扩展 ID 无感升级。
2. **跨平台。** 平台机制与三系统无关；载体汇编覆盖 ELF、Mach-O、COFF；`runner.host = "same-arch"` 处理跨架构不可运行并建议 `run_under`；宿主图作为
   映像使三系统的构建程序一致（§6.3）。所有新 e2e 在三系统 CI 运行，需要 llvm 的沿用 `requires: llvm`。
3. **用户开发体验（应用开发者）。** 已知目标零配置；覆盖只在一处、键与平台相同；报告与 JSON 给出每个值的来源；首次获取可见；离线可预取。
4. **mcpp 使用体验（平台作者、插件作者、IDE）。** 一个包、一段 `[platform]`、一份规范；本地 `path` 开发；与规则插件共用 `mcpp.core`；`mcpp test` 在平台包中
   对每个目标构建并在可运行的目标上运行；IDE 通过 `toolchain list` 与 `plan` 事件取得目标与平台，不解析清单。
5. **简洁优雅。** 键从约 27 个精简为七组（§4）；不引入新模板语法；宿主图复用目标图的全部机制；openkal 在引擎中归零。风险：`[target.<selector>]` 的键增多——
   新键全部是平台字段的同名覆盖，docs/35 以一张表列出。
6. **稳定性。** 约定只能追加；平台身份进入指纹；版本组与改名边界在解析时拒绝而非在链接或运行时失败；锁防止静默前移；自举边界杜绝循环；宿主图
   默认不变，映像化以测量为前提（§6.4）。mcppls 高亮缓存的每一项失配都视为未命中。
7. **可观察性。** 报告行、JSON、`self env`、`doctor`、`-v` 的 `decision` 记录、`explain` 码；mcppls 的 bench 给出时延与字节数。
8. **架构。** 引擎 = 机制 + 规范 + 宿主平台的官方约定；体系知识在平台包；IDE 逻辑在 mcpp-ide，mcpp 不含 IDE 概念；mcppls 不动 clangd。宿主图与目标图对称，
   为日后默认以映像运行构建程序留出的只是一行数据。
9. **规模与风险。** 本批改动面大（mcpp、两个新仓库、六个生态仓库）。缓解：纯函数先以表格测试锁定；openkal 的现有 e2e 是行为不变的判据；与 openkal 无关的
   平台 e2e 证明机制通用；每个仓库内以 commit 作检查点，CI 在每个检查点可运行；并行开发由 §13.1 的依赖图约束。
10. **对外动作。** 新建公开仓库（openkal-platform、mcpp-ide 与其 GitCode 镜像）已确认；两个 Marketplace 由维护者上传，交付物为路径与校验和（§13.3）；
    mcpp-vscode 在上传完成后归档。
11. **IDE 的分工（rev 4）。** 语言功能只有一个来源：mcppls。mcpp-clion 以强依赖（`<depends>`）、VS Code 适配层以扩展包（`extensionPack`，可卸载）引入它，不注册语言服务，不让 CLion 的
    Compilation Database 接管 mcpp 工程；工程功能只有一个来源：mcpp-ide 后端，经 mcpp 的控制契约。两端的能力表相同（part 1 §14.3）。风险：mcppls 的 CLion
    插件未装或版本过旧——依赖声明使 Marketplace 一并安装；后端握手时读 mcppls 的版本（`mcppls --version`），低于要求时在面板中给出升级指引，而不是静默降级。
12. **复审 rev 3 的定案。** 宿主图的平台（§6）与自举边界无循环；mcppls 的 H1–H5 只改 mcppls，判据先测基线再改；#72、#51、#847 各为本仓库唯一 PR；
    索引 CI 的 `MCPP_SOURCE_REF` 与 os 的做法相同。未发现需要改动的定案。

## 16. 定案（rev 5）

1. mcpp-clion 以 `<depends>` 强依赖 mcppls 的 CLion 插件；VS Code 适配层以 `extensionPack` 引入 mcppls 的扩展（一并安装、可卸载）；后端握手检查 mcppls 的版本。
2. 两个 Marketplace 由维护者上传；交付物是本机验证后的产物目录；插件 ID `io.github.mcpp-community.mcpp-clion`。
3. 按 §13 实施；L9、L10 同批解决。

## 17. 补充设计：part 2（打包与分发）

第四、五轮讨论把打包与分发、`.okso` 的本机绑定、体积与新增的 openkal 0.17.2 轮定为本批内容，写在
[part 2](2026-10-28-platforms-part2-packaging-and-distribution-design.md)。要点：

- `mcpp pack` 的选择拆为链接（`--linkage`）、闭包（`--mode`）、容器（`--pkg-format`）三个正交维度与构建腿；`--mode static` 是简写；
  `--pkg-format` 取代 pack、run 的 `--format`（旧写法提示）。
- 平台以 `universal = true` 声明其目标默认打通用包：一个包含全部架构，解压即可在任何系统、任何平台架构上运行；`--systems`、`--merge`
  产出免安装的多系统包。
- 单文件是通用容器（原生：启动桩加载荷；openkal：载体从内存加载）；okb 是 openkal 的通用映像包，格式由 openkal-image 定义，由
  openkal-platform 的插件作为包格式提供。
- 新规范 SPEC-014（打包与分发）；体积在构建、剥离、容器、去重、共享五处优化并实测。
- `.okso` 的本机绑定由 openkal 体系的构建插件 `openkal-bind` 完成；openkal 0.17.2 轮（openkal-image 0.1.2、openkal-musl 0.22.4、
  openkal-llvm-runtime 0.16.4、openkal-platform 0.17.2）在本批最后；APE 单文件为后续（mcpp#849）。
- 定案（part 2 §14）：`[platform] universal`；okb 由平台插件作为包格式提供；`tar` 默认 gzip、zstd 显式选择；openkal-platform 0.17.1 先行、
  0.17.2 为最终钉住。
- 交叉验证（part 2 §15）：mcppls 与 mcpp-ide 逐项采用零声明目标、通用包、okb、单文件、xlings 单资产、宿主图平台与体积优化——
  编辑器扩展只带一个 okb 与各系统 × 架构的 okrun，一个 vsix 服务全部平台；判据写入两个 CI。
