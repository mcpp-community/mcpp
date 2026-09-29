# 09 —— 按场景选命令

**读者：** 已经认识那些名词、现在想找动词的人。

**本章回答的那一个问题：** 手上这件事该用哪条命令 —— 回收磁盘、解释一次
解析、校验一个描述符、诊断环境。

**不在这里：** 每条命令含义上的细节。一个场景点名命令，并链接到拥有它的
那一章。在此之前：[08 —— 测试](08-testing.md)。

命令清单是 `mcpp --help`，每个子命令还带自己的 `--help`。本章回答的是另一个
问题：某个情形已经发生时该用哪条命令 —— 构建目录一直在变大、一次解析结果
出乎意料、一个描述符即将发布、一份索引可能已经陈旧。这里收的都是名字本身
没有说出它所属场景的命令。

相关文档：[01 —— 快速开始](01-getting-started.md)（日常构建与测试循环）、
[20 —— 工具链管理](20-toolchains.md)、
[11 —— 发布一个库](11-publishing-a-library.md)、
[50 —— 机器可读输出](50-machine-output.md)。

下面每一段输出，都由本章所对应版本的 mcpp 实际产生。

## 回收磁盘而不触发重编

有两个存储会增长，增长的原因不同，各由一条命令清空。把两者弄混，代价是一次
全量重编。

| 存储 | 作用域 | 增长时机 | 清空方式 |
|---|---|---|---|
| `target/<三元组>/<指纹>/` | 单个工程 | 一个配置指纹变化，开出一个新目录 | `mcpp clean`、`mcpp clean --stale` |
| 构建缓存（`mcpp cache dir`） | 整台机器 | 任何工程编译一个依赖、一个 `std` 模块，或构建一个 host 工具 | `mcpp cache gc`、`mcpp cache prune`、`mcpp cache clean` |

`mcpp clean` 整个删掉 `target/`，下次构建重编一切。`mcpp clean --stale`
只删已无构建记录使用的指纹目录，在用的配置保留：

```
$ mcpp clean --stale --dry-run
would remove target/x86_64-linux-gnu/0123456789abcdef  (0.0 B)
Would remove 1 directory (0.0 B)
```

"在用"指被 `target/.build_cache` 记录 —— 它由 `mcpp build` 写入，由快
路径读取。这个定义带来三个推论：

- 一个没有记录的目录，不会仅因此就被删除。`mcpp test` 走的构建路径不写
  记录，`--no-cache` 构建同样不写。未被记录、但写在 `--older-than`（默认
  一天）之内的目录会被保留；更旧的会被删，判断出错的代价是重编一个此后
  无人碰过的配置。
- 完全没有记录时，命令拒绝执行，而不是去猜。跑一次 `mcpp build` 就能确立
  什么是当前的。
- `target/` 下不是指纹目录的东西 —— 例如 `mcpp pack` 的 `dist/` —— 从不
  被访问。

`--dry-run` 只列出，不删除。`--stale`、`--dry-run`、`--older-than` 三者
任一都选中这一档：`mcpp clean --older-than 3d` 是一次有范围的请求，不会被
读成一次整删。`--older-than 0` 不保留任何未记录的目录；负的时长被拒绝。

构建缓存是全机共享的，因此工程级命令不得清空它 —— `--stale` 与
`--bmi-cache` 不能同时给出。`mcpp cache list` 列出占用。行没有排序，而像
`0.0 B  (incomplete)` 那样的行，是被中断的构建留下的条目：

```
$ mcpp cache list
key               kind          size       last used  package
8a150ad49d666f94  std       29.6 MiB          6d ago  std gcc@16.1.0 c++23 libstdc++
9234eed9ef786c13  std          0.0 B          2d ago  std  (incomplete)
```

`mcpp cache gc` 要求给出 `--max-size`、`--older-than` 或两者，并且只驱逐
包条目。一份 `std` BMI 被机器上每个工程共享，实现以"重建它是用大量时间
换少量磁盘"为由，把它排除在按体积驱逐之外。`mcpp cache clean --std` 仍是
显式移除它的做法。

## 一个包已发布的版本

`mcpp search` 按子串匹配，并在每个命中行后附上该包发布的版本 —— 跨描述符
的 per-OS 表合并，按 semver 降序：

