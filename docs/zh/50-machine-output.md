# 50 —— 机器可读输出

**读者：** 解析 mcpp 输出的工具、编辑器扩展或 CI 任务的作者。

**本章回答的那一个问题：** 一个程序可以依赖什么，输出如何被版本化，以及协议
如何被识别。

**不在这里：** 面向人的输出，它不携带任何兼容性承诺；以及退出码契约，那是
[SPEC-003](../specs/exit-codes.md)。

mcpp 面向两类读者写作。本章是对第二类读者 —— 程序 —— 的契约。编辑器扩展、
CI 脚本，以及任何解析 mcpp 输出的程序，都可以依赖本章陈述的内容。

## 1. 首要规则

> **靠解析 stdout 来识别协议，不靠退出码，也不靠「命令没有失败」这一点。**

读取 stdout，尝试按 JSON 解析，并要求 `schemaVersion` 与 `kind` 二者都在。
只要缺一个，就说明这个 mcpp 不支持调用方所请求的协议。

这不是风格上的偏好。`mcpp --protocol-version` 看起来理应是入口，在支持它的
版本上确实是个有用的捷径。但在**它存在之前发布的每一个 mcpp** 上，这条命令
本身就是一个未知选项 —— 而未知选项过去的行为是把人类可读的文本打印到
**stdout**、退出码为 1、stderr 为空。成功与失败走的是同一个通道。把它拼成
`--json` 也不会改变什么，两者最终走的是同一条路径。

所以「正向识别」是唯一在各版本上都成立的规则，下面的一切设计都围绕它展开。

## 2. 信封

```jsonc
{
  "schemaVersion": 1,          // the ENVELOPE's version
  "kind": "mcpp.env",          // which document this is
  "kindVersion": 1,            // this kind's own data version
  "effects": [],               // what running the command did — see §4
  "mcpp": {
    "version": "2026.8.8.3",
    "protocol": { "min": 1, "max": 1 }
  },
  "data": { /* specific to `kind` */ },
  "diagnostics": []
}
```

`schemaVersion` 与 `kindVersion` 被特意分开：如果只有一个全局版本号，给
`mcpp.env` 增加一个字段就会推高客户端为 `mcpp.xpkg` 读到的版本号，而客户端
无从判断真正变化的是哪一个。

`effects` 永远存在。空数组表示「什么都没做」；字段缺失会表示「未知」，那是
另一个断言。

命令产出了它自己的文档时，`data` 才存在。命令在拿到文档之前就失败时，省略
`data` 而不是给出一个空对象，原因写在 `diagnostics` 里；`mcpp.build-database`
就是这样的 kind（见 §8）。

### 诊断

```jsonc
{
  "code": "MCPP_MANIFEST_UNKNOWN_KEY",
  "severity": "error" | "warning" | "note",
  "source": "mcpp",
  "message": "unknown key 'standrad'",
  "path": "mcpp.toml",                       // omitted when there is none
  "range": { "start": {"line": 3, "column": 1},
             "end":   {"line": 3, "column": 9} }   // omitted when there is none
}
```

位置从 1 开始计数。`column` 数的是 **UTF-8 字节**，因此索引的正是 mcpp 实际
读到的那份文件。

一条没有位置的诊断会省略 `path` 与 `range`，而不是填 0 —— `line: 0` 会把
读者指向一个不存在的位置。

`code` 永远存在。解析 `code`，永远不要解析 `message`。

## 3. 请求机器输出

```
mcpp <command> --format json
```

目前只支持 `json`。`ndjson` 为将来真正需要流式的场景保留，**现在不被接受**
—— 请求它是一个错误，而不是静默地回落到别的格式。

`--format` 已经用来表示某种**产物**的命令，改用 `--message-format json` 来
请求机器输出，`mcpp test` 就是这样做的。`mcpp pack --format` 表示的是包格式
（`tar`、`dir`、`msi`），所以它的报告方式是
`mcpp pack --message-format json`（2026.9.16.1+）。具体形状随 kind 而定：
`mcpp test` 的测试是随时间陆续完成的，所以它按测试逐条输出流式记录；一次
`mcpp pack` 只有一个结果，所以它输出一个信封。

### 不支持的值与未知选项

两者都写往 **stderr**，退出码为 **2**，并且**不向 stdout 写任何东西**：

