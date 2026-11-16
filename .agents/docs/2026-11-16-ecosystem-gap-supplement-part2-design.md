---
subject: cross-platform
status: draft
---

# 生态缺口补充方案（第二部分）：文件的角色、运行文件、清单书写与第一部分的遗留项

**日期：** 2026-11-16 · **版本：** v2（评审稿，经第 12 节的多角度评审修订） · **基线：** mcpp 2026.11.16.1（main 另含 #929：多行内联表）、
openkal-platform 0.29.0、dirun 0.5.0、ovmf 9.2.4-3、mcpp:plugins 0.22.0（平台以 0.20.0 构建）、xlings v2026.10.26.1 ·
**前文：** [第一部分](2026-11-15-ecosystem-gap-supplement-design.md)（N1–N47，13.12–13.17 为本文的来源）

## 1. 来源与范围

第一部分发布后，同一外部验证工程在 mcpp 2026.11.16.1 与 openkal-platform 0.29.0 上运行，暴露了两类问题。

- **第一类：已有设计的缝隙。** 第一部分 13.12–13.16 记录了这一类。
- **第二类：文件放置的语义不清。** 一个程序运行时读取的文件、随包发布的文件、只在某一次运行中使用的文件，目前没有
  清晰的区分，书写也冗长。

本文给出一个统一的方案，并把每项改动划归到具体的仓库。

已由评审确定、本文只展开细节的项：

| 编号 | 决定 |
|---|---|
| D1 | N44：DOS 行的首个栈由可执行文件声明大小（缺省 1 MiB），越界产生故障 |
| D2 | N46：单次运行的输入文件；不经 runner 时在临时工作目录中运行 |
| D3 | 新增 OpenBSD、NetBSD、illumos 三行，先以平台包提供 |
| D4 | N43 与 N47 都实现：openkal-uefi 读取 `LoadOptions`；dirun 提供可写的 FAT 映像 |

本文新提出、待评审的项：

- 文件角色的模型（第 3 节）；
- `deploy` 的条目语法与许可证文件（第 4 节）；
- 运行文件的书写与契约（第 5 节）；
- 清单书写的简化（第 6 节）；
- 运行器规范的收口（第 7 节）。

## 2. 评审依据

- **第一部分 §2 的分层**：引擎、数据、实现、运行器、插件、包、工程。本文沿用，不重复。
- **mcpp AGENTS.md 的规则作为缺省遵守。** 偏离须写明理由，并作为评审项单列：
  - 引擎只提供机制，事实写在数据里；
  - 每个缺省值是数据，并有四层覆盖；
  - 一个概念一种拼写，已发布的键不改名；
  - 未知键必须报告；
  - 做得少时不沉默。
- **openkal SPEC 0.25.3 的相关条款：**
  - 6.5：由产物的生成方式决定的可用性，在依赖解析时确定，不在运行时报告；
  - 7.6：参数向量原样传递；
  - 7.13：程序把第一个授予的目录视为它的启动目录。

## 3. 文件的角色：运行时与发布的语义区分

### 3.1 四种角色

一个文件按“谁读它、何时需要它”分为四种角色：

| 角色 | 读取者 | 需要的时机 | 构建目录 | 运行时投递 | 打包时暂存 | 例 |
|---|---|---|---|---|---|---|
| 构建输入 | 编译器、链接器、构建程序 | 构建 | — | 否 | 否 | 资源脚本、链接脚本、`link.inputs` |
| **运行时文件** | 程序本身，相对于自身目录 | 每次运行 | 放在程序旁 | 是 | 是 | 插件、ICD 清单、数据文件、程序旁的 DLL |
| **发布文件** | 人与分发系统，程序不读 | 安装与审计 | 不放 | 否 | 是 | 许可证、声明、README、图标、手册页 |
| **运行文件** | 程序本身，相对于工作目录 | 某一次运行 | 不放 | 只在这次运行 | 否 | 测试脚本、测试数据、一次性的配置 |

判别方法有两条：

- 删去一个文件后，程序的行为若不变，它就不是运行时文件；
- 一个文件若不属于产物，它就不随包发布。

### 3.2 现状与角色的对应

| 机制 | 实际承担的角色 | 问题 |
|---|---|---|
| `[runtime] deploy_files` | 运行时文件（放在程序旁） | 与 `deploy` 是同一概念的两种写法 |
| `[runtime] deploy`（`from`、`to`） | 运行时文件（放在 `to` 子目录） | 每条一个文件，没有通配符和目录；`to` 只能是目录，不能改名 |
| `[pack] include`、`exclude` | 发布文件，**只限本工程** | 依赖包没有办法声明“随消费者的包发布”的文件 |
| `share/licenses/<soname>/` | 发布文件，自动生成 | 只在 `self-contained` 方式下，只覆盖共享库 |
| 无 | 运行文件 | 外部工程只能绕过 mcpp 直接调用 dirun（第一部分 N46） |

实例：mcpp-index 的 `compat.onnxruntime` 用 `[runtime] deploy` 放置 `LICENSE` 与 `ThirdPartyNotices.txt`。
- 这两份文件是**发布文件**，被当作运行时文件处理，因而在每次构建中都被复制到 `bin/licenses/`。
- 三个系统各写两条，`to` 都相同，一共六条。

