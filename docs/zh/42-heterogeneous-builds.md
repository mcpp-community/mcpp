# 42 —— 异构硬件构建

**读者:**把程序的一部分编译到 GPU 或 AI 加速器上的人。

**本章回答的那一个问题:**设备代码怎样被编译并链接进一个普通程序,以及一个预建
产物怎样声明它能在哪些设备上运行。

**不在这里:**编写驱动设备编译器的那条规则,那是
[31 —— 编写规则包](31-authoring-a-rule-package.md);以及抵达设备把它跑起来,那是
[41 —— 抵达一台设备](41-devices.md)。

GPU 与 AI 加速器目标,以及宿主/设备混合编译:mcpp 如何构建设备代码,以及一个预建产物如何
声明它能在哪些设备上运行。

## 支持什么

一个构建点名它面向哪些设备后端,而且**可以点多个**:

```toml
[build]
accel = "cuda12.9+{sm_89}, vulkan1.2"
```

`accelerator` 是目标侧的一个**集合**,所以这个构建里 `cfg(accelerator = "cuda")` 与
`cfg(accelerator = "vulkan")` 同时为真,每个后端的规则包各编各的设备单元,一个产物把
它们都带上。逗号分隔的是这个集合里的**条目**;空格分隔的是**同一条目内的修饰符**
(`cuda12.9+{sm_89} ptx>=89` 是一个后端加一组架构再加一条 PTX 下界)。源码集合怎么写
见下面「一个构建里的多个后端」。

`accelerator = "none"` 是**空集** —— 恰好在本次构建完全没有命名加速器时为真。它是
词表里唯一一个不是后端名字的取值,而它之所以存在,是因为词表的其余部分是**开放**的:
一个会增长的集合不能靠列举来否定。见下面「CPU 回退」。

今天有规则包的编程模型是五个 —— CUDA、HIP、SYCL、Vulkan/SPIR-V、Ascend C ——
「各条 lane」那张表写明每个驱动哪个编译器、需要哪些载荷。引擎里不持有任何厂商名字,
所以第六个是**一个包**而不是一次引擎改动。

## 两种形态,以及为什么一套机制够到两者

加速器工具链有两种形态。它们描述的是**一个工具链通常怎么被使用**,不是一个构建系统
需要几套机制。

**岛(island)** 把设备代码放在单独的编译单元里,由单独的编译器编译。CUDA、HIP、
Ascend C 与 Metal 都是这种形态。设备编译器产出一个目标文件(着色语言则产出运行期
资源),汇入普通的链接。

**整目标(whole-target)** 把设备代码放在普通 `.cpp` 里,整个目标由一个能 offload 的
编译器编译。SYCL、OpenMP offload 与 stdpar 通常这样用。

mcpp 只实现**一套**机制 —— 岛。这是关于**设计**的陈述,不是关于支持多少后端的:
一个整目标工具链是**经由**岛、而不是在它旁边被接进来的,所以支持它的代价是一个规则包,
而不是第二套机制。SYCL 之所以在已发布的那一组里,正是这个原因。

**SYCL 的分得开。** 一个 SYCL kernel 是 `submit` 里的一个 lambda,所以一个工程可以
按约定把它们全部收进各自的编译单元,而 `.sycl` 就是这条约定的可检查形式。只有这些
单元交给 SYCL 编译器,目标的其余部分由工程自己的工具链编译 —— `examples/09-heterogeneous/sycl`
实测如此:`app.cppm` 与 `main.cpp` 由 mcpp 的 clang 编,`saxpy.sycl` 由 dpcpp 载荷的编。

**OpenMP offload 与 stdpar 的分不开。** `#pragma omp target` 与
`std::execution::par_unseq` 出现在普通代码里任意的调用点上;没有可搬的单元,也就没有
可施加的岛。「mcpp 未实现的那一种形态」现在指的正是这两个,而这条边界是**模型本身的
性质**,不是本文档志向上的缺口。

两个值得直说的后果:

* 一个 kernel 写在 `.cpp` 里的既有 SYCL 工程**不能原样构建**。它的设备单元要先搬进
  `.sycl` —— 那是一次改名加一条接缝,不是重写。
