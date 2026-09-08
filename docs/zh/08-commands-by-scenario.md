# 08 —— 按场景选命令

命令清单是 `mcpp --help`,每个子命令还有自己的 `--help`。本章回答的是另一个问题:
某个情形已经发生时该用哪条命令 —— 构建目录一直变大、解析结果出乎意料、描述符即将
发布、索引可能陈旧。这里收的都是名字本身没有说出它所属场景的命令。

相关文档:[01 — 快速开始](01-getting-started.md)(日常构建与测试循环)、
[20 — 工具链管理](20-toolchains.md)、
[11 — 发布库到 mcpp-index](11-publishing-a-library.md)、
[50 — 机器可读输出](50-machine-output.md)。

下面每段输出都由本章所在版本的 mcpp 实际产生。

## 回收磁盘而不触发重编

有两个存储会增长,增长的原因不同,各由一条命令清空。把两者弄混的代价是一次全量重编。

| 存储 | 作用域 | 增长时机 | 清空方式 |
|---|---|---|---|
| `target/<三元组>/<指纹>/` | 单个工程 | 配置指纹变化,开出新目录 | `mcpp clean`、`mcpp clean --stale` |
| 构建缓存(`mcpp cache dir`) | 整台机器 | 任何工程编译依赖或 `std` 模块 | `mcpp cache gc`、`mcpp cache prune`、`mcpp cache clean` |

`mcpp clean` 整个删掉 `target/`,下次构建重编一切。`mcpp clean --stale` 只删已无构建
记录使用的指纹目录,在用的配置保留:

```
$ mcpp clean --stale --dry-run
would remove target/x86_64-linux-gnu/0123456789abcdef  (0.0 B)
Would remove 1 directory (0.0 B)
```

「在用」指被 `target/.build_cache` 记录 —— 它由 `mcpp build` 写入,由快路径读取。
这个定义带来三个后果:

- 没有记录并不足以让一个目录被删。`mcpp test` 走的构建路径不写记录,`--no-cache`
  构建同样不写。未被记录但在 `--older-than`(默认一天)之内写过的目录保留;更旧的
  会被删,而判断错误的代价是重编一个此后无人碰过的配置。
- 完全没有记录时,命令拒绝执行而不是猜测。跑一次 `mcpp build` 即可确立什么是当前的。
- `target/` 下不是指纹目录的东西 —— 例如 `mcpp pack` 的 `dist/` —— 从不被访问。

`--dry-run` 只列出,不删除。`--stale`、`--dry-run`、`--older-than` 三者任一都选中这一
档:`mcpp clean --older-than 3d` 是一次有范围的请求,不会被读成整删。`--older-than 0`
不保留任何未记录目录;负的时长被拒绝。

构建缓存是全机共享的,所以工程级命令不得清空它 —— `--stale` 与 `--bmi-cache` 不能同时
给出。`mcpp cache list` 列出占用。行没有排序,而 `0.0 B  (incomplete)` 那样的行,是被
中断的构建留下的条目:

```
$ mcpp cache list
key               kind          size       last used  package
8a150ad49d666f94  std       29.6 MiB          6d ago  std gcc@16.1.0 c++23 libstdc++
9234eed9ef786c13  std          0.0 B          2d ago  std  (incomplete)
```

`mcpp cache gc` 要求给出 `--max-size`、`--older-than` 或两者,并且只驱逐包条目。一份
`std` BMI 被机器上每个工程共享,实现以「重建它是用大量时间换少量磁盘」为由把它排除在
按体积驱逐之外。`mcpp cache clean --std` 仍是显式移除它的做法。

## 一个包发布了哪些版本

`mcpp search` 按子串匹配,并在每个命中行后附上该包发布的版本 —— 跨描述符的 per-OS 表
合并,按 semver 降序:

```
$ mcpp search imgui
  compat:imgui          Dear ImGui immediate-mode GUI library core sources  (1.92.8, 1.92.8-docking)
  mcpplibs:imgui        C++23 module package for Dear ImGui core and GLFW/OpenGL3 backends  (0.0.6, 0.0.5, 0.0.4, ...)
```

末尾的 `, ...` 表示被截断:默认显示三个,没有这个标记就说明列表是完整的。
`--all-versions` 打印全部。描述符读不到的包按两列输出 —— 版本列表是尽力而为的展示,
不会让 search 失败。

`mcpp add` 在名字解析不到时携带同样的信息。建议里给出该写的命名空间,以及它背后的版本:

```
  a package with this name exists under another namespace:
    compat.eui-neo (0.5.6, 0.5.5, 0.5.3)
```

