# SPEC-005:构建数据库(`mcpp emit build-database`)

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-005 |
| 标题 | mcpp 输出的构建数据库:内容、取值规则与不写工程目录的保证 |
| 状态 | 评审中 v1.0 |
| 版本 | 1.0 |
| 最后修改 | 2026-09-15 |
| 对应实现 | mcpp >= 2026.9.15.1 |
| 相关设计文档 | `.agents/docs/2026-09-14-636-build-database-and-the-latest-xlings.md` |
| 相关 issue | #636 |
| 依据的外部规范 | S1「C++ Build Database: IDE Profile」profile 0.2.0 与 S2 0.2.0 §3.4,取自 https://github.com/Sunrisepeak/lsp-mcpp-private 提交 `b82859d`(schema 自提交 `28ecd6e` 起未变);JSON Compilation Database |

## 0. 适用范围

本规范规定 `mcpp emit build-database` 输出的文档、文档中每个字段取自构建计划的
哪一部分,以及这条命令对工程目录的保证。文档格式由 S1 与 JSON Compilation
Database 定义,本规范不重复它们的字段定义,只规定 mcpp 作为生产方的义务。消费方
的行为(监视、防抖、超时、把文档补全到 S1 等级 3)不属于本规范。

规范用语与实现状态标记见 [规范索引](README.md)。

## 1. 命令与文档

| 调用 | 标准输出 |
|---|---|
| `mcpp emit build-database` | S1 文档 |
| `mcpp emit build-database --spec compile-commands` | JSON Compilation Database |
| 以上任一加 `--format json` | [docs/50](../50-machine-output.md) 的信封,文档在 `data.database` |
| 以上任一加 `-o <file>` | 无;原本写到标准输出的内容原子地写入 `<file>` |

- **R1.1** `--spec` 的取值为 `s1`(默认)与 `compile-commands`。其他取值是用法错误:
  标准输出为空,退出码为 2。**已实现**
- **R1.2** 选择器与 `mcpp build` 相同:`--target`、`--toolchain`、`--profile`、
  `--release`、`--dev`、`--features`、`--cap`、`--accel`、`--no-accel`、`--static`、
  `--strict`、`-p`/`--package`、`--workspace`。同一组选择器下,文档描述的构建计划
  与 `mcpp build --configure-only` 计算的计划相同;包有测试时,计划包含测试目标与
  dev-dependency。**已实现**
- **R1.3** 规划过程的叙述写到标准错误,标准输出只有文档或信封。**已实现**

## 2. 不写工程目录

- **R2.1** 命令**禁止**写入工程目录,即根包、工作区成员与 path 依赖的源码树。规划
  写入 `$MCPP_HOME/cache/build-database/<key>`,`<key>` 由工程根与成员决定。该目录
  是缓存,可以随时删除。**已实现**
- **R2.2** 命令不编译:标准库模块被描述而不被编译,也不生成只供链接使用的输入(GCC 的
  `mcpp-clean-link.specs`)。工具链照常被查询(版本、目标三元组、sysroot 等),与
  `mcpp build` 相同;对没有构建程序的工程,驱动只为这些查询运行。**已实现**
- **R2.3** `mcpp.lock` 从工程根读取,从不写回。规划得出的解析与工程中的锁不一致,
  或工程中没有锁而规划会写出一份时,输出警告 `MCPP_LOCK_WOULD_CHANGE`。**已实现**
- **R2.4** 根包 `[build] generated_files` 中缺失或内容与声明不一致的文件不被写入,
  每个输出一条警告 `MCPP_GENERATED_FILE_NOT_MATERIALIZED`。**已实现**
- **R2.5** 构建程序照常运行,工作目录为包根,与 `mcpp build` 相同;构建程序在
  `MCPP_OUT_DIR` 之外写入的内容不在本保证之内。依赖提供的宿主工具照常构建到全局
  工具库。**已实现**
- **R2.6** `mcpp --protocol-version` 为这条命令声明 `init-mcpp-home`、`read-project`、
  `network`、`write-global-cache` 与 `exec-build-script`,不声明 `write-project`。
  **已实现**