* 一个扩展名只有一个含义。让收窄的 glob 去承载 `.cpp`,同一个名字就会因为哪条 glob
  先匹配而选中两个不同的编译器;接缝之所以可读,正是因为文件名说明了一个单元位于它
  的哪一侧。

## 设备编译单元

由另一个编译器消费的语言写成的源文件是**设备编译单元**。mcpp 据此分类并相应处理:
它从不被扫描 import,也从不产出 BMI —— 因为没有任何设备编译器接受 C++20 modules。

判据是编译器,不是语言(下表为 2026.9.5.3+;在此之前只有 `.cu` 与 `.hip`)。`.sycl`
正是这一区别变得可见的地方:它的内容就是普通 C++,文件里没有任何东西会告诉读者别的。
使它成为设备单元的是「它交给一个带设备后端的编译器」—— 一个 mcpp 不驱动、且不接受
C++20 modules 的编译器。

| 语言 | 扩展名 |
|---|---|
| CUDA、HIP | `.cu`、`.hip` |
| SYCL | `.sycl`(2026.9.6.1+) |
| Ascend C | `.asc`、`.cce`(2026.9.6.5+) |
| GLSL(按 stage) | `.comp`、`.vert`、`.frag`、`.geom`、`.tesc`、`.tese`、`.mesh`、`.task`、`.rgen`、`.rint`、`.rahit`、`.rchit`、`.rmiss`、`.rcall` |
| GLSL(无 stage) | `.glsl` |
| HLSL | `.hlsl` |
| OpenCL C | `.cl` |
| Metal Shading Language | `.metal` |

表外的扩展名若出现在 `[build] sources` 中会被点名拒绝 —— 这正是「这张表是一张表」
的含义:mcpp 对该文件没有任何规则,它的目标文件不会被任何东西链接,构建下去只会
在更晚、更不清楚的地方失败。

上表是 mcpp **不需要被告知**就知道的那些:在「包可以自己声明」之前就已经支持的语言。
规则包通过 `[features].<f>.device_extensions`(见
[04 — mcpp.toml](04-mcpp-toml.md) §2.8)向它增补,而这正是**一门新设备语言到达的方式**
—— 不动引擎,也不需要发一版引擎。Slang 是第一个:`.slang` 不在上表里,由
`mcpp:plugins` 的 `rules-slang` 声明。


`.glsl` 不携带 stage。glslang 从扩展名推导 stage,因此拒绝一个无 stage 的名字是
规则包的事 —— 那条消息属于那里,这张表因此不需要知道哪些扩展名指定了 stage。

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


#### 那个 `extern "C"` 头文件,以及省掉它的代价

语言上不是。接缝可以自己声明入口点、岛自己定义它,一个头文件都不写,照样编得过、链得上、
跑得起来。

头文件买来的是**那份声明只有一份**。没有它就有两份副本分属两个编译器,而它们可以静默地
不一致:

```cpp
// 接缝里
extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n);
// 岛里,在有人把计数加宽之后
extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, std::size_t n);
```

C 语言链接不做名字修饰,所以这两个是同一个符号。链接是干净的,而两侧各按自己的 ABI 解释
参数:没有编译错误,没有链接错误,只有一次读过了参数末尾的运行。同样的错误发生在 C++
边界上会被名字修饰在链接期挡下。

所以**迫使这条边界必须是 `extern "C"` 的那条性质 —— 两侧不共享 C++ ABI —— 正是让声明
分裂无法被发现的同一条性质**。一个普通头文件是模块和设备编译器都能读的最小共同物,这就
是它存在的全部理由。

三条替代都试过,没有一条能去掉它:岛不能包含接缝(`.cppm` 不是 nvcc 解析的东西);从某个
单一来源生成头文件就是头文件多加一步;而模块的 global module fragment 什么都不导出,即使
岛的编译器能读 BMI 也够不到。

### 两类 lane,只有一类的接口是生成的

上面那个接缝是手写的,而 shader 那条 lane 的对应物是生成的。这不是不一致 ——
两条 lane 承载的东西不同。

**设备编译单元是代码,而它上面那层接缝是一个设计决定** —— 有哪些函数、什么类型、
失败怎么报。没有任何生成器能把这个决定做好,所以 CUDA、HIP、SYCL 与 Ascend C 有一个
手写的接缝,下游一律写 `import app.saxpy`。

