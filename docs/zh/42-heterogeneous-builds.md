# 42 —— 异构硬件构建

**读者：**把程序的一部分编译到 GPU 或 AI 加速器上的人。

**本章回答的那一个问题：**设备代码怎样被编译并链接进一个普通程序，以及一个预建
产物怎样声明它能在哪些设备上运行。

**不在这里：**编写驱动设备编译器的那条规则，那是
[31 —— 编写规则包](31-authoring-a-rule-package.md)；抵达设备并把它跑起来，那是
[41 —— 抵达一台设备](41-devices.md)。

GPU 与 AI 加速器目标，以及宿主/设备混合编译：mcpp 如何构建设备代码，以及一个预建
产物如何声明它能在哪些设备上运行。

## 支持面

一次构建点名它面向的设备后端，而且**可以点多个**：

```toml
[build]
accel = "cuda12.9+{sm_89}, vulkan1.2"
```

`accelerator` 是目标侧的一个**集合**，因此这次构建里 `cfg(accelerator = "cuda")`
与 `cfg(accelerator = "vulkan")` 同时为真，每个后端的规则包各编各的设备单元，
一个产物把它们全部带上。逗号分隔的是这个集合里的条目；空格分隔的是**同一条目
内部的修饰符**（`cuda12.9+{sm_89} ptx>=89` 是一个后端，加一组架构，再加一条
PTX 下界）。源码集合具体怎么写，见下文「一次构建里的多个后端」。

`accelerator = "none"` 是**空集**——恰好在这次构建完全没有点名任何加速器时
为真。它是这个词表里唯一一个不是后端名字的取值，而它之所以存在，是因为词表的
其余部分是**开放**的：一个会不断增长的集合，不能靠列举来否定。见下文
「CPU 回退」。

今天带有规则包的编程模型有五个——CUDA、HIP、SYCL、Vulkan/SPIR-V 与
Ascend C——「各条 lane」那张表写明了每一个驱动哪个编译器、需要哪些载荷。引擎
本身不持有任何厂商的名字，所以第六个只是**一个包**，而不是一次引擎改动。

## 两种形态，以及两者共用的机制

加速器工具链有两种形态。它们描述的是一个工具链**通常怎样被使用**，不是一个
构建系统需要几套机制。

**岛（island）**把设备代码放进独立的编译单元，由独立的编译器编译。CUDA、HIP、
Ascend C 与 Metal 都是这种形态。设备编译器产出一个目标文件（着色语言则产出
一份运行期资源），汇入普通的链接。

**整目标（whole-target）**把设备代码放进普通的 `.cpp` 里，由一个能够 offload
的编译器编译整个目标。SYCL、OpenMP offload 与 stdpar 通常这样用。

mcpp 只实现**一套**机制——岛。这是一句关于**设计**的陈述，不是关于支持多少
后端的陈述：一个整目标工具链是**经由**岛被接进来的，不是在它旁边另立一套，因此
支持它的代价是一个规则包，而不是第二套机制。SYCL 正因为这个原因被列在已发布的
那一组里。

**SYCL 的设备代码分得开。**一个 SYCL kernel 是 `submit` 里的一个 lambda，因此
一个工程可以按约定把它们全部收进各自的编译单元，而 `.sycl` 就是这条约定的可
检查形式。只有这些单元交给 SYCL 编译器，目标的其余部分由工程自己的工具链编译
——`examples/09-heterogeneous/sycl` 实测正是如此：`app.cppm` 与 `main.cpp` 由
mcpp 的 clang 编译，`saxpy.sycl` 由 dpcpp 载荷的编译器编译。

**OpenMP offload 与 stdpar 的设备代码分不开。**`#pragma omp target` 与
`std::execution::par_unseq` 出现在普通代码里任意的调用点上，没有可搬动的单元，
也就没有可施加的岛。「mcpp 未实现的那种形态」如今指的正是这两个，而这条边界是
**模型本身的性质**，不是本章志向上的缺口。

两个值得直说的后果：

* 一个既有的 SYCL 工程，如果 kernel 写在 `.cpp` 里，**不能原样构建**。它的
  设备单元要先搬进 `.sycl`——那是一次改名加一条接缝，不是重写。
* 一个扩展名只对应一个含义。让收窄过的 glob 去承载 `.cpp`，会让同一个名字因为
  哪条 glob 先匹配而选中两个不同的编译器；接缝之所以可读，正是因为文件名说明了
  一个单元位于接缝的哪一侧。

## 设备编译单元

由另一个编译器消费的语言写成的源文件，是一个**设备编译单元**。mcpp 据此分类
并相应处理它：它从不被扫描 import，也从不产出 BMI——因为没有任何设备编译器接受
C++20 modules。

判据是编译器，不是语言（下表为 2026.9.5.3+；在此之前只有 `.cu` 与 `.hip`）。
`.sycl` 正是这条区分变得可见的地方：它的内容就是普通 C++，文件里没有任何东西
会告诉读者别的。使它成为设备单元的，是「它交给一个带设备后端的编译器」——一个
mcpp 不驱动、也不接受 C++20 modules 的编译器。

| 语言 | 扩展名 |
|---|---|
| CUDA、HIP | `.cu`、`.hip` |
| SYCL | `.sycl` (2026.9.6.1+) |
| Ascend C | `.asc`、`.cce` (2026.9.6.5+) |
| GLSL（按 stage） | `.comp`、`.vert`、`.frag`、`.geom`、`.tesc`、`.tese`、`.mesh`、`.task`、`.rgen`、`.rint`、`.rahit`、`.rchit`、`.rmiss`、`.rcall` |
| GLSL（无 stage） | `.glsl` |
| HLSL | `.hlsl` |
| OpenCL C | `.cl` |
| Metal Shading Language | `.metal` |

