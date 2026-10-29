# SPEC-015:目标集合与腿

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-015 |
| 标题 | 目标集合与腿:集合的写法与展开,命令对集合的语义,腿的规划与构建,测试归档,机器输出 |
| 状态 | 评审中 v1.0 |
| 版本 | 1.0 |
| 最后修改 | 2026-10-30 |
| 对应实现 | mcpp >= 2026.10.30.1 |
| 相关设计文档 | `.agents/docs/2026-10-29-ecosystem-target-sets-packaging-and-editors-design.md`(§3) |
| 相关 issue | mcpp#847 |
| 相关规范 | [SPEC-011](concurrency.md)(锁与原子发布)、[SPEC-013](platforms.md)(平台的选择、运行器、执行宿主)、[SPEC-014](packaging.md)(打包的腿与合并) |
| 使用文档 | [21 —— 目标三元组](../zh/21-the-target-triple.md)、[08 —— 测试](../zh/08-testing.md)、[50 —— 机器输出](../zh/50-machine-output.md) |

用语按 RFC 2119:**必须 / 禁止**为强制,**应当**为建议,**可以**为可选。

## 0. 范围

本规范规定 `--target` 的一种写法:**目标集合**,即一个系统(及至多一个环境)所含的、本 mcpp 构建的全部目标;以及
`build`、`run`、`test`、`pack`、`self env` 对一组目标(**腿**)的行为。腿的打包与合并属于 SPEC-014 §1.4。

## 1. 写法与展开

### 1.1 集合的识别

- **精确目标**是三元组,含义不变(docs/21)。
- **目标集合**是首段属于系统词表的写法。词表:`linux`、`macos`、`windows`、`openkal`、`emscripten`、`ios`、`android`、
  `none`。识别只看首段:`triple::parse` 不校验首段(`unknown-linux-gnu` 解析为架构 `unknown`),所以集合不以解析失败识别。
- 集合由系统与至多一个环境组成:`openkal`、`linux-musl`、`windows-gnu`、`none-eabihf`。多于两段的写法被拒绝。

### 1.2 环境的补全

| 写法 | 系统 | 环境 |
|---|---|---|
| `linux` | linux | `gnu` |
| `linux-<env>` | linux | `<env>` |
| `windows` | windows | `gnu`(不含 `msvc`) |
| `android` | linux | `android`(不接受更多的段) |
| `none` | — | 拒绝:裸机没有缺省环境,写出 `none-elf`、`none-eabi`、`none-eabihf` |
| `none-<env>`、`ios`、`ios-<env>` | 同名 | 按写法 |
| `openkal`、`macos`、`emscripten` | 同名 | 无;写出环境段时被拒绝 |

### 1.3 展开

1. 候选为内置目标表中系统与环境都相同、且等级不是 `planned` 的行。
2. 每个候选按 SPEC-013 的规则选择平台(目标行的平台列、`[target.<sel>] platform`、`MCPP_<T>_PLATFORM`)。全部候选
   **必须**选到同一个平台;否则命令被拒绝,消息逐行写出每个候选及其平台。
3. 没有候选时命令被拒绝;表中有 `planned` 的行时,消息写出这些行。
4. 结果为规范三元组,按目标表的顺序。

| 写法 | 展开(mcpp 2026.10.30.1 的目标表与 openkal-platform 0.18.0) |
|---|---|
| `openkal` | `x86_64-openkal`、`aarch64-openkal` |
| `linux-musl` | `x86_64-linux-musl`、`aarch64-linux-musl`(`planned` 的行不在内) |
| `linux` | `x86_64-linux-gnu`、`aarch64-linux-gnu` |

### 1.4 一条命令的腿

- 一条命令的腿是全部 `--target` 的并集:集合展开为其成员,精确目标保持原写法(单独规划时的补全与报告不变)。
- 同一目标只算一次(以规范形式比较)。顺序为目标表的顺序;表中没有的目标排在其后,保持书写的顺序。
- 机器输出与摘要只写展开后的目标;集合的写法不进入记录。
- `[build] target` **必须**是精确目标;写集合时清单被拒绝。集合可以写在命令行与 `[pack] targets`(SPEC-014 §2.2)。

## 2. 命令的语义

