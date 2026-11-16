---
subject: cross-platform
status: draft
---

# 生态缺口补充方案（第二部分）：文件的角色、运行输入、清单书写与第一部分的遗留项

**日期：** 2026-11-16 · **版本：** v1（评审稿） · **基线：** mcpp 2026.11.16.1（main 另含 #929：多行内联表）、
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
- `deploy` 的条目语法与 `phase`（第 4 节）；
- 运行输入的书写与契约（第 5 节）；
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
| **运行输入** | 程序本身，相对于工作目录 | 某一次运行 | 不放 | 只在这次运行 | 否 | 测试脚本、测试数据、一次性的配置 |

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
| 无 | 运行输入 | 外部工程只能绕过 mcpp 直接调用 dirun（第一部分 N46） |

实例：mcpp-index 的 `compat.onnxruntime` 用 `[runtime] deploy` 放置 `LICENSE` 与 `ThirdPartyNotices.txt`。
- 这两份文件是**发布文件**，被当作运行时文件处理，因而在每次构建中都被复制到 `bin/licenses/`。
- 三个系统各写两条，`to` 都相同，一共六条。

### 3.3 结论

1. 运行时文件只需要一种拼写，并补上通配符、整目录和改名。
2. 依赖包需要有办法声明发布文件：只在打包时暂存，不进入构建目录，也不投递给 runner。
3. 运行输入是一个独立的概念。它的锚点是**工作目录**，而运行时文件的锚点是**程序所在目录**；锚点不同，所以
   不能与 `deploy` 合并（第 5 节）。

## 4. 方案 F：运行时文件与发布文件

### 4.1 条目语法

列表中的每一项是一个字符串，或者一张表。字符串是表的简写：

```toml
deploy = [
    "share/data.bin",                                         # = { from = "share/data.bin" }：放在程序旁，名字不变
    { from = "share/vulkan/icd.d/", to = "vulkan/icd.d" },    # 整个目录
    { from = "res/*.json", to = "res" },                      # 通配符
    { from = "tools/x.conf", name = "app.conf" },             # 改名：from 必须恰好匹配一个文件
    { from = "lib-*/LICENSE", to = "licenses/lib", phase = "pack" },   # 发布文件
]
```

| 字段 | 语义 |
|---|---|
| `from` | 相对于声明它的包的根，用 `/` 分隔。可以含 `*`、`**`、`?`；以 `/` 结尾时表示整个目录，保留相对路径。匹配为空时报错，并指出该条目的序号 |
| `to` | 目的地目录，相对于程序所在目录，缺省为 `"."`。语义与现有的 `deploy.to` 相同 |
| `name` | 目的地的文件名。只在 `from` 恰好匹配一个文件时可用，否则报错 |
| `phase` | `"run"`（缺省）：运行时文件，进入构建目录，投递给 runner，打包时暂存。`"pack"`：发布文件，只在打包时暂存，放在 `to` 下，不进入构建目录，也不投递 |

**沿用的规则：**
- 绝对路径、盘符、空段、`.`、`..` 一律拒绝；
- 同一目的地有多个来源时，内容相同则合并为一份，不同则拒绝（mcpp#723）；
- 构建程序的 `mcpp::deploy(from, to)` 等价于一条表项。协议增加可选的第三个参数 `name` 与 `phase`。

`phase` 沿用 `[runtime] requirements` 中已有的键名，含义相同，都表示“在哪个阶段生效”；只是取值增加了 `pack`。

### 4.2 键名（评审项）

| 方案 | 内容 | 理由 | 代价 |
|---|---|---|---|
| **A（推荐）扩展 `deploy`** | `deploy` 是这个概念的唯一拼写，接受第 4.1 节的全部写法；`deploy_files` 作为它的字符串形式继续有效，文档写成一条规则（“`deploy_files = [x]` 即 `deploy = [x]`”），新文档只展示 `deploy` | 符合“一个概念一种拼写、已发布的键不改名”；不引入第三个名字；`deploy` 在业界（windeployqt、macdeployqt）恰好就是“把运行所需的文件放在程序旁” | “deploy”字面上不涵盖 `phase = "pack"` 的发布文件；需要文档说明 |
| B 新键 `files` | `[runtime] files` 取代两者，旧键作为别名 | 名字更中性，`phase = "pack"` 读起来也通顺 | 偏离已发布键不改名的规则；过渡期内同一概念有三种拼写；旧引擎会**忽略**新键，文件不放置也不报错，这种静默失效比方案 A 的风险更大 |

