# 异构计算与图形的跨平台生态:完整矩阵与补齐方案

> 状态:设计,未实现。每一条「现状」都标注了它是**读**出来的还是**跑**出来的。
> 伴随文档:`docs/20-heterogeneous-builds.md`(规范)、
> `2026-09-07-package-identity-and-doc-alignment.md`(规则自带环境)。

## 0. 这份方案要解决的一句话

**引擎与规则层已经与平台无关,而生态只在 Linux 上是完整的。** 这不是一个工程问题:
四个设备编译器里有三个在 Windows 上、两个在 macOS 上**已经有上游产物**,缺的是把它们
发出来。

## 1. 现状:分层看,缝在哪一层

### 1.1 引擎与规则层:与平台无关(**读** + **跑**)

`accel` 轴、`cfg(accelerator = ...)`、受约束 glob、产物的 accel 标签、规则包的
`feature-xlings` 声明 —— 没有一处按宿主分支。e2e 628(工具版本冲突的拒绝)不设能力门,
三平台都跑。引擎里零个厂商名,由 `test_core_vendor_probes.cpp` 按文件数做分母守着。

**这一层不需要补。**

### 1.2 载荷层:linux-only(**跑** index 查询)

`glslang` `shaderc` `cuda-nvcc` `cuda-cudart` `libcurand` `cuda-cccl`
`hip-nvidia` `dpcpp` `mesa-lavapipe` `cann-toolkit` —— **全部只有 `linux` 块**。

### 1.3 库层:三平台齐全(**跑**)

`compat.vulkan` `glfw` `sdl2` `imgui` `opengl` `vulkan-memory-allocator`
`spirv-reflect` —— 都有 `linux macosx windows`。

**所以缝恰好在中间那一层**:一个 Windows 开发者今天能链接 Vulkan、开窗、画三角形,
只要他自己准备着色器编译器 —— 而那正是构建系统该管的那一件事。

### 1.4 适配器层:linux-only 是结构性的,不是缺口

`compat:vulkan-runtime` / `sycl-runtime` / `cuda-runtime` 修的是**mcpp 私有 loader
够不到宿主驱动**。那是 ELF/`PT_INTERP` 的问题;macOS 走 dyld、Windows 走 PE loader,
两边都没有这一层。**它们不该被"补到三平台",它们在那两个平台上不存在。**

驱动本身在三个平台上是同一条规则:**驱动是宿主能力,设备是载荷**。
`compat.vulkan` 已经把它建模成 `vulkan.icd.driver` 能力(macOS 上是 MoltenVK,
Windows 上是任何 GPU 驱动装的 `vulkan-1.dll`)。

## 2. 关键发现:多数空格是发布工作(**跑**出来的)

逐个核过上游产物的可得性:

| 后端 / 工具 | linux-x86_64 | linux-aarch64 | windows-x86_64 | macos-arm64 |
|---|---|---|---|---|
| CUDA(nvcc, cudart, curand, cccl) | 已发布 | **redist 有**(重打包) | **redist 有**(重打包) | **不可能** |
| SYCL(`dpcpp`) | 已发布 | 未发布 | **`sycl_windows.tar.gz`**(重打包) | 上游不发 |
| SPIR-V(`shaderc`/glslc) | 已发布 | 上游有 | **Google 产物桶有**(重打包) | **Google 产物桶有**(重打包) |
| `glslang` | 已发布(本生态自建) | — | 上游无二进制 | 上游无二进制 |
| HIP(NVIDIA 平台,仅头文件) | 已发布 | 头文件与平台无关 | 头文件与平台无关 | 不适用 |
| Ascend(`cann-toolkit`) | 已发布 | `.run` 内含 aarch64 | 无 | 无 |
| 软件设备(`mesa-lavapipe`) | 已发布(自建 Mesa) | — | 需 Mesa-on-Windows 构建 | 不适用 |
| Metal | 不适用 | 不适用 | 不适用 | **仅 Xcode 内**,不可再分发 |

