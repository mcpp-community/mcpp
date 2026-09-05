# 11 — 机器可读输出

mcpp 面向两类读者。本章是对第二类 —— **程序** —— 的契约。编辑器扩展、CI 脚本,以及任何解析 mcpp 输出的程序,
可依赖的部分在此列出。

设计与背后的实测:`.agents/docs/2026-08-08-machine-readable-output-protocol-design.md`。

## 1. 首要规则

> **靠解析 stdout 来识别协议。不要靠退出码,也不要靠「命令没失败」。**

读 stdout,尝试按 JSON 解析,并要求 `schemaVersion` 与 `kind` 都在。缺任一,说明这个
mcpp 不支持所请求的内容。

这不是风格偏好。`mcpp --protocol-version` 看起来像是入口,在支持它的版本上确实是个
捷径。但在**它出现之前发布的每一个 mcpp** 上,它自己就是一个未知选项 —— 而未知选项
过去会把人类文本打到 **stdout**、退出码 1、stderr 为空。成功与失败同一通道。换成
`--json` 拼写也一样,两者走同一条路径。

所以「正向识别」是唯一跨版本成立的规则。下面所有设计都围绕它。

## 2. 信封

```jsonc
{
  "schemaVersion": 1,          // 信封自身的版本
  "kind": "mcpp.env",          // 这是哪种文档
  "kindVersion": 1,            // 该 kind 的数据版本
  "effects": [],               // 运行这条命令做了什么 —— 见 §4
  "mcpp": { "version": "2026.8.8.3", "protocol": { "min": 1, "max": 1 } },
  "data": { /* 随 kind 而定 */ },
  "diagnostics": []
}
```

`schemaVersion` 与 `kindVersion` 分开是有意的:单一全局版本号意味着给 `mcpp.env` 加
一个字段会推高客户端为 `mcpp.xpkg` 读到的版本,而它无从判断真正变的是哪个。

`effects` 永远存在。空数组表示「没有」;字段缺失会表示「未知」,那是另一个断言。

### 诊断

```jsonc
{
  "code": "MCPP_MANIFEST_UNKNOWN_KEY",
  "severity": "error" | "warning" | "note",
  "source": "mcpp",
  "message": "unknown key 'standrad'",
  "path": "mcpp.toml",                                    // 没有则省略
  "range": { "start": {"line":3,"column":1},
             "end": {"line":3,"column":9} }               // 没有则省略
}
```

位置从 1 开始。`column` 数的是 **UTF-8 字节**,因此索引的是 mcpp 读到的同一份文件。

没有位置的诊断**省略** `path` 与 `range`,而不是填 0 —— `line: 0` 会把客户端指向一个
不存在的位置。

`code` 永远存在。**解析 `code`,永远不要解析 `message`。**

## 3. 请求机器输出

```
mcpp <命令> --format json
```

目前只支持 `json`。`ndjson` 保留给未来真正需要流式的场景,**现在不接受** —— 请求它是
错误,不是静默回落。

### 不支持的值 / 未知选项

两者都走 **stderr**、退出码 **2**,且**不往 stdout 写任何东西**:

```
$ mcpp self env --format yaml
error: unsupported --format 'yaml'; expected: json      # stderr
$ echo $?
2
```

一个还不知道自己会得到什么格式的请求,不该往协议独占的通道里写东西。结合 §1,客户端的
规则就完整了:**stdout 上没有 JSON,就是不支持**,无论原因。

**带信封命令**的退出码 —— 即 `--protocol-version` 声明的那几个 kind。这张表刻意只管
它们:别的命令返回的码不在其中,写进来等于记录一个这些命令给不出的值。mcpp 全局的
映射见[退出码契约](../spec/exit-codes.md)。

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 命令跑了并且失败 —— 见 stderr;当 stdout 带信封时见 `diagnostics` |
| 2 | 用法错误 —— 未知选项、不支持的值 |
| 70 | 内部错误(未捕获异常) |
| 127 | 未知命令 |

⚠️ **`1` 可能与 stdout 上的信封同时出现。** `mcpp xpkg parse` 会把一份违反名字形态的
描述符作为 JSON 报出来**并且**退 1:文档就是答案,退出码说明这个答案是一次拒绝。§1
仍然成立 —— 解析 stdout,不要按退出码分支 —— 但一个把任何非零退出都当作「没有输出」
的客户端,会丢掉它已经拿到的文档。

## 4. effects —— 命令在输出前执行的操作

带 untrusted-workspace 门的 IDE 必须在**运行之前**决定。等信封到手,它描述的事情已经
发生了。所以同一份信息也静态提供:

```
mcpp --protocol-version
```