推荐 A。这一节需要偏离规则才能成立的论据只有“名字更中性”这一条，不足以抵消三种拼写和静默失效的代价。

### 4.3 发布文件放在 `phase`，还是 `[pack]`（评审项）

| 方案 | 内容 | 判断 |
|---|---|---|
| **P1（推荐）`deploy` 条目带 `phase = "pack"`** | 依赖包和工程都能写；暂存位置相对于程序目录，与运行时文件同一套规则 | 一种语法覆盖两种角色；依赖包的发布文件随它的二进制一起进入消费者的包 |
| P2 扩展 `[pack] include`，让依赖包也能贡献 | `[pack]` 现在只作用于本工程的包；改成“为消费者贡献”会改变已发布键的含义 | 违反键义稳定 |

`[pack] include` 保持现有含义：本工程的文件，按相同的相对路径暂存，只放一份。它与 P1 互补：
- `[pack] include` 的锚点是**包的根**（例如 `README`、`share/**`）；
- `phase = "pack"` 的锚点是**程序所在目录**（例如程序旁的 `licenses/`）。

### 4.4 改写示例

`compat.onnxruntime`（xim 的 Lua 数据；多行内联表要等 mcpp 支持 #929 的版本才可用）：

```lua
-- 现状：三个系统，每个两条，共六条
deploy = {
    { from = "onnxruntime-linux-x64-1.25.1/LICENSE", to = "licenses/onnxruntime" },
    { from = "onnxruntime-linux-x64-1.25.1/ThirdPartyNotices.txt", to = "licenses/onnxruntime" },
},
-- 改写：一条，写在三个系统共用的块中
deploy = { { from = "onnxruntime-*/{LICENSE,ThirdPartyNotices.txt}", to = "licenses/onnxruntime", phase = "pack" } },
```

通配符是否支持 `{a,b}` 形式需要决定（评审项）。不支持时写成两条，仍然比现在少四条。

### 4.5 兼容

- 现有清单的行为不变：
  - `to` 的含义不变；
  - 字符串形式与 `deploy_files` 相同；
  - 不写 `phase` 时为 `run`，与现在相同。
- 发布到索引的包只有声明了 `mcpp = ">=<本版本>"` 时，才能使用通配符、`name`、`phase`。原因是旧引擎会把一个含
  `*` 的 `from` 当作字面路径，然后报“文件不存在”，并不会静默。

## 5. 方案 I：运行输入

### 5.1 语义

运行输入是“这一次运行开始时，工作目录中应当有的文件”。它与运行时文件的区别只在锚点：

| | 锚点 | 生命周期 |
|---|---|---|
| 运行时文件（`deploy`） | 程序所在目录 | 与产物相同 |
| 运行输入（`inputs`） | 本次运行的工作目录 | 与本次运行相同 |

### 5.2 书写

```sh
mcpp run app --target x86_64-uefi --input tests/t.bat=TEST.BAT -- /c TEST.BAT
```

```toml
[test]
inputs = ["tests/data/", { from = "tests/t.bat", name = "TEST.BAT" }]

[target.'cfg(os = "uefi")'.test]
inputs = ["tests/uefi/"]
```

- 条目语法与第 4.1 节相同，但没有 `phase`。`to` 和 `name` 相对于工作目录。
- 命令行写作 `FROM[=TO]`，`TO` 以 `/` 结尾时表示目录，否则表示文件名。
- 名字取 `inputs`，沿用平台 `link.inputs` 中“交给一个步骤的文件”的含义（一个概念一种拼写）。