核过的三条直链:

- NVIDIA `redistrib_12.9.1.json` 里 `cuda_nvcc` / `cuda_cudart` / `libcurand` /
  `cuda_cccl` 四个都列着 `linux-x86_64  linux-sbsa  windows-x86_64  linux-aarch64`
  —— **索引今天用的就是这棵 redist 树**,只取了其中一个平台;
- `intel/llvm` 的同一个 release 里同时有 `sycl_linux.tar.gz` 与
  **`sycl_windows.tar.gz`**;
- Google 的 shaderc 产物桶前缀:`linux/` `macos/` `windows-vs2022-amd64-release/`,
  macOS 与 Linux 的 `install.tgz` 直链已核 200。

**结论:CUDA 上 Windows、SYCL 上 Windows、着色器编译器上 Windows 与 macOS,都是重新
打包而不是从源码构建。** 而 macOS 的 CUDA 与 SYCL 是永久的洞(NVIDIA 与 Intel 都不发),
这是要设计**绕过**的事实,不是要补的缺口。

## 3. 可移植性分层:给使用者的模型

生态可用的前提是使用者能**预期**哪条路能走多远。三层:

| 层 | 后端 | 平台 | 使用者写什么 |
|---|---|---|---|
| **T1 可移植** | Vulkan compute、Vulkan graphics、OpenCL | linux / macos / windows | `accel = "vulkan1.2"`,一份 manifest 三平台通 |
| **T2 双平台** | CUDA、SYCL | linux / windows | 同一份 manifest;macOS 上该后端的 `cfg` 不激活,走 CPU 接缝 |
| **T3 单平台** | Metal(macOS)、Ascend(linux)、ROCm(linux) | 各一 | 同上 |

**跨这三层不需要新机制** —— `accel` 是集合、`cfg(accelerator = ...)` 是成员判定、
接缝把实现藏在模块后面,这些已经有了。一个想要「到处能跑」的工程写:

```toml
[build]
accel = "vulkan1.2"
sources = [ "src/*.cppm", "src/*.cpp",
            { glob = "shaders/*.comp", accel = "vulkan1.2" } ]

[target.'cfg(accelerator = "none")'.build]
sources = ["src/cpu/*.cpp"]
```

而一个想要「有 NVIDIA 就用 CUDA」的工程再加一块 `cfg(accelerator = "cuda")`,并在
macOS 上自然退回 T1 或 CPU。

**T1 是这份方案要保证的那一层。** T2/T3 是能力,T1 是承诺。

## 4. CI 能断言到哪:软件设备决定上限

「生态级可用」的判据不是构建通过,是**有东西在设备上跑过**。而无头 runner 上有没有
设备,取决于有没有**软件设备**:

| 平台 | 软件设备 | CI 上限 |
|---|---|---|
| linux | `xim:mesa-lavapipe`(已有,CI 每次构建都跑到它并断言设备名) | **运行** |
| windows | 需要 Mesa-on-Windows 的 lavapipe,或 SwiftShader | 今天是**构建** |
| macos | MoltenVK 之上是否有可用 Metal 设备,**未实测** | 今天是**构建** |

规则写死:**没有软件设备的平台,CI 的上限就是构建,而这一条要写进 README 而不是被
后来的人发现**。把「构建通过」当成「能用」正是本轮反复付过代价的形状。

第四期(§6)专门抬这条上限,因为它是「生态级可用」与「能编译」的分界。

## 5. 图形管线:第一条要补的,而判据不是窗口

### 5.1 现状(**读** + **跑**)

`rules-spirv` 的 `stage_of` 覆盖十四个阶段(`.comp .vert .frag .geom .tesc .tese
.mesh .task` 与五个光追阶段),产物形状对每个阶段相同:一个 shader 一个头,符号
`<stem>_<stage>_spv`。而全树 `.vert`/`.frag`/`.mesh` **零个文件** ——
**声明覆盖、执行为零**。