### 3.3 结论

1. 运行时文件只需要一种拼写，并补上通配符、整目录和改名。
2. 依赖包需要有办法声明发布文件：只在打包时暂存，不进入构建目录，也不投递给 runner。
3. 运行文件是一个独立的概念。它的锚点是**工作目录**，而运行时文件的锚点是**程序所在目录**；锚点不同，所以
   不能与 `deploy` 合并（第 5 节）。

## 4. 方案 F：运行时文件与发布文件

### 4.1 `deploy` 的条目语法（运行时文件）

列表中的每一项是一个字符串，或者一张表。字符串是表的简写：

```toml
[runtime]
deploy = [
    "share/data.bin",                                          # = { from = "share/data.bin" }：放在程序旁
    { from = "share/vulkan/icd.d/**", to = "vulkan/icd.d" },   # 一棵目录
    { from = "res/*.{json,bin}", to = "res" },                 # 通配符
    { from = "tools/x.conf", name = "app.conf" },              # 改名：from 必须恰好匹配一个文件
]
```

| 字段 | 语义 |
|---|---|
| `from` | 相对于声明它的包的根，用 `/` 分隔。通配符与 `sources`、`[pack] include`、`[test] discover` 是同一种（引擎已有的 `expand_glob`：`*`、`**`、`?`、`{a,b}`），不引入第二种方言。匹配为空时报错，并指出该条目的序号 |
| `to` | 目的地目录，相对于程序所在目录，缺省为 `"."`。语义与现有的 `deploy.to` 相同 |
| `name` | 目的地的文件名。只在 `from` 恰好匹配一个文件时可用，否则报错 |

**规则：**
- **目的地的相对路径**：一个匹配的文件放在 `to` 下，路径取它相对于通配符的固定目录的部分。这是 SPEC-017 B1 第 3 条
  为测试程序命名时已有的规则。字面路径的固定目录是它的上级目录，所以现有的每一条 `deploy` 的落点不变。
  - `share/vulkan/icd.d/**` 的固定目录是 `share/vulkan/icd.d`，整棵目录保持结构；
  - v1 中“以 `/` 结尾表示目录”的写法因此去掉：它与 `dir/**` 是同一件事的两种拼写。
- **沿用**：绝对路径、盘符、空段、`.`、`..` 一律拒绝；同一目的地有多个来源时，内容相同则合并为一份，不同则拒绝
  （mcpp#723）。
- **展开的时机**：清单中的通配符在准备阶段对包内已有的文件展开。构建程序生成的文件由 `mcpp::deploy(from, to)`
  以字面路径声明；路径由构建程序自己计算。**插件协议不变**（v1 中拟加的 `name`、`phase` 参数取消）。
- **增量与清理**：展开的结果进入构建图。一个文件不再匹配时，它在程序旁的副本被移除；否则旧文件会留在运行环境中。
  通配符的遍历以固定目录为界（mcpp#225 已有的规则）。

### 4.2 键名：扩展 `deploy`（评审项）

| 方案 | 内容 | 判断 |
|---|---|---|
| **A（推荐）扩展 `deploy`** | `deploy` 是这个概念的唯一拼写；`deploy_files` 作为历史上的字符串形式继续有效，文档写成一条规则（“`deploy_files = [x]` 即 `deploy = [x]`”），新文档只展示 `deploy` | 不引入第三个名字。旧引擎遇到新写法时**报错**，不静默：读源码确认，`deploy` 中的字符串报“must be a table”，条目中的未知键报“unsupported key”（`modules/manifest/src/toml.cppm`、`xpkg.cppm`） |
| B 新键 `files` | `[runtime] files` 取代两者，旧键作为别名 | 偏离“已发布的键不改名”；过渡期内同一概念有三种拼写；旧引擎对未知的 `[runtime]` 键只写一条提示后忽略，文件不会放置 |

规则可以在理由充分时偏离。v1 中支持 B 的唯一论据是“`deploy` 字面上不涵盖发布文件”；第 4.3 节把发布文件移出
`deploy` 之后，这条论据不再成立，所以推荐 A。

### 4.3 发布文件：`license_files`（评审项，取代 v1 的 `phase = "pack"`）

v1 提出在 `deploy` 条目上加 `phase = "pack"`。评审否定了它：

- **语义矛盾**：`[runtime]` 段陈述“产物在运行时需要什么”。`[runtime] deploy = [{ phase = "pack" }]` 的字面意思是
  “运行时放置一个运行时不需要的文件”。
- **`phase` 不是划分**：运行时文件同样进入包，所以 `run` 与 `pack` 是包含关系，不是并列的两个阶段；它与
  `requirements` 中的 `phase`（`run`、`link`，表示在哪个阶段检查）含义不同，却用同一个名字。
- **事实与机制**：依赖包需要随消费者的包发布的文件，实际上只有许可证与声明。“这是许可证”是一个事实，应当写在数据里；
  放在哪里、何时放，是机制，应当由引擎决定。`phase = "pack"` 加 `to = "licenses/onnxruntime"` 让每个包自己决定
  落点，结果不会一致。