### 5.3 工作目录

| 情形 | 工作目录 |
|---|---|
| 没有运行输入 | 不变：本机运行时为调用时的当前目录（无感升级） |
| 有运行输入，宿主的文件系统对程序可见（本机运行、qemu-user、wine、okrun） | `target/<triple>/<fp>/run/<run-id>/`；运行前复制输入，运行后删除。`mcpp test` 在测试失败时保留，并在报告中给出路径 |
| 有运行输入，程序有自己的文件系统（虚拟机、固件、设备） | runner 把输入与运行时文件一起投递到程序旁边；在这些环境中，程序所在目录就是它的工作目录 |

引擎对两种情形**做同一件事**：准备好临时工作目录，再把输入写进运行器契约（7.1）。投递文件的 runner 照常投递，
共享宿主文件系统的 runner 从工作目录读取。因此不需要为 runner 新增“能否投递”的键（被否定的方案见第 13 节）。

openkal 的一致性：okrun 把启动目录作为第一个授予的目录（SPEC 7.13）。所以一个 ok 映像在临时工作目录中看到的输入，
与它在原生程序下看到的相同。

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

### 6.2 同一平台的版本重复（评审项）

外部工程为四行写了同一个平台的同一个版本：

```toml
[target]
i686-linux-musl.platform = "openkal-platform@0.29.0"
i686-openkal.platform    = "openkal-platform@0.29.0"
x86_64-uefi.platform     = "openkal-platform@0.29.0"
i686-dos-musl.platform   = "openkal-platform@0.29.0"
```

这四行的实际意图是：“openkal-platform 用 0.29.0，而不是 mcpp 内置的 0.28.0”。

| 方案 | 内容 | 判断 |
|---|---|---|
| **V1（推荐）平台系列的版本** | `[platforms]` 表，`openkal-platform = "0.29.0"`：目标表或工程选中这个平台包的每一行，都用这个版本。四行变为一行；该平台以后新增的行自动沿用 | 新表需要一个新名字；它的含义是“一个系列的版本选择”（SPEC-013 §14），与 `[target] platform` 的“为某行选平台”正交，不算同一概念的第二种拼写 |
| V2 `cfg` 选择器 | `[target.'cfg(any(os = "uefi", os = "dos", arch = "x86"))'] platform = ...` | 选择器表达的是“哪些行”，而这里要表达的是“哪个版本”；选错行时也难以发现 |
| V3 保持现状 | — | 冗长，平台升级时要改多处 |

### 6.3 索引数据的清理（不改引擎）

mcpp-index 中仍有一些包同时写着 `library_dirs`（旧拼写，只映射到运行时搜索）和 `link_library_dirs`。按 SPEC 的
现行规则，这些改为 `link_library_dirs` 加 `runtime_search_dirs`，在 mcpp-index 的 PR 中完成。

## 7. 运行器规范的收口

runner 已有规范：SPEC-013 §10 规定声明、查找、执行、占位符、状态、启用与合并；SPEC-002 §6 规定组合方式与契约。
本节只补三处。

### 7.1 文件通道（SPEC-002 §6.2）

- `MCPP_RUN_CLOSURE` 是唯一的规范通道。它的 JSON 在 `files`（运行时文件）之外增加 `inputs`：每项有目的地
  （相对于程序所在目录；在有自己文件系统的环境中，这也就是工作目录）和来源的绝对路径。`schema` 版本加一。
- `MCPP_RUNTIME_FILES`（TSV）改为从 `MCPP_RUN_CLOSURE` 派生的镜像，内容是 `files` 与 `inputs` 的并集。dirun 0.4.0
  及以后的版本、Android runner 不需要修改。
- 规范写明：`phase = "pack"` 的文件不进入这两个通道。

### 7.2 工作目录（SPEC-002 §6，新增一条）

第 5.3 节的规则作为条款写入，并给出 `mcpp run` 与 `mcpp test` 各自的行为。

### 7.3 不改的部分