**而接缝**底下**那个 `extern "C"` 边界不是设计决定。** 它是每个入口点的签名被第二次
写出来,写在一个「不一致看不见」的位置上:C 语言链接不做名字修饰,而设备岛与它的宿主
回落**永不同链**。`mcpp.tools.island` 从两个实现里读出被标记的声明,写出那个头文件和
它之上的模块,于是签名只存在一次,而两个半边如果声明不一致,会在两份文本同时摆在生成器
面前的那一刻被拒绝。`examples/09-heterogeneous/cuda` 与 `.../sycl` 是这个形状;
`.../hip` 保留手写的头文件,两者可以并排读。

**shader 或一份被嵌入的文件是数据。** 它的接口是一个地址加一个尺寸,这是机械的,所以
由规则包生成,消费者同样不写出任何生成物的名字,只写 `import myapp.shaders`。在
mcpp 2026.9.7.1 之前,那条 lane 是唯一的例外:生成的头文件**就是**接口,而每个消费者
都得写出它的名字。

所以规则不是「接口要生成」也不是「接口要手写」。规则是:**机械的接口生成,设计出来的
接口手写,而两种情况下头文件都是没有任何消费者会写出其名字的中间产物。**

#### 一份载荷以什么名字到达

模块名与命名空间是同一条标识符路径,由工程已经写下的名字推导。

| 写下的 | 到达时的样子 |
|---|---|
| `[package] name = "myapp"` | 模块根 `myapp` |
| `shaders/scale.comp` | `myapp::shaders::scale_comp()` |
| `shaders/a/scale.comp` | `myapp::shaders::a::scale_comp()` |
| 一个 `MCPP_EXPORT_C` 入口点 | 边界模块里的 `export using ::the_name;` |

根取自**包名**(非标识符字符替换掉),不是目录名 —— 包放在一个通用目录下时两者不同,
而 mcpp 从 2026.9.7.1 起把包名报给构建程序,正是为了这一条。文件名的 stem 与 stage
构成访问器(`scale.comp` -> `scale_comp`),而相对该组基准目录的子目录成为命名空间段 ——
这就是让两个同 stem 的 shader 成为两样东西而不是一次冲突的机制。想要别的名字就传一个:
`examples/09-heterogeneous/cuda` 就传了,于是它的边界是 `app.kernels`,与接缝
`app.saxpy` 同根。

访问器同时回答地址与字节数。在这个边界上 `sizeof` 不只是别扭,而是**答不出来**:
那些字可能放在一个对象里而不是一个数组里,那时根本没有数组可以取 size。

## 编译一个岛

调用设备编译器的那条命令不内置在 mcpp 里,而是由**构建规则包**提供 ——
以 `host-module = true` 消费,emit 输出汇入链接的构建边。机制见
[30 — build.mcpp](30-build-mcpp.md),可用的 CUDA 规则见 `examples/09-heterogeneous/cuda`。

这个划分是刻意的。mcpp 拥有构建图、产物身份与架构集合;厂商的 flag 拼法、
架构语法与宿主编译器要求属于规则包。

## 工具包跟着规则来(2026.9.6.6+)