表外的扩展名若出现在 `[build] sources` 中，会被点名拒绝——这正是「这张表是一
张表」的含义：mcpp 对该文件没有任何规则，它的目标文件不会被任何东西链接，
构建下去只会在更晚、更含糊的地方失败。

上表是 mcpp **不需要被告知**就已经知道的那些：在「包可以自己声明」之前就已经
支持的语言。规则包经 `[features].<f>.device_extensions`（见
[04 —— mcpp.toml](04-mcpp-toml.md) §2.8）向它增补，而这正是一门**新设备语言**
到达的方式——不改引擎，也不需要发一版新引擎。Slang 是第一个：`.slang` 不在上表
里，由 `mcpp:plugins` 的 `rules-slang` 声明。

`.glsl` 不携带 stage 信息。glslang 从扩展名推导 stage，因此拒绝一个无 stage 的
名字是规则包的事——那条消息属于那里，这张表因此不需要知道哪些扩展名指定了
stage。

`.cuh` 与 `.hiph` 被分类为头文件。它们不被编译，但改动其一可以改变构建图应有的
形状，因此与其它任何头文件一样会使快路径失效。

设备扩展名**不在**默认的 source glob 中。一个把 `.cu` vendored 进来、却在别处
构建它的包，不应当在 mcpp 升级后突然开始编译这个文件——那是它的作者一旦那个
版本发出去就无法修复的破坏。设备源文件必须被显式点名才会进入构建。

## 接缝（seam）

设备编译单元不能 import 模块，因此它与工程其余部分的边界是一个头文件。消费者
看不到这个头文件：一个模块在自己的 global module fragment 里包含它，并导出
一份 C++ 接口，下游一律 import 该模块。

这个模块值得起一个名字，叫**接缝**，因为它存在的理由不是模块边界本身。它是
唯一一处可以在不改动任何消费者的前提下替换掉下层的岛（换成 HIP，换成 CPU
回退）的位置，也是 `cfg(accelerator = ...)` 唯一有落点的位置。一个没有接缝的
工程，没有任何边界可供替换后端。

接口上的两条约束来自编译器本身的事实，不是审美。它应当是 `extern "C"`，因为
设备编译器驱动的是一个 mcpp 没有选择的宿主编译器，两侧因此不共享 C++ ABI。岛
本身应当避开标准库，因为一个链接了 libstdc++ 的岛，会把第二份 C++ 运行时放进
一个自身运行时来自 mcpp 工具链的程序里。

#### 那个 `extern "C"` 头文件，以及省掉它的代价

不是因为语言要求。接缝可以自己声明入口点，岛自己定义它，一个头文件都不写，
照样编得过、链得上、跑得起来。

头文件买来的是「那份声明只存在一份」。没有它，就有两份副本分属两个编译器，
而它们可以静默地不一致：

```cpp
// the seam
extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n);
// the island, after someone widened the count
extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, std::size_t n);
```

C 语言链接不做名字修饰，所以这两个是同一个符号。链接是干净的，而两侧各按自己
的 ABI 解释参数：没有编译错误，没有链接错误，只有一次读过了参数末尾的运行。
同样的错误发生在 C++ 边界上，会被名字修饰在链接期挡下。

所以，**迫使这条边界必须是 `extern "C"` 的那条性质——两侧不共享 C++ ABI——
正是让声明分裂无法被发现的同一条性质**。一个普通头文件是模块和设备编译器都
能读的最小共同物，这就是它存在的全部理由。

三条替代都被考虑过，没有一条能去掉它：岛不能包含接缝（`.cppm` 不是 nvcc 解析
的东西）；从某个单一来源生成头文件，不过是头文件多加一道工序；而模块的
global module fragment 什么都不导出，即使岛的编译器能读 BMI，也够不到它。

### 两类 lane，只有一类的接口是生成的

上面那个接缝是手写的，而 shader 那条 lane 的对应物是生成的。这不是不一致——
两条 lane 承载的东西不同。

**设备编译单元是代码，而它上面那层接缝是一个设计决定**——有哪些函数、什么
类型、失败时怎么报。没有任何生成器能把这个决定做好，所以 CUDA、HIP、SYCL 与
Ascend C 都有一个手写的接缝，下游一律写 `import app.saxpy`。

**接缝底下那个 `extern "C"` 边界不是设计决定。**它是每个入口点的签名被第二次
写出来，写在「不一致时看不见」的那个位置上：C 语言链接不做名字修饰，而设备岛
与它的宿主回退永不同链。`mcpp.tools.island` 从两个实现里读出被标记的声明，
写出那个头文件和它之上的模块，于是签名只存在一次，而两个半边如果声明不一致，
会在两份文本同时摆在生成器面前的那一刻被拒绝。`examples/09-heterogeneous/cuda`
与 `.../sycl` 正是这个形状；`.../hip` 保留手写的头文件，两者因此可以并排读。

**shader 或一份被嵌入的文件是数据。**它的接口是一个地址加一个字节数，这是
机械的，所以由规则包生成，消费者同样不写出任何生成物的名字，只写
`import myapp.shaders`。在 mcpp 2026.9.7.1 之前，那条 lane 是唯一的例外：生成的
头文件**就是**接口，每个消费者都得写出它的名字。

所以规则不是「接口要生成」，也不是「接口要手写」。规则是：**机械的接口生成，
设计出来的接口手写，而两种情况下，头文件都是没有任何消费者会写出其名字的
中间产物。**

#### 载荷到达时的名字

