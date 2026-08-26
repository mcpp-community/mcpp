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

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 用法错误 —— 未知选项、不支持的值 |
| 70 | 内部错误(未捕获异常) |
| 127 | 未知命令 |

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

## 6. 稳定性承诺

对每个 `kind`,在同一 `kindVersion` 内:

- 字段只**新增**,不删除
- 字段含义不改变
- 破坏性变更抬版本;需要过渡窗口时,`protocol.min`/`max` 重叠,两版都可读

这个承诺只有被强制执行才值钱,所以每个 kind 都有一个「改字段名就变红」的测试。
**没人能弄坏的 schema 不是 schema** —— `xlings interface --list` 声明了 20 个
capability,其 `outputSchema` 全部只有 `{"exitCode": integer}`,而客户端看到版本号就会
以为背后有契约。

## 7. 各 kind

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
| `compiler` | `{family, version, driver}` —— 真正会跑的驱动器 |
| `triple` | `{requested, toolchain, llvm}` |
| `cLibrary` | `{mode, path, origin, suppliesTarget}`;`mode` 取 `sysroot` / `payload-first` / `none`,`origin` 取 `payload` / `subos` / `host` / `none` |
| `layers[]` | 目标侧五层:`{layer, interface, impl, origin, subset}` |

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

是**新增一个字段**而不是给 `cLibrary` 改名或给 `mode` 加取值,因为 §6 承诺字段
只增不删、且一个字段的含义永不改变。

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
| `other` | 一处还没有被命名的拒绝分支 |

⚠️ **只要问题被回答了就退 0,包括答案是「拒绝」。** 「它能不能构建,不能的话
为什么」被「不能,因为这一行的 pin 是能力陈述」完整地回答了。非零退出的含义是
这次查询本身没跑起来。

⚠️ **它声明的 effects 故意偏宽。** `--protocol-version` 为这条命令列出
`network`、`write-global-cache` 与 `exec-build-script`:答案来自与构建同一次的
解析,而那可能拉取包、安装载荷、并运行某个依赖的构建程序。客户端是在**运行之前**
读这张表来决定放不放行的,漏报一项就是一句不成立的安全承诺。