- **现有实现的问题**（`src/pack/pack.cppm` 的 `collect_licenses`）：引擎内写死七个文件名，从共享库所在目录向上找
  三层，找不到时不报告；只在 `self-contained` 方式下进行，只覆盖共享库。这同时违反“事实写在数据里”与“做得少时
  不沉默”。静态链接进程序的依赖同样有许可证义务，目前完全没有覆盖。

**方案：**

```toml
[package]
license       = "MIT"
license_files = ["LICENSE", "ThirdPartyNotices.txt"]     # 通配符同 4.1；相对于包的根
```

- **机制**：打包时，对程序的链接闭包与运行闭包中的每一个包（静态库、共享库、带 `deploy` 文件的包、工程自身），
  把它的 `license_files` 放到 `share/licenses/<包名>/`，角色为 `metadata`（SPEC-014 §4.5 已有的角色与目录）。
  所有打包方式都进行，不只是 `self-contained`。
- **描述符**：xim 的 Lua 描述符在 `mcpp` 段写同名的键，可以写在各系统共用的块中。
- **没有声明的包**：保留现有的按文件名查找作为退路，并在打包报告中列出“未声明许可证文件”的包（做得少时不沉默）。
- **`[pack] include` 不变**：本工程的其他发布文件（README、图标、手册页）仍由它表达，锚点是包的根。

四种角色因此各有一个归属，互不重叠：

| 角色 | 写在哪里 | 锚点 |
|---|---|---|
| 构建输入 | `[build]`、`[resources]`、平台的 `link.inputs` | — |
| 运行时文件 | `[runtime] deploy`（任何包） | 程序所在目录 |
| 发布文件 | `[package] license_files`（任何包）；`[pack] include`（本工程） | 包的根 |
| 运行文件 | `mcpp run --file`、`[test] files`（第 5 节） | 本次运行的工作目录 |

### 4.4 改写示例

`compat.onnxruntime`（xim 的 Lua 数据）：

```lua
-- 现状：三个系统，每个两条 deploy，共六条；每次构建都把许可证复制到 bin/licenses/
deploy = {
    { from = "onnxruntime-linux-x64-1.25.1/LICENSE", to = "licenses/onnxruntime" },
    { from = "onnxruntime-linux-x64-1.25.1/ThirdPartyNotices.txt", to = "licenses/onnxruntime" },
},
-- 改写：一行，写在三个系统共用的块中；构建目录中不再出现许可证
license_files = { "onnxruntime-*/{LICENSE,ThirdPartyNotices.txt}" },
```

### 4.5 兼容

- 现有清单的行为不变：`to` 的含义不变；字符串形式与 `deploy_files` 相同；字面路径的落点不变。
- 发布到索引的包只有声明了 `mcpp = ">=<本版本>"` 时，才能使用通配符、`name`、字符串形式与 `license_files`。
  旧引擎对前三者报错（4.2）；对 `[package]` 的未知键报告后忽略，后果只是许可证没有进入包，与现状相同。
- 包中许可证的位置从 `bin/licenses/<名字>/` 变为 `share/licenses/<包名>/`，只发生在改写了描述符的包上。

### 4.6 已知的边界（不在本轮）

`to` 拒绝 `..`，所以“程序在 `bin/`，数据在 `../share/<name>/`”的布局不能用 `deploy` 表达；`[pack] include`
能把工程的文件放进包的 `share/`，但构建目录中没有对应的位置。这是既有的限制，本文只记录，不改变。

## 5. 方案 I：运行文件

### 5.1 语义

运行文件是“这一次运行开始时，工作目录中应当有的文件”。它与运行时文件的区别只在锚点和生命周期：

| | 锚点 | 生命周期 |
|---|---|---|
| 运行时文件（`deploy`） | 程序所在目录 | 与产物相同 |
| 运行文件（`--file`） | 本次运行的工作目录 | 与本次运行相同 |

### 5.2 书写与名字（评审项）

```sh
mcpp run app --target x86_64-uefi --file tests/t.bat=TEST.BAT -- /c TEST.BAT
```

```toml
[test]
files = ["tests/data/**", { from = "tests/t.bat", name = "TEST.BAT" }]

[target.'cfg(os = "uefi")'.test]
files = ["tests/uefi/**"]
```

- 条目语法与第 4.1 节相同；`to` 和 `name` 相对于工作目录。
- 命令行写作 `FROM[=TO]`，与 dirun 已发布的 `--file HOST=GUEST` 相同。
- **名字**：v1 把评审时选定的 `--file` 改成了 `--input`、`inputs`。评审认为这一改动不成立，改回：
  - 生态中这个概念已有一个发布过的拼写：dirun 的 `--file`。mcpp 的命令行用另一个词，就是同一概念的两种拼写；
  - `inputs` 与平台的 `link.inputs` 同名，但后者的 `from` 是一条命令的 argv（SPEC-013 §9.2），条目语法不同。
    v1 第 12 节写的换算规则“`{ file = x }` 即 `{ from = x }`”是错的，已删除。
  - 剩余的不足：`[test] files` 可能被读成“测试的源文件”（那是 `discover`）。另一个候选是保留 `inputs`。
    推荐 `--file` 与 `files`。