```
$ mcpp search imgui
  compat:imgui          Dear ImGui immediate-mode GUI library core sources  (1.92.8, 1.92.8-docking)
  mcpplibs:imgui        C++23 module package for Dear ImGui core and GLFW/OpenGL3 backends  (0.0.6, 0.0.5, 0.0.4, ...)
```

末尾的 `, ...` 标记截断：默认显示三个版本，没有这个标记就说明列表是完整的。
`--all-versions` 打印全部。一个描述符读不到的包，按两列输出 —— 版本列表是
尽力而为的展示，从不会让 search 失败。

`mcpp add` 在一个名字解析不到时，携带同样的信息。建议里给出该写的命名
空间，以及它背后的版本：

```
  a package with this name exists under another namespace:
    compat.eui-neo (0.5.6, 0.5.5, 0.5.3)
```

这次扫描只在查找已经失败之后进行，结果只进入错误文本与 search 输出。裸名
不会因此跨命名空间解析。

## 为一次调用换一个工具链

`mcpp build`、`mcpp run`、`mcpp test` 与 `mcpp pack` 接受
`--toolchain <spec>`，它为这一次调用选择编译器，不写入任何东西：

```bash
mcpp test --toolchain llvm@22.1.8
mcpp run --toolchain gcc@16.1.0
mcpp pack --toolchain llvm@22.1.8 --format dir
```

对这一次调用，这个选项取代 `mcpp.toml` 中的 `[toolchain] default`，优先级
与 [20 —— 工具链管理](20-toolchains.md) 给 `MCPP_TOOLCHAIN` 的那一级相同。
每个工具链构建到它自己的输出目录，一次记录下来的构建只为记录它的那个工具链
请求重放。

## 解释一次解析

`mcpp why` 报告一次构建会解析出什么，并且不构建任何东西：

```
$ mcpp why toolchain
toolchain: gcc 16.1.0 (x86_64-linux-gnu)
  abi(libc)=glibc  cxxstdlib=libstdc++  arch=x86_64  os=linux  triple=x86_64-linux-gnu
  reason: [toolchain] in mcpp.toml if set, else platform-native default
```

`mcpp why deps` 在 `mcpp.lock` 各行之前列出解析出的依赖图（2026.9.14.2+）:
每个包、每条请求书写时用的键与所在的表，以及库的链接形态与其原因。锁文件
不记录的 `path` 依赖同样列出：

```
$ mcpp why deps
dependency graph:
  mcpplibs.app@0.1.0  (root)  path+/work/app
  huxdemo.fw@0.1.0  path+/work/fw
      requested by mcpplibs.app@0.1.0 as 'huxdemo.fw' in [dependencies]
      requested by huxdemo.comp@0.1.0 as 'fw' in [dependencies]
      linked static (default)
```

同一张图记录在 `target/<triple>/<fp>/resolution.json` 的 `graph` 下，
每个包一条，根在最前：`package`（规范身份、命名空间、名字、版本、来源）、
`root`、`requested_by`(`requester`、`key`、`table`)，对库还有 `link`
(`form`、`reason`)。

话题是 `toolchain`、`runtime`、`deps` 或 `runners`，不给话题时四者全报。
`--target` 与 `--toolchain` 把报告变成对当前目录并不使用的那一对的查询，
一个目标矩阵正是这样逐格提问的。

诊断里的一个错误码，可以用 `mcpp self explain` 展开：

```
$ mcpp self explain E0006
E0006: index requires a newer mcpp

The package index declares (index.toml [index].min_mcpp) that its
descriptors need a newer mcpp than this binary — parsing them would
silently misbehave, so resolution stops instead. Upgrade mcpp:
```

## 索引新鲜度与离线构建

`mcpp index status` 在不碰网络的前提下，回答本地索引副本是否当前：

```
$ mcpp index status
  index      state    refreshed    revision     path
  xim        fresh    28s ago      1f4b39d      /home/speak/.mcpp/registry/data/xim-pkgindex
  mcpplibs   fresh    28s ago      d4b36d7      /home/speak/.mcpp/registry/data/mcpplibs
```

