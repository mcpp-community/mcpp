# 20 — 加速器

mcpp 如何构建设备代码,以及一个预建产物如何声明它能在哪些设备上运行。

## 两种形态,mcpp 当前实现其中一种

加速器工具链有两种形态,它们不是同一个模型的两种变体。

**岛(island)** 把设备代码放在单独的编译单元里,由单独的编译器编译。CUDA、HIP、
Ascend C 与 Metal 都是这种形态。设备编译器产出一个目标文件(着色语言则产出运行期
资源),汇入普通的链接。

**整目标(whole-target)** 把设备代码放在普通 `.cpp` 里,整个目标由一个能 offload 的
编译器编译。SYCL、OpenMP offload 与 stdpar 属于这一类,没有可分的岛。

本文描述岛这一形态,即 mcpp 当前实现的部分。

## 设备编译单元

扩展名为 `.cu` 或 `.hip` 的源文件是**设备编译单元**。mcpp 据此分类并相应处理:
它从不被扫描 import,也从不产出 BMI —— 因为没有任何设备编译器接受 C++20 modules。

`.cuh` 与 `.hiph` 被分类为头文件。它们不被编译,但改动其一可以改变构建图应有的形状,
因此与任何其它头文件一样会使快路径失效。

设备扩展名**不在**默认 source glob 中。一个 vendored 了 `.cu`、但在别处构建它的包,
不应当在 mcpp 升级后突然开始编译它 —— 这是它的作者无法修复的破坏,因为那个版本的
tarball 已经发出去了。设备源文件必须被显式点名。

## 接缝(seam)

设备编译单元不能 import 模块,因此它与工程其余部分的边界是一个头文件。消费者看不到
这个头文件:一个模块在其 global module fragment 里包含它,并导出 C++ 接口,
下游一律 import 该模块。

这个模块值得叫做**接缝**,因为它存在的理由不是模块边界。它是**唯一**一处可以在不改动
任何消费者的前提下替换下层的岛(换成 HIP、换成 CPU 回落)的位置,也是
`cfg(accelerator = ...)` 唯一有落点的位置。没有接缝的工程,没有任何边界可供替换后端。

接口上的两条约束来自编译器的事实,而不是审美。它应当是 `extern "C"`,
因为设备编译器驱动的是一个 mcpp 没有选择的宿主编译器,两侧因此不共享 C++ ABI。
岛本身应当避开标准库,因为一个链接了 libstdc++ 的岛,会把第二份 C++ 运行时放进一个
自身运行时来自 mcpp 工具链的程序里。

## 编译一个岛

调用设备编译器的那条命令不内置在 mcpp 里,而是由**构建规则包**提供 ——
以 `host-module = true` 消费,emit 输出汇入链接的构建边。机制见
[07 — build.mcpp](07-build-mcpp.md),可用的 CUDA 规则见 `examples/09-cuda-kernel`。

这个划分是刻意的。mcpp 拥有构建图、产物身份与架构集合;厂商的 flag 拼法、
架构语法与宿主编译器要求属于规则包。

## 设备编译器接受哪些宿主编译器

nvcc 拒绝比它在自己的 `crt/host_config.h` 中声明的上界更新的宿主编译器,
而 mcpp 的工具链载荷常常比那个上界更新。由于宿主编译器由 mcpp 提供,
它可以在任何编译发生之前报告这个配对:

```
$ mcpp self doctor
    Checking device toolkit
warning: cuda will refuse this host compiler: gcc 13 exceeds the bound of 12
         stated in /usr/include/crt/host_config.h.
```

上界是从工具包读出的,不是抄在 mcpp 里的表,因此一个 mcpp 从未见过的工具包同样能作答;
而一个 mcpp 无法解析的头文件产生不出上界,也就不产生任何断言。

**载荷先于 host 被读取。** 经 xlings 装的工具包才是构建会用的那个,而且通常更新:
12.9 载荷声明 `gcc <= 14`、13.3 载荷声明 `gcc <= 15`,而发行版的 CUDA 12.0 声明
`gcc <= 12`。两个包 store 都会搜 —— mcpp 自己的,以及 `xlings install` 写入的那个;
host 的位置保留在最后,因为「有发行版工具包、没有载荷」是一种真实配置。