```
$ mcpp self env --format yaml
error: unsupported --format 'yaml'; expected: json      # stderr
$ echo $?
2
```

一个还不知道自己会得到什么格式的请求，不应该往协议独占的那个通道里写东西。
结合 §1，客户端的规则就是完整的了：**stdout 上没有 JSON，就意味着不支持**，
无论原因是什么。

**带信封命令**的退出码 —— 即 `--protocol-version` 所声明的那几个 kind。这张
表刻意只覆盖它们：别的命令返回的退出码不在其中，把它写进来等于记录一件这些
命令给不出的事实。跨越 mcpp 全部命令的完整映射见
[退出码契约](../specs/exit-codes.md)。

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 命令运行了并且失败 —— 见 stderr；当 stdout 携带信封时见 `diagnostics` |
| 2 | 用法错误 —— 未知选项、不支持的取值 |
| 70 | 内部错误（未捕获的异常） |
| 127 | 未知命令 |

**`1` 可以与 stdout 上的信封同时出现。** `mcpp xpkg parse` 会把一份违反命名
形态的描述符以 JSON 形式报出**并且**退出 1：这份文档就是答案，退出码说明这个
答案是一次拒绝。§1 依然成立 —— 解析 stdout，不要按退出码分支 —— 但一个把任何
非零退出都当成「没有输出」的客户端，会丢掉它已经拿到手的文档。

## 4. effects —— 命令在打印结果之前执行的动作

一个带有 untrusted-workspace 门禁的 IDE，必须在**运行之前**就做出决定。等
信封送达时，它所描述的事情已经发生了。所以同一份信息也以静态方式提供：

```
mcpp --protocol-version
```

```jsonc
{
  "schemaVersion": 1,
  "kind": "mcpp.protocol",
  "envelope": { "min": 1, "max": 1 },
  "kinds":    { "mcpp.env": 1, "mcpp.xpkg": 1, "mcpp.cache": 1 },
  "commands": {
    "self env":   { "effects": ["init-mcpp-home"] },
    "xpkg parse": { "effects": [] },
    "cache list": { "effects": [] }
  }
}
```

effects 采用具名的方式而不是一个 `destructive: true|false`，因为布尔值分不
清「无害的动作」与「门禁真正要拦的那件事」：

| effect | 含义 |
|---|---|
| `init-mcpp-home` | 首次使用时可能创建 `$MCPP_HOME`。**位于项目目录之外。** |
| `read-project` | 读取 manifest 与源码 |
| `write-project` | 写入项目树（`target/`、compile DB） |
| `write-global-cache` | 写入共享构建缓存 |
| `network` | 可能发起网络请求 |
| `exec-build-script` | **执行工作区里的代码**（`build.mcpp`） |

多数门禁只关心 `exec-build-script` 与 `write-project`，可以忽略
`init-mcpp-home` —— mcpp 给自己做初始化，不算工作区在动作。

上表列出的是命令**可能**做的事。信封里的 `effects` 是这次运行**实际做了**的
事，其中 `network` 按实际观测记录，而不是按声明：只要本次运行启动过索引
刷新、安装，或者一次 git 远程操作，就会被列出，包括失败了的或被期限终止的
那一次；在 `--offline` 下运行时永远不会列出。

## 5. `--json` 不等于 `--format json`

有两条命令在本协议存在之前就已经发布了 `--json`：

```
mcpp xpkg parse <file> --json     ->  {"namespace": …, "name": …, …}
mcpp cache list --json            ->  {"root": …, "entries": [ … ]}
```

那些 payload 是**裸的**——没有信封——而且已经有消费方在读取它们。所以：

> **`--json` 永久保留它的 payload。带信封的是 `--format json`。**

`--json` 没有被弃用，使用它也不会打印任何警告：客户端正在解析这份输出，一条
警告会落在它中间。

两种拼法出自同一个数据来源，因此永远描述同一件事：一个答案，两种形状。

## 6. 退出状态

`mcpp run` 报告的是程序自身的退出状态。整个取值空间分三段，只有第一段属于
程序：