`mcpp index update` 刷新它们。一个刚发布几分钟、刷新后仍然找不到的包，
是传播问题而不是命名问题 —— 索引以 artifact 而非 git clone 的形式到达
客户端。

索引可以要求比正在运行的 mcpp 更新的版本（`index.toml` 的 `min_mcpp`）。
这不是本次运行的错误（2026.9.28.1+）。一次刷新若取回这样的索引，会保留先前
的副本，运行以一行结尾：

```
tip: the refreshed package index `mcpplibs` requires a newer mcpp; this run used the previous index. It requires mcpp >= 2026.10.1.1; this is mcpp 2026.9.28.1. Upgrade: xlings update mcpp
```

没有刷新索引的运行不提及它。因某个包只由这样的索引提供而失败的运行，在使它
停止的消息中给出 E0006。`mcpp self doctor` 列出正在运行的 mcpp 不满足其下限
的每一个索引。更早的版本在读到这样的索引时，把 E0006 的文字作为 `error:`
打印在运行开头，其后成功的运行也是如此。

`--offline`（或 `MCPP_OFFLINE=1`）在单次调用中禁止网络，宁可失败也不拉取。
`--locked` 在解析结果与 `mcpp.lock` 不一致时失败，而不是改写它，这正是 CI
作业需要的形状。`mcpp index pin <name> <rev>` 把一个自定义索引的某个
commit 记进 `mcpp.toml`;`mcpp index unpin` 移除它。

## 下载进度

每一次获取都由同一个渲染器报告（2026.9.28.1+）：

- 工具链或载荷的安装；
- 来自索引的库包；
- `[xlings]` 载荷；
- 索引刷新；
- `git` 依赖的克隆；
- 沙箱首次运行时的工具。

在终端上，每一项是一个原地重绘的进度条。标准输出不是终端时（例如 CI 日志或
管道），每一项在开始时打印一行（已知时带上大小），结束时打印一行（带上耗时）。
这样的输出不含回车符，也不含擦除序列。`--quiet` 两者都不打印。

当 mcpp 驱动的 xlings 为索引刷新发出进度事件时（xlings 2026.9.28.1+），索引刷新
逐步报告。较旧的 xlings 下，它显示其状态行，然后安静地结束，与以前相同。

## 构建输出

一次构建在每个包开始做事时写出它的一行，并用一行状态行陈述构建的进展（2026.9.30.1 起）：

```console
$ mcpp build
   Workspace building member 'xlings'
  build.mcpp mcpplibs.xpkg v0.0.59        ran 0.64s
      Cached compat.ftxui v6.1.9 (73 units)
      Cached mcpplibs.cmdline v0.0.2 (3 units)
   Compiling cancellation v0.1.0 (modules/cancellation)
   Compiling platform v0.1.0 (modules/platform)
   Compiling mcpplibs.xpkg v0.0.59
   Compiling xlings v2026.9.29.1 (.)

    Finished dev [unoptimized + debuginfo] in 33.63s · plan 3.06s · programs 0.64s · build 29.94s
```

- **何时写出**：包的第一个步骤完成时（依赖扫描不计），或它的第一个 `check`、`prepare` 动作开始时，写出该包的行；此后这一行不再改变。
- **Cached**：`Cached` 表示该依赖的单元由全局构建缓存提供，并给出单元数。
- **不写出的包**：本次无事可做的包没有行。
- **项目内的包**：根包、工作区成员、位于项目根之下的 path 依赖，以短名、版本和目录命名。
- **其余的包**：以完整身份命名。
  - 索引包写出命名空间和名字；若由项目声明的其他索引提供，再附 `(index <名字>)`。
  - git 依赖附其引用。
  - 项目外的 path 依赖附其相对目录。
- **来源配色**：在终端上，名字的颜色表示来源。官方索引为青色，其他索引为品红，git 仓库为蓝色，项目自身的包为默认色。
- **构建程序**：构建程序在运行或失败时有一行，并给出耗时。结果被复用的构建程序只在 `--verbose` 下有行。
- **失败**：失败的步骤在失败时即报告：先写 `error: build failed in <包>`，再写它的诊断信息；与此同时 ninja 等待仍在运行的步骤。
- **Finished**：`Finished` 给出整个命令的耗时。命令耗时达到十秒时，还说明时间的构成；若某一步骤占构建时间的四分之一以上，则给出该步骤。

