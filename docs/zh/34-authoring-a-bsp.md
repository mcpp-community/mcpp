# 34 —— 编写板级支持包

**读者：**要把一块板子支起来，好让工程能用 `mcpp build` 面向它、用 `mcpp run`
抵达它的人。

**本章回答的那一个问题：**一个板级支持包供给什么，以及一个包怎样同时服务模拟器与
真实板子。

**不在这里：**使用一个 BSP，那是
[40 —— 裸机与 freestanding 目标](40-baremetal.md)；消费者看到的 runner 机制，
那是 [41 —— 抵达一台设备](41-devices.md)；把模拟器或探针驱动本身打包，那是
[32 —— 编写一个载荷](32-authoring-a-payload.md)。

在此之前：[33 —— 编写运行时适配包](33-authoring-an-adapter.md)。

## BSP 的供给内容

freestanding 目标没有操作系统，所以 hosted 程序白得的一切都必须来自某处。BSP
就是那个某处，它供给**整个目标世界**：

| | 内容 |
|---|---|
| 内存布局 | 一份链接脚本——程序既推导不出、也猜不到的那一个事实 |
| 启动代码 | `main` 之前跑的东西，以及向量表 |
| 一个导出模块 | 程序 import 它来够到板子的控制台与外设 |
| **runner** | `mcpp run` 与 `mcpp test` 究竟怎样抵达这块板子 |

runner 是最容易被漏掉的一项，少了它，每个消费者都要自己写一遍模拟器的调用。

## 一个包，两种环境

经模拟器抵达的板子，与经调试探针抵达的同一块板子，**差别只在 runner 的
argv，别处一个字都不差**。链接脚本、启动代码、内存布局与导出模块是同一块板子。
为了让四个字符串各不相同而发布两个包，会把这一整套复制一遍，并让两份副本
各自漂移。

所以环境是一个 **feature**：

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[feature-xlings.emulator]
"xim:qemu-arm" = { version = "9.2.4-1", when = "run" }

[feature-xlings.hardware]
"xim:probe-rs" = { version = "", when = "run" }
```

**`emulator` 是默认，而这是一个关于「谁在读」的决定。** 刚接触这个包的人手边
没有板子；手边有板子的人有理由把这一点说出来。要求硬件的默认值，会让所有还
没买任何东西的人第一条命令就失败。

两张表刻意对称：两种环境都不是引擎眼里的「正常」，消费者只下载自己所选
feature 需要的那一份。

**并且两者都在 `run` 档位上——这是第二道独立的闸门。** feature 说的是**谁**
需要这个工具；档位说的是**什么时候**需要。编译固件两者都不需要，只有抵达板子
才需要，所以一个只构建、从不烧录的 CI 任务**一个字节都不下载**。

## C 库也是一个 feature

每一行 `thumb*-none-eabi*` 的 C 库列都是空的，所以面向它的工程一开始**没有
libc**，除非工程自己开口要。BSP 停在这一档：它不引用任何 C 库符号，它的控制台
走 semihosting，而不是 `stdio`。

```toml
libc = {}

[feature-deps.libc]
picolibc.picolibc = "1.8.12.3"
```

C 库以**源码包**的形式到达，用程序自己的旗标编译，因此没有 multilib 要匹配，
也没有 ABI 约定会弄错。`mcpp run --features libc` 就是全部；不选它，零 libc
档原样保留。

## 构建程序

```cpp
int main() {
    mcpp::link_script("cortex-m.ld");
    mcpp::rerun_if_changed("cortex-m.ld");

    const std::string target = mcpp::target() ? mcpp::target() : "";
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"probe-rs", "run", "--chip", "STM32L475VG"})
            mcpp::runner(a);
        mcpp::run_exclusive();
    } else {
        /* the emulator's argv for THIS target */
    }
    return 0;
}
```

**哪个机器型号对应哪个目标，是一张表，不是一个默认值。** 为 `thumbv6m`
构建的镜像，在实现 `thumbv7em` 的型号上跑不起来；猜错的后果是它照常启动，
随后在毫不相干的地方出错。表里没有对应行时，要**点名报错**，而不是静默
缺席——一个没有配置 runner 的 BSP 会让 `mcpp run` 报「缺少 runner」并建议
添加一个 `runner` 键，这句话一般而言是对的，但在这种情况下不是真正的原因。

**写程序名，不写路径。** mcpp 会搜索图中任何包所声明的每一个载荷的
`bin/`，然后才轮到 `PATH`，并且能准确报出它搜过哪些目录。用安装目录拼出来
的绝对路径，会引入一种失败形态：查找返回空，而没有任何东西说明原因。

**引擎不认识上面任何一个 runner 名字。** `flash`、`serve`、`erase` 是这个
包的词汇；另一个包提供的 `serve` 可以指别的意思，两者都不必被 mcpp 知道。

**一个探针，一个使用者。** `mcpp::run_exclusive()` 声明这个目标的运行不可
重叠，`mcpp test` 随后把它们串行化。工程方永远不需要记得加 `-j1`。

## 当前边界

- BSP 把它的模拟器与探针驱动声明为载荷，因此工具链未在某个平台发布的板子，
  就无法从那个平台抵达。
- 机器型号表按三元组组织。板子需要表里没有的型号时，要修改的是 BSP，而不是
  在工程侧覆盖。