```jsonc
{
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

用具名 effects 而不是 `destructive: true|false`,因为布尔分不开「无害的」和「门存在的
理由」:

| effect | 含义 |
|---|---|
| `init-mcpp-home` | 首次使用时可能创建 `$MCPP_HOME`。**位于项目目录之外。** |
| `read-project` | 读 manifest 与源码 |
| `write-project` | 写入项目树(`target/`、compile DB) |
| `write-global-cache` | 写共享构建缓存 |
| `network` | 可能联网 |
| `exec-build-script` | **执行工作区里的代码**(`build.mcpp`) |

多数门只在乎 `exec-build-script` 与 `write-project`,可以忽略 `init-mcpp-home` ——
mcpp 给自己做初始化不是工作区在动作。

## 5. `--json` 不等于 `--format json`

有两条命令在本协议之前就发布了 `--json`:

```
mcpp xpkg parse <file> --json  ->  {"namespace": …, "name": …, …}
mcpp cache list --json         ->  {"root": …, "entries": [ … ]}
```

这些 payload 是**裸的**(没有信封),而且已经有消费者在读。所以:

> **`--json` 永久保留它的 payload。`--format json` 才是带信封的那个。**

`--json` 不弃用,使用它也**不打任何警告** —— 客户端在解析这份输出,警告会落在它中间。

两种拼写由同一个来源产出,所以永远描述同一件事:一个答案,两种形状。

## 6. 退出码

`mcpp run` 报告程序自己的退出码。整个取值空间分三段,只有第一段属于程序:

| 区间 | 含义 |
|---|---|
| `0`–`124` | 程序跑过了,这是它自己的退出码,原样透传 |
| `125`–`127` | 尝试启动但被拒绝 —— `127` 找不到,`126` 找到但不可执行,`125` 其他 |
| `2` | mcpp 在尝试启动之前就拒绝了:用法、配置或解析错误 |

2026.9.4.3 之前,所有非零退出码都被折成 `1`,为的是让 `2` 表示「起不来」以区别于
「跑了但失败」。这个区别值得保留,代价不值得:`main` 返回 `3` 的程序会让
`mcpp run` 退 `1`,qemu 报 `3` 的裸机镜像同样到达为 `1` —— 本项目让人使用的这条
命令因此无法用于分支判断。

中间那一段是 `env`、`timeout`、`nice` 早已在用、且被 shell 文档化的取值,所以
`126` 与 `127` 带着它们惯常的含义到达,而不是本项目分配的编号。

**程序自己也可以退 `125`–`127`,mcpp 不试图靠数字区分。** 区分二者的是:启动失败
一定向 stderr 写出原因,而程序自己的退出码从不写。需要确定的客户端应当读 stderr,
或使用 `--format json` —— 那里退出码是一个字段而不是一条通道。

`mcpp test` 不变,仍为 `0` 或 `1`:它聚合多个程序,没有单一退出码可以透传。
每个测试各自的退出码在 JSON 流的 `exit_code` 字段里(§8)。

## 7. 稳定性承诺

对每个 `kind`,在同一 `kindVersion` 内:

- 字段只**新增**,不删除
- 字段含义不改变
- 破坏性变更抬版本;需要过渡窗口时,`protocol.min`/`max` 重叠,两版都可读

这个承诺只有被强制执行才值钱,所以每个 kind 都有一个「改字段名就变红」的测试。
**没人能弄坏的 schema 不是 schema** —— `xlings interface --list` 声明了 20 个
capability,其 `outputSchema` 全部只有 `{"exitCode": integer}`,而客户端看到版本号就会
以为背后有契约。

## 8. 各 kind

### `mcpp.env` —— mcpp 把东西放在哪

```
mcpp self env --format json
```

```jsonc
{
  "initialized": false,          // 还没有 config.toml
  "mcppHome":    "/home/u/.mcpp",
  "registry":    "/home/u/.mcpp/registry",
  "xlingsHome":  "/home/u/.mcpp/registry",
  "xlingsBinary":"/home/u/.mcpp/registry/bin/xlings",
  "config":      "/home/u/.mcpp/config.toml",
  "buildCache":  "/home/u/.mcpp/build-cache/v1",
  "mcppVersion": "2026.8.8.3"
}
```

这条路径**刻意是只读的**。人类用的 `mcpp self env` 会在 `$MCPP_HOME` 缺失时初始化它
—— 在提示符下敲这条命令的人预期如此 —— 但一个客户端**询问东西在哪**,不该成为把东西
放到那儿的原因。在从未运行过 mcpp 的机器上,得到的是它**将会**使用的路径与
`initialized: false`,而磁盘未被触碰。

这也是它存在的理由:没有它,客户端就得重新实现 mcpp 的 home 解析 —— 包括「PATH 上的
`mcpp` 可能是 xlings shim 而不是真二进制」那一部分。

### `mcpp.xpkg` —— 解析后的描述符

```
mcpp xpkg parse <file.lua> --format json
```

`data` 就是 `--json` 裸打印的那份文档。

`mcpp` 字段为内联表的描述符产出完整文档:`namespace`、`name`、`versions`、
`standard`、`import_std`、`sources`、`include_dirs`、`generated_files`、
`generated_contents`、`targets`、`unknown_keys`。没有内联表的描述符以
`"form": "A"` 代替构建信息。两种形态都携带 `versions` —— 描述符各平台
`xpm` 表的版本键。

### `mcpp.cache` —— 全局构建缓存

```
mcpp cache list --format json
```

`data` 是 `{root, entries[]}`,与 `--json` 裸打印的一致。

### `mcpp.toolchain.list` —— 装了什么,以及这台宿主服务哪些目标

```
mcpp toolchain list --format json
```

`data` 是 `{host, toolchains[], targets[]}`。一个工具链是
`{family, version, default}`;Visual Studio 另有 `source: "system"` —— 它是在
机器上被找到的,不是 mcpp 装的。一行目标是
`{target, note, toolchain, pin, status, default}`,`status` 取
`installed` / `available` / `via dependency graph` / `planned`。

⚠️ **`toolchain` 与 `pin` 不是同一个字段写两遍。** `toolchain` 是这一行关联到
什么 —— 已装的行是装了的载荷,词表行是那一行的约定。`pin` 只承载目标表的约定,
没有约定的行为空。`x86_64-linux-gnu` 装了 gcc 而根本没有约定,所以要挑「约定是
gcc 的行」必须读 `pin`。

### `mcpp.why.toolchain` —— 一对 (目标, 工具链) 会解析成什么

```
mcpp why toolchain [--target <triple>] [--toolchain <spec>] --format json
```

它只解析并报告,不构建。`data`:

| 字段 | |
|---|---|
| `requested` | `{target, toolchain}` —— 问的是什么 |
| `status` | `ok` 或 `refused` |
| `reason` | 拒绝的记号,或 `none` |
| `compiler` | `{family, version, driver, chosenBy}` —— 真正会跑的驱动器,以及为什么是它 |
| `triple` | `{requested, toolchain, llvm}` |
| `cLibrary` | `{mode, path, origin, suppliesTarget}`;`mode` 取 `sysroot` / `payload-first` / `none`,`origin` 取 `payload` / `subos` / `host` / `none` |
| `layers[]` | 目标侧五层:`{layer, interface, impl, origin, subset}` |

⭐ **`compiler.chosenBy` 回答「为什么是它」。** `{origin, requiredBy, replaced}`
—— `origin` 与构建状态行用的是同一句话(`[toolchain] in mcpp.toml`、
`your default`、`target default`、`required by the dependency graph`、
`first-run default`)。当某条 `requires = ["mcpp:compiler=…"]` 做了决定时,
`requiredBy` 点名那个包,`replaced` 点名被顶掉的那个 spec;没有发生时两者都为空。

```jsonc
"compiler": { "family": "clang", "version": "22.1.8", "driver": "…/clang++",
              "chosenBy": { "origin":     "required by the dependency graph",
                            "requiredBy": "openkal-llvm-runtime@0.1.3",
                            "replaced":   "gcc@16.1.0" } }
