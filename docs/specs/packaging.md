# SPEC-014:打包与分发

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-014 |
| 标题 | 打包与分发:链接、闭包、容器三个维度与构建腿,暂存树与包元数据,通用包、单文件、签名、压缩、合并与分发描述 |
| 状态 | 评审中 v1.0 |
| 版本 | 1.0 |
| 最后修改 | 2026-10-28 |
| 对应实现 | mcpp >= 2026.10.27.1;逐条标注 |
| 相关设计文档 | `.agents/docs/2026-10-28-platforms-part2-packaging-and-distribution-design.md`(§2–§7、§14)、`.agents/docs/2026-10-28-platforms-design.md`(§4.7) |
| 相关 issue | mcpp#847、mcpp#849 |
| 相关规范 | [SPEC-007](build-plugins.md)(包格式提供者、artifact action)、[SPEC-011](concurrency.md)(原子发布)、[SPEC-013](platforms.md)(平台、载体) |
| 使用文档 | [10 —— 发布打包](../zh/10-pack-and-release.md) |

用语按 RFC 2119:**必须 / 禁止**为强制,**应当**为建议,**可以**为可选。

## 0. 范围

本规范规定 `mcpp pack` 为一个**程序**目标产出可分发物的模型、管线、暂存树、包元数据、容器与接口,以及
`mcpp run --pkg-format` 与 `mcpp emit xpkg` 读取的部分。库的预编译包(docs/12)不在本规范内。

## 1. 模型:三个维度与构建腿

一次打包的选择由三个正交维度与一组构建腿组成,每个维度回答一个问题:

| 维度 | 选项 | 回答的问题 | 归属 |
|---|---|---|---|
| 链接 `--linkage` | `static`、`shared` | 库在程序里,还是在程序旁边 | 程序属性 |
| 闭包 `--mode` | `system`、`vendored`、`self-contained` | 运行的机器要提供什么 | 打包属性 |
| 容器 `--pkg-format` | `dir`、`tar`、`zip`、`single-file`,以及提供者的格式 | 交付成什么 | 打包属性 |
| 腿 `--target`(可重复) | 三元组 | 一次打包覆盖哪些目标 | 打包属性 |

### 1.1 链接

`--linkage` 对应 `[build] dependency_linkage`,打包按它构建。原生 Linux 目标上 `static` 选择 `<arch>-linux-musl`
行,产物为全静态程序;openkal 目标上 `static` 为一个 `.oke`、无 `.okso`,`shared` 为 `.oke` 加其所需的 `.okso`。
未给出时取 `[pack] linkage`,再取构建的默认值。**未实现**

### 1.2 闭包

| 模式 | 原生目标:包携带 | openkal 目标:包携带 | 运行的机器提供 |
|---|---|---|---|
| `system` | 程序 | 映像 | 全部共享库;openkal 上为加载器与映像所需的 `.okso` |
| `vendored`(缺省) | 程序与非系统共享库 | 映像与其 `.okso` | C 库、C++ 运行时与加载器;openkal 上为加载器 |
| `self-contained` | 程序、全部共享库、C 与 C++ 运行时、加载器 | 映像、`.okso` 与本机程序形式的加载器(载体,SPEC-013 §11) | 无 |

`bundle-project` 与 `bundle-all` 是 `vendored` 与 `self-contained` 的永久别名。**已实现**

`--mode static` 是 `--linkage static --mode self-contained` 的简写:原生目标上为全静态程序;openkal 目标上为一个静态
`.oke` 加载体。**部分实现**:原生目标已实现;openkal 目标上 `--mode static` 当前被拒绝。

### 1.3 容器

| 值 | 产物 |
|---|---|
| `dir` | 暂存树本身 |
| `tar` | 一个归档,单一顶层目录与归档同名;缺省压缩为 gzip(§5.1) |
| `zip` | 一个 ZIP 归档,条目以 stored 存放;PE 目标的 `tar` 产出 `zip` |
| `single-file` | 一个可执行文件(§5.2) |
| 其他名字 | 依赖图中的提供者以 `pack-format` 声明的格式(§6) |

`dir`、`tar` 与提供者格式 **已实现**;`zip` 作为显式取值与 `single-file` **未实现**。