### 5.3 工作目录

| 情形 | 工作目录 |
|---|---|
| 没有运行文件 | 不变（无感升级） |
| 有运行文件，宿主的文件系统对程序可见（本机运行、qemu-user、wine、okrun） | `target/<triple>/<fp>/run/<id>/`：运行前复制运行文件，运行后删除；`mcpp test` 在失败时保留，并在报告中给出路径 |
| 有运行文件，程序有自己的文件系统（虚拟机、固件、设备） | runner 把运行文件与运行时文件一起投递到程序旁边；在这些环境中，程序所在目录就是它的工作目录 |

- **引擎对两种情形做同一件事**：备好工作目录，在其中启动 runner，并把运行文件写进运行器契约（7.1）。投递文件的
  runner 照常投递，共享宿主文件系统的 runner 从工作目录读取。runner 不需要新增键。
- **粒度**：`mcpp run` 一次运行一个目录。`mcpp test` 每个测试程序一个目录，该程序按 `split` 分出的各个进程共用它；
  只有声明了 `files` 的工程付出复制的代价。文件系统支持时用写时复制的克隆，否则复制；不用硬链接和符号链接，
  因为程序写入时会改到源文件。
- **与测试协议一致**：SPEC-017 B5 的 `launch.cwd` 已经是规范的一部分。工作目录在写出 `launch` 记录时备好，
  所以 `--no-run` 之后由外部启动者（包括 O9 的会话）启动的程序，看到的目录相同。
- **用户会注意到的一点**：带 `--file` 的本机运行，工作目录不再是调用时的目录，`--` 之后的相对路径不再指向原处。
  文档写明这一点；`-v` 输出工作目录的路径。
- **openkal 的一致性**：okrun 把启动目录作为第一个授予的目录（SPEC 7.13）。所以一个 ok 映像在工作目录中看到的
  文件，与原生程序看到的相同。
- **范围**：v1 的运行文件是工程中已有的文件。构建时生成的数据经 `deploy` 到达程序旁。

## 6. 清单书写的简化

### 6.1 多行内联表（#929）

下一个 mcpp 版本接受跨多行、带注释、末尾带逗号的内联表（TOML 1.1）。随该版本：

- 文档中 runner、`deploy`、依赖的示例改为多行写法；
- openkal-platform 的 runner 改为多行，在下一个要求该 mcpp 版本的平台版本中进行。现在每个 runner 都是一行超过
  200 个字符的内联表。

```toml
runner = {
    cmd      = ["dirun", "--payload", "{}", "--profile", "{pkg:ovmf}/dirun.toml",
                "--emulator", "{pkg:qemu-x86}/bin/qemu-system-x86_64", "--memory", "2048", "--"],
    hosts    = ["linux-x86_64", "linux-aarch64", "macos-aarch64", "windows-x86_64"],
    requires = ["xim:dirun@0.6.0", "xim:qemu-x86@9.2.4-1", "xim:ovmf@9.2.4-4"],
    enabled  = false,
}
```

### 6.2 同一平台写多行（评审项：本轮不做）

外部工程为四行写了同一个平台的同一个版本（`i686-linux-musl`、`i686-openkal`、`x86_64-uefi`、`i686-dos-musl`
各一行 `openkal-platform@0.29.0`）。v1 推荐新增 `[platforms] openkal-platform = "0.29.0"`。评审后改为不做：

- **这四行不只是选版本，也是选行。** openkal-platform 的 `targets` 还列有 `x86_64-linux-gnu`、
  `x86_64-linux-musl` 等宿主行（它的 hosted 部分）。“这个平台列出的每一行都由它提供”会把本机构建也交给平台，
  这不是四行显式写法的含义。要避免，就得在 `[platforms]` 中再列出行，于是又回到了逐行书写。
- **它是一个新的选择机制。** SPEC-013 §3.1 的六步顺序中要插入一步；解析时要先取得所列的每一个平台包，才能知道
  哪些行属于它。
- **没有功能缺口。** 四行对齐后是清楚的；升级时改四处，替换一次即可。

记录为独立的设计项：若以后做，前提是平台包的行在索引的元数据中可见，并且选择是按行集合而不是按整个平台。

### 6.3 索引数据的清理（不改引擎）

mcpp-index 中仍有一些包同时写着 `library_dirs`（旧拼写，只映射到运行时搜索）和 `link_library_dirs`。按 SPEC 的
现行规则，这些改为 `link_library_dirs` 加 `runtime_search_dirs`，在 mcpp-index 的 PR 中完成。

## 7. 运行器规范的收口

runner 已有规范：SPEC-013 §10 规定声明、查找、执行、占位符、状态、启用与合并；SPEC-002 §6 规定组合方式与契约。
本节只补两处，并列出各条款的落点。

### 7.1 文件通道（SPEC-002 §6.2）