## 3. S1 文档

mcpp 输出的 S1 文档满足 S1 等级 2,不输出 `ide.options`。等级 3 所需的结构化选项由
缺少 `options` 的一方按 S1 §9 规则 1 从 `arguments` 推导。

### 3.1 工具链

- **R3.1** `ide.toolchains` 的键为 `<family>-<version>-<triple>`,其中 `family` 是
  mcpp 的族名(`gcc`、`llvm`、`msvc`),`triple` 是编译器自身的拼写。键对消费方不
  透明,在一份文档内稳定。**已实现**
- **R3.2** `family` 为 `gcc`、`clang` 或 `msvc`。`driver` 为构建调用的驱动的绝对路径;
  `target` 为编译器自身拼写的目标三元组;构建使用 sysroot 时给出 `sysroot`;`stdlib`
  给出 `name`(`libstdc++`、`libc++`、`msvc-stl` 或 `other`)与 `version`,不给出
  `module-metadata`,标准库模块经 §3.4 的单元解析。**已实现**
- **R3.2a** `config-files` 列出驱动在命令行之外读取的配置文件,空数组表示没有:clang
  驱动旁的 `<驱动名>.cfg`,单元带 `--no-default-config` 时不列出;GCC 驱动库目录中
  `lib/gcc/<targetTriple>/<版本>/specs`,版本目录也可以只写主版本号。取值来自驱动
  搜索的目录布局,命令不运行驱动。**已实现**

### 3.2 集合

- **R3.3** 每个包一个集合,名为包的限定名(`<namespace>.<name>` 或 `<name>`)。根包
  测试目标的源文件归入集合 `<包>:test`,标准库模块的单元归入集合 `mcpp:std`。工作区
  文档中,每个集合名带前缀 `<成员>/`。**已实现**
- **R3.4** `visible-sets` 列出同一成员的其余所有集合。引擎在一次调用的一张模块图上
  解析 import,更窄的闭包会描述一条构建并不执行的规则。**已实现**
- **R3.5** `family-name` 为包名,`mcpp:std` 集合的为 `mcpp:std`;`ide.configuration`
  为 profile 名;`ide.kind` 在测试集合为 `test`,在根包集合按其目标为 `library`、
  `executable` 或 `other`,在依赖包集合与 `mcpp:std` 为 `library`。**已实现**
- **R3.6** 单元的 `arguments` 依次是驱动、集合的 `baseline-arguments`、单元的
  `local-arguments`,以及单元自己结尾的 `-c <source> -o <object>`(若有;两个操作数
  相对 `work-directory` 指向 `source` 与 `object`)。`baseline-arguments` 是集合中每个
  单元去掉驱动与该结尾后的最长公共前缀。取前缀而不取公共子集,因为参数顺序决定
  头文件搜索与宏定义。**已实现**

### 3.3 翻译单元

- **R3.7** 除 NASM 单元外,构建计划中的每个编译单元是一个翻译单元。`source`、
  `work-directory`、`arguments`、`object` 与 `compile_commands.json` 中对应条目的
  `file`、`directory`、`arguments`、`output` 取自同一条记录,因而逐字相同。
  **已实现**
- **R3.8** `provides` 把单元提供的模块名映射到空字符串,命令不执行构建(S1-8-6);
  `requires` 为单元导入的模块名,分区写全名 `M:P`。`private` 为 `false`,理由同 R3.4。
  **已实现**
- **R3.9** `ide.role` 取自扫描器读到的模块声明形式:

  | 声明 | `ide.role` |
  |---|---|
  | 无模块声明 | `non-module` |
  | `export module M;` | `module-interface` |
  | `export module M:P;` | `module-partition-interface` |
  | `module M:P;` | `module-partition-implementation` |
  | `module M;` | `module-implementation` |
  | `scan_overrides` 声明的单元;P1689 扫描中无法区分实现单元与导入者的单元 | `unknown` |

  **已实现**