```

没有它,要问「为什么」的消费方只能去解析状态行 —— 而那正是这份文档存在的理由所要
消除的字符串匹配。

⚠️ **`cLibrary` 与 `layers[].c-abi` 回答的是两个问题,`suppliesTarget` 说明哪一个
管用。** `cLibrary` 描述的是**载荷**的链接模型 —— 一份由载荷供给的 C 库会用到的
搜索路径。`layers[].c-abi` 描述的是**这次构建**。当依赖供给 C 库时两者分叉,而在
`suppliesTarget` 之前,同一份文档同时报出两者却没有任何字段说明该信哪个:

```jsonc
"cLibrary": { "origin": "payload", "path": "…/xim-x-glibc/2.44/lib64",
              "suppliesTarget": false },   // ← 新增;载荷并不在产物里
"layers":   [ { "layer": "c-abi", "interface": "musl",
                "impl": "openkal-musl@0.3.5", "origin": "graph" } ]
```

是**新增一个字段**而不是给 `cLibrary` 改名或给 `mode` 加取值,因为 §7 承诺字段
只增不删、且一个字段的含义永不改变。

⚠️ **2026.9.1.1 起,载荷供给的 glibc 让 `layers[].interface` 的**取值**变了** ——
从 `gnu` 变为 `glibc`,Windows 上从 `gnu` 变为 `ucrt`。字段的**含义**没变(它仍然是
「哪个实现」),所以 §7 仍然成立;变的是它不再报三元组的 env 段 —— 那是一次请求而
不是一个实现,也不是任何一个 C 库的名字。现在的取值就是
[14 —— 目标侧](14-target-side.md)一直列着的那些,并且包可以在
`cfg(c-abi = …)` 谓词里与它们比较。按字面量 `gnu` 取值的客户端需要更新;
`musl`、`picolibc`、`libSystem` 不受影响。

⭐ **`reason` 是一个记号,不是一句话。** 拒绝的消息仍然写给人看,仍然点名目标、
规则与出路;而一个要给结果分类的程序读 `reason`:

| `reason` | |
|---|---|
| `unknown-target` | 这个拼写不指向任何一行,`(arch, os)` 也没有对应的组 |
| `ambiguous-request` | 该 `(arch, os)` 有多行受支持,而词法默认不在其中 |
| `compiler-requirement-conflict` | 图要求的编译器在这里用不了 |
| `tier-planned` | 词表里有这一行,还没有任何东西接线 |
| `host-cannot-serve` | 本机没有载荷,依赖也没有供给这个系统 |
| `capability-pin` | 这一行的工具链是能力陈述,不是偏好 |
| `convention-unreplaced` | 约定被推翻了,而没有任何东西接替它 |
| `os-mismatch` | 请求的与解析出的三元组指向不同的操作系统 |
| `layer-requirement` | 某个包要求的层,解析没有给出 |
| `layer-ordering` | 五层叠不起来 |
| `exclusive-capability` | 一个能力有多个提供者,而其中至少一个声明了独占 |
| `version-floor-unmet` | 一个包对机器的要求高于机器被声明拥有的 |
| `accel-mismatch` | 一条 `[build] sources` 条目被约束到本次构建未覆盖的设备集合 |
| `other` | 一处还没有被命名的拒绝分支 |

⚠️ **只要问题被回答了就退 0,包括答案是「拒绝」。** 「它能不能构建,不能的话
为什么」被「不能,因为这一行的 pin 是能力陈述」完整地回答了。非零退出的含义是
这次查询本身没跑起来。

⚠️ **它声明的 effects 故意偏宽。** `--protocol-version` 为这条命令列出
`network`、`write-global-cache` 与 `exec-build-script`:答案来自与构建同一次的
解析,而那可能拉取包、安装载荷、并运行某个依赖的构建程序。客户端是在**运行之前**
读这张表来决定放不放行的,漏报一项就是一句不成立的安全承诺。

### `mcpp test --message-format json` —— 测试流

```
mcpp test [pattern] [--workspace] --message-format json
```

这条流早于 §2 的信封,也不被信封包裹:它是 NDJSON,每个测试结束时一条记录,随后每个
成员一条汇总记录。`--workspace` 运行以一条 `workspace_summary` 记录结束。§7 的保证
对它同样成立 —— 字段只增不减,字段含义不变 —— 下表是 2026.9.2.1 时的契约。

每个测试:

| 字段 | |
|---|---|
| `member` | workspace 成员;workspace 之外为 `""` |
| `test` | 按路径命名的测试名(`tests/00-a/0.cpp` → `00-a/0`) |
| `status` | `pass`、`compile_fail`、`run_fail` 或 `not_run` |
| `exit_code` | 测试的退出状态;`not_run` 时为 `0` |
| `signal` | 状态编码了信号时是信号号,否则 `null` |
| `duration_ms` | 这个测试构建+运行的墙钟时间 |
| `timed_out` | 被 `--timeout` 杀掉时为 `true`(`run_fail`) |
| `compile_output`、`run_output` | 捕获的诊断输出 |
| `reason` | 仅 `not_run`:一句话说明原因;其余为 `""` |

汇总记录 `{"summary": {...}}`:

| 字段 | |
|---|---|
| `member`、`passed`、`failed` | 计数 |
| `not_run` | 已构建但没有执行的测试数 |
| `not_run_reason` | 它们共同的原因,或 `""` |
| `elapsed_ms`、`build_ms`、`run_ms` | 墙钟时间,分段 |

⚠️ **`not_run` 既不是 `pass` 也不是 `run_fail`,退出码也这么说(2026.9.2.1)。**
本机无法加载测试产物(交叉目标未声明 runner 时的 `Exec format error`),或声明的
`[target.<triple>].runner` 找不到、启动不了时,测试为 `not_run`。这是关于整次调用的
事实:确立一次,其余测试直接报告为 `not_run` 而不再启动,进程以 **2** 退出。退出码 1
含义不变 —— 有测试运行并失败;0 表示每个测试都运行并通过。只读退出码判 pass/fail 的
客户端必须处理 2;由 `failed == 0` 推断「全部通过」的客户端还必须读 `not_run`。

`workspace_summary` 增加 `tests_not_run`(各成员之和)与 `unrunnable_members`(所有
测试都 `not_run` 的成员),与既有的 `not_run` 列表并列;后者仍然指
`--workspace-timeout` 到达时尚未开始的成员。