### 5.2 主示例必须是离屏渲染

直觉写法是 glfw + swapchain。**那样 CI 断言不了任何东西**:无头 runner 没有 surface,
而「构建通过」对图形管线几乎零信息量 —— `.frag` 编译成常量色、顶点输入接错,构建同样
通过。

离屏渲染走的是**完整图形管线**(顶点输入 → 光栅化 → 片元输出 → render pass),不需要
窗口/surface/swapchain,结果是确定像素,而且跑在**CI 里已经在工作的那个设备上**。

`examples/10-graphics/offscreen`:渲染顶点色三角形到 image,拷回主机内存,断言像素。

判据:

| # | 断言 | 为什么不空转 |
|---|---|---|
| G1 | `triangle_vert.h` 与 `triangle_frag.h` 都存在 | 只断言其一,只处理第一个源的规则也通过 |
| G2 | 两个头的 SPIR-V magic 都是 `07230203` | 空文件也「存在」 |
| G3 | 程序打印它用的设备名,且是 lavapipe | 悄悄回落 CPU 腿会打印同样的像素 |
| G4 | **中心像素**在三顶点色的插值范围内,**四角**是清除色 | `.frag` 写成常量、顶点输入接错 → 这条红而 G1–G3 全绿 |
| G5 | `--no-accel` 走 CPU 腿并退 0 | 反向腿 |

窗口那一层放进 `--features window`,CI 只构建不运行。**能被断言的和不能被断言的分开
摆** —— 混在一起,CI 只能整个跳过。

### 5.3 已从代码读出、待证实的一处缺陷

着色器输出路径是 `gen / (stem + "_" + stage)`,**不含目录分量**。于是
`shaders/ui/text.vert` 与 `shaders/world/text.vert` 落在同一个 `text_vert.h`。
compute 示例每个工程只有一个 `.comp`,碰不到;真实图形工程按用途分目录是常态。

修法与引擎侧 mcpp#239/#240 的对象路径消歧同形,并且两个源映射到同一输出时必须
**拒绝并点名两者** —— 静默覆盖是唯一不可接受的形态。一期的示例里**故意放两个同名
不同目录的 shader,先看它坏**。

## 6. 补齐路线与分期

| 期 | 内容 | 依赖 | 它买到什么 |
|---|---|---|---|
| 一 | `examples/10-graphics/offscreen`(linux)+ G1–G5 + 同名 stem 修复 | 无 | 图形那一半第一次被执行 |
| 二 | `shaderc` 发 macOS/Windows;`rules-spirv` 按平台声明默认;图形示例三平台**构建** | 一 | **T1 的着色器编译在三平台成立** |
| 三 | CUDA 发 `windows-x86_64` 与 `linux-aarch64`;SYCL 发 `windows-x86_64` | 无(与一、二并行) | **T2 从"仅 linux"变成"linux+windows"** |
| 四 | Windows 软件 Vulkan 设备(Mesa-on-Windows 或 SwiftShader);macOS Metal 设备实测 | 二 | 把 CI 上限从构建抬到**运行** |
| 五 | ROCm / Metal 的准入判定 | 四 | 按需 |

一期与二期顺序不可交换:**先有一个会失败的判据,再去扩平台。** 反过来做,扩平台那次
改动没有任何东西能证明它是对的。三期与一、二期无依赖,可并行。

### 6.1 二期的具体形状

`glslang` 没有上游二进制,三平台自建它买不到任何 shaderc 买不到的东西。**因此不为
macOS/Windows 发 glslang**,而让规则按平台选:

```toml
[target.'cfg(accelerator = "vulkan")'.feature-xlings.rules-spirv]
"xim:shaderc" = ">=2026.3"

[target.'cfg(all(accelerator = "vulkan", linux))'.feature-xlings.rules-spirv]
"xim:glslang" = ">=15.1.0"
```

