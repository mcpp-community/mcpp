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

### 选择 home（2026.10.19.1+）

在首次初始化前指定目录：

```powershell
mcpp self init --home D:\mcpp
```

选择会跨进程保留，不需要设置环境变量。原始 home 按 `MCPP_HOME`、自包含安装根目录、
用户目录下 `.mcpp` 的顺序选择。其 `home.toml` 指向实际使用的 home，mcpp 只读取一次。
默认情况下原始目录与实际目录相同，目标目录中的 locator 不参与解析。`MCPP_HOME` 不会被改写。

已有初始化后的 home 时，需要显式切换：

```powershell
mcpp self init --force --home E:\mcpp
```

该操作保存新默认位置并初始化，旧目录和目标目录已有数据均保留。
`mcpp self config` 和 `mcpp self doctor` 在相关场景报告原始目录、实际目录与前一目录。
前一目录是报告信息，不代表迁移或自动清理。原始目录与目标目录互相嵌套时拒绝操作。
重新选择原始目录会删除其 locator。

Windows 首次显式 `self init` 时，若 D 是可写的本地固定磁盘，且 stdin/stderr 均为终端，
可以提供 `D:\mcpp` 与用户 `.mcpp` 两项选择。Enter 选择 D；仅接受 `1`、`2` 与空行，
其他输入退出并提示使用 `--home`。Linux、macOS、非交互运行，以及已有环境变量、
自包含安装或已保存选择的场景均无菜单。

不带 `--home` 的 `self init --force` 保留原有 registry/cache 重置语义；
重定向场景会先标明实际目标。`--system self init --home` 被拒绝。
完整选择与持久化规则见 [SPEC-010](../specs/home-initialization.md)。

### 初始化与网络诊断（2026.10.19.1+）

`self env`、`self config`、`self doctor` 和 `toolchain list` 读取本地状态，不安装工具。
`self config --mirror auto|CN|GLOBAL` 仅更新本地配置，因此网络故障时仍可使用。

初始化因缺少基础工具而需要联网前，会显示提供者已配置资源的有界检查结果。
`self doctor` 将同类检查结果作为警告报告，本地错误的语义保持不变。
`--offline` 跳过检查与获取；已有工具和缓存包仍可使用。
探测失败不会禁用来源或阻止本地复用。

locator 在本地校验成功后、获取资源之前保存。离线缺资源或联网失败导致初始化未完成时，
退出码为 1，所选目录仍保留。再次运行 `self init` 会在该目录继续。

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

## 当前边界

home locator 仅解析一跳，不迁移或删除旧数据，原始目录与目标目录互相嵌套时拒绝。Windows 磁盘选择仅用于首次显式终端初始化；自定义路径使用 `--home`。网络探测只反映当时的资源状态，不能保证后续下载成功。