这次扫描只在查找已经失败之后进行,结果只进入错误文本与 search 输出。裸名不会因此跨
命名空间解析。

## 解释一次解析

`mcpp why` 报告一次构建会解析出什么,并且不构建任何东西:

```
$ mcpp why toolchain
toolchain: gcc 16.1.0 (x86_64-linux-gnu)
  abi(libc)=glibc  cxxstdlib=libstdc++  arch=x86_64  os=linux  triple=x86_64-linux-gnu
  reason: [toolchain] in mcpp.toml if set, else platform-native default
```

话题是 `toolchain`、`runtime`、`deps` 或 `runners`,不给话题时四者全报。`--target` 与
`--toolchain` 把报告变成对当前目录并不使用的那一对的查询,目标矩阵正是这样逐格提问的。

诊断里的错误码可以用 `mcpp self explain` 展开:

```
$ mcpp self explain E0006
E0006: index requires a newer mcpp

The package index declares (index.toml [index].min_mcpp) that its
descriptors need a newer mcpp than this binary — parsing them would
silently misbehave, so resolution stops instead. Upgrade mcpp:
```

## 索引新鲜度与离线构建

`mcpp index status` 在不碰网络的前提下回答本地索引副本是否当前:

```
$ mcpp index status
  index      state    refreshed    revision     path
  xim        fresh    28s ago      1f4b39d      /home/speak/.mcpp/registry/data/xim-pkgindex
  mcpplibs   fresh    28s ago      d4b36d7      /home/speak/.mcpp/registry/data/mcpplibs
```

`mcpp index update` 刷新它们。一个刚发布几分钟、刷新后仍然找不到的包,是传播问题而不是
名字问题 —— 索引以 artifact 而非 git clone 的形式到达客户端。

`--offline`(或 `MCPP_OFFLINE=1`)在单次调用中禁止网络,宁可失败也不拉取。`--locked` 在
解析结果与 `mcpp.lock` 不一致时失败而不是改写它,这正是 CI 作业需要的形状。
`mcpp index pin <name> <rev>` 把自定义索引的某个 commit 记进 `mcpp.toml`;
`mcpp index unpin` 移除它。

## 发布前校验描述符

`mcpp xpkg parse` 用解析器自己的文法读描述符,所以它报告的就是解析时会看到的:

```
$ mcpp xpkg parse mcpp.plugins.lua
package    mcpp.plugins (namespace 'mcpp')
versions   linux    0.1.1, 0.1.0, latest
versions   macosx   0.1.1, 0.1.0, latest
versions   windows  0.1.1, 0.1.0, latest
form       A — no mcpp segment (build info from the source's mcpp.toml)
parse OK
```

per-OS 列表分开打印是有意的:一个版本只加进了某一个平台表而在其余表里被遗漏,在缺它的
平台上读起来就是「找不到」,而文件里明明含有这个版本字符串。`--json` 以同样的事实供脚本
使用:

```
$ mcpp xpkg parse mcpp.plugins.lua --json
{"namespace":"mcpp","name":"plugins","versions":{"linux":["0.1.1","0.1.0","latest"],"macosx":["0.1.1","0.1.0","latest"],"windows":["0.1.1","0.1.0","latest"]},"form":"A"}
```

`mcpp emit xpkg` 生成要提交的条目。完整路径见
[11 — 发布库到 mcpp-index](11-publishing-a-library.md)。

## 环境诊断

`mcpp self doctor` 检查工具链、`std` 模块、registry、缓存健康与最近一次运行期闭包判定,
并报告它查到了什么,而不只报告失败的部分:

```
$ mcpp self doctor
    Checking toolchain
          ok gcc 13.3.0 (x86_64-linux-gnu) at /usr/bin/g++
    Checking cache health
          ok build cache size = 2.5 GiB
warning: pre-v1 cache at '/home/speak/.mcpp/bmi' occupies 167.5 MiB and is no longer used — `mcpp cache clean --legacy` reclaims it
```

`mcpp self env` 打印路径与已解析的工具链,含 `--format json`。
`mcpp self config --mirror CN|GLOBAL` 选择下载镜像;mcpp 与 xlings 各自持有这个设置,
为其中一个选定不会为另一个选定。

## `[hooks]` —— 项目构建生命周期命令(实验性)

> **实验性。** Hook 目前**不能**决定一次构建成功与否。所有 Hook 失败都以
> **warning** 报出,`mcpp build` 保留它自己挣来的结果;`side_effect = true`
> 会被报错拒绝,而不是被采纳。这个键保留在 schema 里,这样今天写下的 manifest
> 在该功能转正时无需改动。另有两条限制是永久的、不是临时的:**只有根项目的 Hook
> 会执行**,而且**只有 `mcpp build` 会执行它们**。