| 区间 | 含义 |
|---|---|
| `0`–`124` | 程序运行过了；这是它自己的状态，原样透传 |
| `125`–`127` | 尝试过启动但被拒绝 —— `127` 是找不到，`126` 是找到了但不可执行，`125` 是其他原因；对 `mcpp run --format <f>` 而言，一个是目录、且没有任何 runner 能到达的分发物同样得到 `126`，它在启动之前就被拒绝，含义相同（2026.9.14.2+） |
| `2` | mcpp 在尝试启动任何东西之前就拒绝了：用法、配置或解析错误 |

在 2026.9.4.3 之前，所有非零状态都被折叠成 `1`，目的是让 `2` 能表示「起不来」
以区别于「跑了但失败」。这个区分值得保留，但代价不值得：`main` 返回 `3` 的
程序会让 `mcpp run` 退出 `1`，qemu 报出 `3` 的裸机镜像同样到达为 `1` ——
本项目让人使用的这条命令因此无法用于分支判断。

中间那一段，是 `env`、`timeout`、`nice` 早已在用、并且被 shell 文档化的取
值，因此 `126` 与 `127` 到达时带着它们惯常的含义，而不是本项目自行分配的
编号。

**程序自身也可以以 `125`–`127` 退出，mcpp 不试图靠数字去区分两者。** 区分它
们的是：启动失败一定会向 stderr 写出原因，而程序自身的退出状态从不这样做。
需要确定结果的客户端应当读 stderr，或者使用 `--format json` —— 那里退出
状态是一个字段，不是一条通道。

`mcpp test` 保持不变，仍然是 `0` 或 `1`：它聚合了多个程序的结果，没有单一的
状态可以透传。每个测试各自的退出码在 JSON 流的 `exit_code` 字段里（见
§8）。

## 7. 稳定性承诺

对每一个 `kind`，在同一个 `kindVersion` 之内：

- 字段只会被**新增**，从不删除
- 一个字段的含义永远不变
- 破坏性变更会抬升版本号；在需要过渡窗口时，`protocol.min`/`max` 会重叠，
  使两个版本都可读

这个承诺只有被强制执行才有价值，因此每个 kind 都配有一个测试：字段名一改就
变红。**没有人能弄坏的 schema 不是 schema** —— `xlings interface --list`
声明了 20 个 capability，它们的 `outputSchema` 全部只有
`{"exitCode": integer}`，而一个客户端看到版本号就会以为背后有一份契约。

## 8. 各个 kind

### `mcpp.env` —— mcpp 使用的各个路径

```
mcpp self env --format json
```

```jsonc
{
  "initialized": false,          // is there a config.toml yet?
  "mcppHome":    "/home/u/.mcpp",
  "registry":    "/home/u/.mcpp/registry",
  "xlingsHome":  "/home/u/.mcpp/registry",
  "xlingsBinary":"/home/u/.mcpp/registry/bin/xlings",
  "config":      "/home/u/.mcpp/config.toml",
  "buildCache":  "/home/u/.mcpp/build-cache/v1",
  "mcppVersion": "2026.8.8.3"
}
```

这条命令刻意是只读的。人类使用的 `mcpp self env` 会在 `$MCPP_HOME` 缺失时
初始化它 —— 在提示符下敲这条命令的人预期它会这样做 —— 但一个客户端**询问
东西在哪**，不应该成为把东西放到那里的原因。在一台从未运行过 mcpp 的机器
上，得到的输出是它**将会**使用的那些路径，加上 `initialized: false`，而磁盘
未被触碰。

这也是这个 kind 存在的理由：没有它，客户端就得自己重新实现一遍 mcpp 的 home
解析逻辑 —— 包括「PATH 上的 `mcpp` 可能是一个 xlings shim 而不是真正的
二进制」这一部分。

### `mcpp.xpkg` —— 一份被解析的描述符

```
mcpp xpkg parse <file.lua> --format json
```

`data` 就是 `--json` 裸打印出来的那份文档。

`mcpp` 字段是内联表的描述符，产出的是完整文档：`namespace`、`name`、
`versions`、`standard`、`import_std`、`sources`、`include_dirs`、
`generated_files`、`generated_contents`、`targets`、`unknown_keys`。没有
内联表的描述符，会用 `"form": "A"` 代替构建信息。两种形态都携带
`versions` —— 描述符各平台 `xpm` 表的版本键。

### `mcpp.cache` —— 全局构建缓存

```
mcpp cache list --format json
```