规则的选择顺序(`options::compiler` → 环境变量 → glslang → shaderc → PATH)**已经**
会在没有 glslang 时退到 glslc,并自己写出声明抹平两者输出形状的差异。
**跨平台一致性由规则的既有选择能力提供,不由载荷的完全对齐提供。**

代价写明:Linux 与非 Linux 默认编译器不同,同一份着色器经由不同前端。这是可观测的
(规则把用了哪个作为 fact 报出),而**判据落在产物上**(G2 的 magic、G4 的像素)在三个
平台都要成立。

P4:**Linux 上的读数不变**(仍走 glslang)—— 这是「无感升级」那一条。

### 6.2 三期的具体形状

CUDA 与 SYCL 的 Windows 载荷来自索引**已经在用的那棵树**,所以是同一条流水线多跑
几个平台:

1. 下载 `windows-x86_64` 组件 / `sycl_windows.tar.gz`,核对能跑、版本对齐;
2. 按 xim 载荷布局重新打包,上传 `xlings-res/*`,gtc 镜像 GitCode,**两端逐字节比对**;
3. xim recipe 补 `windows` 块;
4. `rules-cuda` / `rules-sycl` 的 `feature-xlings` 声明去掉 linux 限定;
5. 夹具的 CI 从 linux 扩到 windows **构建**。

macOS 产物的架构要先核(本生态 macOS 是 arm64,而 Google 的 `macos` 前缀可能是
x86_64 或 universal)。**查不清就不要发 —— 发一个跑不起来的载荷比不发更坏。**

### 6.3 五期的准入条件,而不是排期

**ROCm/AMD**:规则已存在并按名字拒绝 AMD 平台。两个条件:(a) ROCm 运行时能否作为载荷
分发(许可 + 与内核模块的 ABI 耦合决定它是载荷还是宿主能力);(b) **有没有一台能跑的
机器或一个模拟器**。Ascend 那条之所以可接受,正是因为工具包自带 38 个 SoC 模拟器。
**没有 (b) 就不开这条 lane** —— 只能编译不能运行的后端,判据上限就是编译。

**Metal**:形状不同。`.metal → .air → metallib` 是**两步且第二步是链接**,正是
docs/20 列在「未实现」里的 device link;工具只在 Xcode 里、不可再分发,与「不依赖
Host」相反,需要一次明确的例外裁决。建议**先不做,把它当作 device link 这条通用能力的
第一个消费者来设计**。今天 `llamacpp` 的 `backend-metal` feature 已经能用(上游自己
构建 Metal 部分),说明 Metal 在**依赖层**是通的,缺的只是把 `.metal` 变成设备源。

## 7. 跨平台一致性:四条可执行的规则

从上面抽出来,写下来是为了下一条 lane 不用重新推:

1. **一致性由规则的选择能力提供,不由载荷的完全对齐提供。** 判据落在产物上,不落在
   路线上。
2. **能被断言的和不能被断言的分开摆。** 离屏进默认目标,窗口进 feature;有软件设备的
   平台跑,没有的只构建。
3. **驱动是宿主能力,设备是载荷。** 适配器层是 Linux 私有 loader 的产物,不该被补到
   三平台 —— 它在那两个平台上不存在。
4. **平台的空格分三种,处理方式不同**:上游有产物(发布工作)、上游只有源码(自建,
   要论证值得)、上游不发(结构性,要设计绕过)。**把三者混为一谈是这份方案最想避免的
   错误** —— macOS 的 CUDA 不是"还没做",是不会有。

## 8. 判据总表