模块名与命名空间是同一条标识符路径，由工程已经写下的名字推导而来。一条规则，
两条 lane 共用：

> 模块名是根。组基准目录以下的每一层目录延长**命名空间**。末端标识符由 lane
> 决定：数据 lane 从文件名推导，因为载荷没有自己的名字；岛 lane 取入口点的
> 名字，因为作者已经写了一个。

| 写下的 | 到达时的样子 |
|---|---|
| `[package] name = "myapp"` | 模块根 `myapp` |
| `shaders/scale.comp` | `myapp::shaders::scale_comp()` |
| `shaders/a/scale.comp` | `myapp::shaders::a::scale_comp()` |
| `kernels/saxpy.cu` 里的 `myapp_saxpy` | `myapp::kernels::myapp_saxpy(...)` |
| `kernels/image/blur.cu` 里的 `myapp_blur` | `myapp::kernels::image::myapp_blur(...)` |

根取自**包名**（非标识符字符被替换掉），不是取自目录名——包放在一个通用目录
下时两者不同，mcpp 从 2026.9.7.1 起把包名报给构建程序，正是为了这一条。文件名
的 stem 与 stage 一起构成访问器（`scale.comp` -> `scale_comp`），相对组基准
目录的子目录成为命名空间段——这正是让两个同 stem 的 shader 成为两样东西、而
不是一次冲突的机制。想要别的名字，传一个就是：`examples/09-heterogeneous/cuda`
就传了，于是它的边界是 `app.kernels`，与它的接缝 `app.saxpy` 同根。

**岛 lane 上文件名什么都不代表，理由就在这张表里。**载荷没有名字，所以数据
lane 必须推导一个，并且需要目录把两个同 stem 的文件分开。入口点已经带着作者
写下的名字，而两个同名入口点无论各自位于哪个目录，都是同一个符号——于是文件名
不命名任何东西，把一个函数在同一目录的两个文件之间搬动，也不会改掉消费者写下
的任何名字。

访问器同时回答地址与字节数。在这个边界上，`sizeof` 不只是别扭，而是**答不
出来**：那些字可能落在一个对象里，而不是一个数组里，那时根本没有数组可以取
size。

### 生成这个边界

`extern "C"` 头文件以及它之上的模块都是机械的，`mcpp:plugins` 的
`mcpp.tools.island` 把两者都写出来。它由工程在自己的 `build.mcpp` 里调用；
它不是一条设备规则，`mcpp:plugins` 里也没有任何规则使用它。

**工程要写的只有一个标记，写在入口点被定义的地方。**

```c
// src/kernels/saxpy.cu -- no include: the generated header arrives through the
// compiler's forced-include flag, which is also what defines the marker as
// nothing.

MCPP_EXPORT_C
int app_saxpy(float a, const float* x, const float* y, float* out, unsigned n)
{ ... }
```

`MCPP_EXPORT_C` 命名的是机制，不是领域：被标记的东西，以 C 链接跨越一个生成
出来的边界被导出。它展开为空——由生成的头文件定义——所以它刻意不以 `_API`
结尾，那个后缀按惯例展开为一个可见性属性。标记名可以经 `options::marker`
配置。

标记做的是选择。岛有自己的内部函数，而一个把一个文件里所有东西都导出的生成器，
会让边界变成那个文件恰好包含了什么的意外结果。

```cpp
const std::string root = std::string(mcpp::manifest_dir());

mcpp::tools::island::options opt;
opt.module_name  = "app.kernels";
opt.out_dir      = std::string(mcpp::out_dir()) + "/island";
opt.roots        = { root + "/src/kernels", root + "/src/cpu" };
opt.layout_root  = root + "/src/kernels";     // the default is roots.front()
opt.strip_prefix = "app_";                     // optional, see below

const auto entries = mcpp::tools::island::scan(opt);
const auto out     = mcpp::tools::island::emit(*entries, opt);
mcpp::generated(out->interface_file.c_str());
```

**根是一棵树，其中一个根供给形状。**每个根持有这个边界的一份实现，多个根
意味着同一个入口点被实现了多次——这正是接缝的常见形态，任何一次链接里都只有
一份实现在场。layout root 的目录才延长命名空间；其余的根只需要定义同样的
名字，因此回退树可以是一个平铺的文件，也可以被重新组织而不改掉消费者写下的
任何名字。这是一个命名上的角色，不是等级：每个根都参与编译、参与链接，同等地
是一份实现。

根是磁盘上的目录，并且必须与加速器无关。一个取自 `mcpp::device_sources()` 的
根，在 `--no-accel` 下会被收窄成空，入口点的命名空间就会改从回退树来——同一个
工程的两次构建会给出不同的限定名。

**两条拒绝，回答的是不同的问题。**同一个名字在**一个**根里出现两次是一次
冲突：C 语言链接不做名字修饰，所以那是同一个符号，而看起来把它们分开的命名
空间，会承诺一种链接器并不提供的隔离。同一个名字出现在**多个**根里，是同一个
入口点的多份实现，这时它们的声明必须逐字一致。后者是工具链里没有别的东西能做
的检查；前者是让命名空间不说谎的那一条。

此外还有两种配置错误会被拒绝：相互重叠的根——一个从两个根都能到达的文件有
两条命名空间路径，而它拿到哪一条取决于这个列表的顺序——以及一个被标记的入口点
都没有的根——一个什么都不导出的模块，比一个写错的路径在这里被指出来要晚得多、
也难懂得多。