在终端上，输出下方画一行状态行，并原地更新：

```
   Compiling platform v0.1.0 (modules/platform)
    Building ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢾⡷⠀⠰⣿⠆⠄⠄⠄⠄⠄⠄⠄⠄ 612/707 · 0:35 · gpp.gui: CMAKE ElaWidgetTools 6:10
```

- **阶段**：阶段（`Planning`，构建程序阶段为 `Running`，`Building`，失败后为 `Stopping`，`Checking`）与上方的动词对齐。
- **点阵屏**：阶段之后是由 24 个盲文点字格组成的点阵屏，每次命令随机播放四种动画之一。
  - 吃豆人：位置即进度。
  - 贪吃蛇：每有一个包开始，就吃下一颗该包来源颜色的食物。
  - 横版俄罗斯方块：堆的面积即进度。
  - 离子发射器：离子堆积成进度条。

  动画在 mcpp 工作时缓慢移动，有步骤完成时加快，构建等待时静止。
- **计数与时间**：随后是已完成与计划的步骤数，以及自命令开始的时间。当没有待启动的步骤时，追加 `last N running`。
- **正在运行的动作**：最后是运行最久的 `check` 或 `prepare` 动作。其余步骤只在完成时由 ninja 报告。
- **绘制方式**：状态行在命令开始半秒后才首次绘制；每次更新都以一次写入原地覆盖，不会闪烁。

`MCPP_PROGRESS` 选择点阵屏的内容：

- `random`：默认值；
- 动画名：`chomp`、`snake`、`stack` 或 `ions`；
- `plain`：保留状态行，不显示点阵屏；
- `off`：不绘制实时行，与日志输出相同。

点阵屏需要能绘制盲文点字的终端，即 UTF-8 locale 或 Windows Terminal；否则状态行不含点阵屏。输出不是终端（CI 日志、管道）时，只写出最终的行，并在输出静默一分钟时写出状态行。在终端上设置 `TERM=dumb` 同样选择这种形式。

`--play-game` 在构建期间于点阵屏上玩一个游戏。`build`、`run`、`test` 都接受该选项；`--play-game=NAME` 指定游戏，否则随机选择：

- `snake`：方向键控制方向；
- `stack`：上下键移动方块，左键直接落下，右键或空格旋转，填满的一列会消除；
- `runner`：空格或上键跳跃。

```console
$ mcpp build --play-game=snake
    Building ⠀⠀⠀⢲⠈⠀⠀⠀⠀⠀⠠⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀ 3/7 · 0:12 · snake 4
...
    Finished dev [unoptimized + debuginfo] in 41.20s
      Played snake · best 9
```

- **速度与计数**：游戏以自身的速度运行，旁边的计数陈述构建的进展。
- **按键读取**：按键不回显；Ctrl-C 仍然中断构建。
- **终端模式**：构建结束或被中断时，终端模式会恢复；被强制杀死的进程无法恢复，此时可执行 `stty sane`。
- **使用条件**：游戏要求标准输入和标准输出都是终端；否则写出一行说明原因，构建照常进行。

`--verbose` 列出每个包：无事可做的包记为 `Fresh`，做了事的包记为 `Compiled`，并给出其步骤数和耗时跨度。它还给出每个构建程序的编译与运行时间，并按 ninja 的报告打印每个步骤（`[f/t] <命令>` 及其输出）。`--quiet` 不输出以上内容。机器输出（`--message-format json`）不变。

## 发布前校验描述符

`mcpp xpkg parse` 用解析器自己的文法读一个描述符，所以它报告的就是解析时
会看到的：

```
$ mcpp xpkg parse mcpp.plugins.lua
package    mcpp.plugins (namespace 'mcpp')
versions   linux    0.1.1, 0.1.0, latest
versions   macosx   0.1.1, 0.1.0, latest
versions   windows  0.1.1, 0.1.0, latest
form       A — no mcpp segment (build info from the source's mcpp.toml)
parse OK
```