- **R3.9a** 没有被 `sources` glob 匹配的目标入口源文件(发现的测试、glob 之外的
  `main`)与包源文件由同一扫描器读取,注释与原始字符串中的 `import` 不是导入。扫描器
  拒绝的入口文件(`#if` 块中的 `import`、头文件单元)从未在这条路径上被拒绝,现在也
  不被拒绝:`requires` 为其代码中行首的 `import`,`ide.role` 为 `unknown`。入口声明
  自身提供模块时,`ide.role` 为 `unknown`,因为构建不为该单元产出 BMI。**已实现**

### 3.4 标准库模块

- **R3.10** 构建导入 `std` 时,`mcpp:std` 集合包含 `std` 的单元;工具链有 `std.compat`
  的构建命令时,还包含 `std.compat` 的单元。`provides` 分别为 `std` 与 `std.compat`,
  `std.compat` 的 `requires` 为 `std`,角色均为 `module-interface`。该规则对工具链
  自带的标准库(GCC 的 `bits/std.cc`、libc++ 的 `std.cppm`、MSVC STL 的 `std.ixx`)与
  依赖包提供的 `std.cppm` 相同。**已实现**
- **R3.11** 这些单元的 `arguments` 与 `work-directory` 来自 mcpp 构建该模块时运行的
  命令:mcpp 为宿主 shell 渲染的命令去掉 `cd`、环境变量赋值与重定向,再撤销引号。
  命令中找不到该源文件时,不列出该单元,并输出警告
  `MCPP_BUILD_DATABASE_STD_UNIT_UNDESCRIBED`。**已实现**

## 4. `--spec compile-commands`

- **R4.1** 文档为 `mcpp build --configure-only` 在同一组选择器下写入
  `compile_commands.json` 的条目,差别只在输出路径位于 §2 的工作目录之下。标准库模块
  的单元不在其中。**已实现**

## 5. 信封

- **R5.1** `kind` 为 `mcpp.build-database`,`kindVersion` 为 1。`data` 含 `spec`
  (`{"name": "s1", "version": "0.2.0"}` 或 `{"name": "compile-commands"}`)、
  `database`、`watch` 与 `inputs-fingerprint`。**已实现**
- **R5.2** 失败时信封不含 `data`,`diagnostics` 至少含一条 `error`,退出码为 1:不在
  工程中为 `MCPP_BUILD_DATABASE_NO_PROJECT`,规划失败为
  `MCPP_BUILD_DATABASE_PLAN_FAILED`,工作区中其消息指出成员。工作区中任一成员规划
  失败,整次命令失败。**已实现**
- **R5.3** 信封的 `effects` 为 `read-project` 与 `write-global-cache`,运行了构建程序时
  另有 `exec-build-script`。**已实现**

## 6. `watch` 与 `inputs-fingerprint`

- **R6.1** `watch` 列出:工作区根与每个源码包的 `mcpp.toml`;`mcpp.lock`;存在时的
  `build.mcpp`;每个源码包的源文件 glob;测试发现的 glob(`[test] discover`,默认
  `tests/**/*.cpp`);构建程序声明的输入文件与 glob;`$MCPP_HOME/config.toml`。位于
  工作区根之下的条目写成相对工作区根、以 `/` 分隔的路径或 glob;之外的条目写成绝对
  路径,其 glob 展开为运行时匹配到的文件。**已实现**
- **R6.2** 不监视:环境变量,包括 `MCPP_TOOLCHAIN`、`MCPP_HOME` 与构建程序声明的
  环境变量;存储中的包与 git 依赖的检出,它们的版本或提交写在已监视的清单与锁中。
  **已实现**
- **R6.3** `inputs-fingerprint` 形如 `fnv1a:<16 位十六进制>`,是 mcpp 版本、选择器与
  `watch` 在运行时匹配到的每个文件的路径与内容的摘要;这些输入不变时它不变。它与
  构建指纹无关。**已实现**
- **R6.4** 命令不写入 `watch` 列出的任何文件,这由 §2 保证。**已实现**

## 7. 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-09-14 | 首版(#636)。 |