- `MCPP_RUN_CLOSURE` 是唯一的规范通道。它的 JSON 在 `files`（运行时文件）之外增加 `run_files`：每项有目的地
  （相对于工作目录）和来源的绝对路径。`schema` 版本加一。
- `MCPP_RUNTIME_FILES`（TSV）改为从 `MCPP_RUN_CLOSURE` 派生的镜像，内容是两者的并集。在有自己文件系统的环境中，
  程序所在目录就是工作目录，所以并集对投递文件的 runner 是正确的；dirun 0.4.0 及以后的版本、Android runner
  不需要修改。
- 许可证文件不进入这两个通道。

### 7.2 工作目录（SPEC-002 §6，新增一条）

第 5.3 节的规则作为条款写入：runner 在工作目录中启动；`mcpp run` 与 `mcpp test` 各自的粒度；保留与删除。

### 7.3 条款的落点

| 内容 | 规范 |
|---|---|
| `deploy` 的条目语法、`license_files` | SPEC-004（清单语义） |
| 许可证的暂存位置与报告 | SPEC-014（打包）§4 |
| 运行文件、工作目录、`MCPP_RUN_CLOSURE` | SPEC-002 §6 |
| `[test] files`、`launch.cwd` 的备好时机 | SPEC-017 B1、B5 |
| runner 的键 | SPEC-013 §10：不变 |
| 插件协议 | SPEC-007：不变 |

## 8. 已决定的实现项

### 8.1 N43：openkal-uefi 0.11.1

- **实测**：在 OVMF（edk2 9.2.4）中，由 Shell 启动的映像，其 `LoadOptions` 保留完整的命令行
  （`APP.EFI "two   words" …`）。按 Shell 的规则切分（引号外的空白分隔，`"` 开关引用，`^` 使下一个字符按字面
  读取）后，`two   words`、`ab   cdefg`、`q"x`、`a b` 都完整到达。Shell 交出的 `Argv` 则会截短前两个。
- **规则**：由 Shell 启动时，以 `LoadOptions` 切分的结果作为参数；`LoadOptions` 不可用（不是文本，或为空）时，
  退回 Shell 的 `Argv`；不由 Shell 启动时，行为不变。这一规则对应 openkal SPEC 7.6：参数应当原样到达程序。
- **测试**：openkal-uefi 的 CI 中，`startup.nsh` 增加含连续空白的参数；openkal-platform 的 UEFI 检查同样增加。

### 8.2 N44：openkal-dos 0.1.1

- **大小**：stub 读取 `PT_GNU_STACK` 的 `p_memsz`（lld 的 `-z stack-size=N`）。这是由产物的生成方式决定的属性，
  在链接时确定（openkal SPEC 6.5）。
- **缺省值是数据**：1 MiB 写在 openkal-platform 的 DOS 行的链接参数中（`-z stack-size=1048576`），工程以自己的
  链接参数覆盖。stub 只在程序头没有声明时（不经平台构建的映像）才用内置的 1 MiB。缺省值因此在行的数据中可见，
  并且有显式的覆盖方式。
- **布局**：内存块改为 `[可加载段][保护页][栈]`。保护页通过 DPMI 1.0 的 `0507h`（设置页属性）设为未提交，栈向下
  越界时产生 `#PF`，经现有的故障报告输出。
  - 第一部分 13.13 第 3 条提出“把栈放到块的低端，越界离开描述符界限”。本节否定这一做法：程序的虚拟地址相对于
    块基址是固定的，平坦模型下 DS 与 SS 共用同一个描述符，栈放到低端就要移动全部段的地址。
- **退路**：DPMI 宿主不支持 `0507h` 时，在栈底写入哨兵值，程序结束时检查；被改写时以专用状态报告“栈越界”，并
  在启动时以一行诊断说明保护页不可用（做得少时不沉默）。
- **待实测**：CWSDPMI r7 与 DOSBox Staging 内置的 DPMI 对 `0507h` 的支持。
- **测试**：#3 的最小复现程序在 8 KiB 栈帧下递归 64 层后正常返回；声明 64 KiB 栈时，以 `#PF` 报告。

### 8.3 N45：dist-web 与平台的插件版本

- mcpp.plugins：dist-web 找不到它要包装的启动文件时，以警告写明查找过的路径（N30 的机制）。
- openkal-platform 0.29.1 改为以 mcpp.plugins 的最新版本构建，使选用平台行的工程可以使用新的 dist-web。

### 8.4 N47：dirun 0.6.0，可写的 FAT 映像

- 配置文件中新增 `[payload] disk`：
  - `"directory"`（缺省，即现状 `fat:{dir}` 只读）；
  - `"fat-image"`：dirun 生成一个 FAT32 映像，写入载荷、`--file` 与运行时文件，以可写的盘交给虚拟机。
- **不做读回**（修订 v1）：v1 让 dirun 在运行后把程序写下的文件读回工作目录。但工作目录在运行后即删除（5.3），
  没有读者；测试在虚拟机内自行检查结果，结论经标准输出与退出状态返回。去掉读回之后，dirun 只需要**生成**映像，
  不需要读 FAT。
