# 41 —— 在设备上运行

**读者：** 产物不在构建它的那台机器上运行的所有人——一块板子、一个模拟器，
或者一台远程主机。

**本章回答的那一个问题：** 这样的产物怎样被运行与测试，以及一个包供给
什么，才能让一条命令抵达它。

**不在这里：** 这个目标本身怎么构建，那是 [40 —— 裸机](40-baremetal.md)；
以及 mcpp 认为什么是一个测试，那是 [08 —— 测试](08-testing.md)。

本文规定 mcpp 如何执行一个运行在构建机器之外的产物、包如何提供抵达它的
其它方式，以及工程如何在模拟器与真实硬件之间选择。

相关文档：[40 —— 裸机与 freestanding 目标](40-baremetal.md) 覆盖这一机制
最常应用的那些目标；[30 —— build.mcpp](30-build-mcpp.md) 是包所讲的指令
协议的参考；[50 —— 机器输出](50-machine-output.md) 是调试器客户端或 IDE
使用的接口。

## 一条命令，加具名的例外

无法在构建机器上运行的产物需要有东西站在它前面。那个东西叫 **runner**：
包提供、工具执行的一段 argv，产物被追加或替换 `{}`。

```bash
mcpp run                        # the default runner
mcpp run --runner flash         # a named one
mcpp run --list-runners         # what this project supplies
mcpp why runners                # the same list, beside everything else resolved
mcpp run --features hardware    # the same board, reached the other way
```

`mcpp run` 接受 `--features` 与 `--profile`，与 `mcpp build`、
`mcpp test` 同轴。这正是上面那一行是一条**命令**而不是一次清单改动的
原因：板级包把两种环境表达成 feature，选其中之一与选任何别的 feature
是同一个动作。

`mcpp run` 覆盖了常见情形的全部，真实硬件也不例外。在设备上，「运行一个
程序」意味着写进去、复位、接上它的输出、读回退出状态——这是**一条**
命令（`probe-rs run`、`qemu-system-* -kernel`），不是几条。因此板级包把
它作为**默认** runner，于是开发者从模拟器换到真实板子时，敲的命令不变。

具名 runner 服务剩下的部分：只写不跑、观察一路串口输出、起一个调试
服务端、擦除一个分区、部署但不启动。

**引擎不认识任何 runner 名字。** `flash`、`serve`、`deploy`、`submit`、
`logcat` 对它一样陌生：它只知道包可以提供具名 runner 这件事，然后执行
它找到的 argv。引擎里若有一份固定的名字表，就等于由引擎决定哪些领域可
被表达。

## 包的供给内容

```cpp
mcpp::runner("qemu-system-arm");        // the default: argv token by token
mcpp::runner("-machine"); mcpp::runner("mps2-an385"); …

mcpp::runner("flash", "probe-rs");      // a named runner
mcpp::runner("flash", "download"); …

mcpp::runner_longlived("monitor");      // no natural end
mcpp::run_exclusive();                  // this target's runs cannot overlap
```

**写程序名，不要写路径。** mcpp 会定位它：先找**图中任何一个包**在
`[xlings.workspace]` 下声明的载荷的 `bin/`（消费工程优先，然后是它的
依赖），再找 `PATH`。板级包正是那个知道哪个模拟器或探针能抵达这台机器
的东西，所以由它自己声明该载荷，**消费者什么都不用声明**。用
`mcpp::xpkg_dir` 拼出绝对路径是多余的，而且会引入一个失败模式——声明
不是安装，查询可能返回空，于是没有配置任何 runner，也没有任何话说明
原因。写程序名则让 mcpp 能报出它究竟搜过哪些目录。

## 工程的覆盖项

```toml
[target.thumbv7em-none-eabihf]
runner = ["qemu-system-arm", "-machine", "mps2-an385", "-kernel"]

[target.thumbv7em-none-eabihf.runners]
flash   = ["probe-rs", "download", "--verify", "--chip", "STM32L475VG", "{}"]
monitor = ["probe-rs", "attach", "--chip", "STM32L475VG"]
```

优先级是通常那一个：工程作者写的胜过依赖提供的，而覆盖会被报告，不会
被默默应用。同一个名字只允许一个依赖提供，出现第二个是一个点名两个包的
错误。

## runner 收到的内容

runner 收到一段 argv——它自己的 token，产物路径被追加或替换 `{}`——
以及一个环境变量 `MCPP_RUNTIME_FILES`。`mcpp run` 与 `mcpp test` 启动
的每一个 runner 都会收到这个变量，默认 runner 与具名 runner 一样。
`mcpp run --no-runner` 不启动任何 runner，也不设置它。

