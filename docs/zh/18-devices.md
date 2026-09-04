# 18 - 抵达一台设备

本文规定 mcpp 如何执行一个运行在构建机器之外的产物、包如何提供抵达它的其他方式,
以及工程如何在模拟器与真实硬件之间选择。

相关文档:[13 - 裸机与 freestanding 目标](13-baremetal.md) · [07 - build.mcpp](07-build-mcpp.md)
· [11 - 机器输出](11-machine-output.md)。

## 一条命令,加具名的例外

无法在构建机器上运行的产物需要有东西站在它前面。那个东西叫 **runner**:包提供、
工具执行的一段 argv,产物被追加或替换 `{}`。

```bash
mcpp run                        # 默认 runner
mcpp run --runner flash         # 具名的
mcpp run --list-runners         # 这个工程提供了哪些
```

**`mcpp run` 覆盖了常见情形的全部,真实硬件也一样。** 在设备上,「运行一个程序」
意味着写进去、复位、接上它的输出、读回退出状态 —— 这是**一条**命令
(`probe-rs run`、`qemu-system-* -kernel`),不是几条。因此板级包把它作为**默认**
runner,于是开发者从模拟器换到真板时,敲的命令不变。

具名 runner 服务剩下的部分:只写不跑、看串口、起调试服务端、擦片、部署但不启动。

⚠️ **引擎不认识任何 runner 名字。** `flash`、`serve`、`deploy`、`submit`、
`logcat` 对它一样陌生:它只知道「包可以提供具名 runner」这件事,然后执行它找到的
argv。**引擎里若有一份固定的名字表,就等于由引擎决定哪些领域可被表达。**

## 包提供什么

```cpp
mcpp::runner("qemu-system-arm");        // 默认:一次一个 token
mcpp::runner("-machine"); mcpp::runner("mps2-an385"); …

mcpp::runner("flash", "probe-rs");      // 具名 runner
mcpp::runner("flash", "download"); …

mcpp::runner_longlived("monitor");      // 没有自然终点
mcpp::run_exclusive();                  // 这个目标的运行不能重叠
```

⭐ **写程序名,不要写路径。** mcpp 会定位它:先找本包在 `[xlings] deps` 里声明的
载荷的 `bin/`,再找 `PATH`。用 `mcpp::xpkg_dir` 拼绝对路径是多余的,而且引入了一个
失败模式 —— **声明不是安装**,查询可能返回空,于是没有配置任何 runner 而没有任何
话说明原因。写程序名则让 mcpp 报出它究竟搜过哪些目录。

## 工程覆盖什么

```toml
[target.thumbv7em-none-eabihf]
runner = ["qemu-system-arm", "-machine", "mps2-an385", "-kernel"]

[target.thumbv7em-none-eabihf.runners]
flash   = ["probe-rs", "download", "--verify", "--chip", "STM32L475VG", "{}"]
monitor = ["probe-rs", "attach", "--chip", "STM32L475VG"]
```

优先级是通常那个:工程作者写的胜过依赖提供的,且覆盖会被报告。一个名字只允许一个
依赖提供,第二个是点名两个包的错误。

## 是否终止由声明决定,不由推断

| | 含义 |
|---|---|
| 默认 | 运行到结束;退出码即判决 |
| `runner_longlived(name)` | 没有自然终点;由操作者结束 |

`openocd -c "program image.elf verify reset exit"` 会终止,`openocd -c "init"`
不会,两者拼写直到包所选的那个参数为止都一样。没有任何 argv 能表达这个区别,而引擎
也没有一份名字表可供推断 —— 所以由包陈述。

`mcpp run --runner debug` 启动一个**服务端**,到此为止。连上去的客户端是用户的
调试器或 IDE,它通过机器输出协议获得所需。

## 不能重叠的运行

`mcpp test` 在工作者池上跑测试二进制。一块板配一个探针、一张 GPU、一个串口、
一个单席位 license 的工具,都一次只容一个使用者;两个工作者去够它不会干净地失败
—— 它们互相穿插,产生的判决不描述其中任何一个测试。

包用 `mcpp::run_exclusive()` 说出这件事,`mcpp test` 于是串行化。工程永远不必记得
`-j1`。

**按性质命名而不按硬件命名**:这里没有一处是关于「设备」的。

## 模拟器与硬件是同一个包

经模拟器抵达的板,与经调试探针抵达的同一块板,差别只在 runner 的 argv,别无其他。
链接脚本、启动代码、内存映射与导出的模块都是同一块板。为变化几个字符串而发布两个
包,会把这一切复制一遍并让副本漂移。

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}
```

```cpp
if (mcpp::has_feature("hardware")) {
    for (auto a : {"probe-rs","run","--chip","STM32L475VG"})
        mcpp::runner(a);                       // 移动的是**默认**
    for (auto a : {"probe-rs","gdb","--chip","STM32L475VG"})
        mcpp::runner("debug", a);
    mcpp::runner_longlived("debug");
    mcpp::run_exclusive();
} else {
    for (auto a : {"qemu-system-arm","-machine","mps2-an385","-nographic",
                   "-semihosting","-no-reboot","-kernel"})
        mcpp::runner(a);
}
```

```toml
[dependencies]
cortex-m-rt = { version = "0.1.0", features = ["hardware"] }
```

**消费者的命令不变。** 所选环境不提供的 runner 保持缺席:模拟器没有调试探针,
于是在 emulator 之下 `mcpp run --runner debug` 报告没有这个 runner,并列出有哪些。

⭐ **这不需要任何引擎机制。** 引擎读 runner,对模拟器与探针一无所知;
`mcpp::has_feature` 本来就在。**一个问题不必新增任何东西就能回答,是分层按规定在
起作用。**