- **实现（评审项）**：
  - **推荐**：dirun 自带 FAT32 映像的生成代码，含长文件名（`name` 不限于 8.3）；不依赖外部工具，Windows 与
    macOS 宿主上同样可用。只写不读，规模小。
  - 另一种：调用 mtools，需要为三个宿主平台各提供一个包。
- **复用**：生成代码写成 dirun 仓库中一个独立的模块。UEFI 程序以 ESP 映像分发是一种真实的打包格式，以后的打包
  格式提供者可以直接使用这个模块；本轮不新建包。
- ovmf 9.2.4-4：配置文件写明 `disk = "fat-image"`。r1–r3 不变。

### 8.5 新行：OpenBSD、NetBSD、illumos

**模板（读 `builtin.toml` 与外部工程的 CI 确认）**：FreeBSD 的行只有构建，没有 runner；运行验证在 CI 的虚拟机
动作中进行。新行按同一模板分两步，第一步不依赖磁盘映像。

| 步骤 | 内容 |
|---|---|
| 第一步：构建 | xim 的 sysroot 包（官方安装集中的头文件、crt 与库）；平台包声明行（三元组、clang 三元组、`family = "unix"`、加载器）、工具链 llvm 与 sysroot；工程的 CI 在对应系统的虚拟机动作中运行产物，与 FreeBSD 相同 |
| 第二步：`mcpp run` | 带 dirun 配置的磁盘映像包与 runner（`enabled = false`）。映像的体积、再分发条款与 GitCode 镜像的大小限制都还没有实测，单独立项 |

- **包的划分与名字（评审项）**：推荐每个系统一个平台包：`openbsd-platform`、`netbsd-platform`、
  `illumos-platform`。
  - 已有的平台包按系统命名（`openkal-platform`、`djgpp-platform`）；
  - 平台的版本跟随 sysroot 的系统版本（OpenBSD 7.x、NetBSD 10.x），三个系统共用一个版本号没有含义；
  - 一个系统的问题（见下方的待实测项）不阻塞另外两个的发布；
  - v1 推荐的 `unix-platform` 名不副实：Linux、macOS、FreeBSD 同属 unix，却不在其中。
  - 代价：三个仓库各有一次发布流程。三个包都只有数据，没有源码。
- **待实测**：lld 能否按 OpenBSD 的规则查找带版本号的共享库（`libc.so.N.M`）；NetBSD 下 lld 能否链接（否则用
  GNU ld）；illumos 的 libc 与 crt 的组成；三个系统的虚拟机动作是否可用。
- **约束**：OpenBSD 7.4 起只允许经 libc 发出系统调用（`pinsyscalls`），所以 OpenBSD 上的 okrun 以原生行构建；
  openkal 映像在 OpenBSD 上运行，以这一行为前提。

## 9. 归属

| 仓库 | 改动 | 版本 |
|---|---|---|
| mcpp | `deploy` 的条目语法（4.1）；`license_files` 与打包时的暂存、报告（4.3）；`--file` 与 `[test] files`（5.2）；工作目录（5.3）；`MCPP_RUN_CLOSURE` 的 `run_files` 与 TSV 镜像（7.1）；SPEC-002、SPEC-004、SPEC-014、SPEC-017 的条款；中英文文档；把多行内联表写进示例 | 下一版 |
| mcpp.plugins | dist-web 写明未提交的原因（8.3） | 补丁版本 |
| dirun | `[payload] disk = "fat-image"`（8.4） | 0.6.0 |
| openkal-uefi | 读取 `LoadOptions`（8.1） | 0.11.1 |
| openkal-dos | 栈的大小、保护页与退路（8.2） | 0.1.1 |
| `openbsd-platform`、`netbsd-platform`、`illumos-platform` | 各自的行、工具链与 sysroot（8.5 第一步） | 0.1.0 |
| xim-pkgindex | dirun 0.6.0；ovmf 9.2.4-4；三个系统的 sysroot | — |
| mcpp-index | openkal-uefi 0.11.1、openkal-dos 0.1.1、openkal-platform 0.29.1、三个新平台包、mcpp.plugins；`compat.onnxruntime` 改用 `license_files` 与 `library_dirs` 的清理（等新版 mcpp 发布后进行） | — |
| openkal-platform | 0.29.1：新成员；ovmf r4 与 dirun 0.6.0；DOS 行的缺省栈大小（8.2）；以新版 plugins 构建；UEFI 检查增加含连续空白的参数；runner 改为多行（在要求新 mcpp 的版本中） | 0.29.1 |
| 外部验证工程（fork） | 改用 `--file`，去掉对 dirun 的直接调用和对包目录的 `find`；DOS 与 UEFI 运行完整的脚本；三个新行 | — |

通用的能力在引擎与生态包中（文件的放置、许可证的收集、工作目录、FAT 映像、sysroot、行）；工程中只留下工程自己的
事实（哪些文件是它的测试数据、它选用哪些行）。

## 10. PR 划分与依赖