`MCPP_RUNTIME_FILES` 指向一个文件，列出产物从自己所在目录读取或加载的
东西：`[runtime] deploy` 与 `deploy_files` 的条目，以及构建链接的共享库。
每个文件一行：

```
data/data.txt<TAB>/abs/project/target/<triple>/<fp>/bin/data/data.txt
libfw.so<TAB>/abs/project/target/<triple>/<fp>/bin/libfw.so
```

第一个字段是相对于产物所在目录的目的路径，分隔符为 `/`；对于在子目录中
发现的测试程序，它以 `../` 开头。第二个字段是该文件在输出树中的绝对
路径。两者以 TAB 分隔。每次启动 runner 时这个文件都存在，没有需要携带
的东西时为空；以 `--format` 运行的可分发物收到的是空文件，因为可分发物
自带其文件。

就地执行产物的 runner 会忽略这个变量。把产物移走的 runner——移到设备、
容器或远程主机——会把列出的每个文件复制到被移走产物旁边对应的目的
路径。

## 运行一个可分发物

`mcpp run --format <f>` 打包可分发物 `<f>` 并运行它。不带 `--runner`
时，若图或清单提供了名为 `<f>` 的 runner，就由它抵达，否则由默认
runner 抵达：

```cpp
mcpp::runner("app", "<tool>");          // a package names a runner after its format
```

```bash
mcpp run --format app                   # through the runner named `app`
mcpp run --format app --runner other    # a typed --runner still wins
```

目录形式的可分发物（例如应用包）不会被直接执行。没有 runner 能抵达它
时，`mcpp run --format <f>` 会在启动任何东西之前拒绝，退出状态为
`126`，并点名能抵达它的 runner `<f>` 以及本该声明它的清单表。

## 终止由声明决定，不由推断

| | 含义 |
|---|---|
| default | runs to completion; the exit code is the verdict |
| `runner_longlived(name)` | has no natural end; the operator ends it |

`openocd -c "program image.elf verify reset exit"` 会终止，
`openocd -c "init"` 不会，两者的拼写直到包所选的那个参数为止都一样。
没有任何 argv 能表达这个区别，引擎也没有一份名字表可供推断——所以由包
陈述。

`mcpp run --runner debug` 启动一个**服务端**，到此为止。连上去的客户端
是用户的调试器或 IDE，它通过机器输出协议获得所需信息。

## 不能重叠的运行

`mcpp test` 在一个工作者池上运行测试二进制。一块板配一个探针、一张
GPU、一个串口，或一个单席位授权的工具，都一次只容一个使用者；两个工作
者同时去够它不会干净地失败——它们会互相穿插，产生的判决不描述其中任何
一个测试。

包用 `mcpp::run_exclusive()` 说出这件事，`mcpp test` 随之串行化。工程
永远不必记得 `-j1`。

按性质命名而不按硬件命名：这里没有一处是关于「设备」的。

## 模拟器与硬件是同一个包

经模拟器抵达的板子，与经调试探针抵达的同一块板子，差别只在 runner 的
argv，别无其它。链接脚本、启动代码、内存映射与导出的模块都是同一块板
子。为了变化几个字符串而发布两个包，会把这一切复制一遍，并让两份副本
漂移开来。

因此，这个选择是一个包的一项 feature：

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}
```

```cpp
int main() {
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);                       // the DEFAULT moves
        for (auto a : {"probe-rs", "gdb", "--chip", "STM32L475VG"})
            mcpp::runner("debug", a);
        mcpp::runner_longlived("debug");
        mcpp::run_exclusive();
    } else {
        for (auto a : {"qemu-system-arm", "-machine", "mps2-an385", "-nographic",
                       "-semihosting", "-no-reboot", "-kernel"})
            mcpp::runner(a);
    }
    return 0;
}
```

```toml
[dependencies]
cortex-m-rt = { version = "0.1.0", features = ["hardware"] }
```

消费者的命令不变。所选环境不提供的 runner 保持缺席：模拟器没有调试
探针，于是在 emulator feature 下 `mcpp run --runner debug` 会报告没有
这个 runner，并列出确实存在的那些。

这不需要任何引擎机制。引擎只读 runner，对模拟器或探针一无所知；
`mcpp::has_feature` 本来就已经存在。一个问题不必新增任何东西就能回答，
正是分层按其规定在起作用。

## 当前边界

- **同一个 runner 名字只允许一个依赖提供。** 出现第二个是一个点名两个
  包的错误；没有任何「按顺序取胜」的规则。
- 所选环境没有提供的 runner **就是不存在**。在 emulator feature 下没有
  调试探针，于是 `mcpp run --runner debug` 会报告没有这个 runner，并
  列出确实存在的那些。
- 终止方式是被声明的，不是被推断的。一个不声明自己长驻的 runner，会被
  一直等到操作者结束它。