Hook 是 `mcpp build` **在一段区间内持有**的命令,事件名就是那段区间:

```toml
[hooks]
build_start = "echo build started"
build_failed = "notify-send 'build failed'"
build_finished = "notify-send 'build finished'"

# 可选;以下是默认值。
timeout_seconds = 10
enabled = true
side_effect = false           # 实验期内 `true` 会被拒绝
```

| 键 | 类型 | 默认值 | 它命名的区间 |
|---|---|---:|---|
| `build_start` | 命令 | — | 项目准备完成后开启,命令退出时闭合 |
| `build_finished` | 命令 | — | 构建成功后开启,命令退出时闭合 |
| `build_failed` | 命令 | — | 构建失败后开启,命令退出时闭合 |
| `during_build` | 命令 | — | 构建开始前开启,构建结束后闭合 |
| `timeout_seconds` | 整数,1–86400 | `10` | 单次运行的时限 |
| `enabled` | 布尔 | `true` | 是否启用本表中的全部命令 |
| `side_effect` | 布尔 | `false` | Hook 失败是否让本次构建失败。**保留键**——实验期内只接受 `false` |

前三个区间是**自闭合**的——命令退出,区间就结束。"同步"在这里不是一种单独的模式,
它就是自闭合区间的样子。`during_build` 是唯一由别的东西闭合的区间,而那两个只对其中
一种形状有意义的键,是从这一点推出来的,不是额外规定的例外。

命令写成字符串;需要选项时写成表:

| 表内键 | 适用于 | 含义 |
|---|---|---|
| `cmd` | 所有事件 | 命令本身,必填 |
| `timeout_seconds` | 自闭合事件 | 覆盖本表默认值 |
| `loop` | `during_build` | 命令在区间闭合前退出时重新启动 |

`loop` 写在自闭合事件上、`timeout_seconds` 写在 `during_build` 上,都是**错误**而不是
被忽略的键:自闭合区间随命令退出而结束,没有东西可重启;而 `during_build` 已经由构建
定界。一个被接受却什么都不做的键,读起来就是"这功能坏了"。

命令通过宿主 Shell(`/bin/sh` 或 `cmd.exe`)执行,工作目录是**项目根目录**——不是敲
`mcpp build` 的那个目录,所以 Hook 里的相对路径在哪儿发起构建都指同一处。自闭合命令
的标准输入、输出和错误沿用普通终端行为。没有配置的事件直接跳过。

生命周期为:

```text
during_build 开启
build_start
    ├─ 构建成功 → during_build 闭合 → build_finished
    └─ 构建失败 → during_build 闭合 → build_failed
```

`during_build` 在终止 Hook **之前**闭合,因此两条命令不会重叠执行。

`build_failed` 与 `build_finished` 互斥,而且两者都只在 `build_start` 已经执行之后
才可达。项目**准备**阶段就失败的情况——manifest 非法、依赖无法解析、没有可用工具链
——一个 Hook 都不触发:此时构建尚未开始,而 Hook 程序本身可能正是准备阶段要装的东西。

命令无法启动、返回非零或超过时限均视为 Hook 失败。`during_build` 还多一种:开了 `loop`
的命令**起不来**——连续五次在一秒内以非零状态结束——就不再重启,并被报出来。(很快就
成功结束的命令,正是 `loop` 被要求重复的那件事,不算失败。)以上每一种都以 **warning**
报出,构建保留它自己挣来的结果——`[hooks]` 还在实验期,它没有投票权。Hook 自身失败不会
再触发另一个 Hook。

改变这一点的正是 `side_effect = true`,而今天写它是一个错误:

```text
error: mcpp.toml: error: [hooks].side_effect = true is not available yet:
[hooks] is experimental and cannot decide whether a build succeeded. …
```

是拒绝而不是悄悄降级,因为两种沉默的做法都更糟:采纳它等于让一个实验性功能对每一次
构建都有否决权;忽略它则让项目以为自己的构建被通知程序把着关,而实际上没有。功能转正
后,`true` 的含义是"Hook 失败让构建失败"——而构建自身失败时仍保留它自己的退出码,所以
`mcpp build` 不会把一次编译错误报成通知程序的问题。

关于 `during_build` 有两件事值得单独知道:

- **它的输出被丢弃**,因为它与构建并发写出,否则会插进某条编译诊断的中间。要看它的
  输出就跑 `mcpp build --verbose`。