per-OS 列表分开打印是有意的：一个版本只加进了某一个平台表、在其余表里被
遗漏，在缺它的平台上读起来就是"找不到"，而文件里明明含有这个版本字符串。
`--json` 以同样的事实供脚本使用：

```
$ mcpp xpkg parse mcpp.plugins.lua --json
{"namespace":"mcpp","name":"plugins","versions":{"linux":["0.1.1","0.1.0","latest"],"macosx":["0.1.1","0.1.0","latest"],"windows":["0.1.1","0.1.0","latest"]},"form":"A"}
```

`mcpp emit xpkg` 生成要提交的条目。完整路径见
[11 —— 发布一个库](11-publishing-a-library.md)。

## 环境诊断

`mcpp self doctor` 检查工具链、`std` 模块、registry、缓存健康、最近一次
运行期闭包判定，以及已安装的 GCC 载荷里是否留有构建它的那台机器自己的
fixincludes 冻结头文件，并报告它查到了什么，而不只报告失败的部分：

```
$ mcpp self doctor
    Checking toolchain
          ok gcc 13.3.0 (x86_64-linux-gnu) at /usr/bin/g++
    Checking cache health
          ok build cache size = 2.5 GiB
warning: pre-v1 cache at '/home/speak/.mcpp/bmi' occupies 167.5 MiB and is no longer used — `mcpp cache clean --legacy` reclaims it
```

`mcpp self env` 打印路径与已解析的工具链，包括 `--format json`。
`mcpp self config --mirror CN|GLOBAL` 选择下载镜像；mcpp 与 xlings 各自
持有这个设置，为其中一个选定，不会为另一个选定。

## `[hooks]` —— 项目构建生命周期命令（实验性）

> **实验性。** Hook 目前**不能**决定一次构建是否成功。每一次 Hook 失败
> 都以 **warning** 报出，`mcpp build` 保留它自己挣来的结果；
> `side_effect = true` 会被报错拒绝，而不是被采纳。这个键留在 schema 里，
> 这样今天写下的 manifest，在该功能转正时不必改动。另有两条限制是永久的，
> 不是临时的：**只有根项目的 Hook 会执行**，而且**只有 `mcpp build`
> 会执行它们**。

Hook 是 `mcpp build` **在一段区间内持有**的命令，事件名就是那段区间：

```toml
[hooks]
build_start = "echo build started"
build_failed = "notify-send 'build failed'"
build_finished = "notify-send 'build finished'"

# Optional; these are the defaults.
timeout_seconds = 10
enabled = true
side_effect = false           # `true` is refused while this is experimental
```

| 键 | 类型 | 默认值 | 它命名的区间 |
|---|---|---:|---|
| `build_start` | 命令 | — | 项目准备完成后开启，命令退出时闭合 |
| `build_finished` | 命令 | — | 构建成功后开启，命令退出时闭合 |
| `build_failed` | 命令 | — | 构建失败后开启，命令退出时闭合 |
| `during_build` | 命令 | — | 构建开始前开启，构建结束后闭合 |
| `timeout_seconds` | 整数，1–86400 | `10` | 单次运行的时限 |
| `enabled` | 布尔 | `true` | 是否启用这张表里的全部命令 |
| `side_effect` | 布尔 | `false` | Hook 失败是否让本次构建失败。**保留键** —— 实验期内只接受 `false` |

前三个区间是**自闭合**的 —— 命令一退出，区间就结束。"同步"在这里不是一种
单独的模式，它就是自闭合区间的样子。`during_build` 是唯一由别的东西闭合的
区间，而那两个只对其中一种形状有意义的键，是从这一点推出来的，不是额外
规定的例外。

一条命令写成字符串，需要选项时写成一张表：

| 表内键 | 适用于 | 含义 |
|---|---|---|
| `cmd` | 所有事件 | 命令本身，必填 |
| `timeout_seconds` | 自闭合事件 | 覆盖这张表的默认值 |
| `loop` | `during_build` | 命令在区间闭合之前退出时重新启动 |

`loop` 写在一个自闭合事件上、`timeout_seconds` 写在 `during_build` 上，
都是**错误**，而不是被忽略的键：自闭合区间随它的命令退出而结束，没有东西
可重启；而 `during_build` 已经由构建本身定界。一个被接受、却什么都不做的
键，读起来就是"这功能坏了"。