`data` 是 `{root, entries[]}`，与 `--json` 裸打印出来的一致。

### `mcpp.toolchain.list` —— 已安装的工具链，以及这台宿主能服务的目标

```
mcpp toolchain list --format json
```

`data` 是 `{host, toolchains[], targets[]}`。一个工具链的形状是
`{family, version, default}`；一份 Visual Studio 安装另有
`source: "system"` —— 它是在机器上被定位到的，不是由 mcpp 安装的。一行目标
的形状是 `{target, note, toolchain, pin, status, default}`，`status` 取
`installed` / `available` / `via dependency graph` / `planned` 之一。

**`toolchain` 与 `pin` 不是把同一个字段写了两遍。** `toolchain` 表示这一行
关联到什么 —— 已装的行是装好的载荷，词表行是它自己的约定。`pin` 只承载目标
表自己的约定，没有约定的行留空。`x86_64-linux-gnu` 装了 gcc，却根本没有约定，
所以要挑出「约定是 gcc」的那些行，必须读 `pin`。

### `mcpp.why.toolchain` —— 一对 (target, toolchain) 的解析结果

```
mcpp why toolchain [--target <triple>] [--toolchain <spec>] --format json
```

它只解析并报告，不执行构建。`data`：

| 字段 | |
|---|---|
| `requested` | `{target, toolchain}` —— 请求的内容 |
| `status` | `ok` 或 `refused` |
| `reason` | 拒绝的记号，或 `none` |
| `compiler` | `{family, version, driver, chosenBy}` —— 真正会运行的驱动器，以及选中它的原因 |
| `triple` | `{requested, toolchain, llvm}` |
| `cLibrary` | `{mode, path, origin, suppliesTarget}` —— `mode` 取 `sysroot` / `payload-first` / `none`，`origin` 取 `payload` / `subos` / `host` / `none` |
| `layers[]` | 目标侧的五个层：`{layer, interface, impl, origin, subset}` |

**`compiler.chosenBy` 回答「为什么是它」。** `{origin, requiredBy,
replaced}` —— `origin` 与构建的状态行使用的是同一句话
（`[toolchain] in mcpp.toml`、`your default`、`target default`、
`required by the dependency graph`、`first-run default`）。当一条
`requires = ["mcpp:compiler=…"]` 做出了这个决定时，`requiredBy` 点名那个
包，`replaced` 点名被顶掉的那个 spec；两者在没有发生时都为空。

```jsonc
"compiler": { "family": "clang", "version": "22.1.8", "driver": "…/clang++",
              "chosenBy": { "origin":     "required by the dependency graph",
                            "requiredBy": "openkal-llvm-runtime@0.1.3",
                            "replaced":   "gcc@16.1.0" } }
```

没有它，一个要问「为什么」的消费方就只能去解析状态行 —— 而消除这种字符串
匹配，正是这份文档存在的理由。

**`cLibrary` 与 `layers[].c-abi` 回答的是两个不同的问题，`suppliesTarget`
说明该信哪一个。** `cLibrary` 描述的是**载荷**自己的链接模型 —— 一份由载荷
供给的 C 库会使用的搜索路径。`layers[].c-abi` 描述的是**这次构建**本身。当
某个依赖供给了 C 库时，两者会出现分歧，而在 `suppliesTarget` 出现之前，
同一份文档会同时报出两者，却没有任何字段说明二者该信哪一个：

```jsonc
"cLibrary": { "origin": "payload", "path": "…/xim-x-glibc/2.44/lib64",
              "suppliesTarget": false },   // ← added; the payload is not in the artifact
"layers":   [ { "layer": "c-abi", "interface": "musl",
                "impl": "openkal-musl@0.3.5", "origin": "graph" } ]
```

这里新增的是一个字段，而不是给 `cLibrary` 改名或者给 `mode` 加一个取值，
因为 §7 承诺字段只增不减，且一个字段的含义永远不变。