一个要 CUDA 岛的工程只写一条边:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-cuda"], host-module = true }
```

**别的什么都不写。** 厂商工具包、它的运行时,以及规则需要的其余东西,都由**规则包**
在选中它的那个 feature、它所服务的加速器之下声明:

```toml
# 写在 mcpp:plugins 里,不写在你的工程里
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = ">=12.9.86"
"xim:cuda-cudart" = ">=12.9.79"
"xim:libcurand"   = ">=10.3.10.19"
"xim:cuda-cccl"   = ">=12.9.27"
```

**两重门,在下载第一个字节之前都要开。** feature 说「要不要这个规则」,
`cfg(accelerator = ...)` 说「哪些构建真的需要设备工具包」。不带加速器的 `mcpp build`
两道门都不过——这正是让最便宜的那次构建保持便宜的原因,而那是 CI 跑的那一次。

每条 lane 都是这样:`rules-cuda`、`rules-hip`、`rules-sycl`、`rules-spirv`、
`rules-ascendc` 各自带着自己的环境。**不存在需要工程自己补环境的 lane**,因为「有时候
需要」是一条没有任何地方能说清楚到可用的规则。

**覆盖是工程里的一行。** 哪个包、最低到哪一版,是规则作者的知识;**具体哪一版**有时
属于工程——一条 CUDA 线耦合着驱动下界,而那是产物将要运行的那批机器的属性:

```toml
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc" = "13.3.33"
```

更近的声明赢,装一个版本,并且 mcpp 说出用了哪条。不满足规则下界的钉会被拒绝并点出
两侧,而不是与它并排装下来。完整规则见 [23 — The Project Environment](23-the-project-environment.md) 的
「一个包一个版本」;`examples/09-heterogeneous/multi-backend` 是本仓库里唯一走覆盖
路径的示例,其余每一个都只写那条边。

## 规则包在第一次编译之前报告的事

设备工具包有三件事出错得很晚,而没有一件是关于构建图的事实。它们由驱动这些工具的
**规则包**读取并报告 —— `mcpp:plugins` 里的 `mcpp.rules.cuda` 逐一演示 —— 引擎一件都
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
[30 — build.mcpp](30-build-mcpp.md) 的探针通道。引擎读到的是一个名字、一个关系、一个
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

## 一个构建里的多个后端

`accel` 点的是一个集合,所以支持多个设备的工程按后端各写一节,集合自己会合成:

```toml
[package]
accelerators = ["cuda", "vulkan"]

[build]
accel   = "cuda12.9+{sm_89}, vulkan1.2"
sources = [
  "src/*.cppm", "src/*.cpp",
  { glob = "src/kernels/**/*.cu",  accel = "cuda12.9+{sm_89}" },
  { glob = "shaders/*.comp",       accel = "vulkan1.2" },
]

[target.'cfg(accelerator = "cuda")'.build]
sources = ["src/cuda/*.cpp"]

[target.'cfg(accelerator = "vulkan")'.build]
sources = ["src/vulkan/*.cpp"]
```

每条收窄的 glob 都被这个集合收窄,所以只写 `--accel vulkan1.2` 时着色器被编译、`.cu`
被留在外面,不需要任何手写条件。

### 多个后端下的 CPU 回退

四个示例各自写的是 `cfg(not(accelerator = "<它自己那个>"))`。这对**只有一个后端**的
工程是对的,对有多个后端的工程是**错的**:一次 CUDA 构建同样满足
`not(accelerator = "vulkan")`,于是 CPU 实现会和 CUDA 实现一起进链接。有接缝时两者定义
同一批 `extern "C"` 符号,结果是实测过的:

```
ld: obj/src/cpu/impl.o: multiple definition of `impl';
    obj/src/cuda/impl.o: first defined here
```

响,而且发生在链接期而不是运行期 —— 但这是 manifest 本可以避免的失败。直接说
「本次构建没有命名任何后端」:

```toml
[target.'cfg(accelerator = "none")'.build]
sources = ["src/cpu/*.cpp"]
```

`none`(mcpp 2026.9.6.5)是**空集**:它在本次构建完全没有命名加速器时成立,并且在生态
长出新后端之后**继续**成立。

**它取代的那种枚举写法当时是对的,但不会一直对。** `none` 之前唯一的拼法是否定整个集合,

```toml
[target.'cfg(not(any(accelerator = "cuda", accelerator = "vulkan")))'.build]
```

于是这一行成了**后端集合被写下的第二个地方**。`any` / `all` / `not` 仍然作为普通布尔
组合子在成员判定之上组合,也确实有需要它们的谓词 —— 但这里不是。`accelerator` 是一个
**开放**词表:第三个后端是一个**包**而不是一次引擎改动,所以它出现的那天,没人记得去
扩的那份枚举就不再表示它写着的意思。而且它**不会响**:视乎忘掉的是哪一半,得到的要么
是给有设备的构建静默选上了 CPU 实现,要么是上面那条链接错误。

`cfg(not(accelerator = "none"))` 是同一句话的反面 —— 「本次构建至少命名了一个后端」
—— 这正是多个后端共用的分发器应当条件化的东西。