命令通过宿主 Shell（`/bin/sh` 或 `cmd.exe`）执行，工作目录是**项目根
目录** —— 不是敲下 `mcpp build` 的那个目录，所以 Hook 里的相对路径，不论
在哪里发起构建都指同一处。一个自闭合命令沿用普通终端的输入、输出。没有
配置的事件直接跳过。

生命周期为：

```text
during_build opens
build_start
    ├─ build succeeds → during_build closes → build_finished
    └─ build fails    → during_build closes → build_failed
```

`during_build` 在终止 Hook **之前**闭合，因此这两条命令从不重叠执行。

`build_failed` 与 `build_finished` 互斥，而且两者都只在 `build_start` 已经
执行之后才可达。项目**准备**阶段就失败的情形 —— manifest 非法、依赖无法
解析、没有可用工具链 —— 一个 Hook 都不触发：构建此时尚未开始，而 Hook
程序本身可能正是准备阶段要装的那个东西。

一个 Hook 命令无法启动、返回非零，或超过时限，都算作一次 Hook 失败。
`during_build` 还多一种：一个开了 `loop` 的命令**起不来** —— 连续五次
在一秒之内以非零状态结束 —— 就不再被重启，并被报出来。（一个很快就成功
结束的命令，正是 `loop` 被要求重复的那件事，不算失败。）以上每一种都以
**warning** 报出，构建保留它自己挣来的结果 ——`[hooks]` 还在实验期，它
没有投票权。一个 Hook 自身的失败不会触发另一个 Hook。

改变这一点的正是 `side_effect = true`，而今天写下它是一个错误：

```text
error: mcpp.toml: error: [hooks].side_effect = true is not available yet:
[hooks] is experimental and cannot decide whether a build succeeded. …
```

是拒绝而不是悄悄降级，因为两种沉默的做法都更糟：采纳它，等于让一个实验性
功能对每一次构建都有否决权；忽略它，则让项目误以为自己的构建被一个通知
程序把着关，而实际上没有。功能转正之后，`true` 的含义将是"一次 Hook 失败
让构建失败" —— 而构建自身失败时，仍会保留它自己的退出码，所以 `mcpp build`
从不会把一次编译错误报成通知程序的问题。

关于 `during_build` 命令，有两件事值得单独知道：

- **它的输出被丢弃**，因为它与构建并发写出，否则会插进某条编译诊断的
  中间。要看它的输出，跑 `mcpp build --verbose`。
- **停止的单位是进程树，不是单个进程。** `player & wait` 让播放器成为
  mcpp 所启动那条命令的孙子进程，只停掉后者，会让音频设备在构建结束后
  仍被占用。mcpp 把命令放进它自己的进程组（Windows 上是 job object）
  并停止整组，构建被 Ctrl-C 打断时也一样。

作用范围，精确地说：

- 只有 `mcpp build` 执行 Hook。`mcpp run`、`mcpp test` 与
  `mcpp build --configure-only` 同样会构建，但有意不执行 Hook。
- Hook 属于**被构建的那个包**。workspace 展开时，就是逐个成员各自的
  `[hooks]`、各自的构建、各自的根目录。**虚拟** workspace 根（只有
  `[workspace]` 没有 `[package]`）不构建任何东西，写在那里的 `[hooks]`
  永不触发。
- 一个依赖的 `[hooks]` **一律跳过**，只有根项目的会执行。mcpp 解析的每
  一份 manifest 都带着这一节，依赖的也带，而没有任何东西去读它 —— 这正是
  "装一个包"不会变成"在我下次构建时跑包作者的 Shell 命令"的原因。这是
  设计本身的性质，不是一个等着被打开的默认值。
- 声明了一个生效的 Hook，就等于让项目放弃空转快路径，因为 `build_start`
  规定要在准备阶段之后执行。对一个已经是最新状态、又带 Hook 的项目，
  `mcpp build` 的代价是一次准备，而不是毫秒级。

`[hooks]` 里、以及某个事件表里不认识的**键**都是 warning（`--strict` 下
为错误），因此为更新版 mcpp 写的 manifest，在这一版仍能加载；不认识的
**值** —— `cmd` 缺失或不是字符串、`timeout_seconds` 不在 1–86400 之间、
一个键写给了错误的区间 —— 是 manifest 错误。

