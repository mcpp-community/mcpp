# 25 —— home、索引与 store

**读者：** 决定 mcpp 把安装内容放在哪里的人：已有 `~/.xlings` 的开发者、多人共用机器的管理员、Luban 系统。

**本章回答的那一个问题：** mcpp home 中有什么，各部分归哪个程序所有，一个 home 如何复用另一个 home 已有的包。

**不在这里：** 选择工具链见 [20 —— 工具链管理](20-toolchains.md)；项目自己的构建环境见
[23 —— 项目环境](23-the-project-environment.md)。

mcpp 经由其包提供者 xlings 安装工具链与包。payload（一个包的一个已安装版本）只由提供者写入，此后不再被修改：mcpp
不修补、不标记、不复制它。构建需要而 payload 中没有的东西（加载器、运行路径、sysroot）由构建自己的命令行给出。

## 1. 布局

```
~/.mcpp/                    (MCPP_HOME)
  config.toml
  indices/<name>/           mcpp 所服务的每个索引的数据（mcpplibs、[index.repos.<name>]）
  xpkgs/                    这些索引所服务的 payload
  registry/                 提供者自己的 home：其索引（xim）、运行环境与 store
    data/xpkgs/             工具链与工具；条目可以是指向上游 payload 的链接
  build-cache/  cache/  provisioned/ …

<project>/.mcpp/
  indices/<name>/           项目自己的 [indices] 的数据
  xpkgs/                    它们所服务的 payload
```

归属按索引划分。mcpp 所服务的索引，其数据与 payload 在 mcpp 的 home 中；提供者自己的索引（`xim` 及其附带的索引）留在
`registry/` 下。本地路径索引（`path = "…"`）在原处读取。

早期 mcpp 建立的 home 继续可用。其索引数据在第一次运行时移动到 `indices/<name>`，只移动一次；已安装在
`registry/data/xpkgs` 下的 payload 留在原处且仍能找到，基于它们构建的产物继续可以运行。早期 mcpp 写进 payload
的标记（`.mcpp_ok`、`.mcpp-fixup.json`）不再被读取。

## 2. 刷新索引

mcpp 所有的索引被拉取到它旁边的暂存目录，在那里按其声明的下限（`index.toml` 的 `min_mcpp`）检查，只有当前 mcpp
能读取时才与正在使用的副本交换。需要更新的 mcpp 的索引保留之前的副本，并在命令自身输出之后报告一次。

## 3. 复用另一个 home：上游

```toml
# ~/.mcpp/config.toml
[xlings]
upstream = ["~/.xlings"]
```

上游是本 home **以链接方式**从中安装的 home：上游中身份一致（命名空间、名字、版本与配方修订号）且完整的 payload，
以符号链接（Windows 上为 junction）的形式发布到本 home 的 store。不下载任何内容，上游永不被写入。在本 home
删除该包只删除链接。上游的 payload 消失后，下一次安装在同一路径上安装本 home 自己的 payload，基于该路径构建的产物继续可用；
`--offline` 下无法安装，命令失败。

| 上游 | 提供 |
|---|---|
| xlings home（`~/.xlings`、`/xlings`） | 提供者自己的包（`xim`） |
| mcpp home（另一个 `~/.mcpp`、系统 home） | 其 registry 中的 `xim` 包，以及其 `xpkgs` 中 mcpp 索引的包 |

mcpp 自己的包只从 mcpp home 借用，从不从 xlings home 借用。

**信任。** 上游属于当前用户或系统（root；Windows 上还包括 Administrators 与 SYSTEM），不属于当前用户时，当前用户不能写入它。
不满足的 home 在读取配置时被拒绝：使用另一个人的 home，等于运行那个人随时可以替换的程序。不存在的上游，或两种 home
都不是的目录，报告为降级（本次不复用）。

**读取位置。** 用户 `config.toml` 中的 `upstream`；没有时，系统级配置（`/etc/mcpp/config.toml`；Windows 上为
`%ProgramData%\mcpp\config.toml`）；仍没有时为空，即默认的自包含 home。

**已有副本的 home。** `mcpp self store dedupe` 列出本 home 自己安装、且上游有相同副本的 payload；
`mcpp self store dedupe --apply` 将其逐个原子地替换为链接。

## 4. 机器的系统 home

在多个用户共用的机器上，管理员为所有用户维护一个 home：

```bash
sudo mcpp --system self init
sudo mcpp --system toolchain install gcc 16.1.0
```

| | |
|---|---|
| 位置 | 系统级配置的 `[system] home`；否则为 `/var/lib/mcpp`（macOS 为 `/Library/Application Support/mcpp`，Windows 为 `%ProgramData%\mcpp`） |
| 写入方式 | `mcpp --system` 加 `self init`、`self doctor`、`self store`、`toolchain install`、`toolchain remove`、`toolchain default`、`index update` 之一 |
| 拒绝 | `--system` 与其他任何命令（以 root 构建会把 root 所有的文件写进用户的项目）；对系统 home 没有写权限的 `--system`（消息会提示 `sudo`） |
| 写入约定 | 所有用户可读（umask 022），同一时间只有一个写入者，忽略调用者的 `HOME` 与 `MCPP_HOME` |
| 标记 | `.mcpp-home`，其中 `mode = "system"` |

用户无需 root 即可读取。系统级配置把它写为用户的上游，并可以给出默认工具链：

```toml
# /etc/mcpp/config.toml
[xlings]
upstream = ["/var/lib/mcpp"]

[toolchain]
default = "gcc@16.1.0"
default_target = "x86_64-linux-gnu"
```

每个用户保留自己的 home：自己的索引数据、自己的构建缓存（不在用户之间共享）以及指向系统 home 中 payload
的链接。用户的索引解析出更新的版本时，安装到用户自己的 home。

在 Luban OS 上，系统级配置同时写出 `/xlings`，因此机器上已有的 payload 只存在一份。

## 5. 提供者程序

`[xlings] binary` 选择 mcpp 运行的 xlings：`"bundled"`（mcpp 随附的，默认）、`"system"`（`PATH` 上的）或一个路径。
无论哪一个，mcpp 都会询问它是什么（`xlings interface --version`）：版本、协议以及 mcpp 依赖的特性。不能服务当前 mcpp
的，让位于随附的 xlings，并以一条 note 说明原因。

`[xlings] home` 移动 registry，它指向的必须是 mcpp 建立的 home。不是 mcpp 建立的 home（例如 `~/.xlings`）在这里被拒绝：
复用它的包应使用 `upstream`（§3）。

## 6. 文件系统与长路径

home 可以在任何文件系统上。mcpp 测量其所在文件系统能做什么（拒绝覆盖已有名字的原子改名、原子交换、硬链接、符号链接），
并按测量结果发布。`mcpp self doctor` 显示结果，以及那里能否从上游复用（需要符号链接）。

Windows 上路径长度限制为 260 个字符，除非系统启用长路径（`LongPathsEnabled` = 1）。`mcpp.exe` 声明自己支持长路径。
长路径关闭且 `MCPP_HOME` 长到工具链最深的文件会超过限制时，mcpp 报告降级并给出两种办法：启用长路径，或使用更短的
`MCPP_HOME`。mcpp 不修改该设置；`mcpp self doctor` 报告它。

程序自己的清单陈述它是否支持长路径：`windows_long_paths`（[04](04-mcpp-toml.md)）。