### 另一种形态:不在链接期做选择

需要排除,只是因为**接缝**用一个实现替换另一个,所以只能链进去一个。如果目标是「一个
二进制在它遇到的任何机器上都能跑」,就不该在链接期选:

* 把构建时带上的**每一个**后端都编进去 —— 每个 `cfg(accelerator = ...)` 节各加各的
  源码,CPU 实现无条件在内;
* 给每个实现一个不同的名字,让接缝在**运行期**去问哪些设备在场。

这样就完全没有排除关系要维护,而产物回答的是二进制的使用者真正的问题。ggml 就是
这个形态:每个后端自己注册,程序运行时由 `ggml_backend_reg_by_name` 选一个。
`ggml-org:llamacpp` 就是这样构建的,它的 `backend-vulkan` 相对 `backend-cpu` 是**可加的**
而不是互斥的。

选哪个形态是**程序自己的性质**,不是 mcpp 的:用接缝换实现的程序要链接期选择,而要发到
没见过的机器上去的程序要运行期选择。

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

## 各条 lane,以及每条驱动什么

一个规则包拥有一个编译器的拼写。引擎不认识其中任何一个名字:
`tests/unit/test_core_vendor_probes.cpp` 断言剥掉注释后 `src/` 里不出现任何厂商工具名,
并自带分母(枚举到的文件数)。

| `mcpp:plugins` 的 feature | 模块 | 它驱动的编译器 | 它声明的载荷 | `[build] accel` |
|---|---|---|---|---|
| `rules-cuda` | `mcpp.rules.cuda` | 工程自己的 clang(`-x cuda`),或 GCC 工具链下的 nvcc | `xim:cuda-nvcc`、`xim:cuda-cudart`、`xim:libcurand`、`xim:cuda-cccl` | `cuda12.9+{sm_89} ptx>=89` |
| `rules-hip` | `mcpp.rules.hip` | NVIDIA 平台上是工程自己的 clang(`-x cuda`) | 上面那些,再加 `xim:hip-nvidia` | `hip, cuda12.9+{sm_89}` |
| `rules-sycl` | `mcpp.rules.sycl` | `xim:dpcpp` 载荷里的 clang(`-fsycl`) | `xim:dpcpp`;Linux 上另有 `xim:gcc`、`xim:glibc`、`xim:linux-headers`;NVIDIA 目标另加 `xim:cuda-nvcc` | `sycl` 或 `sycl, cuda12.9+{sm_89}` |
| `rules-spirv` | `mcpp.rules.spirv` | `glslangValidator` 或 `glslc` | Linux 上 `xim:glslang`,macOS 与 Windows 上 `xim:shaderc` | `vulkan1.2` |
| `rules-slang` | `mcpp.rules.slang` | `slangc` | `xim:slang` | `vulkan1.2` |
| `rules-ascendc` | `mcpp.rules.ascendc` | CANN 工具包里的 `bisheng`(`-x asc`) | `xim:cann-toolkit` | `ascend8.5+{dav-c220}` |

载荷那一列是每条规则在 `cfg(accelerator = ...)` 之下**为自己**声明的东西,列出来是为了
让一条 lane 的代价在选它之前就可读。工程一个字都不用写——见上文「工具包跟着规则来」。

HIP 与 SYCL 两行的 `accel` 值是**两段**而不是一段。第一段命名编程模型,第二段命名设备,
于是一个设备在本生态里只有一种拼法,无论有多少个模型去够它:`sm_89` 无论被哪条规则读到
都是同一个 `sm_89`。只写 `accel = "sycl"` 而没有第二段,则编译到 SPIR-V,由运行期挑设备。

**NVIDIA 平台上的 HIP 是一层头文件,不是第二个运行时。** 每一个 HIP 入口点都是对应
CUDA 入口点的内联包装,所以目标文件链接的是 CUDA 运行时,机器上没有 ROCm。
`xim:hip-nvidia` 因此不含任何二进制。AMD 平台需要一个本生态尚未发布的 ROCm 运行时,
规则会点名拒绝,而不是产出一个没有东西能链接的目标文件。

