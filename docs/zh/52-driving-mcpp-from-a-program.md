# 52 —— 从程序或 agent 驱动 mcpp

**读者：** 编写 agent 指令、编辑器后端或 CI 任务的作者，这些程序在 shell 中运行 mcpp，
并按得到的结果行动。

**本章回答的那一个问题：** 哪些命令、输出与退出状态，使一个程序不经终端、不经 IDE
就能构建、运行并检查一个 mcpp 工程。

**不在这里：** 各份文档的形状，见 [50 —— 机器可读输出](50-machine-output.md)；退出码
契约，见 [SPEC-003](../specs/exit-codes.md)；以及建立在本章内容之上的编辑器集成本身。

构建一个工程的每条调用链都终止于一次 mcpp 运行：终端里的人、只有 shell 的 agent、
编辑器的保存时构建、CI 任务。因此 mcpp 把一条链需要了解的另一条链的构建写到磁盘上，
并以 shell 可以分支的退出状态陈述结果。程序使用其中任何一项，都只需要一个 shell。

## 1. 构建并读取结果

```sh
mcpp build --message-format json > build.ndjson
```

标准输出的每一行是一条 JSON 记录。最后一行是 `mcpp.build` 信封，它的 `data` 是结果
记录：`status`（`ok`、`failed`）、`artifacts`（每个可运行产物附带启动方式）与
`diagnostics`（每条带 `path` 与 `range`）。之前的各行是事件（`plan`、`waiting`、
`progress`、`diagnostic`、`artifact`），供报告进度的读者使用。叙述 —— 人读的内容 ——
在标准错误上。

没有发起构建的程序从磁盘读取同一份记录：

```sh
cat target/.last-result.json                 # 指向记录及其状态
cat "$(jq -r .data.result target/.last-result.json)"
```

终端里的构建与编辑器里的构建写下同一种记录，因此双方都能看到对方的结果。

## 2. 运行，或自行启动程序

`mcpp run` 先构建，然后以程序替换自身，标准输出属于程序；退出状态是程序的状态（构建
失败时为 `101`）。自行启动程序的调试器或测试框架请求启动描述：

```sh
mcpp run app --print-launch
```

`mcpp.launch` 信封含 `argv`、`cwd` 与 `env` —— 只有 mcpp 为程序设置的变量 —— 与
`mcpp run` 使用的完全一致。

## 3. 退出状态

| 状态 | 含义 | 程序的处理 |
|---|---|---|
| `0` | 完成 | 读取记录 |
| `1` | 工程或环境失败 | 读取 `diagnostics`，修复后重跑 |
| `2` | 用法错误 | 修正命令行 |
| `5` | 运行在调用方拒绝的副作用之前停下 | 请调用方审批，然后去掉拒绝重跑 |
| `101` | `mcpp run` 无法构建程序 | 同 `1` |
| `130`、`143` | 被 SIGINT、SIGTERM 结束（POSIX） | 记录为 `cancelled`；下一次构建从中断处继续 |

## 4. 限制一次运行可以做的事

agent 的宿主，或处于不受信任工作区的编辑器，陈述一次运行不得产生的副作用：

```sh
mcpp --deny network,exec-build-script build
MCPP_DENY_EFFECTS=network mcpp build          # 同上，作用于一个会话的每条命令
```

副作用为 `init-mcpp-home`、`write-project`、`write-global-cache`、`network` 与
`exec-build-script`。需要被拒绝副作用的运行在它之前停下，在标准错误上点名，退出 `5`；
没有任何事做了一半。不需要它的运行不受影响：计划已记录的工程在
`--deny exec-build-script` 下照常构建。`mcpp --protocol-version` 在任何运行之前列出每条
命令可能产生的副作用。

## 5. 同时进行的多次运行

同一配置的两次构建不会互相破坏：第二次等待第一次，并说明等待的是谁
（`Waiting for the build directory, held by pid 1234 (mcpp build, from my-agent, 8s)`）。
设置 `MCPP_CLIENT` 会在这一行与结果记录中写明调用方：

```sh
MCPP_CLIENT=my-agent mcpp build
```

`mcpp run` 启动的程序不持有锁，因此在它运行期间重新构建是安全的；链接产物整体替换。
查询从不等待。契约见 [SPEC-011](../specs/concurrency.md)。

## 6. 不需要构建的查询

| 问题 | 命令 |
|---|---|
| 索引提供的包 | `mcpp search [keyword] --format json` |
| 已配置的注册表 | `mcpp index list --format json` |
| `mcpp pack` 接受的包格式 | `mcpp pack --list-formats` |
| 每个编译单元的编译命令 | `mcpp emit build-database` |
| 已安装的工具链 | `mcpp toolchain list --format json` |
| mcpp 使用的各个路径 | `mcpp self env --format json` |
| 以另一个 home 运行一次 | `mcpp --home <dir> <command>` |

## 7. 读取面向人的输出

程序只有叙述可读时 —— CI 日志、终端记录 —— 形态是固定的
（[SPEC-012](../specs/human-output.md)）：状态行以一个右对齐的词开头（`Compiling`、
`Finished`）；mcpp 的消息以 `error:`、`warning:` 或 `note:` 开头；关于某个文件的消息以
`<path>:<line>:<col>:` 开头；下一步单独成行，以 `help:` 开头。管道中没有颜色，除非
`CLICOLOR_FORCE` 或 `FORCE_COLOR` 要求。这些词本身没有兼容承诺；上文的记录有。