**2026.9.1.1 起，一份由载荷供给的 glibc 使 `layers[].interface` 的取值发生了
变化** —— 从 `gnu` 变为 `glibc`，在 Windows 上从 `gnu` 变为 `ucrt`。字段的
含义没有变（它依然表示「是哪一个实现」），因此 §7 仍然成立；变化的是它不再
报告三元组的 env 段 —— 那是一次请求，而不是一个实现，也不是任何 C 库的名字。
现在的取值正是 [22 —— 目标侧](22-target-side.md) 一直列出的那些，包也可以在
`cfg(c-abi = …)` 谓词里与它们比较。按字面量 `gnu` 取值判断的客户端需要更新；
`musl`、`picolibc`、`libSystem` 不受影响。

**`reason` 是一个记号，不是一句话。** 拒绝的消息仍然是写给人看的，仍然点名
目标、规则与出路 —— 但一个要给结果分类的程序读的是 `reason`：

| `reason` | |
|---|---|
| `unknown-target` | 该拼写不对应任何一行，`(arch, os)` 也没有对应的组 |
| `ambiguous-request` | 该 `(arch, os)` 有多行受支持，且没有一行是默认值 |
| `compiler-requirement-conflict` | 图所要求的编译器在这里不可用 |
| `tier-planned` | 词表里存在这一行，但还没有任何东西接线 |
| `host-cannot-serve` | 本机没有载荷，也没有依赖供给这个系统 |
| `capability-pin` | 这一行的工具链是一项能力陈述，不是一个偏好 |
| `convention-unreplaced` | 约定被推翻了，而没有任何东西接替它 |
| `os-mismatch` | 请求的三元组与解析出的三元组命名不同的系统 |
| `layer-requirement` | 某个包要求的层，解析结果没有提供 |
| `layer-ordering` | 五个层无法叠加 |
| `exclusive-capability` | 一项能力有多个提供者，其中至少一个声明了独占 |
| `version-floor-unmet` | 某个包对机器的要求，高于机器被声明拥有的水平 |
| `accel-mismatch` | 一条 `[build] sources` 条目被约束到本次构建未覆盖的设备集合 |
| `accel-backend-undeclared` | 一条 `[build] sources` 条目命名的后端，未出现在该包自己的 `[package] accelerators` 中 |
| `device-source-unconsumed` | 一个设备类源文件没有到达任何 action，因此没有任何东西编译它 |
| `host-module-missing` | `build.mcpp` 导入的模块，没有任何依赖以 host module 的形式提供 |
| `tool-version-conflict` | 两处声明把同一个 xlings 包定在不能同时成立的两个版本上 |
| `shared-library-cxx-runtime` | 图所在的构建中，C++ 运行时是一个包，而某个依赖的 C++ 共享库没有声明私有副本 |
| `offline-download-required` | 本次运行离线，而规划需要下载某样东西：工具链、包、git 修订，或包索引 *(2026.9.16.1+)* |
| `package-cycle` | 依赖图中存在一个包的环；消息列出环上的边 *(2026.9.16.1+)* |
| `program-cxx-runtime-split` | 一个声明了自含 C++ 运行时的程序或测试，加载了本次构建中耦合到共享运行时的 C++ 共享库 *(2026.9.16.1+)* |
| `static-package-in-two-images` | 一个静态包被本次构建的多个映像到达，而在该目标上一个映像不能使用另一个映像里的副本 *(2026.9.16.1+)* |
| `c-env-unrealisable` | 解析出的 C 库的 `[c-abi]` 声明在这个目标上没有已知的实现方式，或者解析出的编译器做不到这一点 *(2026.9.18.1+)* |
| `c-env-verification-mismatch` | 用已实现出的 `[c-abi]` 配置编译出的探针，其结果与声明不符 *(2026.9.18.1+)* |
| `platform-dependency` | `[build] platform-dependencies = "refuse"`，而图里有某个包带来了平台 SDK *(2026.9.18.1+)* |
| `interface-not-provided` | 某个包的 `[kernel-abi] requires-interfaces` 点名了一个解析出的实现并不提供的接口 *(2026.9.20.1+)* |
| `apple-sdk-absent` | 目标需要本机没有的一个 Apple SDK；它是被定位而不是被安装的，因为它不可再分发 |
| `lld-required-absent` | 目标直接通过 lld 链接，而解析出的工具链载荷不带 lld |
| `host-tool-toolchain` | 一个交叉 `--target` 下的 `build.mcpp` 需要一个可解析的**宿主**工具链，而一个都没有配置 |
| `std-module-precompile` | 标准库的模块在这个配置下无法被预编译 |
| `other` | 一个尚未被赋予记号的拒绝分支 |