### 1.4 构建腿

`--target` 可以重复。每条腿是一次构建;各腿暂存进同一棵树(§4)。平台以 `[platform] universal = true` 声明其目标
默认打**通用包**:对该平台任一目标的打包缺省覆盖平台的全部 `targets`;`--no-universal` 只打所选目标。一个产物为
可执行文件的目标,只有在通用包中或在产物为共享对象的行(Android 的 `kind = "app"`)上接受多条腿。
Android 的多腿 **已实现**;`universal` 与 `--no-universal` **未实现**。

### 1.5 组合的合法性

组合在任何构建开始之前检查;不可能的组合**必须**被拒绝,消息写出冲突的两个选择与出路。

| 组合 | 结果 |
|---|---|
| `--pkg-format single-file` 与 `--mode system` | 拒绝:单文件携带它所运行的东西;出路 `--mode vendored` 或 `self-contained` |
| 需要宿主提供能力的程序(docs/04 §2.11)与 `--mode self-contained` 或 `static` | 拒绝:携带自己 C 库的包不能使用宿主提供的库;出路 `--mode vendored` |
| `cxx_runtime = "toolchain-coupled"` 与 `--mode system` | 拒绝 |
| 产物为可执行文件、非通用包,且多条 `--target` | 拒绝:一个可执行文件属于一个目标 |
| `--systems` 而非 `--mode self-contained` 的通用包 | 拒绝:只有自带加载器的包按系统区分 |
| `--merge` 的输入缺少 `mcpp-package.json`,或名字、版本、平台不同 | 拒绝,写出不一致的字段 |
| `--merge` 的两个输入含同名映像而摘要不同 | 拒绝,写出该映像与两个摘要 |
| 库目标(`lib`、`shared`)与提供者格式 | 拒绝:库包没有单一暂存树 |

前三行与库目标一行 **已实现**;其余 **未实现**。

## 2. 选项与配置

### 2.1 选项的全局用法

| 选项 | 命令 | 含义 |
|---|---|---|
| `--message-format json` | 构建类命令:`build`、`test`、`run`、`pack` | 机器输出 |
| `--format <编码>` | 报告类命令:`self env`、`toolchain list`、`emit build-database` 等 | 输出编码 |
| `--pkg-format <格式>` | `pack`、`run` | 包格式 |

`pack` 与 `run` 继续接受 `--format <格式>`,行为与 `--pkg-format` 相同,并以一条警告写出它是旧写法。**已实现**

### 2.2 `[pack]`

| 键 | 语义 |
|---|---|
| `default_mode` | 未给 `--mode` 时的闭包模式 |
| `linkage` | 未给 `--linkage` 时的链接 |
| `format` | 未给 `--pkg-format` 时的容器 |
| `compress` | 未给 `--compress` 时的压缩(§5.1) |
| `sign` | 签名步骤的 argv,`{}` 为文件(§7) |
| `strip`、`debug_symbols`、`include`、`exclude`、`[pack.bundle-project]` | 既有键 |

`default_mode` 与既有键 **已实现**;`linkage`、`format`、`compress`、`sign` **未实现**。

## 3. 管线

一次打包依次经过:

1. **构建各腿**:按维度与腿构建,`release` 为缺省构建配置。
2. **收集闭包**:按 `--mode` 决定每条腿携带的库;闭包记录在暂存清单(docs/50)中,未解析的名字使 `dir` 与 `tar`
   拒绝。
3. **生成暂存树**:§4 的布局;通用包生成启动器;`self-contained` 加入加载器或载体。
4. **变换**:strip、重定位、平台的封装(SPEC-013 §9.3)、签名(§7)。
5. **写容器**:§5;提供者格式在第二遍准备中读取暂存树(§6)。
6. **报告**:产物、每个产物的校验和、`mcpp.pack` 信封(§9)。

`--merge` 是"读入若干暂存树 → 合并 → 写容器",不经第 1、2 步。第 1、2、3、4(strip、重定位、封装)、5、6 步的
单腿形式 **已实现**;签名、通用包与 `--merge` **未实现**。

## 4. 暂存树与包元数据

### 4.1 单腿布局