**`strip_prefix` 是一种拼法，不是第二个实体。**岛的符号对整个程序是全局的，
所以入口点无论是否落在命名空间里，都带着一个包前缀，命名空间随后又把它重复
一遍。这个选项在 `using ::app_blur;` 旁边写出
`inline constexpr auto blur = app_blur;`。作者写下的名字仍然是规范名——它是
符号，也是 `nm`、链接错误、profiler 与 `dlsym` 显示出来的东西。

**四个等级，每一级覆盖上一级：**

| 级 | 手写的部分 | 消费者写 |
|---|---|---|
| L0 | 只有被标记的入口点 | `import app.kernels` —— 岛自己的 C 形状接口 |
| L1 | 生成模块之上的一个接缝模块 | `import app.saxpy` —— 项目设计的接口 |
| L2 | 接缝，加上用 `island::declared` 构造的条目 | 同上，用于 scan 看不见的入口点 |
| L3 | 头文件与模块都手写 | 同上，签名写了两遍 |

生成的头文件经 `mcpp::tools::island::force_include_flags` 到达岛，这些旗标
交给驱动设备编译器的那条**规则**，不走 `mcpp::cxxflag`——把一个头强制灌进
每个 C++ 翻译单元，会让声明出现在模块接口的 `export module` 行之前，那是
非良构的。由 mcpp 自己编译的宿主实现没有这样一条命令行，它写一行普通的
`#include`。

[`examples/09-heterogeneous/boundary`](../../examples/09-heterogeneous/boundary/)
是 L0，并写明了每一级的代价；`.../cuda` 与 `.../sycl` 是 L1；`.../hip`、
`.../vulkan` 与 `.../cann` 保留手写的头文件，于是两者可以对照着读。

## 编译一个岛

调用设备编译器的那条命令不内置在 mcpp 里，而是由一个**构建规则包**提供——以
`host-module = true` 消费，emit 出的构建图边把输出汇入链接。机制见
[30 —— build.mcpp](30-build-mcpp.md)，可用的 CUDA 规则见
`examples/09-heterogeneous/cuda`。

这个划分是刻意的。mcpp 拥有构建图、产物身份与架构集合；厂商的 flag 拼法、
架构语法与宿主编译器要求属于规则包。

## 工具包跟着规则一起来（2026.9.6.6+）

一个想要 CUDA 岛的工程只写一条边：

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-cuda"], host-module = true }
```

别的什么都不写。厂商工具包、它的运行时，以及规则需要的其余东西，都由**规则包**
在选中它的那个 feature、它所服务的加速器之下声明：

```toml
# mcpp:plugins, not your project
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = ">=12.9.86"
"xim:cuda-cudart" = ">=12.9.79"
"xim:libcurand"   = ">=10.3.10.19"
"xim:cuda-cccl"   = ">=12.9.27"
```

两重门，在下载第一个字节之前都要打开。feature 回答「要不要这条规则」；
`cfg(accelerator = ...)` 选择器回答「哪些构建真的需要这份设备工具包」。不带
加速器的 `mcpp build` 两道门都不打开——这正是让最便宜的那次构建保持便宜的
原因，而那正是 CI 跑的那一次。

每条 lane 都是这样：`rules-cuda`、`rules-hip`、`rules-sycl`、`rules-spirv`
与 `rules-ascendc` 各自带着自己的环境。没有一条 lane 期待工程自己补上这份
环境，因为「有时候需要」是一条没有任何地方能把它说清楚到可用的规则。

**覆盖是工程里的一行。**哪个包、最低到哪一版，是规则作者的知识；**具体是
哪一版**有时属于工程，因为一条 CUDA 线耦合着一个驱动下界，而那是产物将要
运行的那批机器的属性：

```toml
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc" = "13.3.33"
```

更接近的声明胜出，只装一个版本，mcpp 会说出用了哪一个。一个不满足规则下界
的钉会被拒绝，并点名两侧，而不是与规则并排装下来。完整规则见
[23 —— The Project Environment](23-the-project-environment.md) 的「一个包，
一个版本」；`examples/09-heterogeneous/multi-backend` 是本仓库里唯一走覆盖
路径的示例，其余每一个都只写那一条边。

## 规则包在第一次编译之前报告的事

设备工具包有三件事出错得很晚，而没有一件是关于构建图的事实。它们由驱动这些
工具的**规则包**读取并报告——`mcpp:plugins` 里的 `mcpp.rules.cuda` 逐一演示
了它们——引擎一件都不拥有（`tests/unit/test_core_vendor_probes.cpp` 守住这条
线，于是第二个后端不会在 mcpp 里长出第二份拷贝）。

**设备编译器接受哪些宿主编译器。**nvcc 拒绝比它在自己的 `crt/host_config.h`
中声明的上界更新的宿主编译器，而 mcpp 的工具链载荷往往比那个上界更新。走
nvcc 路线时，规则从它解析到的工具包读出这条上界——载荷先于宿主被读，因为经
xlings 安装的工具包才是构建实际会用的那个，通常也是更新的那个（12.9 载荷写着
`gcc <= 14`，而一个发行版的 CUDA 12.0 写着 `gcc <= 12`）——并经 `mcpp::warning`
说出它选了哪个编译器、为什么。走主路线时没有这条上界：`clang -x cuda` 自己
就是宿主编译器。

**设备编译器能否够到自己的后端。**一个工具包可以装好、完整、在 `PATH` 上，
却仍在第一阶段就失败：nvcc 以裸名调用 `cicc`、`cudafe++`、`ptxas`、
`fatbinary`，靠的是它从自己二进制旁边的 `nvcc.profile` 前置进来的一条
`PATH`，而一个替换了 `/etc` 的沙箱会拿走 Debian 打包的那份 profile。规则
向 nvcc 要它的计划（`nvcc --dryrun`），而不是假设一个，逐个解析各阶段，
点名第一个解析不到的、以及提供它的那个载荷。一次产不出计划的 dryrun，不
产生任何结论。

**驱动是否新到足以承载这份运行时。**设备运行时不能比它将要遇到的驱动更新；
更新时，构建编译干净、链接干净，直到第一次内存分配才以
*"CUDA driver version is insufficient for CUDA runtime version"* 失败。
规则经驱动自己的库读出驱动版本（经由 sentinel 包够到，绝不经 `/usr/lib`）并
把它陈述为事实；陈述它的运行时需要的下界；引擎在编译任何东西之前比较两者——
见 [30 —— build.mcpp](30-build-mcpp.md) 的探针通道。引擎读到的是一个名字、
一个关系、一个版本；`cuda.driver` 是流过引擎的数据。

凡是「答错比不答更贵」的地方，只报告，不强制：一台工程里没有规则包的机器，
没有任何厂商相关的话要说，于是什么都不说；够不到答案的探针，不会发明一个
答案。

## 声明一次构建的目标

```toml
[build]
accel = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