**一次 SYCL 构建携带两个 C++ 运行时,而接缝是使这件事安全的东西。** `libsycl.so` 是
对着 libstdc++ 编译的,而 mcpp 的产物链接 libc++,于是两者都在同一个映像里,mcpp 的
重复符号检查会报出它们共有的那些 unwinder 符号。任何东西都不得穿过接缝:SYCL 异常在
设备编译单元里被捕获并转成返回码,因为抛出它的那个运行时不是调用方会用来展开的那个。

## 每条 lane 到得了哪些平台

一条 lane 在某个平台上成立,要三件事同时为真:设备编译器为它发布了、产物需要的运行时
在那里够得到、以及这条规则自己那段按宿主分岔的代码在那里编译得过。第三件是最容易被默认
成立的那一件。`mcpp:plugins` 会为矩阵里的每个平台编译每一条规则
(`tests/all-rules-compile` —— 一个不点名任何 accelerator 的夹具,因此一个字节都不
下载,只问六个模块编不编得过);它把三处潜伏的宿主差异变成了对应 runner 上的编译错误,
而三处没有一处是 Linux 构建看得见的。

| lane | Linux | macOS | Windows | 由什么决定 |
|---|---|---|---|---|
| `rules-spirv` | 是 | 是 | 是 | 着色器编译器三个平台都有发布:Linux 上 `xim:glslang`,macOS arm64 与 Windows x86_64 上 `xim:shaderc` |
| `rules-cuda` | 是 | 否 | 是 | NVIDIA 为 Linux 与 Windows 发布可再分发组件,而自 CUDA 10.2 之后没有为 macOS 发布过工具包 |
| `rules-sycl` | 是 | 否 | 仅 Level Zero 与 OpenCL | Intel 从同一个 tag 发布 `sycl_linux` 与 `sycl_windows`,macOS 一个都没有;上游写明 Windows 不构建 CUDA 与 HIP 插件,而那份资产也确实只带 Level Zero 与 OpenCL 两个适配器 |
| `rules-hip` | 是 | 否 | 否 | NVIDIA 平台的头文件包只为 Linux 发布;AMD 平台要一个本生态在任何平台上都还没发布的 ROCm 运行时 |
| `rules-ascendc` | 是 | 否 | 否 | CANN 工具包只为 Linux 发布 |

**「到得了」在每一行上不是同一个断言,而这个差别写出来而不是留给人推。** 三个平台上
真的**跑过**的是 `rules-spirv`:着色器编译器产出两个 SPIR-V 阶段、产物链接得上,而
Linux 上还渲染出来并把像素与一个软件光栅器逐字节比对。Windows 上**装上并编译过**的是
CUDA 与 SYCL 那条 lane:组件装得上、注册出程序,规则也为那个宿主编译过,但还没有任何
一台 runner 在那里端到端驱动过 `nvcc` 或 `dpcpp`。所以一行写「是」的意思是上面三个条件
成立,不是说有 runner 执行过那条 lane。

**厂商没有为某个平台发布,这个问题就到此为止。** 再多的引擎工作也变不出一个 macOS 的
CUDA 工具包。生态能做的是在一次构建请求跨过那条边界的地方把它说出来,而每条 lane 正是
这样做的:SYCL 规则在 Windows 上拒绝一个提前编译的 NVIDIA 目标,并点名决定这件事的那
条上游发布说明,而不是编出一个运行时装不进去的东西。

**一条 lane 到得了的平台,它到达的方式是同一个。** 一条规则按宿主区别对待的全部内容
就是下面四处,而每一处都是宿主的性质,不是设备的:

- **程序名上的后缀。** `nvcc` 与 `nvcc.exe` 是同一个工具。
- **库在哪里。** ELF 宿主上是 `lib` 与 `lib64`,NVIDIA 的 Windows 布局里是 `lib/x64`。
- **设备编译器驱动的是哪个宿主编译器。** Windows 上 CUDA 规则一律走 clang 路线,与
  项目的编译器无关:nvcc 路线把设备单元的宿主那一半交给 `-ccbin` 命名的编译器,而在那
  个宿主上它只接受 MSVC 的 `cl.exe`,找到它要问机器的 Visual Studio 安装。clang 路线
  不驱动第二个编译器,它自己就会定位 MSVC 的头文件。
