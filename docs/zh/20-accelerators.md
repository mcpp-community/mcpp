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

## 规则包在第一次编译之前报告的事

设备工具包有三件事出错得很晚,而没有一件是关于构建图的事实。它们由驱动这些工具的
**规则包**读取并报告 —— `examples/09-cuda-kernel/rules-cuda` 逐一演示 —— 引擎一件都
不拥有(`tests/unit/test_core_vendor_probes.cpp` 守住这条线,于是第二个后端不会在
mcpp 里长出第二份拷贝)。

**设备编译器接受哪些宿主编译器。** nvcc 拒绝比它在自己的 `crt/host_config.h` 中声明
的上界更新的宿主编译器,而 mcpp 的工具链载荷往往比那个上界更新。在 nvcc 路线上,规则
从它解析到的工具包读出上界 —— 载荷先于宿主,因为经 xlings 安装的工具包才是构建会用
的那个,通常也是更新的那个(12.9 载荷写着 `gcc <= 14`,发行版的 CUDA 12.0 写着
`gcc <= 12`)—— 并通过 `mcpp::warning` 说出它选了哪个编译器、为什么。主路线没有这条
上界:`clang -x cuda` 自己就是宿主编译器。

**设备编译器能否够到自己的后端。** 工具包可以装好、完整、在 `PATH` 上,却在第一阶段
就失败:nvcc 以裸名调用 `cicc`、`cudafe++`、`ptxas`、`fatbinary`,靠的是它从自己二进制
旁边的 `nvcc.profile` 前置进来的一条 `PATH`,而替换了 `/etc` 的沙箱会拿走 Debian 打包的
那个 profile。规则向 nvcc 要它的计划(`nvcc --dryrun`)而不是假设一个,逐个解析各阶段,
点名第一个解析不到的以及提供它的载荷。产不出计划的 dryrun 不产生任何结论。

**驱动是否新到足以承载运行时。** 设备运行时不能比它将遇到的驱动更新;更新时,构建编译
干净、链接干净,到第一次分配才以 *"CUDA driver version is insufficient for CUDA runtime
version"* 失败。规则经驱动自己的库(经由 sentinel 包够到,绝不经 `/usr/lib`)读出驱动
版本并陈述为事实;陈述它的运行时需要的下界;引擎在编译任何东西之前比较两者 —— 见
[07 — build.mcpp](07-build-mcpp.md) 的探针通道。引擎读到的是一个名字、一个关系、一个
版本;`cuda.driver` 是流过引擎的数据。

凡是错答比不答更贵的地方都只报告不强制:工程里没有规则包的机器没有任何厂商相关的话
要说,于是什么都不说;够不到答案的探针不发明答案。

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

**`--accel` 与 `--no-accel` 是 `build`、`run`、`test` 三者的选项**(run 与 test 自 2026.9.5.2 起),与 `--target`、`--profile` 同级;`pack` 与其它构建输入一样从 manifest 读 `[build] accel`。它起初只挂在 `build` 上,实测的后果是一个工程的 CPU-only 变体能构建却不能运行:`mcpp build --no-accel` 产出了它,而 `mcpp run` 交回的是设备构建。
<!-- 下面一段保留原有说明 -->
**历史:`--accel` 曾只是 `build` 的选项**,与 `--static`、`--toolchain` 同级,
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