**其中一个记号也由 `mcpp build` 自己打印。** `interface-not-provided` 会
出现在拒绝消息本身里，用方括号括着，与 `E0006` 是同一个约定。一个只有人能
认出来的拒绝，会逼迫每一个机器消费方去匹配一段散文 —— 而这段散文，一个包
自己的编译错误恰好也可能包含；mcpp-index 的兼容性测量正是靠这个记号，把
「这个图没有供给这个成员所要求的」与「这个成员没能构建」区分开，而这个区分
决定了一个成员算不算进兼容率。

**只要问题被回答了就退出 0，包括答案是「拒绝」这种情况。** 「这次构建能不能
成立，不能的话为什么」被「不能，因为这一行的 pin 是一项能力陈述」完整地
回答了。非零退出的含义是这次查询本身没能运行起来。

**它声明的 effects 故意偏宽。** `--protocol-version` 为这条命令列出了
`network`、`write-global-cache` 与 `exec-build-script`：答案来自与一次构建
相同的解析过程，而那个过程可能拉取包、安装载荷，并运行某个依赖的构建程序。
客户端是在**运行之前**读这张表来决定是否放行的，漏报任何一项都等于给出一句
不成立的安全承诺。

### `mcpp.build-database` —— 以构建数据库表达的构建计划 *(mcpp 2026.9.15.1+)*

```
mcpp emit build-database [--spec s1|compile-commands] --format json
```

它按 `mcpp build --configure-only` 的方式、用相同的选择器规划，不写入项目
目录。`data` 为：

| 字段 | |
|---|---|
| `spec` | `{"name": "s1", "version": "0.2.0"}`；使用 `--spec compile-commands` 时为 `{"name": "compile-commands"}` |
| `database` | 该规范对应的文档：一份 S1 构建数据库，或者 `mcpp build --configure-only` 写入 `compile_commands.json` 的那些条目 |
| `watch` | 一旦发生变化就可能改变这份文档的输入：相对工作区根目录的路径与 glob，或绝对路径 |
| `inputs-fingerprint` | `fnv1a:<16 位十六进制>`，对上述输入、mcpp 版本与选择器求出的摘要 |

不带 `--format` 时，命令只输出这份文档；`-o <file>` 会把原本要输出的内容
写入 `<file>`。文档的内容、不写入项目目录这条保证，以及 `watch` 的规则，见
[SPEC-005](../specs/build-database.md)。

`emit` 独立规划每一个被选中的成员（#699 第 1 项）：一个成员的规划失败不会
连累它的兄弟成员。不在项目中，或者被选中的成员全部规划失败时，信封省略
`data` 并以 1 退出，每个失败的成员各带一条诊断：不在项目中是
`MCPP_BUILD_DATABASE_NO_PROJECT`；离线规划（`--offline`、`MCPP_OFFLINE`、
`MCPP_NO_AUTO_INSTALL`）需要下载某样东西（工具链、包、git 修订，或包索引，
消息会指出第一个）时是 `MCPP_OFFLINE_DOWNLOAD_REQUIRED`；因其他原因规划失败
时是 `MCPP_BUILD_DATABASE_PLAN_FAILED`。离线这一种不是项目的缺陷：不带
`--offline` 再运行一次即可消除它。每个成员的诊断都带 `path`，即该成员的
`mcpp.toml`，相对工作区根目录。

只要有一个被选中的成员规划成功，`data` 就会出现，并描述每一个规划成功的
成员：规划失败的成员不贡献任何集合，只贡献上面那样一条 `error` 诊断，它的
`mcpp.toml` 与存在时的 `build.mcpp` 一并加入 `watch`。只要诊断里有一条是
`error`，退出码依然是 1——因此消费方靠结构就能读出三种结果：没有 `data`；
`data` 伴随若干 `error` 诊断，描述了诊断所指之外的一切；`data` 且没有
`error`，不需要解析消息文本。