```
openkal-uefi ─┐
openkal-dos ──┼─> mcpp-index（成员）─> openkal-platform 0.29.1 ─> mcpp-index（平台）─┐
dirun ─> xim-pkgindex（dirun、ovmf r4）───────────────────────┘                  │
mcpp.plugins ─> mcpp-index（plugins）─> openkal-platform 0.29.1                    ├─> 外部验证工程
mcpp（F、I）─> 发布 ────────────────────────────────────────────────────────────┤
xim-pkgindex（sysroot）─> 三个新平台包 ─> mcpp-index（新平台包）─────────────────────┘
```

- 每个仓库一个 PR；xim-pkgindex 与 mcpp-index 是发布渠道，按前置的就绪时间各分两批（第一部分 §7 的规则）。
- 第一层的五个仓库（openkal-uefi、openkal-dos、dirun、mcpp.plugins、mcpp）互不依赖，可以并行；新行的 sysroot
  需要先实测（8.5），也可以与第一层并行。

## 11. 验证

| 项 | 方法 |
|---|---|
| F | e2e：通配符与 `{a,b}`、目录树保持结构、改名、碰撞规则、`deploy_files` 等价、不再匹配的文件被移除；字面路径的落点与旧版相同 |
| 许可证 | e2e：静态与共享的依赖各一个，三种打包方式下都出现在 `share/licenses/<包名>/`；未声明的包出现在报告中 |
| I | e2e：本机运行在工作目录中，且没有运行文件时目录不变；qemu-user 与 okrun 看到文件；dirun 收到文件（TSV 镜像）；`mcpp test` 失败时保留目录；`--no-run` 的 `launch.cwd` 指向备好的目录 |
| 7.1 | 单元测试：`MCPP_RUN_CLOSURE` 的 `run_files` 与 TSV 镜像一致 |
| 8.1 | openkal-uefi 与 openkal-platform 的 CI 在 OVMF 中检查含连续空白的参数 |
| 8.2 | openkal-dos 的 CI 中运行 #3 的复现程序，覆盖缺省栈、声明的栈与越界报告 |
| 8.4 | dirun 的单元测试覆盖 FAT 映像的生成（含长文件名）；在 OVMF 中写文件并在同一次运行中读出 |
| 8.5 | 新平台包的 CI：各构建一个 C 程序与一个 C++ 程序，并在虚拟机动作中运行 |
| 端到端 | 外部验证工程：DOS 与 UEFI 运行完整的脚本（含写文件的部分）；三个新行运行脚本；CI 中不出现包目录的路径 |

## 12. 多角度评审（v2）

本节记录对 v1 的评审。依据是实际读到的规范条款、源码与数据，不是推断。每一条给出发现与处理。

| # | 角度 | 发现 | 依据 | 处理 |
|---|---|---|---|---|
| R1 | 语义清晰 | `[runtime] deploy` 上的 `phase = "pack"` 与段的含义矛盾；`run` 与 `pack` 是包含关系，不是划分 | SPEC-004 §1（`[runtime]` 陈述产物）；`requirements.phase` 的取值是 `run`、`link` | 去掉 `phase`；发布文件改为 `license_files`（4.3） |
| R2 | 数据驱动 | 引擎内写死许可证的文件名，按目录猜测，找不到时不报告；静态依赖没有覆盖 | `src/pack/pack.cppm` `collect_licenses` | 事实写进包的数据；引擎只负责暂存位置；未声明的包进入报告（4.3） |
| R3 | 一致、复用 | v1 为 `deploy` 另设通配符规则，并把是否支持 `{a,b}` 列为待定 | `expand_glob`（`src/modgraph/scanner.cppm`）已支持分支，`sources` 的缺省值就用它 | 复用同一实现；“以 `/` 结尾表示目录”去掉；落点规则沿用 SPEC-017 B1 第 3 条（4.1） |
| R4 | 兼容 | v1 只断言旧引擎不会静默 | 读两处读取代码：`deploy` 中的字符串与未知键都报错；`[runtime]` 的未知键报告后忽略 | 方案 A 的依据由推断变为事实（4.2） |
| R5 | 插件架构 | v1 拟为 `mcpp::deploy` 增加 `name`、`phase` 参数 | 构建程序可以自己计算路径与生成文件 | 插件协议不变（4.1） |
| R6 | 一个概念一种拼写 | v1 把 `--file` 改为 `--input`、`inputs` | dirun 已发布 `--file HOST=GUEST`；`link.inputs` 的 `from` 是 argv（SPEC-013 §9.2） | 改回 `--file`、`files`；删除错误的换算规则（5.2） |
| R7 | 规范化 | v1 没有考虑已有的 `launch.cwd` 与外部启动者 | SPEC-017 B5 | 工作目录在写出 `launch` 时备好；写明粒度（5.3） |
| R8 | 性能 | 每个测试进程一个目录时，`split = "case"` 的复制次数不受控；目录形式的 `deploy` 每次构建都复制 | SPEC-017 O1 | 每个测试程序一个目录；写时复制的克隆；`deploy` 的展开进入构建图并清理（4.1、5.3） |
| R9 | 简洁 | dirun 读回文件没有读者 | 5.3：工作目录运行后删除 | 去掉读回；dirun 只生成映像（8.4） |
| R10 | 通用架构 | `[platforms]` 会把平台列出的宿主行一并接管；它是新的选择机制 | openkal-platform `mcpp.toml` 的 `targets`；SPEC-013 §3.1 | 本轮不做，单独立项（6.2） |
| R11 | 缺省值是数据 | v1 把 DOS 的缺省栈大小只放在 stub 中 | AGENTS.md §2 | 写进平台行的链接参数，stub 的内置值只作退路（8.2） |
| R12 | 生态一致 | v1 让新行同时带磁盘映像与 runner；`unix-platform` 名不副实 | FreeBSD 的行没有 runner（`builtin.toml`），运行在 CI 的虚拟机动作中；已有平台包按系统命名 | 分两步；每个系统一个平台包（8.5） |
| R13 | 用户体验 | 带 `--file` 的本机运行会改变工作目录 | 5.3 | 文档写明；`-v` 输出路径 |
| R14 | 跨平台 | FAT 的 8.3 文件名会限制 `name` | — | 映像生成含长文件名（8.4） |
| R15 | 边界 | `to` 拒绝 `..`，`../share` 的布局不能表达 | 4.1 的沿用规则 | 记录为已知边界，不在本轮（4.6） |