SPEC-013 §10 的键集合不变。runner 不新增键（见第 13 节）。

## 8. 已决定的实现项

### 8.1 N43：openkal-uefi 0.11.1

- **实测**：在 OVMF（edk2 9.2.4）中，由 Shell 启动的映像，其 `LoadOptions` 保留完整的命令行
  （`APP.EFI "two   words" …`）。按 Shell 的规则切分（引号外的空白分隔，`"` 开关引用，`^` 使下一个字符按字面
  读取）后，`two   words`、`ab   cdefg`、`q"x`、`a b` 都完整到达。Shell 交出的 `Argv` 则会截短前两个。
- **规则**：由 Shell 启动时，以 `LoadOptions` 切分的结果作为参数；`LoadOptions` 不可用（不是文本，或为空）时，
  退回 Shell 的 `Argv`；不由 Shell 启动时，行为不变。这一规则对应 openkal SPEC 7.6：参数应当原样到达程序。
- **测试**：openkal-uefi 的 CI 中，`startup.nsh` 增加含连续空白的参数；openkal-platform 的 UEFI 检查同样增加。

### 8.2 N44：openkal-dos 0.1.1

- **大小**：stub 读取 `PT_GNU_STACK` 的 `p_memsz`（lld 的 `-z stack-size=N`）；为 0 或缺失时取 1 MiB。
  这是由产物的生成方式决定的属性，在链接时确定（openkal SPEC 6.5）。
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
  - `"fat-image"`：dirun 生成一个 FAT32 映像，写入载荷、`--file` 与运行时文件；运行结束后，把程序写下的文件读回到
    本次运行的工作目录。
- 这与第 5 节一致：工作目录被镜像进虚拟机的盘，再镜像回来。
- **实现方式（评审项）**：
  - **推荐**：dirun 自带最小的 FAT32 读写代码，不依赖外部工具；Windows 与 macOS 宿主上同样可用。
  - 另一种：调用 mtools，需要为三个宿主平台各提供一个包。
- ovmf 9.2.4-4：配置文件写明 `disk = "fat-image"`。r1–r3 不变。

### 8.5 新行：OpenBSD、NetBSD、illumos

| 部分 | 内容 |
|---|---|
| xim 包 | `x86_64-openbsd-sysroot`（官方安装集 base 与 comp 中的头文件、crt 与库）、`openbsd-disk-image`（带 dirun 配置）；NetBSD 与 illumos（OmniOS）各一组 |
| 平台包 | 一个新仓库，三行：三元组、clang 三元组、执行系统、`family = "unix"`；工具链 llvm；runner 为 `dirun --profile {pkg:<image>}/dirun.toml`，`enabled = false`。名字与仓库归属是评审项：推荐 `mcpplibs/unix-platform` |
| 待实测 | lld 能否按 OpenBSD 的规则查找带版本号的共享库（`libc.so.N.M`）；NetBSD 下 lld 能否链接（否则用 GNU ld）；illumos 的 libc 与 crt 的组成 |
| 约束 | OpenBSD 7.4 起只允许经 libc 发出系统调用（`pinsyscalls`），所以 OpenBSD 上的 okrun 以原生行构建；openkal 映像在 OpenBSD 上运行，以这一行为前提 |

## 9. 归属