| # | 期 | 断言 |
|---|---|---|
| G1–G5 | 一 | 图形示例:两个头存在、magic 正确、跑到 lavapipe、像素在插值范围、`--no-accel` 反向腿 |
| S1 | 一 | 两个同名不同目录的 shader:**拒绝并点名两者**(今天静默覆盖) |
| P1 | 二 | 三平台各产出两个 SPIR-V 头,magic 正确 |
| P2 | 二 | 规则报出的编译器 fact 与平台表一致 |
| P3 | 二 | 工程侧 `[xlings.workspace]` 为空 |
| P4 | 二 | **Linux 读数不变** |
| C1 | 三 | Windows 上 `rules-cuda` 夹具构建通过,工程侧零声明 |
| C2 | 三 | 两端镜像逐字节比对,一个 sha256 命名两者 |
| D1 | 四 | Windows/macOS 上**跑到设备**并断言设备名 —— 这一条成立,CI 上限才从构建变成运行 |

## 9. 自我 review:最可能错的三处

| 判断 | 为什么可能错 | 怎么发现 |
|---|---|---|
| 「离屏渲染在 lavapipe 上像素确定」 | 光栅化允许实现差异,插值精度未必逐位确定。**而 G4 是这个示例存在的理由** | 一期先跑一次读真实值,再把断言写成**区间**;区间写不出来就退到「中心像素明显不等于清除色」。**不要先写断言再调实现** |
| 「同名 stem 会覆盖」 | 从代码读出来的,没跑过 | 一期示例里放两个同名不同目录的 shader,先看它坏 |
| 「Windows 的 CUDA 载荷装上就能用」 | redist 的 Windows 组件是 `.zip` 且布局与 Linux 不同;规则的路径推导按 Linux 布局写的 | 三期先在一台 Windows 上手工解开、跑 `nvcc --version`,再动 recipe |

第一条最该被推翻。第三条最容易被低估:**「上游有产物」不等于「重打包就能用」**,规则
侧的路径推导是按一个平台的布局写的,而那正是本轮 `xpkg_dir` 缺陷的同一种形状 ——
一个函数只在它被写的那个环境里被验证过。

---

## 10. 实施回填(2026-09-07)

这一节把上面的判断与实际发生的事对齐。**跑**出来的写在这里,**读**出来的如果被推翻
也写在这里。分期没有被采纳:一二三期在同一天一起做了,四五期落成了记录在案的决定。

### 10.1 §9 的三条自我 review,读数

| 判断 | 结果 |
|---|---|
| 「离屏渲染像素确定」 | **比预期强**。lavapipe 与自写的软件光栅器给出**逐字节相同**的中心像素 `(124, 70, 62, 255)`,断言不需要写成区间。写法仍按 §9 的要求:先跑一次读真实值,再写断言 |
| 「同名 stem 会静默覆盖」 | **被推翻,但缺陷是真的**。ninja 在加载图时就报 `multiple rules generate .../triangle_vert.h` —— 从来不是静默的。真正的缺陷是它报的是**生成的文件**而不是那两个源、以图加载失败的形态到达、并且不说出路。规则现在自己先查一遍并点名两者 |
| 「Windows 的 CUDA 载荷装上就能用」 | **成立,而且是这一轮里最贵的一条**。见 10.2 |

### 10.2 「上游有产物」与「重打包就能用」之间的四处

Windows 的组件确实只需要一个 `windows` 段,但那四个配方与另外二十个共用一套
install/config 形状,而那套形状是按 Linux 写的:

- `payload_root()` 只剥 `.tar.xz`。Windows 组件是 zip,按名字找不到解包目录,回落到
  扫描 —— 而被扫的是**共享**下载目录,谁先在那里解出一个 `bin/` 就装谁。
- `scan_dir()` 用 `find` 加 GNU 参数。Windows 上 `find` 是 System32 那个按内容搜
  字符串的命令:它拒绝这些参数、往 stderr 写一行用法、返回空。**一个什么都没注册的
  组件,和一个本来就没有程序的组件,读数完全一样。**