构建程序失败的包（#699 第 2 项）会被描述为不含该程序产生的指令：清单自身
的那部分配置、工具链、模块图与标准库单元仍照常描述，另附一条 `error` 诊断
`MCPP_BUILD_DATABASE_PROGRAM_FAILED` 点名它，`path` 为它的 `build.mcpp`。若
后续失败是由缺失的指令引起的，则按上面的规则使整个成员失败。包请求的宿主
工具构建失败则降级为警告 `MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT`，点名工具、
所属包与失败信息的第一行；规划继续进行，只点名该工具而不运行它的构建程序
会像该工具构建成功时一样完成配置。这两者都不影响 `mcpp build`：构建程序或
宿主工具在其中失败仍然会使构建失败。

| 诊断码 | 严重级别 | |
|---|---|---|
| `MCPP_LOCK_WOULD_CHANGE` | 警告 | 解析结果与项目的 `mcpp.lock` 不一致，命令不写这个文件 |
| `MCPP_GENERATED_FILE_NOT_MATERIALIZED` | 警告 | 根包 `[build] generated_files` 中的某个文件缺失或内容已过期，命令不写这个文件 |
| `MCPP_BUILD_DATABASE_STD_UNIT_UNDESCRIBED` | 警告 | 没有任何标准库构建命令点名它的模块源文件，该单元因此不被列出 |
| `MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT` | 警告 | 被请求的宿主工具构建失败；该工具仍会被构建，它的 `check` 动作仍会运行 |
| `MCPP_BUILD_DATABASE_PROGRAM_FAILED` | 错误 | 构建程序失败；它所属的包被描述为不含它产生的指令 |

`--protocol-version` 为这条命令声明 `init-mcpp-home`、`read-project`、
`network`、`write-global-cache` 与 `exec-build-script`，从不声明
`write-project`。

### `mcpp.pack` —— 一次打包的产物 *(mcpp 2026.9.16.1+)*

```
mcpp pack [target] [--format <f>] [--target <triple>...] --message-format json
```

信封在命令结束后输出一次；所有给人看的行都改走 stderr，包括打包过程启动的
构建程序与工具输出的内容。`data` 为：

| 字段 | |
|---|---|
| `artifacts` | 每个产出的产物一条记录：`path`（绝对路径）、`type`（`file` 或 `directory`）、`format`（`--format` 的取值，省略时为 `tar`），以及 `targets`（进入该产物的每条腿的规范三元组）。被分派的格式报告本次请求引入的那些 action 的终端输出；一个多 `--target` 的 Android 打包报告一个产物，其 `targets` 列出每一条腿 |
| `stage` | 该产物所来自的那棵树：`dir`、`manifest`（即下文的暂存清单）与 `closure`（`walked` 或 `not-walked`）；库包，以及没有暂存任何树的情形，此字段为 `null` |

失败时省略 `data`，以命令自身的退出码退出，并携带诊断码
`MCPP_PACK_FAILED`；原因写在 stderr 上。每次运行的 `effects` 是
`read-project`、`write-project` 与 `write-global-cache`，若运行了某个构建
程序则再加上 `exec-build-script`。`--protocol-version` 为 `pack` 声明
`init-mcpp-home`、`read-project`、`write-project`、`network`、
`write-global-cache` 与 `exec-build-script`。

### `mcpp test --message-format json` —— 测试流

```
mcpp test [pattern] [--workspace] --message-format json
```

这条流早于 §2 的信封，也不被它包裹：它是 NDJSON，每个测试结束时一条记录，随后
每个成员一条汇总记录。`--workspace` 运行以一条 `workspace_summary` 记录
结束。§7 的保证对它同样成立 —— 字段只增不减，字段含义永不改变 —— 下表是
2026.9.2.1 时点的契约。

每个测试：

| 字段 | |
|---|---|
| `member` | 所属的 workspace 成员；在 workspace 之外为 `""` |
| `test` | 按路径命名的测试名（`tests/00-a/0.cpp` → `00-a/0`） |
| `status` | `pass`、`compile_fail`、`run_fail`、`not_run` 或 `built` |
| `exit_code` | 该测试的退出状态；`not_run` 与 `built` 时为 `0` |
| `signal` | 状态编码了信号时为信号编号，否则为 `null` |
| `duration_ms` | 该测试构建加运行的墙钟时间 |
| `timed_out` | 被 `--timeout` 杀掉时为 `true`（`run_fail`） |
| `compile_output`、`run_output` | 捕获到的诊断输出 |
| `reason` | 仅 `not_run` 时：一句话说明原因；其余情况为 `""` |