> **Hook 是代码，而 `mcpp.toml` 是仓库的一部分。** 构建一个刚克隆下来的
> 项目，会以执行 `mcpp build` 的那个账户的权限，运行它 `[hooks]` 里写的
> 任何东西。这与 `build.mcpp`([30 —— build.mcpp](30-build-mcpp.md))
> 已经要求的信任是同一份；`[hooks]` 扩大的是它的范围，而不是引入了一份
> 新的信任。

Hook 程序可以作为普通的 xlings 依赖安装。例如，一个音频通知程序可以把
音频文件内置进自己的可执行文件，不必让 mcpp 处理媒体资源：

```toml
[hooks]
build_finished = "mcpp-hooks-audioplayer niulai-mm"
build_failed = "mcpp-hooks-audioplayer niulai-niulai"
side_effect = false

[xlings.workspace]
"xim:mcpp-hooks-audioplayer" = "0.0.1"
```

构建成功或失败播放不同的提示音。`side_effect = false` 被明确写出来，而
不是留给默认值：它是这份 manifest 自己就想要的值 —— 缺一个音频设备不该
让构建失败 —— 所以等这个键有了不止一个可接受的值之后，它仍然会这么写。

## 失败所属的阶段

一次构建要跨过若干阶段，而一条消息会点名失败的那一段。先读出这一点，能省
下打开错误章节的功夫。

| 消息里出现 | 阶段 | 参考章节 |
|---|---|---|
| 一个包名、一个版本，或"没有候选" | 解析 | [05](05-dependencies.md)、[11](11-publishing-a-library.md) |
| 一次下载、一份载荷，或一条版本下界 | 供给 | [20](20-toolchains.md)、[23](23-the-project-environment.md) |
| 一个三元组，或"不支持的目标" | 目标 | [21](21-the-target-triple.md) |
| 一个模块读不到，或没有人提供它 | 模块图 | [30](30-build-mcpp.md) |
| 工程自己某个文件内部的编译或链接错误 | 一段都不是 | 编译器自己的消息 |

最后一行是最有用的一行：当错误是关于代码本身时，mcpp 的任何一段都没有
参与，它的文档不会有帮助。

## 当前边界

- `mcpp why --format json` 只对 `toolchain` 话题有定义。其余话题报
  `'<topic>' has no machine-readable shape yet` 并以非零退出。
- `mcpp search` 按子串匹配；没有字段选择器，也没有把搜索限定到单个命名
  空间的办法。
- `mcpp clean --stale` 读 `target/.build_cache`，它保存的近期条目数量
  有上限。一个工程如果构建过的（目标， profile）组合数超出这个上限，最旧的
  条目会被挤掉；条目被挤掉的目录随后按未记录处理 —— 在 `--older-than`
  之内保留，超出之后删除。
- `mcpp cache gc --older-than 0` 以 `bad --older-than value '0'
  (expected <N>{s,m,h,d})` 被拒绝,而 `mcpp clean --stale --older-than 0`
  接受同样的值。两个选项共用一个 parser，但这一种取值上不一致。

`mcpp emit xpkg` 写出一个 `mcpp xpkg parse` 不认识的键。对一个自带
`mcpp.toml` 的包，产出的 `mcpp` 段以 `manifest = "mcpp.toml"` 结尾，而
描述符解析器把它报为未知键：

```
error: unknown mcpp-segment key 'manifest' — silently ignored at build time
       by this mcpp version
error: synthesised manifest missing sources (mcpp segment must declare
       `sources = { ... }`)
```

第二个错误由第一个导出：键被忽略，于是没有从它点名的那份 manifest 推导出
任何源。手工补上 `sources = { … }` 只能消掉第二个，消不掉第一个，
`mcpp xpkg parse` 仍然以 1 退出。

`mcpp-index` 里没有任何描述符使用那个键 —— 218 个里 0 个。自带
`mcpp.toml` 的包**整个省略 `mcpp` 字段**，由 mcpp 在版本目录下查找那份
manifest。实测于 2026.9.8.1。