**这里不检查什么,以及为什么。** 设备运行时不得比它将运行于其上的驱动更新;
更新时构建**干净地编译并链接**,却在第一次分配时失败,报
*"CUDA driver version is insufficient for CUDA runtime version"*。
mcpp 知道这个关系 —— `driver_accepts_toolkit` 陈述一个版本何时可以遇上另一个,
包括「小版本兼容」意味着 12.9 的运行时在只服务到 12.4 的驱动上没问题 ——
但它**不去问机器装的是哪个驱动**,因为问就意味着运行一个厂商的工具,而引擎不持有
任何厂商探针。那两个数字改以**声明**的形式抵达:工具包载荷声明它需要的驱动,
持有宿主驱动的那个包声明宿主有什么。

这是报告而非强制:一个不编译任何设备代码的工程,不受不兼容配对的影响。

## 设备编译器能否够到自己的后端

一个工具包可以安装完整、就在 `PATH` 上,却仍然在第一个阶段失败。
nvcc 以裸名调用 `cicc`、`cudafe++`、`ptxas` 与 `fatbinary`,依赖的是它自己
从紧邻其二进制的 `nvcc.profile` 前置进来的一条 `PATH`。在 Debian 系的打包里,
那个 profile 是指向 `/etc` 的符号链接,于是任何替换了 `/etc` 的容器或沙箱都会移除它。
nvcc 随即沿用环境里原有的 `PATH`,并报出:

```
sh: 1: cicc: not found
```

这条消息既没有提到 nvcc,也没有提到 profile,而工具包本身一样不缺,
于是所有显而易见的检查都会通过。`mcpp self doctor` 因此去问 nvcc 要它的计划,
而不是假设一份:

```
$ mcpp self doctor
    Checking device toolkit
warning: nvcc cannot reach its own back-end: it invokes 'cicc' by name, and
         that name does not resolve on the search path it states.
```

计划来自 `nvcc --dryrun` —— 它打印各个阶段与 nvcc 将要使用的 `PATH`,
而不编译任何东西。一次没有产生计划的 dryrun(没有 nvcc,或输出不是一份计划)
不产生任何结论:一个够不到答案的探测不应当发明一个。

## 声明一次构建的目标

```toml
[build]
accel = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

单次构建可用 `--accel` 覆盖 —— 这与 `--target` 对 `[toolchain]` 的关系相同。
`--no-accel` 不是「没有写 `--accel`」,它是**显式请求不要加速器**,
这正是在一个同时发布了设备构建的包中选中 CPU-only 变体所需要的。

源码包可以声明它支持哪些后端:

```toml
[package]
accelerators = ["cuda", "rocm"]
```

这与 `[package] platforms` 同形:一个意图声明与 CI 矩阵提示,不是门。
它与产物的 `accel` 刻意是不同的字段 —— 声明由人手写、可以是期望值,
而产物的字段是从产生它的那次构建**测量**出来的。

## 一个预建产物声明了什么

携带设备代码的产物把它记在兼容性标签旁边:

```toml
[[runtime.artifacts]]
role       = "static-library"
path       = "lib/libgpukit.a"
provenance = "mcpp-pack/1"
abi        = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
accel      = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

它是标签之外的独立字段而不是标签的一段,因为架构列表是一个**集合**,
而标签是用 `-` 拼接的字符串,其中的 triple 本身就含有数量不定的 `-`。

`accel` 缺席表示该产物不携带设备代码,因而不施加任何约束 ——
这就是为什么一个纯 CPU 的库可被任何构建使用。

### 消费者如何被匹配

当一次构建请求的每个后端,产物都声明了该后端、工具包主版本一致、
且覆盖了请求的每个架构时,该产物满足这次构建。一个架构被覆盖,当它被直接列出,
或列出了同 major、minor 不高于它的家族目标,或内嵌可移植形式的下界不高于它。

