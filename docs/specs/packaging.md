# SPEC-014:打包与分发

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-014 |
| 标题 | 打包与分发:链接、闭包、容器三个维度与构建腿,腿的合并,暂存树与包元数据,运行覆盖、单文件、容器列表、签名、压缩、合并与分发描述 |
| 状态 | 评审中 v2.0 |
| 版本 | 2.0 |
| 最后修改 | 2026-10-30 |
| 对应实现 | mcpp >= 2026.10.30.1 |
| 相关设计文档 | `.agents/docs/2026-10-29-ecosystem-target-sets-packaging-and-editors-design.md`(§4、§13)、`.agents/docs/2026-10-28-platforms-part2-packaging-and-distribution-design.md`(§2–§7、§14)、`.agents/docs/2026-10-28-platforms-design.md`(§4.7)、`.agents/docs/2026-10-29-openkal-run-test-pack-design.md`(§2.2、§2.6) |
| 相关 issue | mcpp#847、mcpp#849、mcpp#860 |
| 相关规范 | [SPEC-007](build-plugins.md)(包格式提供者、artifact action)、[SPEC-011](concurrency.md)(原子发布)、[SPEC-013](platforms.md)(平台、运行器、载体)、SPEC-015(目标集合与腿) |
| 使用文档 | [10 —— 发布打包](../zh/10-pack-and-release.md) |

用语按 RFC 2119:**必须 / 禁止**为强制,**应当**为建议,**可以**为可选。

## 0. 范围

本规范规定 `mcpp pack` 为一个**程序**目标产出可分发物的模型、管线、暂存树、包元数据、容器与接口,以及
`mcpp run --pkg-format` 与 `mcpp emit xpkg` 读取的部分。库的预编译包(docs/12)不在本规范内。

ok 映像是 x86_64 与 aarch64 上位置无关的 ELF,只导入 `kal_*`,运行时需要加载器发布的可执行内存(SPEC-013 §14);
平台的树(§4.2)、okb 与映像的单文件只装 ok 映像。Web 经 `wasm32-emscripten` 目标到达,其包为 WebAssembly 的同名文件族
(§4.1),发布为页面经 `dist-web` 提供者(§6);以 openkal API 写的代码经 openkal-emscripten 到达 Web。

## 1. 模型:三个维度与构建腿

一次打包的选择由三个正交维度与一组构建腿组成,每个维度回答一个问题:

| 维度 | 选项 | 回答的问题 | 归属 |
|---|---|---|---|
| 链接 `--linkage` | `static`、`shared` | 库在程序里,还是在程序旁边 | 程序属性 |
| 闭包 `--mode` | `system`、`vendored`、`self-contained` | 运行的机器要提供什么 | 打包属性 |
| 容器 `--pkg-format` | `dir`、`tar`、`zip`、`single-file`,提供者的格式,或一棵树的容器列表(§5.4) | 交付成什么 | 打包属性 |
| 腿 `--target`(可重复) | 目标或目标集合(SPEC-015) | 一次打包覆盖哪些目标 | 打包属性 |
| 系统 `--systems` | `<os>`、`<os>-<arch>`、`all` | 自带的运行器与单文件为哪些系统而做 | 打包属性 |

### 1.1 链接

`--linkage`(未给出时取 `[pack] linkage`)是这次打包的构建的依赖链接,优先于 `[build] dependency_linkage`
与构建配置中的值;都未给出时每个库按它自己的声明链接,包元数据记为 `declared`。原生 Linux 目标上 `static`
选择 `<arch>-linux-musl` 行:未给出目标且工程没有 `[build] target` 时为本机架构的 musl 行,`--target` 的 `-linux-gnu`
目标换成对应的 musl 行,工程自己的 `[build] target` 照原样打包;全静态程序不论 `--mode` 为何都整体打包;
openkal 目标上 `static` 为一个 `.oke`、无 `.okso`,`shared` 为 `.oke` 加其所需的 `.okso`。

### 1.2 闭包

| 模式 | 原生目标:包携带 | openkal 目标:包携带 | 运行的机器提供 |
|---|---|---|---|
| `system` | 程序 | 映像 | 全部共享库;openkal 上为加载器与映像所需的 `.okso` |
| `vendored`(缺省) | 程序与非系统共享库 | 映像与其 `.okso` | C 库、C++ 运行时与加载器;openkal 上为加载器 |
| `self-contained` | 程序、全部共享库、C 与 C++ 运行时、加载器 | 映像、`.okso` 与 `--systems` 的每个系统 × 腿的每个架构的运行器(载体的运行器形式,SPEC-013 §11;§4.4),腿数不论 | 无 |

`bundle-project` 与 `bundle-all` 是 `vendored` 与 `self-contained` 的永久别名。

`--mode static` 是 `--linkage static --mode self-contained` 的简写:原生目标上为全静态程序;openkal 目标上为每个架构
一个静态 `.oke` 加运行器的树。把映像嵌入一个程序(载体携带映像,SPEC-013 §11 的 M3)只属于单文件(§5.2):
`--linkage static --pkg-format single-file`。

`[pack] default_mode` 接受规范名 `system`、`vendored`、`self-contained`、`static` 与别名 `bundle-project`、`bundle-all`。