可用 `--accel` 为单次构建覆盖——这与 `--target` 对 `[toolchain]` 的关系相同。
`--no-accel` 不是「没有写 `--accel`」，它是**显式请求不要加速器**，这正是在
一个同时发布了设备构建的包里选中 CPU-only 变体所需要的。

一个源码包可以声明它支持哪些后端：

```toml
[package]
accelerators = ["cuda", "rocm"]
```

这与 `[package] platforms` 同形：一个意图声明与 CI 矩阵提示，不是一道门。
它与产物的 `accel` 刻意是两个不同的字段——声明由人手写，可以是期望值；而
产物的字段是从产生它的那次构建**测量**出来的。

## 预建产物声明的内容

携带设备代码的产物，把它记在兼容性标签旁边：

```toml
[[runtime.artifacts]]
role       = "static-library"
path       = "lib/libgpukit.a"
provenance = "mcpp-pack/1"
abi        = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
accel      = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

它是标签之外的独立字段，不是标签的一段，因为架构列表是一个**集合**，而标签是
用 `-` 拼接的字符串，其中的 triple 本身就含有数量不定的 `-`。

`accel` 缺席意味着该产物不携带设备代码，因而不施加任何约束——这正是一个
纯 CPU 库可被任何构建使用的原因。

### 消费方的匹配

当一次构建请求的每个后端，产物都声明了该后端、工具包主版本一致、并且覆盖了
请求的每个架构时，该产物就满足这次构建。一个架构被覆盖，当它被直接列出，或
一个同 major、minor 不高于它的家族目标被列出，或内嵌可移植形式的下界不高于
它。

家族目标与可移植形式，是让变体矩阵保持有限的机制。一颗芯片发一份产物不可
扩展，一个世代发一份可以。

### 语法是开放的

`cuda`、`hip`、`vulkan`、`sycl` 不是一个封闭集合。**后端名 + 版本 + 架构
集合 + 可选下界**就是全部的形状，mcpp 在没有一张「谁存在」的表的情况下比较
它们：

```
vulkan1.3+{spirv1.6} floor>=1.4
sycl2020+{spir64,nvptx64-sm_89}
hip6.4+{gfx942}
```

一个拼法里不带前导数字的架构——比如 `gfx942`——按**相等**比较，因为那里读
不出任何序。这是答案，不是权宜之计：从中间抠出 `942` 会发明一个 AMD 并未
定义的等级。

`floor>=` 是可移植形式下界的**后端中立**拼法。`ptx>=` 是 CUDA 对同一字段
的说法，仍然被接受，因此此前写下的描述符不受影响；一个可移植形式是 SPIR-V
的后端写 `floor>=`，不必借用 NVIDIA 的词。

各拼法对某个具体后端意味着什么，是**那个后端的规则包**的事。引擎持有的只是
形状与比较方式。

没有任何产物匹配时，拒绝会点名维度与两侧的取值：

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

这正是这个维度存在所要搬移的那种失败。没有它，构建会干净地链接完成，而程序
在第一次 kernel 启动时才失败，消息里既没有包名，也没有任何一侧期望的架构。

### 发布多个变体

一个包可以发布多个产物，消费方取第一个标签接受它的。**把 CPU-only 的产物排
在第一位。**一个早于 `accel` 字段的 mcpp 会忽略这个字段，而顺序正是让这样的
客户端仍然拿到一个到处都能跑的产物的唯一机制。

## 按后端条件化

`accelerator` 是目标侧的一层，在依赖图解析之后才被求值，并且它持有一个
**集合**，而不是单个取值：

```toml
[target.'cfg(accelerator = "rocm")'.build]
cxxflags = ["-DMYAPP_ROCM"]
```

比较是**成员判定**，因此一次同时启用 CUDA 与 ROCm 的构建，对两者都答真。
`any`、`all`、`not` 作为普通布尔组合子在其上组合。

## 一次构建里的多个后端

`accel` 点名的是一个集合，因此支持多个设备的工程按后端各写一节，集合自己
会合成：

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

每条收窄过的 glob 都被这个集合收窄，因此只写 `--accel vulkan1.2` 时，着色器
被编译，`.cu` 被留在外面，不需要任何手写的条件。

### 多个后端下的 CPU 回退

四个示例各自写的都是 `cfg(not(accelerator = "<它自己那个>"))`，这对只有**一个**
后端的工程是对的，对有多个后端的工程是**错的**：一次 CUDA 构建同样满足
`not(accelerator = "vulkan")`，于是 CPU 实现会和 CUDA 实现一起进入链接。有
接缝时，两者定义同一批 `extern "C"` 符号，结果是实测过的：

```
ld: obj/src/cpu/impl.o: multiple definition of `impl';
    obj/src/cuda/impl.o: first defined here
```