汇总记录，`{"summary": {...}}`：

| 字段 | |
|---|---|
| `member`、`passed`、`failed` | 计数 |
| `not_run` | 已构建但没有执行的测试数 |
| `not_run_reason` | 它们共同的原因，或 `""` |
| `built` | 在 `--no-run` 下构建、本就不打算执行的测试数 |
| `elapsed_ms`、`build_ms`、`run_ms` | 墙钟时间，分段给出 |

**`built` 与 `not_run` 是两个不同的答案，分开计数。** 两者描述的都是一个
编译过、没有执行的测试，相似之处到此为止：`not_run` 意味着 mcpp 试过而做不
到，问题因此仍然悬而未决，退出码是 2；`built` 意味着 `--no-run` 要求不要
执行，构建就是问题的全部，退出码是 0。把两者相加的消费方，会把一次它从未要求
过的运行，报告成一次没能完成的运行。

**`not_run` 既不是 `pass` 也不是 `run_fail`，退出码也说明了这一点
（2026.9.2.1）。** 当本机无法加载测试产物时（比如交叉目标未声明 runner 时的
`Exec format error`），或者声明的 `[target.<triple>].runner` 找不到、启动不
了时，该测试被标记为 `not_run`。这是关于整次调用的事实：它被确立一次之后，
其余测试直接报告为 `not_run` 而不再启动，进程以 **2** 退出。退出码 1 的含义
不变 —— 有测试运行过并且失败了；0 表示每个测试都运行过并且通过了。只按退出
码判断 pass/fail 的客户端必须处理 2 这个值；由 `failed == 0` 推断「全部通过」
的客户端也必须读取 `not_run`。

`workspace_summary` 增加了 `tests_not_run`（各成员之和）、`tests_built`
（`--no-run` 下构建的测试数之和）与 `unrunnable_members`（所有测试都是
`not_run` 的那些成员），与既有的 `not_run` 列表并列；后者仍然指
`--workspace-timeout` 到期时尚未开始的那些成员。`tests_built` 与
`tests_not_run` 分开计数，理由与逐成员的那两个字段相同：一个是被悬置的问题，
另一个是压根没被问过的问题。

### 暂存清单

`mcpp pack` 在它暂存的树旁边写出 `<staged tree>.stage-manifest`，一个点名
`${mcpp.stage_dir}` 的 `artifact` action 依赖这个文件（见
[30](30-build-mcpp.md#产出可分发物pack_format-与-stage_dir20269111)）。它是
一个按行组织的文本文件，而不是一个 JSON 信封，各行按下列顺序构成三块：

| 行 | 内容 |
|---|---|
| `closure = walked` 或 `closure = not-walked` | 第一行：树所需要的每一个库是否都已解析 |
| `reason = <text>` | 只在 `closure = not-walked` 时出现；一行 |
| `needs<TAB><name><TAB><where>` *(mcpp 2026.9.14.2+)* | 闭包读到的每一个库名各占一行，已排序；`<where>` 是暂存库相对于树的路径、目标机提供的库记为 `platform`，或 `unresolved` |
| `<size> <path>` 或 `link <path>` | 每一个暂存文件或符号链接各占一行，已排序 |

`<name>` 按需要它的对象自己的拼写给出：一条 `DT_NEEDED` 条目、一个 PE 导入
名，或一个 Mach-O install name。`needs` 行的各字段以 TAB 分隔，因为名字与
路径都可能包含空格。一棵由不打包任何东西的 mode（`system`、`static`）暂存
出的树不带 `needs` 行，由更早版本的 mcpp 暂存出的树同样不带；自行放置库的
读者应当读取这些行，而不是从 `lib/` 下的文件推断闭包。

## 当前边界

- **退出码表的作用域仅限于它点名的那些命令。** 别的命令返回的退出码不在表
  里，把它加进来等于记录一件这些命令承诺不了的事。
- 一条没有位置的诊断会省略 `path` 与 `range`，而不是发送 0：`line: 0` 会指向
  一个不存在的位置。
- stdout 上没有 JSON，就意味着「不支持」，无论原因是什么 —— 一个较旧的引擎、
  一个未知选项，或者一条根本没有机器格式的命令。客户端仅凭这一条流本身，分辨
  不出这三种情况。