### 1.3 容器

| 值 | 产物 |
|---|---|
| `dir` | 暂存树本身 |
| `tar` | 一个归档,单一顶层目录与归档同名;缺省压缩为 gzip(§5.1) |
| `zip` | 一个 ZIP 归档,单一顶层目录;条目以 DEFLATE 压缩,`--compress none` 时存储(§5.1) |
| `single-file` | 一个可执行文件(§5.2) |
| 其他名字 | 依赖图中的提供者以 `pack-format` 声明的格式(§6) |
| 逗号分隔的列表 | 同一棵暂存树的各个容器,依次写出(§5.4) |

未给出 `--pkg-format` 与 `[pack] format` 时,Windows(PE)程序的容器为 `zip`,其余为 `tar`;显式给出的 `tar`
在任何目标上都写 tar。`[pack] format` 为字符串或列表。

### 1.4 构建腿与腿的合并

一次打包的腿为每个 `--target`,未给出时为 `[pack] targets`,都未给出时为构建的目标(`[build] target`,否则本机);
目标集合(`openkal`、`linux-musl`)按 SPEC-015 展开为目标表中所选平台的行。平台不放大腿:打包的腿只是命令所命名的。
其余的腿逐条规划、一起构建(SPEC-015 §3),在暂存之前完成。多条腿按目标的种类与平台合并:

| 目标的种类与平台 | 多条腿的结果 |
|---|---|
| `lib`、`shared` | 一个库包(docs/12) |
| 程序,平台声明 `runner`(openkal) | 一棵树(§4.2),包名 `<name>-<version>-<平台名>` |
| 程序,Android 的行 | 一个 APK 树(`lib/<abi>/`) |
| 其他程序 | 每条腿一个包,各以三元组命名;多于一个文件时 `-o` 只能是目录 |

单腿的包名为三元组,模式后缀的规则不变。工作空间多成员的打包接受目标集合,每个成员按上表。一个单腿的 openkal 打包在
`Packed` 之后写出提示:只含该架构,`--target openkal` 或 `[pack] targets = ["openkal"]` 打平台的全部架构。

`-o` 以 `/` 结尾或为已存在的目录时命名一个目录:这次打包写出的每个文件以其缺省名写在其下(`--pkg-format dir`
时 `-o` 仍命名树本身)。

### 1.5 组合的合法性

组合在任何构建开始之前检查;不可能的组合**必须**被拒绝,消息写出冲突的两个选择与出路。

| 组合 | 结果 |
|---|---|
| `--pkg-format single-file` 与 `--mode system` | 拒绝:单文件携带它所运行的东西;出路 `--mode vendored` 或 `self-contained` |
| `--compress` 与 `--pkg-format dir` 或提供者的格式 | 拒绝:只有容器被压缩;出路 `--pkg-format tar`、`zip` 或 `single-file` |
| `--compress zstd` 或 `xz` 与 `--pkg-format zip` | 拒绝:zip 的条目为 DEFLATE 或存储;出路 `--compress gzip`、`none`,或 `--pkg-format tar` |
| `--compress gzip` 或 `xz` 与 `--pkg-format single-file` | 拒绝:启动器解压 zstd;出路 `--compress zstd[:<level>]` 或 `none` |
| `--pkg-format single-file` 与 WebAssembly 目标(`wasm32-*`,不论目标来自 `--target`、`[build] target` 或其他来源) | 拒绝:单文件是一个系统的可执行文件,WebAssembly 模块由 JavaScript 引擎运行;出路 `--pkg-format tar`、`zip`、`dir` 或提供者的格式(页面为 `dist-web`)(2026.10.29.1) |
| 需要宿主提供能力的程序(docs/04 §2.11)与 `--mode self-contained` 或 `static` | 拒绝:携带自己 C 库的包不能使用宿主提供的库;出路 `--mode vendored` |
| `cxx_runtime = "toolchain-coupled"` 与 `--mode system` | 拒绝 |
| `--systems` 而非 `self-contained` 的树或映像的单文件 | 拒绝:只有自带运行器的树与映像的单文件按系统区分;`[pack] systems` 不相容时写出提示后搁置 |
| `--systems` 的 `<os>-<arch>` 不是任何腿的架构 | 拒绝 |
| 映像的单文件,`--systems` 中写出的系统在腿的架构上的行为本 mcpp 不构建的行(目标表标为 planned,如 `x86_64-macos`;或未列出,如 `aarch64-windows-gnu`) | 拒绝,写出该系统与行;出路为能构建的 `<os>-<arch>` 组成的 `--systems`。`all` 只取能构建的行,其余写为未覆盖 |
| 结果多于一个文件(多条腿的原生程序、多个单文件)而 `-o` 命名一个文件 | 拒绝;出路 `-o <目录>/` |
| 平台未声明 `runner` 的映像,多条腿,且不是单文件 | 拒绝:多条映像腿合并为一个包只经平台的运行器;出路每腿一次打包,或 `--pkg-format single-file` |
| 腿不是所选平台的目标 | 拒绝 |
| 目标集合展开为空,或候选选到不同平台 | 拒绝(SPEC-015 §1) |
| `--pkg-format` 列表含 `dir` 或 `single-file`;列表与命名一个文件的 `-o`;`mcpp run --pkg-format` 的列表 | 拒绝(§5.4) |
| `self-contained` 的树而格式只有 `okb` | 拒绝:okb v1 只含映像,运行器会被丢弃;出路 `--pkg-format tar,okb` |
| 树的 `runs_on` 为空(`self` 与 `with` 都空,§4.7) | 拒绝;平台未声明 `runner.hosts` 而模式不是 `self-contained` 时写出提示,不拒绝 |
| `--merge` 的输入缺少 `mcpp-package.json`,或名字、版本、平台、链接、模式不同 | 拒绝,写出不一致的字段 |
| `--merge` 的两个输入在同一路径上有摘要不同的文件 | 拒绝,写出该路径、两个摘要与两个输入 |
| 库目标(`lib`、`shared`)与提供者格式 | 拒绝:库包没有单一暂存树 |