单腿包的布局由目标的二进制格式决定(ELF 的 `bin/` 与 `lib/`,PE 的扁平目录,Mach-O 的 `bin/`,Android 的 `lib/`,
WebAssembly 的同名文件族),规定见 docs/10。**已实现**

### 4.2 通用包布局

```
<name>-<version>-<platform>/
  bin/<name>                         POSIX 启动器
  bin/<name>.cmd                     Windows 启动器
  lib/<arch>/<name>.oke, *.okso      每个架构一组映像
  libexec/<loader>/<os>-<arch>/...   仅 --mode self-contained
  share/...                          资源,只放一份
  mcpp-package.json
```

包名带平台名,不带系统与架构。**未实现**

### 4.3 启动器

启动器由引擎生成,内容只来自平台的 `runner` 与腿:

- 架构目录按 `uname -m`(POSIX)或 `%PROCESSOR_ARCHITECTURE%`(Windows)选择;映射为 `x86_64`、`amd64`、`AMD64` →
  `x86_64`,`aarch64`、`arm64`、`ARM64` → `aarch64`;没有对应目录时以非零状态退出,写出本机架构与包含的架构。
- 运行 `runner.cmd`,`{}` 替换为所选架构目录中的入口映像,其余参数原样传递;`self-contained` 时先在
  `libexec/<loader>/<os>-<arch>/` 中找加载器,再找 `PATH`。

**未实现**

### 4.4 `--systems`

`--systems <os>[,<os>...]`(缺省为本机系统)为每个系统 × 平台架构构建加载器程序并放入 `libexec/`。缺少某个系统的
SDK 载荷时拒绝,写出该系统与载荷。**未实现**

### 4.5 `mcpp-package.json`

暂存树根的 `mcpp-package.json` 是包格式提供者、`--merge` 与 `mcpp emit xpkg` 的输入:

```json
{
  "schema": 1,
  "name": "hello",
  "version": "0.1.0",
  "platform": "mcpplibs.openkal-platform@0.17.2",
  "linkage": "shared",
  "mode": "vendored",
  "legs": ["x86_64-openkal", "aarch64-openkal"],
  "systems": [],
  "launch": {"runner": ["okrun", "{}"], "entry": {"x86_64": "lib/x86_64/hello.oke", "aarch64": "lib/aarch64/hello.oke"}},
  "files": [{"path": "lib/x86_64/hello.oke", "role": "image", "leg": "x86_64-openkal", "sha256": "..."}]
}
```

| 字段 | 语义 |
|---|---|
| `schema` | 本 schema 的版本,当前为 1 |
| `name`、`version` | 包的名字与版本 |
| `platform` | 平台包与版本;无平台时为 `null` |
| `linkage`、`mode` | §1 的两个维度 |
| `legs` | 腿的规范三元组 |
| `systems` | `libexec/` 中加载器所属的系统;无加载器时为空 |
| `launch` | 运行方式:`runner` 的 argv 与每个架构的入口;单腿原生包为程序的相对路径 |
| `files` | 每个文件:`path`(相对于树根,`/` 分隔)、`role`(`program`、`image`、`library`、`loader`、`launcher`、`resource`、`metadata`)、`leg`(所属的腿,共用文件为 `null`)、`sha256` |

**未实现**

### 4.6 合并

`mcpp pack --merge <a> <b> ...` 读入若干包(目录或归档),按 `mcpp-package.json` 合并为一棵树:腿与系统取并集,同一路径
的文件摘要**必须**相同,资源只放一份;再按 `--pkg-format` 写容器。**未实现**

## 5. 容器

### 5.1 压缩

`--compress <zstd|gzip|xz|none>[:<level>]` 与 `[pack] compress` 选择 `tar` 的压缩;`tar` 的缺省为 gzip,产物扩展名为
`.tar.gz`、`.tar.zst`、`.tar.xz` 或 `.tar`。`zip` 的条目以 stored 存放。单文件的载荷使用 zstd。okb 中的映像不压缩
(SPEC-013 平台插件的格式规定)。gzip **已实现**;`--compress` 与其余取值 **未实现**。

### 5.2 单文件

