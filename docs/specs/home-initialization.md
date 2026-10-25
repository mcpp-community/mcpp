# SPEC-010: home 选择与初始化

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-010 |
| 标题 | home 的入口、实际目录、初始化阶段与网络边界 |
| 状态 | 草案 v0.2 |
| 最后修改 | 2026-10-26 |
| 对应实现 | mcpp >= 2026.10.19.1；网络预算依赖提供者能力，逐条标注；§6 mcpp >= 2026.10.26.1、xlings >= 2026.10.26.1 |
| 相关设计文档 | `.agents/docs/2026-10-19-first-run-home-and-network-design.md`、`.agents/docs/2026-10-26-control-contract-runtime-and-ide-ecosystem-design.md` §9.1、§10.3（D18） |
| 相关 issue | mcpp#822；openxlings/xlings#683、openxlings/xlings#707 |
| 使用文档 | [25 — home、索引与存储](../zh/25-home-indices-and-store.md) |

本规范定义普通用户 home 的选择、保存与初始化边界。系统 home 的管理员权限和共享策略继续按使用文档执行。
用语按 RFC 2119：**必须 / 禁止**为强制，**应当**为建议，**可以**为可选。

## 1. 两阶段解析

1. 原始 home **必须**按非空 `MCPP_HOME`、可识别自包含安装根、用户 `.mcpp` 的顺序选择。
2. 实际 home **必须**按 `self init --home` 显式目标、原始 home 的 `home.toml` 目标、原始 home 本身选择。
3. 默认原始 home 与实际 home 相同。选择过程**禁止**改写 `MCPP_HOME`，**禁止**读取目标中的第二个 locator。
4. `MCPP_HOME` 与自包含安装**必须**只读取自己的原始目录，**禁止**读取另一份用户默认 locator。
5. 系统 home **禁止**采用用户 locator；`--system self init --home` **必须**拒绝。

## 2. 入口文件

`<原始 home>/home.toml` 保存绝对路径，字符串采用 TOML 转义：

```toml
home = "D:/mcpp"
previous_home = "C:/Users/user/.mcpp"
```

`home` **必须**为可访问的目录。`previous_home` 为可选报告字段，最多保存前一实际目录，
**禁止**作为回退位置或第二跳。解析或目录访问失败**必须**显式报错，提示使用
`mcpp self init --force --home <path>` 修复，**禁止**静默改用其他 home。

实际路径采用绝对、规范化形式；原始目录与目标互为祖先或后代时**必须**拒绝，相同目录可以使用。
写入口**必须**在原始目录的写锁内，以关闭后的同目录临时文件原子替换；
普通读取**禁止**要求目录可写。切回原始目录**必须**删除其 locator，保留两边其他内容。

## 3. 切换与 force

| 命令 | 必须执行的语义 |
|---|---|
| `self init --home P` | 初始化 P 并设为默认；已有不同的初始化后 home 时拒绝并提示 force |
| `self init --force --home P` | 授权覆盖默认指向；禁止删除旧 home 或清空目标已有数据 |
| `self init --force` | 保留当前 registry/cache 重置语义；重定向时先标明实际目标与保留入口 |
| `self config`、`self doctor` | 相关场景报告原始、实际和前一 home；禁止每条普通命令重复提示 |

切换**禁止**迁移数据。存在旧数据时，原始 `.mcpp` 可以同时保留数据和 locator。
入口**必须**在本地校验、配置初始化成功后保存，在网络获取前发布。
网络失败或离线缺资源**必须**返回 init incomplete（退出码 1），保留已保存选择，供下次继续。

## 4. Windows 首次交互

菜单仅适用于 Windows 首次显式 init：未设置 `MCPP_HOME`、非自包含、无 locator、
无初始化后的 home、stdin/stderr 均连接终端，且 D 为可写的本地固定磁盘。
已有 `D:\mcpp` 必须为非重解析点目录；缺失可作为候选，属性读取失败则不推荐。
不得为判断推荐资格而跟随重解析点；显式 `--home` 仍按路径选择规则验证目标。
Linux/macOS 和非交互运行**禁止**显示菜单。

