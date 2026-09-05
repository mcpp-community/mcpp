# 21 — 按场景选命令

命令清单是 `mcpp --help`,每个子命令还有自己的 `--help`。本章回答的是另一个问题:
某个情形已经发生时该用哪条命令 —— 构建目录一直变大、解析结果出乎意料、描述符即将
发布、索引可能陈旧。这里收的都是名字本身没有说出它所属场景的命令。

相关文档:[00 — 快速开始](00-getting-started.md)(日常构建与测试循环)、
[03 — 工具链管理](03-toolchains.md)、
[10 — 发布库到 mcpp-index](10-publishing-a-library.md)、
[11 — 机器可读输出](11-machine-output.md)。

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
[10 — 发布库到 mcpp-index](10-publishing-a-library.md)。

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