| 组合 | 产物 | 运行 |
|---|---|---|
| 原生目标,`vendored` 或 `self-contained` | 引擎的启动桩加 zstd 压缩的暂存树 | 首次运行把载荷解到用户缓存,之后直接执行 |
| 原生 Linux 目标,`--linkage static` | 静态启动桩加静态程序 | 同上,不依赖任何系统库 |
| openkal 目标,`self-contained` | 平台的载体加本架构的映像组 | 载体经加载器库的 bundle 接口从内存加载,不写盘 |

启动桩的规定:

1. 源码随引擎提供,打包时为目标构建;它只做校验、解压与执行。
2. 缓存根:`$XDG_CACHE_HOME`,未设置时 `~/.cache`(Linux);`~/Library/Caches`(macOS);`%LOCALAPPDATA%`
   (Windows);其下按载荷的 sha256 寻址。
3. 解压前**必须**校验载荷摘要;解到同一目录下的临时名,完成后原子改名;**禁止**复用不完整的目录;缓存目录只对
   当前用户可写。
4. 以原有的参数与环境执行暂存树中的程序,退出状态即程序的退出状态。

**未实现**

### 5.3 一个文件在多个系统上运行

一个文件同时在多个系统与架构上运行(APE)不在本规范内(mcpp#849)。

## 6. 包格式提供者

一个包以构建程序的 `mcpp::provides_pack_format("<name>")` 声明一个格式,以 `role = "artifact"` 的 action 生成产物
(SPEC-007)。`--pkg-format <name>` 找到图中声明该名字的包,在第二遍准备中把暂存树交给它:暂存树目录
(`mcpp::pack_stage_dir()`、`${mcpp.stage_dir}`)、`mcpp-package.json`、`MCPP_PACK_*` 环境;它报告的终端输出即产物。
未知的名字被拒绝,消息列出图中可用的格式。**已实现**(`mcpp-package.json` 除外,**未实现**)

平台插件(SPEC-013 §12)**可以**在平台阶段声明包格式与 artifact action;平台阶段的指令表因此包含 `pack-format`
与 artifact action。该平台的目标打包时,这些格式与引擎的格式并列。**未实现**

## 7. 签名

`[pack] sign = ["<prog>", ..., "{}"]` 对暂存树中的每个程序与映像运行一次,`{}` 为文件,在写容器之前;一个包格式
提供者或构建插件**可以**代替它签名。任一次失败使打包失败,写出 argv 与输出。**未实现**

## 8. 分发描述

`mcpp emit xpkg` 由 `mcpp-package.json` 生成索引条目:每个系统 × 架构一项;或通用包时一个资产对应所有系统,
URL 相同,并写出运行所需的依赖(加载器的包)。**未实现**

## 9. 机器输出

`mcpp pack --message-format json` 打印一个 `mcpp.pack` 信封(docs/50)。`data` 增加:

| 字段 | 语义 |
|---|---|
| `linkage`、`mode`、`pkgFormat` | 三个维度的取值 |
| `legs` | 腿的规范三元组 |
| `systems` | 加载器所属的系统 |
| `artifacts[].sha256` | 每个文件产物的 sha256;目录产物为 `null` |

`artifacts[].format` 保留,与 `pkgFormat` 相同。**未实现**

## 10. 判据

1. 每个不合法的组合在任何编译之前被拒绝,消息写出冲突的选择与出路。
2. `--format` 在 `pack` 与 `run` 上与 `--pkg-format` 等价,并写出警告。
3. 通用包在每个平台架构的机器上经启动器运行;`self-contained` 的通用包在未安装加载器的机器上运行。
4. `--merge` 的结果与一次打出全部腿与系统的包逐文件相同。
5. 单文件在载荷摘要不符、缓存目录不完整时不执行不可信的内容;openkal 单文件运行时缓存目录中无新文件。
6. `mcpp-package.json` 的 `files` 覆盖树中每个文件,摘要与文件一致。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-10-28 | 首版(mcpp#847):三个维度与构建腿、组合的合法性、选项的全局用法与 `--pkg-format`、`[pack]` 的新键、管线、单腿与通用包的布局、启动器、`--systems`、`mcpp-package.json`、`--merge`、压缩、单文件、包格式提供者与平台插件、签名、分发描述、`mcpp.pack` 的新字段。 |