| 仓库 | 改动 | 版本 |
|---|---|---|
| mcpp | `deploy` 的条目语法与 `phase`（4.1–4.3）；`inputs` 与 `--input`（5.2）；工作目录（5.3）；`MCPP_RUN_CLOSURE` 的 `inputs` 与 TSV 镜像（7.1）；`[platforms]`（6.2，如采纳）；SPEC-002、SPEC-013、SPEC-017 的条款；中英文文档；把多行内联表写进示例 | 下一版 |
| mcpp.plugins | dist-web 写明未提交的原因（8.3） | 补丁版本 |
| dirun | `[payload] disk = "fat-image"` 与读回（8.4） | 0.6.0 |
| openkal-uefi | 读取 `LoadOptions`（8.1） | 0.11.1 |
| openkal-dos | 栈的大小、保护页与退路（8.2） | 0.1.1 |
| 新平台包 | OpenBSD、NetBSD、illumos 三行（8.5） | 0.1.0 |
| xim-pkgindex | dirun 0.6.0；ovmf 9.2.4-4；三个系统的 sysroot 与磁盘映像 | — |
| mcpp-index | openkal-uefi 0.11.1、openkal-dos 0.1.1、openkal-platform 0.29.1、新平台包、mcpp.plugins；`compat.onnxruntime` 的改写与 `library_dirs` 的清理（等新版 mcpp 发布后进行） | — |
| openkal-platform | 0.29.1：新成员；ovmf r4 与 dirun 0.6.0；以新版 plugins 构建；UEFI 检查增加含连续空白的参数；runner 改为多行（在要求新 mcpp 的版本中） | 0.29.1 |
| 外部验证工程（fork） | 改用 `--input`，去掉对 dirun 的直接调用和对包目录的 `find`；DOS 与 UEFI 运行完整的脚本；三个新行；`[platforms]`（如采纳） | — |

## 10. PR 划分与依赖

```
openkal-uefi ─┐
openkal-dos ──┼─> mcpp-index（成员）─> openkal-platform 0.29.1 ─> mcpp-index（平台）─┐
dirun ─> xim-pkgindex（dirun、ovmf r4）───────────────────────┘                  │
mcpp.plugins ─> mcpp-index（plugins）─> openkal-platform 0.29.1                    ├─> 外部验证工程
mcpp（F、I、V1）─> 发布 ───────────────────────────────────────────────────────┤
xim-pkgindex（sysroot、映像）─> 新平台包 ─> mcpp-index（新平台包）──────────────────┘
```

- 每个仓库一个 PR；xim-pkgindex 与 mcpp-index 是发布渠道，按前置的就绪时间各分两批（第一部分 §7 的规则）。
- 第一层的五个仓库（openkal-uefi、openkal-dos、dirun、mcpp.plugins、mcpp）互不依赖，可以并行；新行的 sysroot
  与磁盘映像需要先实测（8.5），也可以与第一层并行。

## 11. 验证

| 项 | 方法 |
|---|---|
| F | e2e：通配符、目录、改名、`phase = "pack"` 不进入构建目录但进入包、碰撞规则、`deploy_files` 等价；`mcpp::deploy` 的新参数 |
| I | e2e：本机运行在临时工作目录中，且不运行时目录不变；qemu-user 与 okrun 看到输入；dirun 收到输入（TSV 镜像）；`mcpp test` 失败时保留目录 |
| 7.1 | 单元测试：`MCPP_RUN_CLOSURE` 的 `inputs` 与 TSV 镜像一致 |
| 8.1 | openkal-uefi 与 openkal-platform 的 CI 在 OVMF 中检查含连续空白的参数 |
| 8.2 | openkal-dos 的 CI 中运行 #3 的复现程序，覆盖缺省栈、声明的栈与越界报告 |
| 8.4 | dirun 的单元测试覆盖 FAT 的读写；在 OVMF 中写文件并读回 |
| 8.5 | 新平台包的 CI：三个系统各构建一个 C 程序与一个 C++ 程序，并在虚拟机中运行 |
| 端到端 | 外部验证工程：DOS 与 UEFI 运行完整的脚本（含写文件的部分）；三个新行运行脚本；CI 中不出现包目录的路径 |

## 12. 自我评审