| 命令 | 一条腿 | 多条腿 |
|---|---|---|
| `build` | 构建这条腿 | 构建全部腿,每条腿在 `target/<triple>/` 下;一个 `Finished` |
| `run` | 本机架构经运行器;另一架构经执行宿主(SPEC-013 §10),否则拒绝 | 选第一条能到达的腿,本机架构优先;只构建这一条;写出所选的腿;都不能到达时拒绝 |
| `test` | 同 `run` 的到达规则;到达不了即拒绝 | 构建全部腿,运行能到达的腿,其余逐条报告"已构建、未运行"及原因;**一个测试都没有运行时失败**(退出码 1) |
| `pack` | 一个包 | SPEC-014 §1.4 |
| `self env` | 不变 | `data.legs[]` 每项是一份精确目标的报告;顶层取第一条腿 |

平台包中不带 `--target` 的 `mcpp test` 遍历平台的 `targets`,即多条腿的一个特例,行为同上表。

## 3. 规划与构建

- 腿**逐条串行规划**:一次规划一条腿,`mcpp.lock` 只在规划阶段写入,避免 SPEC-011 §4 的乐观写入冲突。
- 规划完成后各腿**一起构建**:`[build] jobs` 按腿静态分摊;构建目录锁按目标名排序获取。
- 一条腿失败时,其余腿完成构建;命令的状态为第一条失败的腿的状态。
- 单腿时规划、构建、`.last-result.json` 与记录的 `configuration` 与 2026.10.29.1 相同。多腿时 `.last-result.json` 记录每条腿,
  根目录的编译数据库为第一条腿的。

## 4. 测试归档

- **写**:`mcpp test [--target <t>...] --no-run --archive <file>` 构建各腿的测试程序,与清单 `mcpp-tests.json`(每个测试的腿、
  参数、工作目录)一起写为一个 tar 归档。归档是引擎的格式,与平台的容器无关。
- **读**:`mcpp test --archive <file>` 在同一项目的检出中运行归档内本机能到达的测试,不构建;工作目录为项目检出;报告与
  `mcpp test` 相同。
- 读取时:有测试失败则退出码 1;一个测试都没有运行则退出码 1 并写明;本机到达不了的腿报告为"未运行"。
- 原生交叉同样适用:在 Linux 上构建的 `x86_64-windows-gnu` 测试可以在 Windows 上运行。

## 5. 机器输出

- `mcpp.build`:单腿时逐字节不变。多腿时 `data` 的顶层字段取第一条腿,`status` 汇总全部腿,`legs[]` 每项为一份完整的结果
  记录;`artifact` 与 `diagnostic` 事件带 `triple`。
- `mcpp.env`(`self env --format json`):多腿时增加 `legs[]`。
- `mcpp.pack`:`legs`、`pkgFormats`、`runsOn`(SPEC-014 §9)。
- 新字段只增加,不改变已有字段的含义(docs/50)。

## 6. 判据

1. `mcpp build --target openkal` 构建两条腿,各在 `target/<triple>/` 下,一个 `Finished`;信封的 `data` 为第一条腿的记录,
   `legs` 含两条,每个 `artifact` 事件写出其腿(e2e 963)。
2. `mcpp run --target openkal` 运行本机的腿并写出所选的腿;`mcpp test --target openkal` 运行本机的腿,另一条报告为
   已构建、未运行,退出码 0(e2e 963)。
3. 各腿都到达不了时 `mcpp test` 不运行任何测试,退出码 1 并写明(e2e 963)。
4. `mcpp self env --target openkal --format json` 报告两条腿(e2e 963)。
5. `none`(无环境)、`linux-msvc`(无成员)与 `[build] target` 中的集合被拒绝(e2e 963)。
6. 集合的识别、补全、展开、去重与顺序,`planned` 的行不在内,候选选到不同平台被拒绝(unit `TargetSet`)。
7. 腿的规划串行、构建一起、作业分摊与第一条失败的腿(unit `BuildLegs`)。
8. `--no-run --archive` 写出的归档含两条腿与清单;在没有 `target/` 的检出中 `--archive` 运行本机的腿、不构建;失败的
   测试使退出码为 1;没有一条腿能到达的归档不运行任何测试且退出码为 1(e2e 964)。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-10-30 | 首版(mcpp 2026.10.30.1,设计 2026-10-29 §3):集合的识别、补全与展开,一条命令的腿,`build`、`run`、`test`、`self env` 对多条腿的语义,串行规划与一起构建,测试归档,机器输出;判据 1–8(e2e 963、964,unit `TargetSet`、`BuildLegs`)。 |