`[pack] compress` 是项目的缺省值:与所选容器不相容时在这次打包中被搁置,只有 `--compress` 选项被拒绝。

## 2. 选项与配置

### 2.1 选项的全局用法

| 选项 | 命令 | 含义 |
|---|---|---|
| `--message-format json` | 构建类命令:`build`、`test`、`run`、`pack` | 机器输出 |
| `--format <编码>` | 报告类命令:`self env`、`toolchain list`、`emit build-database` 等 | 输出编码 |
| `--pkg-format <格式>` | `pack`、`run` | 包格式 |

`pack` 与 `run` 继续接受 `--format <格式>`,行为与 `--pkg-format` 相同,并以一条警告写出它是旧写法。

`pack` 的其余选项:`--linkage static|shared`、`--compress <算法>[:<级别>]`、`--systems <词>[,<词>...]`(§4.4)、
`--merge <包>`(可重复,也可逗号分隔)。`mcpp pack --help` 分两组:常用(`--target`、`--mode`、`--pkg-format`、
`--systems`、`--linkage`、`-o`)在前,其余在后。

`--no-universal` 是已弃用的写法(docs/51 的 `no-universal-flag`):与一个目标同用时接受,写出布局的变化(映像的包为
§4.2 的树;一个携带映像的本机程序为 `--pkg-format single-file`);与目标集合同用时拒绝。

### 2.2 `[pack]`

| 键 | 语义 |
|---|---|
| `default_mode` | 未给 `--mode` 时的闭包模式 |
| `linkage` | 未给 `--linkage` 时的链接(`static`、`shared`) |
| `format` | 未给 `--pkg-format` 时的容器:字符串或列表 |
| `compress` | 未给 `--compress` 时的压缩(§5.1) |
| `systems` | 未给 `--systems` 时的系统:字符串或列表;与这次打包不相容时写出提示后搁置 |
| `targets` | 未给 `--target` 时的腿:目标或目标集合,字符串或列表(SPEC-015) |
| `sign` | 签名步骤的 argv,`{}` 为文件(§7) |
| `universal` | 已弃用(docs/51 的 `pack-universal`):`true` 被拒绝,出路 `[pack] targets = ["openkal"]`;`false` 写出提示后忽略 |
| `include`、`exclude` | 工程中按 glob 选出的文件（相对工程根），放在暂存树中同样的相对路径下，一个包只放一份（树亦然）；`exclude` 去掉其中匹配的文件；`mcpp-package.json` 中角色为 `resource` |
| `strip`、`debug_symbols`、`[pack.bundle-project]` | 既有键 |

## 3. 管线

一次打包依次经过:

1. **构建各腿**:按维度与腿构建,`release` 为缺省构建配置;树与单文件的其余腿在暂存之前构建。
2. **收集闭包**:按 `--mode` 决定每条腿携带的库;闭包记录在暂存清单(docs/50)中,未解析的名字使 `dir` 与 `tar`
   拒绝。
3. **生成暂存树**:§4 的布局;树生成启动器;`self-contained` 加入加载器或运行器。
4. **变换**:strip、重定位、平台的封装(SPEC-013 §9.3)、签名(§7),然后写出 `mcpp-package.json`(§4.5)。
5. **写容器**:§5;提供者格式在第二遍准备中读取暂存树(§6);列表的其余容器读同一棵树(§5.4)。
6. **报告**:产物、每个文件产物的 sha256、`Runs on` 与未覆盖的组合(§4.7)、`mcpp.pack` 信封(§9)。

`--merge` 是"读入若干包 → 合并 → 描述 → 写容器",不经第 1、2、3 步(§4.6)。

## 4. 暂存树与包元数据

### 4.1 系统程序的布局

系统程序的包的布局由目标的二进制格式决定(ELF 的 `bin/` 与 `lib/`,PE 的扁平目录,Mach-O 的 `bin/`,Android 的
`lib/`,WebAssembly 的同名文件族),规定见 docs/10。平台未声明 `runner` 的映像按 ELF 的单腿布局放在 `bin/`。

### 4.2 平台的树

声明 `runner` 的平台的映像只有一种布局,与腿数和 `--systems` 无关:

```
<name>-<version>-<openkal | x86_64-openkal>/
  bin/<name>                         POSIX 启动器
  bin/<name>.exe                     Windows 启动器(引擎的启动程序)
  lib/<arch>/<name>.oke, *.okso      每条腿一组映像
  libexec/<runner>/<os>-<arch>/...   仅 --mode self-contained
  mcpp-package.json
```

多条腿的树以平台名(平台包名去掉 `-platform`)命名,不带系统与架构;一条腿的树以三元组命名;模式不是 `vendored`
时带模式的后缀(`-system`、`-static`、`self-contained` 为 `-bundle-all`),与原生包的命名相同。`[runtime] deploy`
放置的文件位于每条腿的映像旁边。不生成 `.cmd`。

### 4.3 启动器

启动器由引擎生成,内容只来自平台的 `runner` 与腿,是派生文件:`--merge` 为合并后的腿重新生成(§4.6)。

- 包的根为启动器所在目录的上一级。
- 架构目录按 `uname -m`(POSIX)或 `%PROCESSOR_ARCHITEW6432%`、`%PROCESSOR_ARCHITECTURE%`(Windows)选择;映射为
  `x86_64`、`amd64`、`AMD64` → `x86_64`,`aarch64`、`arm64`、`ARM64` → `aarch64`;没有对应目录时以状态 126 退出,
  写出本机架构与包含的架构。
- 运行 `runner.cmd`,与 `mcpp run` 的展开相同:含 `{}` 的词中 `{}` 替换为所选架构目录中的入口映像,没有词含 `{}`
  时映像追加在末尾;其余词原样传递,随后是启动器自己的参数;运行器先在 `libexec/<runner>/<os>-<arch>/` 中找,
  再找 `PATH`。
- POSIX 启动器把每个词写成单引号字符串,沿符号链接找到自身的位置(不依赖 `readlink -f`);`uname -s` 为 `MINGW*`、
  `MSYS*`、`CYGWIN*` 时交给 `bin/<name>.exe`。
- Windows 启动器是一个程序:源码随引擎提供,写明运行器的词与入口映像的名字,在打包时为 `x86_64-windows-gnu`
  构建(平台的托管安排服务该行时以它构建);它从自身路径(`GetModuleFileNameW`)找到包,按本机架构选
  `lib/<arch>/`,包内没有该架构时写出包含的架构。它不含腿的列表,因而对任一组腿相同。本 mcpp 不构建该行时包中
  没有它,写出原因,`runs_on` 不含 Windows。

### 4.4 `--systems`

`--systems` 的每个词为 `<os>`(`linux`、`macos`、`windows`,取腿的全部架构)、`<os>-<arch>`(只取该架构的腿)或
`all`(三个系统);可重复,也可逗号分隔;缺省为本机系统;未给出时取 `[pack] systems`。对 `self-contained` 的树,为
每个系统 × 腿的每个架构构建平台的载体(不携带任何内容,即运行器形式,SPEC-013 §11)并放入
`libexec/<runner>/<os>-<arch>/`;对映像的单文件,为每条腿与每个系统写一个文件(§5.2)。每个系统的程序在其行
(`<arch>-linux-musl`、`<arch>-macos`、`<arch>-windows-gnu`)上构建;平台的托管安排(SPEC-013 §5.4)服务该行时以它
构建,于是同一程序在任何打包的机器上相同,不需要该系统的 SDK 与 C 运行时;Linux 的程序为静态。托管安排不服务该行时
按惯常行构建,与本机系统和架构相同时取本机的行。本 mcpp 不构建程序的行(目标表标为 planned 或未列出)不放入包,并
写出该系统与行。

### 4.5 `mcpp-package.json`

暂存树根的 `mcpp-package.json` 是包格式提供者、`--merge` 与 `mcpp emit xpkg` 的输入,每次打包都写出(提供者的
格式也不例外):

```json
{
  "schema": 1,
  "name": "hello",
  "version": "0.1.0",
  "platform": "openkal-platform@0.17.3",
  "linkage": "declared",
  "mode": "vendored",
  "legs": ["x86_64-openkal", "aarch64-openkal"],
  "systems": [],
  "launch": {"runner": ["okrun", "{}"], "entry": {"x86_64": "lib/x86_64/hello.oke", "aarch64": "lib/aarch64/hello.oke"}},
  "files": [{"path": "lib/x86_64/hello.oke", "role": "image", "leg": "x86_64-openkal", "sha256": "..."}]
}
```

| 字段 | 语义 |
|---|---|
| `schema` | 本 schema 的版本,当前为 1;读取者拒绝其他版本 |
| `name`、`version` | 包的名字与版本 |
| `platform` | 平台包与版本(`<包>@<版本>`);无平台时为 `null` |
| `linkage` | `static`、`shared`,或 `declared`(每个库按自己的声明) |
| `mode` | §1.2 的模式:`system`、`vendored`、`self-contained`;`--mode static` 的原生包为 `static`,映像为 `self-contained` |
| `legs` | 腿的规范三元组 |
| `systems` | 包携带的运行器的 `<os>-<arch>`,即 `libexec/<runner>/` 中的;无运行器时为空。它是输入的记录,`runs_on` 是答案 |
| `launch` | 树为 `runner`(平台的 argv)与每个架构的 `entry`;系统程序的包为 `program`(程序的相对路径) |
| `runs_on` | 包在哪里运行(§4.7):`self` 与 `with.<runner>`,各为 `<os>-<arch>` 的列表 |
| `files` | 树中每个文件(符号链接除外):`path`(相对于树根,`/` 分隔)、`role`、`leg`(所属的腿,共用文件为 `null`)、`sha256` |