- `reunite_backend()` 的 `ln` 在 Windows 上不存在。12.x 线自带后端所以到不了,但一个
  13.x 的 Windows 条目会到 —— 它现在拒绝而不是执行。
- 版本齐平检查(`tests/test_platform_version_parity.py`)的入口是
  `content.find("xpm")` —— 文件里第一次出现这三个字母,而这些配方的注释里写着
  `xpm.<os>.deps`。**十五个配方对这条规则完全不可见**,其中 node.lua 声明了三个平台。
  修好之后补了一条分母判据。

### 10.3 三处只在 Linux 之外成立的缺陷,由「每条规则都为本宿主编译一次」抓到

`tests/all-rules-compile` 是这一轮加的夹具:不点名任何 accelerator(每条规则立刻
返回,一个字节都不下载),只问六个模块编不编得过。它在第一批运行里抓到:

1. **macOS 14:`std::println` 不是 header-only。** 它的两个重载都要到 libc++
   **dylib** 里取 `__is_posix_terminal` 与 `__get_ostream_file`,而这两个符号是在
   macOS 14 不带的那一版里加进去的。规则改用 `std::format` 再 stream。

   **引擎侧那一半试过了,而显而易见的修法更糟。** 直觉是让
   `host_link_tokens` 的「信任 cfg」出口也发 `-L<载荷>/lib`,于是 `-lc++` 找到工具链
   自己那份 —— 而那正是 `dist::mechanism_for` 在 Mach-O 上明确拒绝的
   ToolchainCoupled:LLVM 的 macOS libc++abi 与 libunwind dylib **向上链接**
   `/usr/lib/libc++`,系统 libc++ 与工具链的那份同时载入,跨两份释放的对象在 libmalloc
   里 abort(#202)。CI 报的正是这条路的第一步 —— 链接停在 `__cxa_end_catch` 与其余
   那些系统 libc++ 会再导出、载荷那份不会的 ABI 符号上。

   **这条是这一轮里唯一一个「我的修复本身被实测推翻」的。** 它被推翻的方式值得记:
   本地全部单测绿、判据是我自己写的那条正向断言,而**红在一个我没想到会受影响的对象上**
   —— 图形示例的构建程序,在 macOS 15 上。改回去,把不对称与它的理由写进代码,并把判据
   改成陈述这条决定(`OnlyTheSpelledOutExitNamesTheToolchainRuntimeDirs`)。
   **限制照实写下来:macOS 14 上构建程序不能用 `std::print` / `std::println`。**
2. **Windows:`popen` 拼作 `_popen`。** `rules-spirv` 的宿主模块在那里编译失败。
3. **Windows:版本约束里的 `>` 被 cmd.exe 读成重定向。** mcpp 把供给请求作为 JSON
   参数放在 shell 命令行上;`shell::quote` 回答的是子进程的 argv 解析(`\"`),而
   cmd 不认这个转义,于是走到 `>` 时引号数是偶数。`>=` 正是每个规则包声明下界用的
   形态,而**在此之前没有任何一条能在 Windows 上生效的声明带过 `>`**。已修(先按子
   进程规则引用,再给每个 cmd 元字符前缀 `^`),判据是两个解析器的模拟器加一条反向腿。

三条的共同形状:**一段代码的正确性依赖于宿主,而 CI 只在其中一个宿主上执行它**。
这正是 §9 第三行说的那个形状,只是它出现的次数比预期多。

### 10.4 分期没有采纳,四期与五期落成决定

- **一期(图形离屏)**:`examples/10-graphics/offscreen`,判据是像素。已实现。
- **二期(shaderc 三平台)**:xim-pkgindex #778,规则按平台选编译器。已实现。
- **三期(CUDA/SYCL 上 Windows)**:xim-pkgindex #779(五个包),规则侧的路径推导与
  路线判断。已实现;端到端由 `windows-test` job 装卸五个包并断言注册的程序验证。
- **四期(软件设备把 CI 上限抬到运行)**:**未实现,记录为发布工作**。Windows 上需要
  一个 Mesa-on-Windows 的 `vulkan_lvp` 构建;macOS 上 runner 自带 GPU 与 MoltenVK,
  所以那一侧不需要软件设备,需要的是把 `compat.vulkan` 的 macOS 腿接到示例上。
  今天的上限:三平台**构建**,Linux **运行**。
- **五期(ROCm / Metal 准入)**:**决定是不收**,理由写在 §6.3,这里只补两条读数 ——
  Metal 的编译器只在 Xcode 内、不可再分发,所以它不是一个打包问题;ROCm 的运行时可
  再分发,但它需要的是一个 `rules-hip` 的 AMD 平台实现,而不是一个包。

### 10.4b 第四处「读出来的写法是错的」:依赖不能被 layer 条件化

图形示例最初把 Vulkan loader、运行时适配器与软件设备三条都门控在
`cfg(accelerator = "vulkan")` 上,理由是「`--no-accel` 一个字节都不装」。**这条做不
到,而且它失败的方式是安静的一半**:`accelerator` 是**从依赖图里解出来的**,所以一个
由它选出的依赖会决定它自己正在问的那个答案 —— mcpp 因此忽略这个谓词并给出警告,而
**同一个谓词下的 `[build]` 源照常生效**。结果是包被丢掉、包含它的源被留下:

    src/vulkan/render.cpp:20:10: fatal error: vulkan/vulkan.h: No such file or directory

判据是构建本身。谓词现在按平台写(`cfg(linux)` 用于只有 Linux 有的两项),包是无条件
的,accelerator 只选 `[build]` 源 —— 也就是既有的 `examples/09-heterogeneous/vulkan`
一直在用的形状。示例 README 把这一条写成了正文,因为它是一条使用者会撞上的规则。

顺带一条读数:两条腿的中心像素**逐字节相同**(`(124, 70, 62, 255)`),所以 CI 的反向
腿从「CPU 腿跑起来了」加强成「两条腿报出同一个像素、不同的设备名」。

### 10.4c 图形示例在三个平台上都构建通过,而先红的是判据自己

macOS 与 Windows 上的构建都成功了 —— 规则供给了 `xim:shaderc@2026.3`,两个着色器阶段
都编译了,Vulkan 那一半也链接上了 loader 包。**红的是我写的那条断言**:

    target/.build-mcpp/out/spirv/triangle_vert.h carries no SPIR-V magic

因为规则会**选**编译器,而两个编译器把声明拆得不一样:glslang 写出完整的
`const uint32_t ...[] = {...}`(magic 在 `.h` 里),glslc 写出初始化列表、由规则在外面
补声明(magic 在 `.inc` 里)。**一条只点名 `.h` 的断言,是一条关于某一个编译器的断言**
—— 正是这个 job 存在的理由所要抓的那种形状,只不过这次它抓到的是自己。

顺带一个可迁移的小陷阱:改成 `grep -qs '<pat>' a.h a.inc` 是错的。**GNU grep 在被
点名的文件不存在时返回 2,即使它在前一个文件里匹配到了,也即使加了 `-s`** —— `-s` 压的
是消息不是状态。于是这条判据在「只产出 header」的那条路上永远失败,而失败的原因与被测
的性质无关。正确写法是逐个文件测。

### 10.5 一条留下的不一致,以及它什么时候消失

`mcpp:plugins` 0.2.5 里 `xim:shaderc` 在 macOS 与 Windows 上是**精确版本**,而
Linux 上的 `xim:glslang` 是**下界**。这不是形态判断的差异,是 10.3 第 3 条的后果:
发布中的引擎(2026.9.6.6)传不过去一个 `>`。引擎修复发布之后,那两处改回
`>=2026.3`,而**那时 plugins 的 Windows job 就是这个引擎修复的端到端判据** ——
今天它只有单元级的两个模拟器。