响，而且发生在链接期而不是运行期——但这是 manifest 本可以避免的失败。直接说
「这次构建没有点名任何后端」：

```toml
[target.'cfg(accelerator = "none")'.build]
sources = ["src/cpu/*.cpp"]
```

`none`（mcpp 2026.9.6.5）是**空集**：它在这次构建完全没有点名加速器时成立，
并且在生态长出新后端之后**继续**成立。

**它取代的那种枚举写法，当时是对的，但没有一直对下去。**在 `none` 之前，唯一
的拼法是否定整个集合，

```toml
[target.'cfg(not(any(accelerator = "cuda", accelerator = "vulkan")))'.build]
```

于是这一行成了**后端集合被写下的第二个地方**。`any`、`all`、`not` 仍然作为
普通布尔组合子，在成员判定之上组合，也确实存在需要它们的谓词——但这里不是。
`accelerator` 是一个**开放**的词表：第三个后端是一个**包**，不是一次引擎改动，
所以它出现的那一天，没人记得去扩的那份枚举，就不再表示它写着的意思。而且它
**不会响**：视乎忘掉的是哪一半，得到的要么是给一个有设备的构建静默选上了
CPU 实现，要么是上面那条链接错误。

`cfg(not(accelerator = "none"))` 是同一句话的反面——「这次构建至少点名了
一个后端」——这正是多个后端共用的分发器应当条件化的东西。

### 另一种形态：不在链接期做选择

需要排除，只是因为**接缝**用一个实现替换另一个，所以只能链进去一份。如果
目标是「一个二进制在它遇到的任何机器上都能跑」，就不应该在链接期选择：

* 把构建时带上的**每一个**后端都编进去——每个 `cfg(accelerator = ...)` 节
  各加各的源码，CPU 实现无条件在内；
* 给每一份实现一个不同的名字，让接缝在**运行期**去问哪些设备在场。

这样就完全没有排除关系要维护，产物回答的是二进制的使用者真正的问题。ggml
就是这个形态：每个后端自己注册，程序运行时由 `ggml_backend_reg_by_name`
选一个。`ggml-org:llamacpp` 就是这样构建的，它的 `backend-vulkan` feature
相对 `backend-cpu` 是**可加的**，不是互斥的。

选哪个形态是**程序自己的性质**，不是 mcpp 的性质：一个用接缝替换实现的程序，
想要链接期选择；一个要发到没见过的机器上的程序，想要运行期选择。

## 链接行上出现第二个 C++ 运行时时，只保留一个 unwinder

一条 lane 的设备编译器，如果是按 libstdc++ 配置的，链接行上就会出现
libstdc++，而产物本身静态链接 libc++。两者同时出现在镜像里，mcpp 的重复符号
检查会报出它们共有的那些符号。

对其中大多数符号，后果只是「调用了另一份可互换的实现」。对 unwinder 不是。
静态归档只贡献被引用到的成员，所以这种抢占**按构造是部分的**：在 SYCL lane
上实测，libgcc 的 18 个 `_Unwind_*` 入口点里，有 10 个来自产物，8 个仍留在
libgcc_s 里，其中包括 personality 例程要用的那几个访问器。于是 libstdc++ 的
personality 拿 libgcc 的访问器去读一个 LLVM libunwind 的 context，找不到
landing pad，越过三帧之上一个本应命中的 handler，直接调用了
`std::terminate`。在有东西抛出之前，这个程序一直是正确的。

因此，当链接行上出现 libstdc++、而工具链自带的标准库是 libc++ 时，mcpp 改从
libgcc 取 unwinder（`--unwindlib=libgcc`），不再链载荷的 `libunwind.a`，并用
`--exclude-libs` 把静态归档的符号挡在动态符号表之外。libgcc_s 本来就在
进程里——libstdc++ 需要它——所以这一步只是点名一个已有的库，不是新增一个，
C++ 运行时仍然是内嵌的。链接行上没有第二个运行时的构建，一字节不变。

## 两条值得写明的边界

**`--accel` 与 `--no-accel` 是 `build`、`run`、`test` 三者共有的选项**
（run 与 test 自 2026.9.5.2 起），与 `--target`、`--profile` 同级；`pack` 与
读取其它任何构建输入一样，从 manifest 读 `[build] accel`。这个 flag 起初只
挂在 `build` 上，实测的后果是一个工程的 CPU-only 变体能构建却不能运行：
`mcpp build --no-accel` 产出了它，而 `mcpp run` 交回的却是设备构建。

**`mcpp pack` 不写出 `accel` 字段。**它本可以把 manifest 声明的值原样写进去，
而这恰恰是它不这么做的理由：该字段陈述的是产物**携带**了什么，而 mcpp 目前
不自己编译设备代码——形态 A 走规则包，mcpp 没有可测量的对象。把一个声明写进
一个含义是「测量所得」的字段，会让身份恰好以这个维度存在要防止的那种方式
说谎。今天由发布者显式写这个字段，索引描述符就是这么做的；等
`kind = "device"` 把设备编译放进 mcpp 之后，`mcpp pack` 才会发出它。

## 各条 lane，以及它们各自驱动的工具链

一个规则包拥有一个编译器的拼写。引擎不认识其中任何一个名字：
`tests/unit/test_core_vendor_probes.cpp` 断言剥掉注释之后，`src/` 里不出现
任何厂商工具名，并自带分母（枚举到的文件数）。