- **要把宿主的哪些库挡在外面。** Linux 上 SYCL 规则点名 `xim:gcc`、`xim:glibc` 与
  `xim:linux-headers`,因为 dpcpp 的 clang 不是 mcpp 解析的那个 clang,两个库都没有
  被配置过。Windows 上只有一个 C++ 运行时 —— MSVC 的 —— 两个编译器用的是同一个,所以
  那三条声明在那里根本不存在:要求它们会让构建因为三个本生态不为那个平台发布、而那个
  平台的编译器也不需要的包而失败。

**运行时适配层是 Linux 的构造。** `compat:cuda-runtime`、`compat:sycl-runtime` 与
`compat:vulkan-runtime` 存在,是因为 Linux 上 mcpp 的产物跑在一个不查 `/usr/lib` 的
私有 loader 后面,于是驱动包装的厂商库必须被搬到产物自己的搜索路径上。macOS(dyld)
与 Windows(PE loader)按构造就没有这一层,面向它们的项目一条适配声明都不写。

## 在此之上,一个框架是什么形状

五条 lane 证明了规则包能驱动五个编译器,其中最新的一个来自 NVIDIA 与 Khronos 两个
谱系之外的厂商。框架是下一个问题 —— 这套机制能不能扛起一个
真会被部署的东西 —— 而它自己有一个形状,是在 llama.cpp 的 Vulkan 后端上量出来的。

**框架自带生成器,生态应当驱动它而不是替换它。** llama.cpp 的着色器由一个与后端放在
一起的工具生成,两者在同一个仓库里构成同一份契约。重新实现那套生成,等于造出这份契约
的第二个版本,并且各按各的节奏漂移。`mcpp.rules.spirv` 是给「自己写着色器」的工程用的;
一个**已经有**着色器流水线的工程需要的是把它**声明**出来,不是被替换掉。

**到了那个规模,「声明」就是全部差别。** 134 组着色器在构建程序里生成,是串行的、每次
prepare 跑一遍的,失败只会报成 `build.mcpp exited 1`。同样这 134 组写成 `mcpp::action`
的边,则是增量的、并行的,每一条失败时都说得出自己是谁。这个门槛并不高:
`mcpp.rules.sycl` 已经在声明两条了。

**能力探测属于构建程序,而且只做一次。** 一个着色器编译器接受哪些扩展,是它**怎么被
构建**的性质而不是版本的性质,所以上游读的是编译器自己的拒绝信息。这个答案有两个读者
—— 决定发射哪些变体的生成器,和决定去找哪些变体的后端源码 —— 而两者都在编译开始前
就定死了。问两遍会把一个真相变成两个。

**可选后端是一个 feature,它需要的一切都挂在这个 feature 下。** 包走
`[feature-deps.<f>]`,工具走 `[feature-xlings.<f>]`,于是不点名这个后端的消费者一样
都不会获得。这个主张的判据不是「CPU 构建仍然能用」,而是「CPU 构建的解析结果里不出现
任何属于该后端的包」。

**软件设备并不自动成为硬件的替身。** ggml 只保留类型不为 `eCpu` 的 Vulkan 设备,所以
Mesa 的 lavapipe 仅因类型就被排除 —— 尽管它声明了后端要求的每一项能力。这是框架的
政策而不是打包缺陷,绕过它要用框架自己的选择器,而不是去改打包。

`ggml-org:llamacpp` 以 `backend-vulkan` feature 承载这一整套。

## 当前边界

- 岛形态的 device target,及其隐含的 device link(RDC)。
- OpenMP `target` offload,以及 stdpar。
- HIP 的 AMD 平台。`rules-hip` 只到达 NVIDIA 平台。
- Metal(`.metal`)与 OpenCL C(`.cl`)。两个扩展名都被归类为设备源,而没有任何
  已发布的规则包认领它们,因此声明了其中之一的构建会被拒绝,并点名该文件。
- CUDA 的 13.x 线在 Windows 上。Windows 承载的是 12.x 线。
- `mcpp pack` 不产出 `accel` 字段。需要它的发布方写进描述符。

每条 lane 的按平台边界见*每条 lane 到达哪些平台*一节的表格。