**家族目标与可移植形式是让变体矩阵保持有限的东西。** 一颗芯片一份产物不可扩展,
一个世代一份可以。

没有任何产物匹配时,拒绝会点名维度与两侧的取值:

```
error: mcpplibs.gpuonly@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23  accel=cuda12.8+{sm_86}
  published tags :
                   x86_64-linux-gnu  accel=cuda12.8+{sm_90f}
  closest is x86_64-linux-gnu, and it differs on:
    accel     needs cuda12.8+{sm_90f}, this build has cuda12.8+{sm_86}
  fix: build for an architecture the package carries (--accel), or take
       a variant that carries no device code (--no-accel), or ask the
       publisher for one covering yours.
```

这正是该维度存在所要搬移的那个失败。没有它,构建干净地链接完成,
而程序在第一次 kernel 启动时失败,消息里既没有包名,也没有任何一侧期望的架构。

### 语法是开放的

`cuda`、`hip`、`vulkan`、`sycl` 不是一个封闭集合。**后端名 + 版本 + 架构集合 +
可选下界**就是全部形状,mcpp 在没有「谁存在」这张表的情况下比较它们:

```
vulkan1.3+{spirv1.6} floor>=1.4
sycl2020+{spir64,nvptx64-sm_89}
hip6.4+{gfx942}
```

拼法里不带前导数字的架构 —— 如 `gfx942` —— 按**相等**比较,因为那里读不出任何序。
这是答案而不是回避:从中间抠出 `942` 会发明一个 AMD 并未定义的等级。

`floor>=` 是可移植形式下界的**后端中立**拼法。`ptx>=` 是 CUDA 对同一字段的说法,
仍然被接受,所以此前写下的描述符不受影响;可移植形式是 SPIR-V 的后端写 `floor>=`,
不必借用 NVIDIA 的词。

各拼法对某个后端具体意味着什么,是**那个后端的规则包**的事。引擎持有的是形状与比较。

### 发布多个变体

一个包可以发布多个产物,消费者取第一个标签接受它的。**把 CPU-only 的产物排在第一位。**
早于 `accel` 字段的 mcpp 会忽略该字段,而顺序是让这样的客户端仍然拿到一个
到处都能跑的产物的唯一机制。

## 按后端条件化

`accelerator` 是目标侧的一层,在依赖图解析之后才可求值,并且它持有一个**集合**
而不是单个取值:

```toml
[target.'cfg(accelerator = "rocm")'.build]
cxxflags = ["-DMYAPP_ROCM"]
```

比较是**成员判定**,因此一次同时启用 CUDA 与 ROCm 的构建对两者都答真。
`any`、`all`、`not` 作为普通布尔组合子在其上组合。

## 两条值得写明的边界

**`--accel` 是 `build` 的选项**,与 `--static`、`--toolchain` 同级,
不在 `run`、`test`、`pack` 上重复。那些命令与读取任何其它构建输入一样,
从 manifest 读 `[build] accel`;这个 flag 的用途是覆盖单次构建,而那正是 `build` 覆盖的场景。

**`mcpp pack` 不写 `accel` 字段。** 它本可以把 manifest 声明的值写进去,
而这恰恰是它不这么做的理由:该字段陈述产物**携带**了什么,
而 mcpp 目前不自己编译设备代码 —— 形态 A 走规则包,mcpp 没有可测量的对象。
把一个声明写进一个含义是「测量值」的字段,会让身份**恰好以该维度要防止的方式**说谎。
今天由发布者显式写这个字段,索引描述符就是这么做的;
等 `kind = "device"` 把设备编译放进 mcpp 之后,`mcpp pack` 才会发它。

## 尚未实现

整目标形态(SYCL、OpenMP offload、stdpar)、device target 及其隐含的 device link、
含设备代码的静态库,以及经由 xim 提供的加速器载荷。这些所依据的设计见
`.agents/docs/2026-09-05-accelerator-support-design.md`。