| `mcpp:plugins` 的 feature | 模块 | 它驱动的编译器 | 它声明的载荷 | `[build] accel` |
|---|---|---|---|---|
| `rules-cuda` | `mcpp.rules.cuda` | 工程自己的 clang（`-x cuda`），或 GCC 工具链下的 nvcc | `xim:cuda-nvcc`、`xim:cuda-cudart`、`xim:libcurand`、`xim:cuda-cccl` | `cuda12.9+{sm_89} ptx>=89` |
| `rules-hip` | `mcpp.rules.hip` | NVIDIA 平台上是工程自己的 clang（`-x cuda`） | 上面那些，再加 `xim:hip-nvidia` | `hip, cuda12.9+{sm_89}` |
| `rules-sycl` | `mcpp.rules.sycl` | `xim:dpcpp` 载荷里的 clang（`-fsycl`） | `xim:dpcpp`；Linux 上另有 `xim:gcc`、`xim:glibc`、`xim:linux-headers`；NVIDIA 目标另加 `xim:cuda-nvcc` | `sycl` 或 `sycl, cuda12.9+{sm_89}` |
| `rules-spirv` | `mcpp.rules.spirv` | `glslangValidator` 或 `glslc` | Linux 上 `xim:glslang`，macOS 与 Windows 上 `xim:shaderc` | `vulkan1.2` |
| `rules-slang` | `mcpp.rules.slang` | `slangc` | `xim:slang` | `vulkan1.2` |
| `rules-ascendc` | `mcpp.rules.ascendc` | CANN 工具包里的 `bisheng`（`-x asc`） | `xim:cann-toolkit` | `ascend8.5+{dav-c220}` |

载荷那一列是每条规则在 `cfg(accelerator = ...)` 之下为自己声明的东西，列
出来是为了让一条 lane 的代价在被选中之前就可读。工程一个字都不用写——见上文
「工具包跟着规则一起来」。

HIP 与 SYCL 两行的 `accel` 值是**两段**，不是一段。第一段命名编程模型，第二段
命名设备，因此一个设备在本生态里只有一种拼法，不论有多少个模型去够它：`sm_89`
不论被哪条规则读到，都是同一个 `sm_89`。只写 `accel = "sycl"`、没有第二段时，
编译目标是 SPIR-V，由运行期挑选设备。

**NVIDIA 平台上的 HIP 是一层头文件，不是第二个运行时。**每一个 HIP 入口点都是
对应 CUDA 入口点的内联包装，因此目标文件链接的是 CUDA 运行时，机器上没有
ROCm。`xim:hip-nvidia` 因此不含任何二进制。AMD 平台需要一个本生态尚未发布的
ROCm 运行时，规则会点名拒绝，而不是产出一个没有东西能链接的目标文件。

**一次 SYCL 构建携带两个 C++ 运行时，而接缝是让这件事安全的机制。**
`libsycl.so` 是对着 libstdc++ 编译的，而一个 mcpp 产物链接 libc++，于是两者
都在同一个镜像里，mcpp 的重复符号检查会报出它们共有的那些 unwinder 符号。
任何东西都不得穿过接缝：一个 SYCL 异常在设备编译单元里被捕获，转成一个返回码，
因为抛出它的那个运行时，不是调用方会用来展开的那一个。

## 每条 lane 能到达的平台

一条 lane 在某个平台上成立，要三件事同时为真：设备编译器为它发布了、产物
需要的运行时在那里够得到、以及这条规则自己那段按宿主分岔的代码在那里编译
得过。第三件是最容易被默认成立的一件。`mcpp:plugins` 会为矩阵里的每个平台
编译每一条规则（`tests/all-rules-compile`——一个不点名任何 accelerator 的
夹具，因此一个字节都不下载，只问这些模块编不编得过）；这个夹具把三处潜伏的
宿主差异，变成了对应 runner 上的编译错误，而这三处没有一处是 Linux 构建
看得见的。

| lane | Linux | macOS | Windows | 决定因素 |
|---|---|---|---|---|
| `rules-spirv` | 是 | 是 | 是 | 着色器编译器三个平台都有发布：Linux 上是 `xim:glslang`，macOS arm64 与 Windows x86_64 上是 `xim:shaderc` |
| `rules-cuda` | 是 | 否 | 是 | NVIDIA 为 Linux 与 Windows 发布可再分发组件，自 CUDA 10.2 之后没有再为 macOS 发布过工具包 |
| `rules-sycl` | 是 | 否 | 仅 Level Zero 与 OpenCL | Intel 从同一个 tag 发布 `sycl_linux` 与 `sycl_windows`，macOS 一个都没有；上游写明 CUDA 与 HIP 插件不为 Windows 构建，而那份资产也确实只带 Level Zero 与 OpenCL 两个适配器 |
| `rules-hip` | 是 | 否 | 否 | NVIDIA 平台的头文件包只为 Linux 发布；AMD 平台需要一个本生态在任何平台上都还没发布的 ROCm 运行时 |
| `rules-ascendc` | 是 | 否 | 否 | CANN 工具包只为 Linux 发布 |

**「到得了」在每一行上不是同一个断言，而这个差别是写出来的，不是留给人推。**
三个平台上真的**跑过**的是 `rules-spirv`：着色器编译器产出两个 SPIR-V 阶段、
产物链接得上，在 Linux 上它还渲染出来了，像素与一个软件光栅器逐字节比对过。
Windows 上**装上并编译过**的是 CUDA 与 SYCL 那条 lane：组件装得上、注册出
程序，规则也为那个宿主编译过，但还没有任何一台 runner 在那里端到端驱动过
`nvcc` 或 `dpcpp`。所以一行写「是」的意思，是上面三个条件成立，不是说有
runner 执行过那条 lane。