```console
      Choose the mcpp home:

             1) D:\mcpp                  [default]
             2) C:\Users\user\.mcpp

      Select [1/2, Enter=1]:
```

Choose 与 Select **必须**左对齐，标题与选项隔一空行。只接受 `1`、`2`、空行；
空行选择 1。其他输入**必须**退出 2 并提示 `--home`；EOF 和 Ctrl+C 取消，不保存选择。

## 5. 配置与网络边界

只读查询（env/config/doctor/toolchain list）**禁止**隐式安装或 bootstrap；
mirror 更改只写本地配置。配置读取、本地初始化、能力获取为独立阶段。

首次缺少 bootstrap 能力时，在第一个可能联网的提供者初始化前**必须**报告网络检查。
所有端点发现和资源探测共用 5 秒 wall-clock 预算，由父进程终止超时子进程。
探测**必须**使用提供者实际配置的资源、代理与 TLS 策略，**禁止**仅以域名 TCP 成功认定资源可用。
失败只影响诊断，**禁止**阻止已安装能力复用。doctor 网络失败只计警告，本地错误保留退出语义。
`--offline` **禁止**启动探测或下载。

提供者元数据握手**必须**在隔离临时目录中读取，避免旧提供者在握手前改写原 home。
状态查询要求 `read_only_queries` 能力；缺失时**必须**跳过查询，继续本地诊断，
**禁止**为了诊断安装替代提供者。

下载连接上限 8 秒、无字节进展上限 30 秒；已有更短限制保留。
heartbeat 或重复字节值**禁止**更新下载进展时间。小型索引请求保留 120 秒总保护，
大型安装保留 3 小时最终进程保护。实际下载源和候选重试次数来自提供者事件，
**禁止**将探测成功的候选伪报为实际下载来源。

提供者缺失或不支持诊断时**必须**报告 unavailable/unsupported，继续本地检查，
**禁止**为了 doctor 下载提供者。旧提供者不能满足新下载预算时，应按能力握手选择兼容的随附版本。

## 6. 单次运行的 `--home` 与生态一致性 已实现

1. `mcpp --home <dir> <command>` **必须**与 `MCPP_HOME=<dir> mcpp <command>` 等价：`<dir>`
   是这一次运行的原始 home，其 `home.toml` 按 §1 第 2 条读取一跳。选择只对这一次运行生效，
   **禁止**写入任何 locator。
2. `--home` **可以**写在任何子命令之前或之后。与 `self init --home` 同时出现时，两个值
   指向同一目录则照常执行；指向不同目录**必须**以用法错误（退出 2）拒绝，**禁止**取其一。
3. xlings 采用相同的模型：全局 `--home <dir>` 是单次运行的 `XLINGS_HOME`；`self init
   --home <dir>` 初始化 `<dir>` 并在原始 xlings home 中写入入口文件 `home.json`
   （`{"home": "<dir>"}`），之后的运行跟随它一跳；`self init --force --home <dir>` 授权改写
   已有入口；以原始目录为目标时删除入口。入口无法读取或目标不可访问时**必须**显式报错并给出修复命令，**禁止**静默
   改用原始 home。
4. 两个 home 互相独立：mcpp 的 home 选择**禁止**改变 xlings 的 home，反之亦然。mcpp 通过
   提供者调用 xlings 时，按 mcpp home 中记录的提供者位置调用，与用户自己的 xlings home
   选择无关。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v0.2 | 2026-10-26 | 新增 §6：mcpp 与 xlings 的单次运行 `--home`、与 `self init --home` 的组合规则、xlings 的入口文件 |
| v0.1 | 2026-10-19 | 定义两阶段单跳 home、非删除切换、Windows 选择与初始化网络边界 |