- **停止的单位是进程树,不是进程。** `player & wait` 让播放器成为 mcpp 所启动那条命令
  的孙子进程,只停掉后者会让音频设备在构建结束后仍被占着。mcpp 把命令放进它自己的
  进程组(Windows 上是 job object)并停止整组,构建被 Ctrl-C 打断时也一样。

作用范围:

- 只有 `mcpp build` 执行 Hook。`mcpp run`、`mcpp test` 和
  `mcpp build --configure-only` 同样会构建,但有意不执行。
- Hook 属于**被构建的那个包**。workspace 展开时就是逐个成员:各自的 `[hooks]`、
  各自的构建、各自的根目录。**虚拟** workspace 根(只有 `[workspace]` 没有
  `[package]`)不构建任何东西,写在那里的 `[hooks]` 永不触发。
- 依赖的 `[hooks]` **一律跳过**,只有根项目的会执行。mcpp 解析的每一份 manifest 都
  带着这一节,依赖的也带,而没有任何东西去读它——这正是"装一个包"不会变成"在我下次
  构建时跑包作者的 Shell 命令"的原因。这是设计的性质,不是一个等着被打开的默认值。
- 声明了生效的 Hook 就等于让项目放弃空转快路径,因为 `build_start` 规定在准备阶段之后
  执行。对已经是最新状态的带 Hook 项目,`mcpp build` 的代价是一次准备,而不是毫秒级。

`[hooks]` 里、以及某个事件表里不认识的**键**都是 warning(`--strict` 下为错误),所以为更新版 mcpp 写的
manifest 在这一版仍能加载;不认识的**值**——`cmd` 缺失或不是字符串、`timeout_seconds` 不在
1–86400 之间、键写给了错误的区间——是 manifest 错误。

> **Hook 是代码,而 `mcpp.toml` 是仓库的一部分。** 构建一个刚克隆下来的项目,会以
> 执行 `mcpp build` 的那个账户的权限,运行它 `[hooks]` 里写的任何东西。这与
> `build.mcpp`([30 — build.mcpp](30-build-mcpp.md))已经要求的信任是同一份;
> `[hooks]` 扩大的是它的范围,而不是引入了一份新的信任。

Hook 程序可以作为普通 xlings 依赖安装。例如,音频通知程序可以把音频内置进自己的
可执行文件,无需让 mcpp 处理媒体资源:

```toml
[hooks]
build_finished = "mcpp-hooks-audioplayer niulai-mm"
build_failed = "mcpp-hooks-audioplayer niulai-niulai"
side_effect = false

[xlings]
deps = ["xim:mcpp-hooks-audioplayer@0.0.1"]
```

根据构建成功或失败播放不同提示音。`side_effect = false` 写出来而不是靠默认值:它是
这份 manifest 自己就想要的值——缺个音频设备不该让构建失败——所以等这个键有了不止
一个可接受的值之后,它仍然会这么写。

## 当前边界

- `mcpp why --format json` 只对 `toolchain` 话题有定义。其余话题报
  `'<topic>' has no machine-readable shape yet` 并以非零退出。
- `mcpp search` 按子串匹配;没有字段选择器,也没有把搜索限定到单个命名空间的方式。
- `mcpp clean --stale` 读 `target/.build_cache`,它保存的近期条目数量有上限。一个工程
  如果构建过的 (目标, profile) 组合多于这个上限,最旧的条目会被挤掉;条目被挤掉的目录
  随后按未记录处理 —— 在 `--older-than` 之内保留,超出则删除。
- `mcpp cache gc --older-than 0` 以 `bad --older-than value '0'
  (expected <N>{s,m,h,d})` 被拒绝,而 `mcpp clean --stale --older-than 0` 接受。
  两个选项共用一个 parser,但这一种取值上不一致。

`mcpp emit xpkg` 写出一个 `mcpp xpkg parse` 不认识的键。对一个自带 `mcpp.toml`
的包,产出的 `mcpp` 段以 `manifest = "mcpp.toml"` 结尾,而描述符解析器把它报为
未知键:

```
error: unknown mcpp-segment key 'manifest' — silently ignored at build time
       by this mcpp version
error: synthesised manifest missing sources (mcpp segment must declare
       `sources = { ... }`)
```

第二个错误由第一个导出:键被忽略,于是没有从它点名的那份 manifest 推导出任何
源。手工补上 `sources = { … }` 只消掉第二个,消不掉第一个,`mcpp xpkg parse`
仍然以 1 退出。

`mcpp-index` 里没有任何描述符使用那个键 —— 218 个里 0 个。自带 `mcpp.toml` 的包
**整个省略 `mcpp` 字段**,由 mcpp 在版本目录下查找那份 manifest。实测于
2026.9.8.1。