**评审后保持不变的部分：**

| 角度 | 检查 | 结论 |
|---|---|---|
| 架构 | 引擎是否出现系统名或工具名 | 没有：投递由 runner 完成；工作目录规则不区分 runner；FAT 的知识只在 dirun 中；许可证的文件名由包声明 |
| 规范 | 已发布的键不改名、键义稳定 | `deploy`、`deploy_files`、`[pack] include` 的含义都不变 |
| 规范 | 做得少时不沉默 | 通配符匹配为空报错；`name` 用于多个匹配报错；未声明许可证的包进入报告；DOS 的保护页不可用时给出诊断 |
| openkal | 7.6、7.13、6.5 | 8.1 恢复参数原样到达；工作目录经 okrun 授予；栈大小在链接时声明 |
| 兼容 | 升级后现有工程的行为 | 不变：字面路径的落点不变；没有运行文件时工作目录不变；旧的 ovmf 修订与旧平台不受影响 |
| 简洁 | 新增的键 | `deploy` 增加 `name` 一个字段与两种写法；`license_files`；`[test] files` 与 `--file`。runner、插件协议没有新增 |

## 13. 被否定的方案

| 方案 | 否定的原因 |
|---|---|
| `[runtime] files` 取代 `deploy` 与 `deploy_files`（4.2 方案 B） | 过渡期内同一概念有三种拼写；旧引擎忽略新键，文件不会放置 |
| `deploy` 条目的 `phase = "pack"`（v1 的 P1） | 与 `[runtime]` 的含义矛盾；落点由各包自定；不覆盖静态依赖（R1、R2） |
| 让依赖包通过 `[pack] include` 贡献发布文件（v1 的 P2） | 改变已发布键的含义 |
| 运行文件并入 `deploy` | 锚点不同（工作目录与程序所在目录），合并之后，本机运行时的落点无法说清 |
| `--input`、`inputs`（v1） | 与 dirun 的 `--file` 是同一概念的两种拼写；与 `link.inputs` 同名而语法不同（R6） |
| runner 新增 `files = "delivers" \| "host"` 之类的键 | 引擎对两种 runner 做同一件事即可（5.3），这个键不带来新信息 |
| 用插件包装 runner 来投递运行文件 | 运行文件属于运行期，插件在构建期执行；还需要复制平台 runner 的命令行 |
| 为 `mcpp::deploy` 增加参数（v1） | 构建程序可以自己计算（R5） |
| dirun 读回程序写下的文件（v1） | 没有读者（R9） |
| `[platforms]`（v1 的 V1） | 会接管平台列出的宿主行；是新的选择机制；没有功能缺口（R10） |
| 把栈放在块的低端，越界离开描述符界限（第一部分 13.13 的第 3 条） | 平坦模型下需要移动全部段的地址 |
| ovmf 直接改用 QEMU 的可写 vvfat（`fat:rw:`） | QEMU 文档说明它不可靠，写入可能损坏宿主目录 |
| 一个 `unix-platform` 包含三个系统（v1） | 名不副实；版本无法跟随各自的系统版本（R12） |

## 14. 待评审的决定点

1. 四种角色与各自的归属（3.1、4.3 末尾的表）。
2. `deploy` 的条目语法：字符串形式、通配符、`name`，落点规则；键名用方案 A（4.1、4.2）。
3. 发布文件用 `[package] license_files`，引擎统一放到 `share/licenses/<包名>/`，覆盖静态依赖（4.3）。
4. 运行文件的名字：`--file` 与 `[test] files`（推荐），或 `--input` 与 `inputs`（5.2）；工作目录的规则与粒度（5.3）。
5. `[platforms]` 本轮不做（6.2）。
6. dirun 自带 FAT32 映像的生成，不做读回（8.4）。
7. 新行分两步；每个系统一个平台包（8.5）。
8. DOS 的缺省栈大小写在平台行的数据中（8.2）。
