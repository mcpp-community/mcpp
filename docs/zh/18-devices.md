# 18 - 抵达一台设备

本文规定 mcpp 如何执行、烧录、观察与调试一个运行在构建机器之外的产物,以及一个
工程如何在模拟器与真实硬件之间做选择。

相关文档:[13 - 裸机与 freestanding 目标](13-baremetal.md) 覆盖这些动作适用的目标;
[07 - build.mcpp](07-build-mcpp.md) 是板级包所说的指令协议的参考;
[11 - 机器输出](11-machine-output.md) 是调试客户端或 IDE 使用的接口。

## 四个动作,一种形状

一个无法在构建机器上运行的产物,需要有东西站在它前面。人们对这样的产物提出四种
要求,而这四种都是「板知道、工具执行」的一段 argv:

| 命令 | 槽 | 做什么 |
|---|---|---|
| `mcpp run` | `runner` | 执行产物 |
| `mcpp flash` | `flash` | 把它写进设备 |
| `mcpp monitor` | `monitor` | 观察设备打印什么 |
| `mcpp debug` | `debug` | 启动设备的调试服务端 |

每一个都以同样的方式声明,由板级包:

```cpp
mcpp::flash("probe-rs");
mcpp::flash("download");
mcpp::flash("--verify");
mcpp::flash("--chip");
mcpp::flash("STM32L475VG");
```

或者由工程声明,覆盖依赖所提供的:

```toml
[target.thumbv7em-none-eabihf]
flash = ["probe-rs", "download", "--verify", "--chip", "STM32L475VG", "{}"]
```

产物路径被追加,或在模板包含 `{}` 时替换它。一次调用一个 token:argv 是有序的,
一个字符串说不出它的边界在哪里。

程序由 mcpp 定位而不是由系统定位:先看已声明载荷的 `bin/`,再看 `PATH`。哪里都
找不到的工具是一个在任何进程启动之前就作出的错误,而不是回落到裸执行。

## 是否终止是槽的性质

这四个动作在一件引擎必须据以行动的事情上不同,而这件事没有任何 argv 能表达。

| 语义 | 槽 | 含义 |
|---|---|---|
| `OneShot` | `run`、`flash` | 运行到结束;退出码即判决 |
| `LongLived` | `monitor`、`debug` | 没有自然的终点;由操作者结束它 |

`openocd -c "program image.elf verify reset exit"` 会终止,而 `openocd -c "init"`
不会,两者的拼写直到板所选的那个参数为止都一样。因此引擎从**槽**读出这件事,
板级包也就不可能因为把 argv 写成另一个样子而弄错它。

`mcpp debug` 启动一个**服务端**,到此为止。连上去的客户端是用户的调试器或 IDE,
它通过机器输出协议获得所需。mcpp 不驱动客户端。

## 缺席被报告,而不被替代

`mcpp run` 在没有 runner 的 hosted 目标上直接执行产物,因为宿主跑得动它。
「没有烧录器」没有对应的读法:没有别的东西会把镜像写进设备。因此未声明的
`flash`、`monitor` 或 `debug` 在**每一个**目标上都是错误,并点名那个槽、打印可以
粘贴的键。

靠在构建宿主上运行程序来让 `mcpp flash` 成功,正是这个槽存在所要防止的那种失败。

## 独占的设备

一块物理板是一把互斥锁,模拟器不是。`mcpp test` 在一个工作者池上跑测试二进制,
而两个进程去够同一个探针不会干净地失败 —— 它们互相穿插,产生的判决不描述其中
任何一个测试。

板自己说出这件事:

```cpp
mcpp::runner_exclusive();
```

`mcpp test` 于是在那个目标上一次跑一个测试,并报告它正在这么做。工程永远不必记得
`-j1`。

## 模拟器与硬件是同一个包

经模拟器抵达的板,与经调试探针抵达的同一块板,差别只在设备槽的 argv,别无其他。
链接脚本、启动代码、内存映射与导出的模块都是同一块板。为了变化四个字符串而发布
两个包,会把这一切复制一遍,并让两份副本各自漂移。

因此这个选择是**一个包的一个 feature**:

```toml
[features]
default  = ["emulator"]
emulator = []
hardware = []
```

```cpp
int main() {
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);
        for (auto a : {"probe-rs", "download", "--verify", "--chip", "STM32L475VG"})
            mcpp::flash(a);
        mcpp::runner_exclusive();
    } else {
        mcpp::runner(qemu_path());
        for (auto a : {"-machine", "mps2-an385", "-nographic", "-semihosting",
                       "-no-reboot", "-kernel"})
            mcpp::runner(a);
    }
    return 0;
}
```

消费者在它选择其他一切的地方选择环境:

```toml
[dependencies]
demo-board-rt = { version = "0.1.0", features = ["hardware"] }
```

所选环境不提供的槽保持缺席。模拟器没有调试探针,所以在 emulator feature 之下
`mcpp debug` 报告没有配置,而不是发明一个。

⭐ 这**不需要任何引擎机制**。引擎读槽,对模拟器与探针一无所知;
`mcpp::has_feature` 本来就存在。这个问题不必新增任何东西就能回答,正是分层按规定
在起作用。

## 优先级与报告

每个槽都有两个生产者,优先级是通常的那个:工程作者写的胜过依赖提供的。覆盖会被
报告,而不是静默应用。

```
        note [target.thumbv7em-none-eabihf].flash overrides the flash a dependency supplied
```

一个槽只允许一个依赖提供。两个依赖的链接标志会拼接,那是对的;两个烧录器不能,
拼接产生的 argv 不属于其中任何一个。第二个提供者是一个点名两个包的错误。