**一个厂商没有为某个平台发布，这个问题就到此为止。**再多的引擎工作，也变不出
一份 macOS 的 CUDA 工具包。这个生态能做的，是在一次构建请求跨过那条边界的地方
把它说出来，而每条 lane 正是这样做的：SYCL 规则在 Windows 上拒绝一个提前编译
的 NVIDIA 目标，并点名决定这件事的那条上游发布说明，而不是去编出一个运行时
装不进去的东西。

**一条 lane 到得了的平台，它到达的方式是同一个。**一条规则按宿主区别对待的
全部内容，就是下面四处，而每一处都是宿主的性质，不是设备的性质：

- **程序名上的后缀。**`nvcc` 与 `nvcc.exe` 是同一个工具。
- **库在哪里。**ELF 宿主上是 `lib` 与 `lib64`，NVIDIA 的 Windows 布局里是
  `lib/x64`。
- **设备编译器驱动的是哪个宿主编译器。**Windows 上，CUDA 规则一律走 clang
  路线，与工程的编译器无关：nvcc 路线把设备单元的宿主那一半交给 `-ccbin`
  命名的编译器，而在那个宿主上它只接受 MSVC 的 `cl.exe`，找到它要问机器的
  Visual Studio 安装。clang 路线不驱动第二个编译器，它自己会定位 MSVC 的
  头文件。
- **要把宿主的哪些库挡在外面。**Linux 上，SYCL 规则点名 `xim:gcc`、
  `xim:glibc` 与 `xim:linux-headers`，因为 dpcpp 的 clang 不是 mcpp 解析
  出来的那个 clang，两个库都没有被它配置过。Windows 上只有一个 C++ 运行时
  ——MSVC 的——两个编译器用的是同一个，因此那三条声明在那里根本不存在：
  要求它们，会让构建因为三个本生态不为那个平台发布、而那个平台的编译器也
  不需要的包而失败。

**运行时适配层是 Linux 特有的构造。**`compat:cuda-runtime`、
`compat:sycl-runtime` 与 `compat:vulkan-runtime` 之所以存在，是因为
Linux 上一个 mcpp 产物运行在一个不查 `/usr/lib` 的私有 loader 之后，因此
一份由驱动包装好的厂商库，必须被搬到产物自己的搜索路径上。macOS（dyld）与
Windows（PE loader）按构造就没有这一层，面向它们的工程一条适配声明都不写。

## 建立在此之上的框架的形状

五条 lane 证明了一个规则包能驱动五个编译器，其中最新的一个来自 NVIDIA 与
Khronos 两个谱系之外的厂商。框架是下一个问题——这套机制能不能扛起一个真会被
部署的东西——而它自己有一个形状，是在 llama.cpp 的 Vulkan 后端上量出来的。

**框架自带生成器，这个生态应当驱动它，而不是替换它。**llama.cpp 的着色器，
由一个和后端放在一起的工具生成，两者在同一个仓库里构成同一份契约。重新实现
那套生成，等于造出这份契约的第二个版本，并且各按各的节奏漂移。
`mcpp.rules.spirv` 是给「自己写着色器」的工程用的；一个**已经有**着色器
流水线的工程，需要的是把它**声明**出来，不是被替换掉。

**到了那个规模，「声明」就是全部差别。**134 组着色器如果在构建程序里生成，
是串行的、每次 prepare 都要跑一遍的，失败也只会报成
`build.mcpp exited 1`。同样这 134 组写成 `mcpp::action` 的边，则是增量的、
并行的，每一条失败时都说得出自己是谁。这个门槛并不高：`mcpp.rules.sycl` 已经
在声明两条了。

**能力探测属于构建程序，而且只做一次。**一个着色器编译器接受哪些扩展，是它
**怎么被构建**的性质，不是它版本的性质，所以上游读的是编译器自己的拒绝信息。
这个答案有两个读者——决定发射哪些变体的生成器，和决定去找哪些变体的后端
源码——而两者都在编译开始前就定死了。问两遍，会把一个真相变成两个。

**可选后端是一个 feature，它需要的一切都挂在这个 feature 下面。**包走
`[feature-deps.<f>]`，工具走 `[feature-xlings.<f>]`，因此不点名这个后端的
消费方，一样都不会拿到。这个主张的判据不是「CPU 构建仍然能用」，而是
「CPU 构建的解析结果里，不出现任何属于该后端的包」。

**软件设备并不自动成为硬件的替身。**ggml 只保留类型不是 `eCpu` 的 Vulkan
设备，因此 Mesa 的 lavapipe 仅因为类型就被排除——尽管它声明了这个后端要求的
每一项能力。这是框架的策略，不是打包上的缺陷，绕过它要用框架自己的选择器，
而不是去改打包。

`ggml-org:llamacpp` 用它的 `backend-vulkan` feature 承载这一整套。

## 当前边界

- 岛形态的设备目标，以及它隐含的设备链接（RDC）。
- OpenMP `target` offload，以及 stdpar。
- HIP 的 AMD 平台。`rules-hip` 只到达 NVIDIA 平台。
- Metal（`.metal`）与 OpenCL C（`.cl`）。两个扩展名都被归类为设备源文件，而
  没有任何已发布的规则包认领它们，因此一次点名了其中之一的构建会被拒绝，
  并点名该文件。
- CUDA 的 13.x 线在 Windows 上。Windows 上承载的是 12.x 线。
- `mcpp pack` 不产出 `accel` 字段。需要它的发布方把它写进描述符。

每条 lane 按平台划分的边界，见上文「每条 lane 能到达的平台」一节的表格。