| 角度 | 检查 | 结论 |
|---|---|---|
| 架构 | 引擎是否出现系统名或工具名 | 没有：投递由 runner 完成，工作目录规则不区分 runner；FAT 的知识只在 dirun 中 |
| 规范 | 一个概念一种拼写 | `deploy` 保持唯一（4.2 方案 A）；`inputs` 与 `phase` 沿用已有的名字；`[platforms]` 是新概念，单列待评审 |
| 规范 | `inputs` 与平台 `link.inputs` 的条目字段 | 名字相同、含义相同（交给一个步骤的文件）；字段不同：`link.inputs` 只需 `file`，运行输入需要 `from`、`to`、`name`，沿用 `deploy` 的条目。写成一条规则：`link.inputs` 的 `{ file = x }` 即 `{ from = x }` |
| 规范 | 已发布的键不改名、键义稳定 | `deploy_files` 与 `[pack] include` 的含义不变（4.2、4.3） |
| 规范 | 做得少时不沉默 | 通配符匹配为空时报错；`name` 用于多个匹配时报错；DOS 的保护页不可用时给出诊断 |
| openkal | 7.6 参数原样到达 | 8.1 恢复了这一点 |
| openkal | 7.13 启动目录是第一个授予的目录 | 5.3 的工作目录经 okrun 授予，映像与原生程序看到的相同 |
| openkal | 6.5 由生成方式决定的属性在链接时确定 | 栈大小由 `PT_GNU_STACK` 声明，不在运行时询问 |
| 自我修正 | 第一部分 13.13 的“栈放在低端” | 平坦模型下不可行，改为保护页（8.2） |
| 自我修正 | 第一部分 13.15 的候选 `--file` | 改名为 `--input`，与 `inputs` 一致，并与运行时文件区分锚点 |
| 兼容 | 升级后现有工程的行为 | 不变：缺省 `phase` 为 `run`；不写输入时工作目录不变；旧的 ovmf 修订与旧平台不受影响 |
| 跨平台 | Windows 宿主 | 临时工作目录与复制在三个宿主上相同；FAT 映像由 dirun 自行生成，不依赖 mtools |
| 简洁 | 新增的键 | `deploy` 增加 `name`、`phase` 两个字段；`[test] inputs` 与 `--input`；`[platforms]`（待评审）。runner 没有新增键 |
| 测试 | 每项都有自动化验证 | 见第 11 节；8.2 的 `0507h` 与 8.5 的 lld 先实测，再写成测试 |

## 13. 被否定的方案

| 方案 | 否定的原因 |
|---|---|
| `[runtime] files` 取代 `deploy` 与 `deploy_files`（4.2 方案 B） | 过渡期内同一概念有三种拼写；旧引擎忽略新键，会静默失效 |
| 让依赖包通过 `[pack] include` 贡献发布文件（4.3 方案 P2） | 改变已发布键的含义 |
| 运行输入并入 `deploy`（加上 `phase = "run-input"`） | 锚点不同（工作目录与程序所在目录），合并之后，本机运行时的落点无法说清 |
| runner 新增 `files = "delivers" \| "host"` 之类的键 | 引擎对两种 runner 做同一件事即可（5.3），这个键不带来新信息 |
| 用插件包装 runner 来投递输入 | 输入属于运行期，插件在构建期执行；还需要复制平台 runner 的命令行 |
| 把栈放在块的低端，越界离开描述符界限（第一部分 13.13 的第 3 条） | 平坦模型下需要移动全部段的地址 |
| ovmf 直接改用 QEMU 的可写 vvfat（`fat:rw:`） | QEMU 文档说明它不可靠，写入可能损坏宿主目录 |
| 以 `cfg` 选择器为多行指定同一平台（6.2 方案 V2） | 选择器表达的是“哪些行”，不是“哪个版本” |

## 14. 待评审的决定点

1. 文件的四种角色（3.1），以及“运行时文件”与“发布文件”的判别方法。
2. 键名用方案 A（扩展 `deploy`）还是方案 B（新键 `files`）（4.2）。
3. 发布文件用 `phase = "pack"`（P1）表达（4.3），以及通配符是否支持 `{a,b}`（4.4）。
4. 运行输入的名字 `inputs` 与 `--input`，以及工作目录的三条规则（5.2、5.3）。
5. `[platforms]` 平台系列版本（6.2 方案 V1）及其名字。
6. dirun 的 FAT 读写自带实现，还是调用 mtools（8.4）。
7. 新平台包的名字与仓库（8.5，推荐 `mcpplibs/unix-platform`）。