`role` 取 `program`、`image`、`library`、`loader`、`launcher`、`resource`、`metadata` 之一:树中 `lib/<arch>/`
下的文件为该腿的 `image`,生成的启动器为 `launcher`;其余按位置与名字判定 —— `libexec/` 下为 `loader`;
`README*`、`LICENSE*`、`HOST-REQUIREMENTS`、`share/licenses/` 为 `metadata`;`.so` 与 `.so.<版本>`、`.dylib`、`.dll`、`.okso` 为
`library`;`.oke` 为 `image`;`bin/` 下的可执行文件为 `program`;`.cmd`、`.sh` 与树根的可执行文件为 `launcher`;
其他为 `resource`。

### 4.6 合并

`mcpp pack --merge <a> --merge <b> ...`(也可 `--merge <a>,<b>`)读入至少两个本命令写出的包(目录,或 `.zip`、
`.tar`、`.tar.gz`、`.tgz`、`.tar.zst`、`.tar.xz` 归档),按 `mcpp-package.json` 合并为一棵树:名字、版本、平台、
链接与模式**必须**相同;腿、系统与 `runs_on` 取并集(同一映像在 Linux 与 macOS 上构建出相同字节;Windows 主机上
clang 在 `__FILE__` 中保留它拼接包含路径时用的 `\`,其映像不同,合并时被拒绝,mcpp#850);没有 `runs_on` 的输入由其
`systems` 与映像的架构算出;每个文件在复制前计算摘要并与记录的摘要比较,不符时拒绝并写出文件、输入与两个摘要;同一
路径的文件摘要**必须**相同,每个文件只取一份。启动器是派生文件,不参与同一路径的比较:POSIX 启动器为合并后的腿
重新生成,Windows 启动器取一份(§4.3);合并后的树重新描述。`mcpp-package.json` 中的每个
路径**必须**是包内的规范相对路径。输出**必须**是新的路径,或一个已有的、不是输入之一的本命令写出的归档;目录输出
只写新目录。
容器取 `--pkg-format`,否则取 `[pack] format`,否则取 `-o` 的扩展名,否则取第一个输入的容器;`-o` 缺省为当前目录下与树同名的文件。
合并后的包的目录以输出命名,与 `mcpp pack -o` 相同:归档名去掉其扩展名(`-o app-1.0-openkal.zip` 含
`app-1.0-openkal/`);不带 `-o` 时取第一个输入的目录名,树的目录名中第一条腿的三元组换成平台名
(`app-1.0-x86_64-openkal` → `app-1.0-openkal`)。
不进行任何构建。

### 4.7 运行覆盖(`runs_on`)

打包时计算一次,写入 `mcpp-package.json`(schema 1 的字段,不升级 schema):

| 包 | `self` | `with.<runner>` |
|---|---|---|
| 树,`self-contained` | 腿的架构 × 构建出的运行器的 `<os>-<arch>` | 腿的架构 × 平台的 `runner.hosts`,减去 `self` |
| 树,`vendored`、`system` | 空 | 腿的架构 × 平台的 `runner.hosts` |
| 系统程序的单腿包 | 三元组的 `<os>-<arch>` | 空 |
| 单文件 | 每个文件各自的 `<os>-<arch>` | 空 |
| WebAssembly、Android、iOS、裸机 | 空 | 空 |

`runner.host = "any"` 时 `hosts` 列出运行器程序存在的组合,覆盖与腿的架构无关。树中没有 Windows 启动器时 `runs_on`
不含 Windows 的组合。`self` 与 `with` 都为空的树被拒绝(§1.5)。`Packed` 之后写出 `Runs on <组合>; with <runner>:
<组合>`;所请求而未覆盖的组合各写一行 `Not covered <组合> (<原因>)`(本 mcpp 不构建该行、没有 Windows 启动器、
不在平台的 `runner.hosts` 中)。`mcpp emit xpkg`、`--merge`、`mcpp.pack` 的 `runsOn` 读这里,不再计算。

## 5. 容器

### 5.1 压缩

`--compress <zstd|gzip|xz|none>[:<level>]` 与 `[pack] compress` 选择 `tar` 的压缩:mcpp 自己写出 ustar 归档(按
路径排序;长名与链接目标用 pax 头;属主为 0,时间为 0,模式 0755、0644、0777),以自带的编解码器(zlib、zstd、
liblzma)压缩,不调用 `tar` 等外部程序;产物扩展名为 `.tar.gz`(缺省)、`.tar.zst`、`.tar.xz` 或 `.tar`。级别
范围:gzip 1–9,zstd 1–22,xz 0–9;写出的级别照用(含 `xz:0`、`zstd:1`),未写时为 gzip 的缺省(6)、zstd 19、xz 6。`zip` 的条目在变小时以 DEFLATE 压缩(`gzip[:<level>]` 设定级别),否则存储;
`none` 时全部存储。单文件的载荷总是一个 zstd 帧(启动器只解 zstd),`none` 取级别 1。okb 中的映像不压缩(SPEC-013 平台插件的格式规定)。同一棵树的
归档在每个宿主上字节相同:gzip 成员不带名字与时间、系统字节为 255,zip 条目使用固定日期。

**读取归档**(`--merge`、`mcpp emit xpkg --package`)在进程内完成,读取 ustar、pax 与 GNU 长名及首项为 `./` 的
归档,zip 的每个条目校验 CRC-32。写出任何文件之前拒绝:绝对路径或含 `..` 的条目、位于链接条目之下的条目、重复的
条目、指向树外的链接(链接目标逐段解析);链接在全部文件之后创建;目标目录必须不存在或为空。

### 5.2 单文件

| 组合 | 产物 | 运行 |
|---|---|---|
| 原生目标,`vendored` 或 `self-contained` | 引擎的启动器,携带暂存树的一个 zstd 帧 | 首次运行把树解到用户缓存,之后直接执行 |
| 原生 Linux 目标,`--linkage static` | 静态启动器加静态程序 | 同上,不依赖任何系统库 |
| openkal 目标 | 平台的载体,携带映像,或全部映像的一个 zip | 载体经加载器库从内存加载,不写盘 |
| WebAssembly 目标 | 不产出(§1.5) | — |

原生目标的启动器:

1. 源码随引擎提供,每次打包写入 `target/single-file/<name>/`(依赖 mcpplibs.os 与 zstd)并为目标构建:Linux 上为
   静态(musl),macOS 上为 `vendored`,Windows 上为 `system`;它只做校验、解压与执行。树暂存在
   `target/single-file/stage/` 下;产物为 `target/dist/<name>-<version>-<triple>`(Windows 上带 `.exe`)。
2. 载荷是暂存树的记录流(`MCPPTREE1\n`,每项为类型字节 `d`/`f`/`x`/`l`、32 位路径长度、路径、64 位数据长度、数据;
   小端,按路径排序)的一个 zstd 帧,帧写明内容大小与内容校验和。
3. 缓存根:`$XDG_CACHE_HOME`,未设置时 `~/.cache`(Linux);`~/Library/Caches`(macOS);`%LOCALAPPDATA%`
   (Windows);其下为 `mcpp/single-file/<载荷的 sha256>/`,该摘要由 mcpp 打包时写入启动器。
4. 解压时**必须**校验帧的内容校验和,不符时不解开;解到同一目录下的临时名(`.<摘要>-<进程号>`),全部文件写完后
   写入 `.mcpp-complete` 并以不替换的方式改名到位;**禁止**使用没有 `.mcpp-complete` 的目录;另一次运行先改名到位时
   使用它的结果;缓存目录在 POSIX 上只对当前用户可写,`mcpp/`、`single-file/` 或摘要目录为符号链接或属于其他
   用户时启动器拒绝运行。
5. 以原有的参数执行树中的程序(`bin/<name>`;self-contained 的 Linux 树为根部的包装脚本;Windows 树为根部的
   `<name>.exe`),退出状态即程序的退出状态;启动器自身失败(载荷不可读、校验不符、无法执行)时写出原因并以 127 退出。

openkal 目标的单文件:载体携带一条腿的映像本身(映像不引用其他映像时),否则携带该腿全部映像的一个 stored zip,
各映像数据按 64 KiB 对齐,`mcpp_carried_name` 为入口映像(SPEC-013 §11);每条腿 × `--systems` 在该腿架构上的每个
系统(缺省为本机系统)各写一个文件 `<name>-<version>-<os>-<arch>`(Windows 上带 `.exe`),即覆盖"腿 × 系统"的最少
载体文件;多于一个文件时 `-o` 只能是目录。`--systems all` 略去本 mcpp 不构建的行,各写为未覆盖。运行时不写盘。

### 5.3 一个文件在多个系统上运行

一个文件同时在多个系统与架构上运行(APE)不在本规范内(mcpp#849)。

### 5.4 容器列表

`--pkg-format`(与 `[pack] format`)可以是逗号分隔的列表,如 `tar,okb`:一次构建、一棵暂存树,依次写出各容器。

- 列表只能含读同一棵暂存树的容器:`tar`、`zip` 与提供者的格式;`dir`、`single-file` 不进列表。
- 列表时 `-o` 只能是目录(§1.4)。`--compress` 只作用于列表中的引擎容器;列表中没有引擎容器时拒绝。
- `self-contained` 的树而列表只有 `okb` 时拒绝(okb v1 只含映像)。okb 含哪些架构由腿决定。
- `mcpp run --pkg-format` 不接受列表。

## 6. 包格式提供者

一个包以构建程序的 `mcpp::provides_pack_format("<name>")` 声明一个格式,以 `role = "artifact"` 的 action 生成产物
(SPEC-007)。`--pkg-format <name>` 找到图中声明该名字的包,在第二遍准备中把暂存树交给它:暂存树目录
(`mcpp::pack_stage_dir()`、`${mcpp.stage_dir}`)、其中的 `mcpp-package.json`、`MCPP_PACK_*` 环境;它报告的终端
输出即产物。未知的名字被拒绝,消息列出图中可用的格式。

平台插件(SPEC-013 §12)**可以**在平台阶段声明包格式与 artifact action;平台阶段的指令表因此包含 `pack-format`
与 `role = "artifact"` 的 action(其他角色的 action 仍被拒绝)。该平台的目标打包时,这些格式与引擎的格式并列。

`mcpp pack --list-formats` 规划这次打包会构建的目标(`--target`,多次给出时取最后一个;未给出时为工程的目标)而不构建,
列出引擎的格式与该目标的依赖图(含其平台)声明的格式(2026.10.29.1)。

## 7. 签名

`[pack] sign = ["<prog>", ..., "{}"]` 在写容器之前对暂存树中角色为 `program`、`image`、`library`、`loader` 的每个
文件与 Windows 启动器(`bin/<name>.exe`)运行一次,也对每个单文件运行一次,`{}` 为文件路径;一个包格式提供者或构建插件**可以**代替它签名。任一次失败
使打包失败,写出 argv 与输出。签名后 `mcpp-package.json` 重新计算摘要。

## 8. 分发描述

`mcpp emit xpkg --package <归档> [--package ...] --url <前缀> [-o <文件>]` 由本命令写出的包的 `mcpp-package.json`
生成 xim-pkgindex 条目(spec 2),资产地址为 `<前缀>/<文件名>`。每个归档各以一个 `--package` 给出;其后不带选项的
词(例如 shell 通配展开的第二个文件)被拒绝,而不是不加描述。

- 树是一个资产,每个系统一项只写 `runs_on`(§4.7)覆盖到的架构,URL 与 sha256 相同;`runs_on` 为空时拒绝。包不携带
  运行器(非 `self-contained`)时各项以运行器为依赖(`deps = { "xim:<runner>" }`,`<runner>` 为平台运行器 argv 首项的
  程序名,即 xim-pkgindex 中同名的包;openkal 为 `okrun`)。依赖带命名空间:不带命名空间的名字在家目录列出的第二个
  索引也有同名包时有歧义,xim-pkgindex 拒绝它。
- 各系统各架构一个的包:每个系统一项,一个以 `${arch}` 代替架构的 URL 与每个架构的 sha256;文件名除架构外不同时
  拒绝。单腿包按其**程序**运行的系统与架构描述:腿为 Linux(Android 除外)、macOS 或 Windows 的三元组时,为该三元组的
  系统(`linux`、`macosx`、`windows`)与架构。程序不是任何系统的程序时(WebAssembly 包、Android 与 iOS 应用、裸机
  映像、平台未声明运行器的 ok 映像包),条目**必须**被拒绝,消息写出包、三元组与出路:WebAssembly 包以 `dist-web`
  发布为页面,ok 映像以平台的树(`--target openkal`)或单文件描述。原因:条目的
  `xpm.<系统>` 安装该系统的程序,`archs` 为机器架构;WebAssembly 包没有启动器,其程序由 JavaScript 引擎运行,
  `wasm32` 不是任何系统的机器架构;一个 `.oke` 需要运行器(2026.10.29.1)。
- 安装把归档的唯一顶层目录移到安装目录。该目录以归档命名(`mcpp pack` 的写法),安装取下载文件的名字去掉扩展名,
  对每个系统、架构与索引日后追加的版本都成立;打包后改名的归档与 xim 不解开的文件(非 `.tar.gz`、`.tar.zst`、
  `.tar.xz`、`.tar`、`.zip`)在写出条目时被拒绝;配置以 xvm 注册程序(树为 `bin/<name>`,Windows 上为
  `bin/<name>.exe`;单腿原生包为程序所在的目录)。各系统的包把程序放在不同目录时(Windows 的程序在包根,POSIX
  的程序在 `bin/`,包根为便利脚本),条目按系统写出各自的目录(`bindir[os.host()]`)。

不带 `--package` 时 `mcpp emit xpkg` 照旧生成源码包的描述。

## 9. 机器输出

`mcpp pack --message-format json` 打印一个 `mcpp.pack` 信封(docs/50)。`data` 增加:

| 字段 | 语义 |
|---|---|
| `linkage`、`mode`、`pkgFormat` | 三个维度的取值;`pkgFormat` 为第一个容器 |
| `pkgFormats` | 写出的全部容器,按命名的顺序(2026.10.30.1) |
| `legs` | 腿的规范三元组;每腿一个包时为全部包的腿,`artifacts[].targets` 为各自的腿 |
| `systems` | 运行器所属的 `<os>-<arch>` |
| `runsOn` | `{ "self": [...], "with": { "<runner>": [...] } }`,同 §4.7(2026.10.30.1) |
| `artifacts[].sha256` | 每个文件产物的 sha256;目录产物为 `null` |

`artifacts[].format` 保留,与 `pkgFormat` 相同。新字段只增加,不改变已有字段的含义。库目标的打包不带这些字段。

## 10. 判据

1. 每个不合法的组合在任何编译之前被拒绝,消息写出冲突的选择与出路(e2e 954)。
2. `--format` 在 `pack` 与 `run` 上与 `--pkg-format` 等价,并写出警告(e2e 954)。
3. `--target openkal` 是一棵树,在本机架构上经启动器运行;`self-contained` 的树在 `PATH` 上没有运行器时运行;一个
   目标只打一条腿,布局相同(e2e 955、983)。
4. `--merge` 的结果含两个输入的全部腿、系统与文件,摘要一致;冲突与不同版本被拒绝;两个单腿的树合并后启动器
   覆盖两条腿(e2e 957、983)。
5. 单文件首次运行解压一次,再次运行不解压;静态单文件不带解释器;映像的单文件运行时缓存目录中无新文件(e2e 956)。
6. `mcpp-package.json` 的 `files` 覆盖树中每个文件,摘要与文件一致(e2e 954、957)。
7. 树的 `mcpp emit xpkg` 条目为一个资产,只写 `runs_on` 覆盖的系统与架构,并以运行器为依赖;`runs_on` 为空时
   拒绝(e2e 957、961)。
8. `mcpp emit xpkg` 拒绝 `wasm32-emscripten` 的包,写出包、三元组与 `dist-web`;`--pkg-format single-file` 在 WebAssembly
   目标上于任何编译之前被拒绝(e2e 962)。
9. 映像的单文件是映像架构的程序;单腿 `self-contained` 的树的 `systems` 与 `runs_on.self` 为其运行器的
   `<os>-<arch>`,`mcpp emit xpkg` 以该系统与架构描述它;`mcpp pack --target <t> --list-formats` 列出该目标的平台
   提供的格式(`okb`),本机目标的列表不含它(e2e 961)。
10. 原生程序的目标集合每腿一个包,`-o` 命名一个文件时被拒绝(e2e 984)。
11. `--pkg-format` 列表一次构建写出各容器;`dir`、`single-file` 在列表中、列表与一个文件的 `-o`、`mcpp run` 的列表、
    `self-contained` 只配 `okb` 被拒绝(e2e 985)。
12. 映像的单文件覆盖腿 × 系统;`--systems all` 写出未覆盖的组合;多个文件与一个文件的 `-o` 被拒绝(e2e 986)。
13. `--no-universal`、`[pack] universal`、`[platform] universal` 按 §2 迁移;`mcpp pack --help` 分组(e2e 987)。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-10-28 | 首版(mcpp#847):三个维度与构建腿、组合的合法性、选项的全局用法与 `--pkg-format`、`[pack]` 的新键、管线、单腿与通用包的布局、启动器、`--systems`、`mcpp-package.json`、`--merge`、压缩、单文件、包格式提供者与平台插件、签名、分发描述、`mcpp.pack` 的新字段。 |
| 1.1 | 2026-10-28 | 与实现对齐:zip 的条目以 DEFLATE 压缩;`[pack] universal`;压缩的拒绝与项目缺省值的搁置;`--systems` 也用于映像的单文件;启动器的根与退出状态;单文件的载荷格式、校验与执行;映像单文件的载体内容;`mcpp-package.json` 的 `linkage` 取值与角色判定;`--merge` 的输入、选项写法与容器;签名的范围;`mcpp emit xpkg --package`;判据对应 e2e 954–957。 |
| 1.2 | 2026-10-28 | 包名的模式后缀;单文件启动器自身失败的退出状态 127;`--merge` 取 `[pack] format`,合并后的目录以输出命名;`--linkage static` 改为 musl 的行;`mcpp emit xpkg --package` 的每系统程序目录、带命名空间的运行器依赖、以归档名定位的顶层目录与多余词的拒绝。 |
| 1.3 | 2026-10-29 | mcpp 2026.10.29.1:§0 ok 映像与 Web;§1.2 单腿 `self-contained` 包的载体为映像架构的程序;§1.5、§5.2 WebAssembly 目标上的单文件被拒绝;§4.5 单腿 `self-contained` ok 映像包的 `systems` 记录其载体;§6 `--list-formats` 规划所选目标;§8 单腿包按其程序运行的系统与架构描述(系统的三元组,或 ok 映像的载体),程序不是任何系统的程序的被拒绝;判据 8、9(e2e 961、962)。 |
| 2.0 | 2026-10-30 | mcpp 2026.10.30.1(设计 2026-10-29 §4、§13):§1.4 腿只是命令命名的目标(目标集合按 SPEC-015 展开),腿按目标的种类与平台合并,原生程序每腿一个包,`-o <目录>/`;删除通用包与 `[platform] universal`、`[pack] universal`、`--no-universal` 的作用(迁移见 §2、docs/51);§1.2、§4.2 声明运行器的平台的映像只有一种布局(树),M3 只属于单文件,`--mode static` 为静态树;§4.3 Windows 启动器为引擎的程序,不再生成 `.cmd`,启动器为派生文件;§4.4 `--systems` 接受 `<os>`、`<os>-<arch>`、`all` 与 `[pack] systems`;§4.6 合并重新生成启动器、合并 `runs_on`、树以平台名命名;§4.7 运行覆盖 `runs_on` 与 `Runs on`、未覆盖的摘要,空覆盖被拒绝;§5.2 单文件覆盖腿 × 系统;§5.4 容器列表;§7 签名含 Windows 启动器;§8 条目按 `runs_on` 描述;§9 `pkgFormats`、`runsOn`;判据 3、4、7、9 改写,判据 10–13(e2e 983–987)。 |
