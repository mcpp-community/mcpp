# 加速器支持:完整设计方案

2026-09-05 · **v5,完整方案**,未实现,供 review

前置:[生态调研报告](2026-09-04-ai-accelerator-toolchain-ecosystem-survey.md) ·
取代 [`2026-09-05-accelerator-support-scenarios-and-design.md`](2026-09-05-accelerator-support-scenarios-and-design.md)(v1/v2)

对现状的陈述一律读 `origin/main`(d18cd8b,2026-09-04)。
**[已有]** = 当前代码里就有;**[提议]** = 本文要加的。

---

## 0. 决定记录

| # | 决定 | 落在 |
|---|---|---|
| 1 | `accel` **不进 tag 字符串**,作为并列结构化字段,仍由 `tag_check` 一并比较 | §7.1 |
| 2 | `kind = "device"` + `backend`,不用 `language = "cuda"` | §3.1 |
| 3 | 逐 glob 架构收窄**第一版就做** | §3.2 |
| 4 | capability 同名符号边界**先修** | §7.4 |
| 5 | cfg 用**组合表达**,不引入第二个谓词 | §3.4 |
| 6 | 第一版 device target **不产静态库** | §6.3 |
| 7 | 岛的 C++ 运行时耦合**必须核验**,复用 `mcpp.runtime.elf` | §7.3 |
| 8 | 形态 B 的工具链声明**沿用 mcpp 既有的 `family@version` 写法** | §4 R8 |
| 9 | 小众后端(需注册的国产三家、编模型的那一类)**本轮不支持** | §1.2 |
| 10 | 方案必须含**生态闭环**:索引侧的包、真实验证、真实可用 | §8 |
| 11 | `archs` 缺省 ⇒ **报错并列出该 backend 的已知取值**,不猜 | §3.2 |
| 12 | `backend` 是**请求**(第一趟可见),绑定在第二趟,缝上有显式 satisfied 记录 | §3.5 |
| 13 | 形态 B 的产物核验 = **引擎读节名,节名表由后端描述符提供** | §7.5 |
| 14 | Metal 一类产物走**已有的 `deploy_files` 通道**,不新增产物类别 | §6.4 |
| 15 | 形态 B 的字段**统一叫 `archs`**,与形态 A 一致 | §4 R8 |
| 16 | **不加**「已构建未运行验证」标注 —— 交叉编译产物本来就没在目标上跑过 | §9 RK-4 |

### 0.1 v4 相对 v3 的改动

- **§4 R8**:`[toolchain] compiler = "icpx"` 是我发明的写法,**错了**。mcpp 既有形态是
  `[toolchain] default = "gcc@16.1.0"` / `linux = "…"` / `macos = "…"`,值为
  `family@version`。⇒ SYCL 应当是 `default = "oneapi@2026.1"` 或 `"adaptivecpp@25.02"`
  —— **oneAPI 与 AdaptiveCpp 是工具链家族,和 gcc/llvm/msvc 平级。**
- **§1.2**:范围收窄,小众后端明确不做。
- **新增 §8 生态闭环**、**新增 §9 风险**。自我 review(§10)按新内容重写。

### 0.2 v5 相对 v4 的改动

- **RK-2 由代码给出了答案,风险等级下调**,但暴露出一个不同的残余风险(§9 RK-2)。
- **新增 §8.5「没有 GPU 能验到哪一步」** —— 部分后端的 kernel **可以在 CPU 上真跑**,
  这改变了 AdaptiveCpp 优先的理由,也缩小了 RK-1。
- **新增 §11 源码分发与二进制分发**。
- v4 的六条待决全部拍板(决定 11–16),`§12` 只剩真正未决的。

---

## 1. 范围

### 1.1 本轮覆盖

| 类别 | 形态 | 代表 |
|---|---|---|
| CUDA 系单源方言 | A(岛) | CUDA、HIP/ROCm |
| 单源整目标 | B | SYCL(oneAPI DPC++ / AdaptiveCpp)、OpenMP offload |
| 着色/计算语言 | A(产资源非目标码) | Metal、Vulkan/SPIR-V |
| 纯绑定层 | 都不是 | 厂商 runtime 的 shim,只是普通库 |

### 1.2 本轮明确不做(决定 9)

- **需注册或需联系厂商才能拿到工具链的**:天数智芯 CoreX、壁仞 BRCC、燧原 TopsCC。
  包管理器无法供给的东西,做了也是半截。
- **「编模型不编 C++」的那一类**:Google TPU、AWS Neuron 主线、Qualcomm QNN、
  ARM Ethos-U、Rockchip RKNN。报告 §5.4:那里根本没有 C++ 编译步骤。
- **昇腾 Ascend C、寒武纪 BANG、摩尔线程 MUSA**:形态上属于 A,框架天然容得下
  (§3.1 给了 manifest 长什么样),但**本轮不做规则包与载荷**,留给生态或后续。

⭐ 但**框架不为它们开特例**:§3.1 会证明同一组原语能表达它们。
「不做」是排期决定,不是设计缺口。说清楚边界本身是产品的一部分。

---

## 2. 两种形态

### 2.1 形态 A —— 岛

设备代码在独立编译单元里,由另一个编译器编译,产物汇进宿主构建。

```
kernels.cu ──nvcc──→ kernels.o(内嵌设备镜像) ──┐
                                                ├─→ 宿主链接
main.cppm  ──g++───→ main.o  ────────────────────┘
```

⚠️ **岛的产物不一定是目标文件。** Metal 是
`xcrun metal` → `.air`、`metal-ar` → `.metalar`、`metallib` → `.metallib`,
最终是**运行期资源**;Vulkan 的 `.spv` 同理。

⇒ 岛的产物角色必须可声明。**[已有]** `BuildAction::Role` 恰好有 `Object`(进链接)
与 `Artifact`(不进链接)两档,规则包声明用哪个。**框架零新增。**

### 2.2 形态 B —— 整目标

设备代码就在普通 `.cpp` 里,由一个能 offload 的编译器整体编译。**没有岛。**

```
main.cpp ──icpx -fsycl -fsycl-targets=…──→ main.o(含 SPIR-V/PTX/amdgcn 镜像)
```

形态 B 里「加速器支持」= **换一个工具链家族 + 一组 target flag**,不是加一个 target。
用到 P3 + P4 + 工具链选择,**不用 P1**。

### 2.3 ⚠️ 形态 B 与 modules 的关系是**已知的未知**

形态 A 有确定答案:nvcc 不支持 modules,岛不进 module graph(报告 §6)。

形态 B 没有。`icpx` 是 clang 派生、支持 `-std=c++20`,但
**「SYCL kernel 所在的 TU 能否是 module interface」本次调研找不到任何肯定或否定的文档**
(报告 §8 第 1 条)。AdaptiveCpp 同理。

⇒ 不假设可行,也不假设不可行。第一版按「该目标退回 header 模式」处理,
并把实测列为阶段 0 的探针(§11)。**这是本设计唯一一处已知的未知,单独标出而不藏在假设里。**

---

## 3. 主体框架:五个原语

| # | 原语 | 是什么 | 形态 | 状态 |
|---|---|---|---|---|
| **P1** | device target | 不进 module graph 的编译单元集合 | A | **[提议]** |
| **P2** | 接缝模块 | 普通模块;收窄可见性/类型,**且是后端替换的落点** | A | **[已有]** |
| **P3** | `accel` 字段 | 产物身份:含哪些 (后端, 架构集合) | A+B | **[提议]** |
| **P4** | `accelerator` layer key | cfg 一档:解析后的后端身份 | A+B | **[提议]** |
| **P5** | 规则包 | 厂商 flag 拼法的持有者 | A | **[已有]** |

零改动复用:`BuildAction::Role`、`tag_check` + `prebuilt.cppm` 的选择循环、
capability 绑定、11 字段指纹、xlings 载荷、`mcpp.runtime.elf`、`CommandDialect`。

### 3.1 P1 —— device target,且不为任何厂商开特例(决定 2)

```toml
[targets.kernels]
kind    = "device"
backend = "cuda"                # 可省略:只有一个 device 规则包时自动绑
sources = ["src/kernels/*.cu"]
archs   = ["sm_80", "sm_90f"]
```

**本轮不做、但框架已经容得下的**(证明同一组原语够用):

```toml
# 华为昇腾:.asc + --npu-arch=dav-2201
[targets.ops] kind="device"; backend="ascend"; sources=["src/*.asc"]; archs=["dav-2201"]

# 寒武纪:.mlu + 可重复的 --bang-mlu-arch
[targets.ops] kind="device"; backend="bang"; sources=["src/*.mlu"]; archs=["mtp_372","mtp_290"]

# Metal:产物走 Role::Artifact,由规则包声明
[targets.shaders] kind="device"; backend="metal"; sources=["src/*.metal"]
```

⭐ `kind = "device"` 说的是**图上的角色**,不说语言。这一点在 Intel Gaudi 上兑现:
TPC-C 是 **C99 派生而非 C++**,`kind = "device"` 对它照样成立 ——
若当初选 `language = "cuda"` 式命名,这里就要开特例。

### 3.2 架构收窄:一个键(决定 3)

目标级声明全集,glob 级**收窄**;收窄即求交,不需要第二个键。

```toml
[targets.myops]
kind  = "device"
archs = ["sm_80", "sm_90f", "sm_100f"]

[targets.myops.sources]
"src/gemm_sm90/*.cu"   = { archs = ["sm_90f"] }
"src/gemm_common/*.cu" = { archs = ["sm_80", "sm_90f"] }
```

规则:glob 级必须是目标级的**子集**;不是子集是配置错误,不是隐式扩展。

⚠️ **收窄成空集是错误,不是「不编」。** 空集意味着这批源码在当前请求的架构下没有
有效目标;静默跳过会产出缺 kernel 的库 —— 报告里 `no kernel image` 的又一个来源。

理由(报告 §4.2):vLLM / ONNX Runtime / TensorRT-LLM 三家独立且趋同地做了逐 kernel
家族的收窄,因为无条件为每个 SM 实例化每个 CUTLASS 模板,编译太慢、二进制太胖。

#### `archs` 缺省 ⇒ 报错,不猜(决定 11)

```
error: [targets.kernels] backend = "cuda" requires `archs`.
       mcpp does not guess: the set you compile for is not the set this machine has.
  known values for cuda : sm_80  sm_86  sm_89  sm_90  sm_90a  sm_100  sm_100f  sm_120
  family targets (preferred, one artifact covers a generation):
                          sm_90f  sm_100f
  or, for local iteration only:  archs = ["native"]
```

三条理由,都指向同一个方向:

1. ⭐ **mcpp 已有的先例是「拒绝并告诉你该写什么」**,不是猜。
   `[toolchain] … = "system"` 被明确拒绝,错误里直接给出该写 `linux = "gcc@16.1.0"`。
2. 报告 §2.2 记的 CMake `native` 陷阱,根因就是**把「本机有什么」当成「要编成什么」**;
   猜一个默认就是把这个陷阱抄过来。
3. 源码分发时(§11.1)一个错的默认值可能是一小时的编译。**猜错的代价不对称。**

### 3.3 P2 —— 接缝模块

不是新原语,但是**架构单元**,因为它同时做三件事,而第三件决定它不能省:

| 它做什么 | 没有它会怎样 |
|---|---|
| 可见性收窄:下游 `import`,不见设备头 | 下游被迫包含设备头 |
| 类型收窄:`const float*, size_t` → `std::span<const float>` | 裸指针泄漏到整个工程 |
| ⭐ **后端可替换**:换掉岛而接口一个字不变 | **`cfg` 无处安放,每个消费者都变成后端相关的** |

边界处可以不写头文件(GMF 里直接 `extern "C"`),但 ⚠️ `extern "C"` 不做名字修饰
⇒ **改了 `.cu` 的签名而声明不跟着改,链接照样成功,运行期读错内存**。
⇒ 默认用头文件(签名只有一份拷贝);「由规则包生成声明」作为可选能力。

### 3.4 P4 —— `accelerator` 多值 layer key,组合表达(决定 5)

**[已有]** `kCfgLayerKeys` 现有五项,由 `merge_layer_conditional_config` 的**第二趟**
在目标侧解析后求值,未知键有 `unknown_tokens()` 诊断。加 `accelerator` 是加一项。

⚠️ 但它必须是**多值** layer(现有五项都是单值),而语义只有一行:

> **`accelerator = "<x>"` ⟺ `<x> ∈` 集合。处处如此。**

`any` / `all` / `not` 作为普通布尔组合子在其上组合,不改变操作数的含义。
单后端构建的集合是 `{cuda}`,于是它对 `accelerator = "cuda"` 答真、
对 `accelerator = "rocm"` 答假 —— 这正是 R2/R7 需要的;
多后端构建的集合是 `{cuda, rocm}`,两者都答真 —— 这正是 R5 需要的。

⚠️⚠️ **本节最初写的是另一套语义**(裸键表示集合相等、`any(...)` 才表示成员判定),
实施时发现它是错的:让组合子改变操作数的含义,会使
`all(accelerator = "cuda", accelerator = "rocm")` 变成**不可满足**,
而不是「两个后端都启用」。改为处处成员判定之后,不但正确,而且
**用户少学一条规则** —— R5 的 manifest 里也不再需要写 `any(...)`。
详见 §14.1 第 1 条。实现里这个差异只落在一个函数(`Ctx::layer_matches`)上。

### 3.5 P5 —— 规则包,`backend` 走 capability

**[已有]** 规则包机制 2026.8.5.1 落地,`examples/08-build-rules/` 在 main 上。

`backend = "cuda"` **不是包名**,是能力名:

```toml
[package]
name     = "rules-cuda"
provides = ["mcpp:device-rule=cuda"]
```

**[已有]** capability 绑定规则原样适用(恰好一个自动绑;多个未 pin 报错并列候选;
零个报错),**绝不静默猜测**。⭐ 用户声明**意图**而非包名。

#### `backend` 在第几趟解析(决定 12)

`backend = "cuda"` 是**请求**,第一趟(triple-only)就能读到它这个字符串;
把它**绑定**到具体规则包要走 capability,而 capability 需要解析后的图 ⇒ **第二趟**。

⭐ 这不是新规矩,是 mcpp 已有的一条经验的直接套用 ——
memory `a-printed-value-promoted-to-a-compared-value` 记的:
**「请求侧与答案侧保留各自的拼写,在缝上放一个显式的 `*_request_satisfied`」**。

⇒ 具体化为三条:

- `targets[].backend` 是**请求侧**的字符串,不做归一,第一趟可见;
- `mcpp:device-rule=<backend>` 的绑定结果是**答案侧**,第二趟产生;
- 缝上记一个显式的 `backend_request_satisfied`,**它有读者**(诊断与 `mcpp why`),
  否则就是 memory 里「答案已解析,却没有接到决定上」那一类。

这样也切开了 §10.1 提到的循环嫌疑:第一趟不需要知道哪个规则包会赢。

---

## 4. 七类使用者 × 两种形态

| # | 使用者 | 形态 | 原语 | 新原语? |
|---|---|---|---|---|
| R1 | 应用开发者:只想用 GPU 库 | — | P3 | 否 |
| R2 | 混合工程:C++ + kernel | A | P1+P2+P5 | P1 |
| R3 | 算子库作者:独立发布 | A | P1+P2+P3 | P1、P3 |
| R4 | 算子开发工作流 | A | P1(逐 glob 收窄) | 否 |
| R5 | 推理框架:一份产物多后端 | A×N | P1×N+P3(集合)+P4 | 否 |
| R6 | 底层运行时/绑定层 | — | P4 + capability | **零新增** |
| R7 | 换厂商/国产迁移 | A 或 B | P4 + capability + P2 | 否 |
| R8 | SYCL / OpenMP / stdpar | **B** | P3+P4+工具链家族 | 否 |

### R1 —— 只想用一个 GPU 库(数量最大)

```toml
[dependencies]
onnxruntime = "1.23.0"
```

**[已有]** 消费侧遍历 `runtimeConfig.artifacts`,逐个 `tag_check`,取第一个通过的;
全不通过报**分歧最少的那一个**:

> Keep the CLOSEST refusal to show: the one that disagrees least is the one the
> user is most likely able to act on.

**这就是 PEP 817 的选择算法,已经在跑。**

```
error: onnxruntime@1.23.0: no prebuilt artifact matches this build.
  your build : x86_64-linux-gnu-gcc16-libstdcxx16-c++23   accel=cuda12.8+{sm86}
  published  : (no accel)                                  ← CPU-only
               accel=cuda12.8+{sm80,sm90f} ptx-floor=90
  closest differs on:
    accel    needs sm80|sm90f, or PTX floor ≤ 86; this build asks sm86
note: the CPU-only variant is compatible — take it with --no-accel.
```

### R5 —— 一份产物含多个后端(ggml 形态)

```toml
[features]
cuda = {}
rocm = {}                        # 不互斥:可同时开

[target.'cfg(any(accelerator = "cuda"))'.targets.k-cuda]
kind = "device"; backend = "cuda"; archs = ["sm_90f"]

[target.'cfg(any(accelerator = "rocm"))'.targets.k-rocm]
kind = "device"; backend = "rocm"; archs = ["gfx942"]
```

⇒ `accel` 是**集合**,判据是「请求的 (后端, 架构) 在不在里面」;
单后端退化成集合大小为一,R1 语义不变。

### R8 —— SYCL / OpenMP(形态 B),沿用 mcpp 既有工具链写法(决定 8)

**没有 device target。** 变的是工具链家族:

```toml
# mcpp 既有写法:[toolchain] <os|default> = "<family>@<version>"
[toolchain]
default = "adaptivecpp@25.02"        # 或 "oneapi@2026.1"

[build]
accel = { backend = "sycl", archs = ["generic"] }       # [提议];字段名与形态 A 统一(决定 15)
```

```toml
# OpenMP offload:工具链仍是 gcc,但载荷必须带 offload
[toolchain]
default = "gcc@16.1.0+offload"       # [提议] 载荷变体,见 §5.3
[build]
accel = { backend = "openmp", archs = ["nvptx-none"] }
```

⭐ **oneAPI 与 AdaptiveCpp 是工具链家族,和 gcc/llvm/msvc 平级。** 于是
`@version` 钉定、自动安装、`mcpp toolchain` 命令、载荷映射**全部复用现有机制**,
加一个家族 = 注册表加一行 + 载荷映射,与当初加 llvm 是同一件事。

⭐ 一个待验证的简化:icpx 与 AdaptiveCpp 都是 clang 派生,flag 用 GNU 风格
(**[已有]** `CommandDialect` 的 `gnu` 实例可直接复用),BMI 也是 clang 的。
⇒ **可能不需要给 `CompilerId` 加新值,只需要加工具链家族。** 列为阶段 0 的探针。

⚠️ OpenMP offload **不是加 flag,是载荷要重建** —— GCC 必须在构建时配置了
`--enable-offload-targets=nvptx-none=…`。没有 offload 支持的 GCC 遇到 `-foffload=`
**只会安静地什么都不做** ⇒ 判据是载荷的构建配置,不是编译能否通过。

---

## 5. 工具链供给

### 5.0 DPC++ 与 NVHPC 怎么接进来(2026-09-05 追加,已查证)

这一节回答「intel/llvm 与 NVIDIA 的编译器要不要也走 xim、像普通工具链一样」。
答案不是同一个,而且查证之后**比原设计更简单**。

| 编译器 | 接法 | 理由(一手材料) |
|---|---|---|
| **intel/llvm DPC++** | **`Family::Llvm` 的一个载荷变体 + provider 能力位**,**不是新家族** | 它就是 clang:同一个 driver、同一套 GNU 风格 flag、同一种 BMI。用法是 `clang++ -fsycl` |
| **nvcc** | **device 规则包**(形态 A) | 不支持 C++20 modules(报告 §6) |
| **nvc++ (NVHPC)** | **device 规则包,不是工具链家族** | HPC SDK 用户指南自述主标准是 *"ISO/ANSI C++17 with GNU compatibility"*,未见 modules 支持 ⇒ **当不了 modules-first 工程的宿主编译器** |

#### 两处对 v4 的更正

**其一,开源 DPC++ 不需要 Codeplay 插件。** §5.1 原写「NVIDIA/AMD 后端还要 Codeplay
插件」—— 那是 **Intel 二进制发行版**的情况。开源的 intel/llvm 用
`buildbot/configure.py --cuda` / `--hip` 把后端**编进编译器本身**,可用的
`-fsycl-targets` 直接包括 `nvptx64-nvidia-cuda`、`amdgcn-amd-amdhsa`。
先决条件:CUDA ≥ 11.0(建议 11.6+)、ROCm ≥ 5.7。
(来源:`intel/llvm` 的 `sycl/doc/GetStartedGuide.md`)

**其二,DPC++ 另有一个 `native_cpu` 后端。** 与 AdaptiveCpp 的 omp flow 同类:
**SYCL kernel 可以在 CPU 上跑**,于是形态 B 有第二条可在无卡 CI 里端到端自证的路径。

#### ⭐⭐ 由此,形态 B 不需要新的 `CompilerId`,也不需要新的 `Family`

`src/toolchain/registry.cppm` 的 `Family` 是封闭枚举(`Gcc`/`Llvm`/`Msvc`),
加一项要动约十处。**而 DPC++ 一处都不用动** —— 它是 `Llvm` 家族的另一个载荷,
差异落在两个已有的地方:

1. **载荷映射**:registry 里 family→ximName 的映射扩成 family+变体→ximName;
2. **能力位**:`ProviderCapabilities`(`src/toolchain/provider.cppm`)加
   `has_sycl` 与可用的 `-fsycl-targets` 列表 —— 该模块的存在理由原文就是
   *"Previously these decisions were scattered as ad-hoc is_clang(tc) / is_gcc(tc)
   checks. This module centralises them into a single query point."*

⇒ 形态 B 的工程量从「加一个工具链家族」降到「加一个载荷变体 + 一个能力位」。

#### nvc++ 归形态 A,不归形态 B

这条值得单独说,因为它反直觉:nvc++ 提供 `-stdpar`,看起来正是形态 B 的样子。
但**形态 B 的前提是那个编译器能当整个工程的宿主编译器**,而 mcpp 的工程全都
`import std`。一个主标准是 C++17、无 modules 的编译器编不了它们。
⇒ nvc++ 只能作为**某些目标的设备编译器**出现,与 nvcc 同类,走规则包。

### 5.0b ⭐⭐ 宿主依赖:轴已经划好了,而且 xim 里已经有半边

设计原则(用户长期约束):**尽量走 xlings 生态,越少依赖 host 越好。**

这条在本课题上不需要新规则 —— memory 里已有的轴划分逐字适用:

> **工具链 = mcpp 的契约 ⇒ 拒绝 `system`;程序链接的库 = 程序自己的事 ⇒ warn 指回 mcpp-index**

套到加速器上,轴是这样切的:

| 组件 | 归属 | 理由 |
|---|---|---|
| `nvcc`、`ptxas`、`nvlink`、`fatbinary`、CRT 头 | **xim 载荷** | 它是工具链,是 mcpp 的契约。NVIDIA 的 redist 清单允许再分发 |
| `cudart`、`cublas`、`cudnn` 等运行库 | **xim 载荷**(mcpp-index 侧的 `compat.*` 消费) | 同样可再分发 |
| **`libcuda.so.1`(驱动 userspace)** | ⚠️ **不可分发,只能指向 host** | 驱动 EULA 禁止第三方再分发,且它与内核模块严格 ABI 锁步 |

⭐⭐ **xim 里已经有这条轴的下半边**:`pkgs/l/libcuda-host-link.lua` 是一个
**sentinel 包**,只装一个指向 host `libcuda.so.1` 的符号链接,recipe 自己写明:

> DOES NOT: Redistribute libcuda.so.1. The NVIDIA Driver EULA forbids
> third-party redistribution, and even if it didn't, the userspace lib is in
> strict ABI lockstep with the kernel module — versioning it as an xpkg is
> impossible.

而且它已经解决了「消费者各自探测 host」的问题:所有 GPU 包读
`pkginfo.dep_install_dir("libcuda-host-link").."/lib/libcuda.so.1"`,
不各自重实现 ldconfig 探测;宿主库的定位统一走 `libs/hostlib.lua`
(`ldconfig -p` + ELF class 过滤 + first-hit,并有 CI 不变量
「no recipe enumerates distro library directories」)。

#### 由此得到的三条实施要求

1. **mcpp 的设备规则包不得从 `PATH` 取 `nvcc`。** 它必须解析到 xim 载荷的路径,
   与现有 `[xlings] deps` 的载荷查找同一条路(`mcpp.build.runner_lookup` 的形状)。
   本机的 `/usr/bin/nvcc` 只能作为**开发期的回落**,且必须在诊断里说明它来自 host。
2. **缺的是上半边:xim 里还没有 CUDA 工具链包。** 按 NVIDIA 的 redist 组件划分,
   编译只需要 `cuda_nvcc` + `cuda_cudart`(百 MB 级,不是把整个 toolkit 打进去的 GB 级)
   —— 这与 Bazel hermetic CUDA 的按组件取法一致(报告 §2.5)。
3. ⭐ **这让 §5.2 的宿主编译器配对从「校验」升级为「可满足」**:mcpp 同时提供
   CUDA 版本(xim)与宿主编译器(载荷),所以它可以**选一对兼容的**,
   而不只是在不兼容时报错。本机的现状(载荷 gcc 16.1.0 + host nvcc 12.0,
   上界是 gcc 12)正是「只有校验、无法满足」的样子。

### 5.1 本轮只做「可分发」这一档

| 载荷 | 用于 | 备注 |
|---|---|---|
| `adaptivecpp` | 形态 B | ⭐ **构建期不需要厂商工具链**(SSCP flow 嵌后端无关 LLVM IR,运行期 JIT),体积最小,是形态 B 成本最低的入口 |
| `oneapi-dpcpp` | 形态 B | 体积大;NVIDIA/AMD 后端还要 Codeplay 插件 |
| `cuda-toolkit` | 形态 A | ⚠️ 数 GB,见 §9 RK-3 |
| `rocm` | 形态 A | ⚠️ 更大 |

其余(需注册的、编模型的)见 §1.2,不做。

### 5.2 ⭐⭐ 宿主编译器配对:一处没人占的位置

nvcc 要求宿主编译器版本落在支持表内。报告 §2.2:**CMake 明确不校验**
(#23322 / #24267 / #22202),直接转发 `-ccbin` 让 nvcc 自己报
`Unsupported gnu version`;xmake 同样不校验。

mcpp **自己供给宿主编译器载荷**,所以可以:(1) `-ccbin` 钉到自管载荷;
(2) **在调用 nvcc 之前**校验版本落在支持表内,报错时两边版本都印出来;
(3) 由此让「岛与宿主共享同一份标准库」**由构造成立**。

⭐ 第 3 条同时消解了报告 §6 那条结构性障碍(跨边界传 `std::string` 因
clang/libc++ 撞 nvcc/libstdc++ 而挂)——**它只在两侧标准库不同时成立**。

### 5.3 offload 载荷是载荷侧的活

带 offload 的 GCC 是一个**独立的载荷变体**(`gcc@16.1.0+offload`),
成本落在 xlings 侧,与引擎并行推进。

---

## 6. 图与链接

### 6.1 岛的产物怎么汇入

`.cu` 编出的是**宿主目标文件内嵌设备镜像**(nvcc 的 fatbinary;clang 是
`.llvm.offloading` 节带 `SHF_EXCLUDE`),就是个普通 `.o`,直接进链接。
**[已有]** *"Object — its outputs ARE the link edge's inputs"*。
Metal/Vulkan 的产物走 `Role::Artifact`。

### 6.2 device link 只在跨 TU 时需要,且必须声明

⚠️ 推断「需不需要」要解析设备代码,mcpp 不做也不该做:

```toml
[targets.kernels]
device-link = false      # 默认。true 时图上多一个 Role::Object 的 action
```

⚠️ 它**改变图的形状** ⇒ 必须进指纹,否则改了它 fast path 会 replay 旧图。

> 命名用 `device-link` 而非 `rdc`:`rdc` 是 NVIDIA 的词,这件事在 AMD 叫别的、
> 在 Metal 根本不存在。**引擎的词表不该带厂商口音。**

### 6.4 不进链接的产物走已有的 `deploy_files` 通道(决定 14)

Metal 的 `.metallib`、Vulkan 的 `.spv` 是**运行期资源**,不进链接。
问题是它们怎么随产物走。

**[已有]** mcpp 已经有这个通道:`deploy_files = ["bin/widget.dll"]`
(`docs/05-mcpp-toml.md`),经 `runtimeConfig.linkIntent.deployFiles` →
`plan.runtimeDeployFiles` → `ninja_backend`,是当初为 **Windows 无 RPATH 只能把 DLL
拷到 exe 旁**做的。

⇒ **规则包把 `Role::Artifact` 的产物登记进 `deploy_files`,不新增产物类别。**
`mcpp pack` 侧也不需要新分类 —— 它已经在处理 deploy 文件。

⭐ 这条符合「尽量不加专用东西」:一个 `.metallib` 与一个 `.dll` 对构建系统是同一件事
——**必须与可执行文件一起送到用户手里的、不参与链接的文件。**

### 6.3 第一版不产含设备代码的静态库(决定 6)

报告 §2.2 列了 CMake 的三个未决 issue:#19238、#17586、#21967。
⇒ **主动避开一个九年没解决的形状。** 要发布走 R3 形态(岛的目标码 + 接缝的模块接口)。

---

## 7. 身份与分发

### 7.1 `accel` 是并列结构化字段(决定 1)

```
artifact:
  abi   : "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"      ← 四维字符串,不变
  accel : [ { backend:"cuda", version:"12.8",
              archs:["sm_80","sm_90f"], ptx_floor:"90" },
            { backend:"rocm", version:"6.4", archs:["gfx942"] } ]
```

**一个比较器,两个存储位置。** `tag_check` 一并比较,不新增第二个比较器 ——
这是本仓库反复踩过的那类缺陷。理由:集合含 `,`/`;`,而现有 tag 是 `-` 拼接、
从末尾解析三段。**文件名不该承载集合。**

### 7.2 比较语义:包含 ∪ PTX 下界

被接受 ⟺ backend 在集合里,**且** `arch ∈ archs` **或**
(`ptx_floor` 存在且 `arch ≥ ptx_floor`)。

| 备选 | 判断 |
|---|---|
| 硬拒 | ❌ 拒掉真能跑的产物,逼回「一颗芯片一份二进制」 |
| 警告后放行 | ❌ **最坏**:问题就是它当前静默,降级成警告等于保持静默 |
| **包含 ∪ PTX 下界** | ✅ |

⭐ **不是新概念:`standard` 已是 tag 里的非对称下界维**,`tag_check` 已有
`standard_level()` 的 floor 分支。⭐ AMD 无 PTX ⇒ `ptx_floor` 空 ⇒ 退化成纯包含,
家族覆盖由 ROCm generic target 从集合侧提供。**同一条规则,两家各按自己的机制满足。**

⚠️ clang 编 CUDA 默认只产 SASS,PTX 要 `--cuda-include-ptx` ⇒ `ptx_floor` 由**实际产物**决定。

### 7.3 岛的 C++ 运行时耦合必须核验(决定 7)

**「接口是 C 形状」不等于「不依赖 C++ 运行时」。** CUDA C++ 就是 C++:岛内部用了
`std::`/异常/虚函数,目标码就会引用 libstdc++/libc++ 符号。链进用另一套标准库的程序,
就是 memory 里「链接期用 A 运行期加载 B」。

⇒ 判据是**目标码里有没有未定义的 C++ 运行时符号**,不是头文件长什么样。
**[已有]** `mcpp.runtime.elf` 与 `symbol_provision` 已在读符号表,复用。

### 7.5 形态 B 的 `accel` 由产物核验得出,节名表由后端提供(决定 13)

§10.1 指出:形态 B 里 mcpp 无从知道一个目标里到底有没有 kernel,
只知道用户请求了 `-fsycl` ⇒ 光凭声明发 `accel` 会**让产物身份说谎**。

⇒ 判据落在产物上:**镜像里有没有该后端的设备段。**

| 后端 | 节名(示例) |
|---|---|
| CUDA(nvcc) | `.nv_fatbin` |
| clang offload / OpenMP / SYCL | `.llvm.offloading` |
| HIP | `.hip_fatbin` |

⚠️ 节名逐后端不同,而**引擎不该持有厂商知识**(与 §3.5 规则包持有 flag 拼法同一条原则)。

⇒ **引擎读节名,节名表由后端描述符提供。** 引擎侧只需要
「读 ELF/Mach-O 节名表,判断给定名字是否存在」——
**[已有]** `mcpp.runtime.elf` 与 `symbol_provision` 已经在做这件事。

⭐ 同一个核验顺带答了 §7.3 的问题(岛有没有未定义的 C++ 运行时符号):
**一次读符号/节表,回答两个问题。**

### 7.4 先修:capability 同名符号边界(决定 4)

`docs/05-mcpp-toml.md` §2.8.1 实测:绑定**只做选择,不裁剪 link line**;
对「多个 provider 定义同名符号」的能力,两个同时在图里是**缺陷而非可 pin 的歧义**。

⚠️⚠️ cuBLAS / rocBLAS / muBLAS 恰好是这一类(白皮书说 muBLAS 刻意与 cuBLAS 同名)。
⇒ 需在**绑定**这一级拒绝:同一 capability 的多 provider 必须走
`optional = true` + 互斥 feature。**R6 与 R7 都压在这条上。**

#### ⚠️⚠️ 这条按原样无法实现,2026-09-05 实施时发现

决定 4 是「先修」,而写实现时它立不住:

- **不能对「一个 capability 有多个 provider 在图里」一律报错。** `docs/05-mcpp-toml.md`
  §2.8.1 把 OpenBLAS / MKL 这类**导出不同符号集、按链接选择**的库明确写成**正常用法**,
  一律报错会把它们一起拒掉,而那是一个已发布的、有文档的行为。
- **也不能只对「符号重叠的」报错**,因为绑定发生在**任何目标文件存在之前**,
  引擎那时读不到符号。`symbol_provision` 能读,但它在链接之后 —— 这正是本条要提前的那一刻。

⇒ 缺的是**一个声明**:某个 capability 的 provider 是否互斥,只有能力的定义者知道。
形如 `provides = ["gpu-blas"]` 之外再声明 `exclusive = true`,或让 capability 本身
带一个「同名符号」的性质。**这是一个新的 manifest 键与一次设计决定,不是一次实现。**

因此本轮**没有实现决定 4**,理由不是排期而是它需要先被重新设计。
⭐ 与之相关的观察:R6 / R7 目前并不真的被它阻塞 —— 互斥 feature 已经能表达这件事
(`examples` 与索引里的 `compat.*` 都这么用),缺的只是**在用户写错时报错**,
而不是**能不能写对**。

---

## 8. 生态闭环(决定 10)

⭐⭐ **引擎认识一个概念 ≠ 有包声明它。** 没有索引侧的包,这套设计对真实开发者
等于不存在。本节定义「闭环」的具体内容与顺序。

### 8.1 四层,每层验证一件事

**L0 工具链载荷(xim-pkgindex)**

| 包 | 验证 | 顺序理由 |
|---|---|---|
| `adaptivecpp` | 形态 B 全链路 | ⭐ **构建期不需要厂商工具链**,体积最小,**能在没有 CUDA 载荷的情况下先把形态 B 跑通** |
| `cuda-toolkit` | 形态 A + `-ccbin` 配对 | 体积风险见 RK-3 |
| `rocm` | 第二家,证明不是 CUDA 特例 | ⭐ **两家跑通才排除「为 CUDA 定制」** |

**L1 规则包(mcpp-index)** —— `rules-cuda`、`rules-rocm`,各
`provides = ["mcpp:device-rule=<backend>"]`。验证 P5 与 `backend` 的 capability 解析。

**L2 能力提供者(mcpp-index)** —— `compat.cublas` / `compat.rocblas`,
各 `provides = ["gpu-blas"]`。⚠️ 这两个同时在图里就是 §7.4 的形状,
**所以它们本身就是决定 4 的验证夹具**。

**L3 真实的库(mcpp-index)** —— 这一层才是证据。

| 包 | 一个包验证几类 | 为什么选它 |
|---|---|---|
| ⭐ **ggml** | R1 + R3 + R5 | **纯 C++、无 Python 层、多后端(CUDA/HIP/Metal/Vulkan/SYCL)、已按后端出预建产物。** 一个包覆盖三类使用者,是单位投入产出最高的 |
| **CUTLASS** | R4 | header-only + 架构门控(`CUTLASS_NVCC_ARCHS` 是按 CUDA 版本门控的白名单),正好压测逐 glob 收窄 |
| **oneMath / oneDNN** | R6 + R8 | UXL 治理的中立库;验证 capability 与形态 B |
| **onnxruntime** | R1 | 已按 EP 分包名,是现成的多变体消费对象 |

⇒ **首选 ggml。** 理由不是它最流行,是它**一个包同时压到 R1/R3/R5 三类**,
而且它没有 Python 打包层,不会把 wheel 的问题混进来。

### 8.1b 要补进生态的包(2026-09-05 追加)

引擎能力落地之后,生态里必须有**真实流行的**运行时 / 库 / 框架 / SDK,
否则这套能力对开发者等于不存在。下表按依赖顺序排,每一行注明它验证什么。

#### xim 侧(工具链与 SDK 载荷)

| 包 | 内容 | 验证什么 | 依赖 |
|---|---|---|---|
| ⭐ `adaptivecpp` | SYCL,SSCP + **omp 后端** | **形态 B 可在无卡 CI 端到端跑,含 kernel 真执行** | LLVM 载荷 |
| `cuda-nvcc` | `nvcc`/`ptxas`/`nvlink`/`fatbinary`/`cicc` + CRT 头 | 设备工具链不再依赖 host(§5.0b) | —— |
| `cuda-cudart` | CUDA 运行时库与头 | 程序能链、能跑 | `libcuda-host-link`(**已有**) |
| `dpcpp` | intel/llvm 开源版,`configure.py --cuda/--hip` | 形态 B 的 NVIDIA/AMD 后端(无需 Codeplay) | —— |
| `cudnn` | 深度学习原语 | 真实推理框架可用 | `cuda-cudart` |
| `nccl`(后续) | 多卡通信 | 多卡场景 | `cuda-cudart` |

⚠️ 体积按组件取,不打整个 toolkit(§9 RK-3)。`cuda-nvcc` + `cuda-cudart` 是百 MB 级。

#### mcpp-index 侧(规则包、能力包、真实库)

| 包 | 提供 | 验证什么 |
|---|---|---|
| `rules-cuda` | `mcpp:device-rule=cuda` | P5 规则包;`backend` 经 capability 解析 |
| `compat.cudart` | `gpu-runtime` | capability 绑定 |
| `compat.cublas` | `gpu-blas` | ⚠️ 与 `compat.rocblas` **同名符号** ⇒ 它们本身就是决定 4 的验证夹具 |
| `compat.cccl` | header-only(Thrust/CUB/libcu++) | 最小可用的 GPU 算法库;无二进制分发问题 |
| `compat.cutlass` | header-only,按 CUDA 版本门控架构白名单 | **逐 glob 架构收窄(R4)的压测对象** |
| ⭐⭐ `llama.cpp-m` 加 CUDA 后端 | —— | **R1 + R3 + R5 三类;而且它已经在 `mcpplibs/` 里** |
| `compat.onnxruntime`(后续) | 按 EP 分变体 | R1 的多变体选择,真实体量 |
| `compat.opencv` + CUDA(后续) | —— | 真实的混合工程 |

⭐⭐ **`llama.cpp-m` 是首选验证对象,理由不是它流行,是它一个包同时压到三类使用者**,
而且它没有 Python 打包层,不会把 wheel 的问题混进来。它已经存在于生态中,
所以这一步是「给它加一个后端」,不是「从零收录一个大工程」。

⭐ `compat.cccl` 与 `compat.cutlass` 都是 header-only,**没有二进制分发与体积问题**,
所以它们是 §11.1 说的「源码分发这条最短路径」上最先能落地的两个真实库。

### 8.2 ⚠️⚠️ 发布顺序是一个环,必须拆开

memory 记过两条硬的:「消费者先发布,索引 `latest` 才能动」、
「⚠️⚠️ PR 绿 ≠ 合入后 main 绿,判据是 `origin/main` HEAD SHA 上的 run」。

这里的环是:

```
引擎支持 accel  →  索引包才能发 accel  →  才能做真实验证  →  才知道 accel 形状对不对
      ↑                                                                    │
      └────────────────────────────────────────────────────────────────────┘
```

拆法:**用 path 依赖与本地包在索引之外先闭环一次**,再发布。
即阶段顺序必须是「本地闭环 → 引擎发布 → 索引发布 → 沙箱验证」,四步不能并。

⚠️ 而 §9 RK-2 说明:**`accel` 字段进已发布描述符之前,必须先确认旧客户端读它不会失败。**

### 8.3 ⭐ 真实验证:分清哪些判据不需要 GPU

这是应对 RK-1 的关键,也是本节最有价值的一条。

**不需要 GPU 就能验的(占绝大多数)**:

- `tag_check` 的拒绝与诊断文本
- 架构收窄成空集/非子集的报错
- 宿主编译器配对不合时**在 nvcc 之前**报错
- `.cu` 不出现在 P1689 结果里;改 `.cuh` 触发重编
- `cfg(any(accelerator=…))` 的合并与未知键诊断
- 同名符号的 provider 在**绑定期**被拒
- **编译与链接本身** —— 在无卡机器上编 `sm_90` 的代码完全正常

**必须有 GPU 才能验的(少数,要隔离)**:

- kernel 真的跑起来
- `--accel native` 的探测
- 运行期多后端派发(R5)

⚠️⚠️ memory:「`# requires: llvm` 的 e2e 从未在 CI 跑过 —— 两 shard 都无 llvm 且
skip 退 0;⭐**守卫住在 job 里**」。
⇒ **需要 GPU 的用例不得与其余 e2e 混在同一套件里用 `# requires:` 跳过**,
否则它们会永远绿而从未运行。它们必须住在**一个独立的、可见其存在与否的 job** 里;
该 job 不存在时,状态是「未覆盖」而不是「通过」。

### 8.5 ⭐⭐ 没有 GPU,能验到哪一步

「有没有模拟 GPU」这个问题的答案分三档,而它**改变了排期的理由**。

**第一档 —— 编译期判据,完全不需要 GPU(占绝大多数)**

`tag_check` 的拒绝与诊断、架构收窄成空集/非子集的报错、宿主编译器配对提前报错、
`.cu` 不进 P1689、改 `.cuh` 触发重编、`cfg` 合并与未知键诊断、绑定期同名符号拒绝、
以及**编译与链接本身** —— 在无卡机器上编 `sm_90` 的代码完全正常。

**第二档 —— kernel 可以在 CPU 上真跑(这一档我原来漏了)**

| 后端 | CPU 上跑 kernel 的路径 | 证据强度 |
|---|---|---|
| **SYCL / AdaptiveCpp** | `--acpp-targets=omp`(`omp.library-only` / `omp.accelerated`);其后端列表含 **host CPU (LLVM)** | ⭐ 已从 AdaptiveCpp 官方 `doc/compilation.md` 核实 |
| SYCL / Intel DPC++ | OpenCL CPU runtime 提供一个 CPU device | 未在本轮核实 |
| Vulkan compute | Mesa **lavapipe** / Google **SwiftShader**(纯软件 Vulkan 实现) | 未在本轮核实 |
| OpenCL | **PoCL** | 未在本轮核实 |

**第三档 —— 没有生产可用的模拟路径**

- **CUDA**:`-deviceemu` 早已从工具链移除;GPGPU-Sim / Accel-Sim 是学术模拟器,
  速度与版本跟进都不适合 CI。⇒ **CUDA kernel 的执行只能上真卡。**
- **HIP**:有 HIP-CPU(header-only),覆盖面受限。
- **Metal**:没有 CPU 实现。

#### ⭐⭐⭐ 由此改写 AdaptiveCpp 优先的理由

v4 说先做 AdaptiveCpp 是因为「载荷体积最小、构建期不需要厂商工具链」。
这是对的但不是最重要的。真正的理由是:

> **形态 B 经 AdaptiveCpp 的 omp 后端,可以在没有任何 GPU 的 CI 里
> 端到端跑完 —— 包括 kernel 真的执行。**

也就是说,**RK-1(CI 无 GPU)对形态 B 几乎不成立**;它只对形态 A 的
「kernel 真的跑起来」这一条成立。⇒ 排期上先形态 B,不只是省钱,是**先拿到一条
可以完整自证的链路**,再去做只能部分自证的形态 A。

### 8.4 生态 CI 从 PR 分支现场构建

memory:「生态 CI 从 mcpp 的 PR 分支现场构建(`MCPP_SOURCE_REF`);⭐⭐按 step 穷举
不是按仓库;⚠️没有任何 CI 解析已发布的包 ⇒ 沙箱那步唯一」。

⇒ 索引侧的加速器包必须在 **mcpp PR 分支的引擎**上验证,否则验的是旧引擎;
而「已发布物真的能用」只有沙箱那一步能答。

---

## 9. 风险

按「做错了谁能自救」排序。

### RK-1 ⚠️⚠️ CI 没有 GPU,而这套设计的目的正是防一个运行期错误

**这是最大的风险。** 本设计存在的理由是把
`no kernel image is available for execution on the device` 从运行期提前到构建期,
而项目**没有 GPU 可以证明这个提前真的发生了**。

历史先例就在本仓库:`# requires: llvm` 的 e2e 因两个 shard 都无 llvm、skip 退 0,
**从未在 CI 跑过而一直是绿的**。

**缓解,分两层**:

1. §8.3 —— 绝大多数判据不需要 GPU;需要的那少数**隔离到独立 job**,
   job 不存在时读数是「未覆盖」而非「通过」。
2. ⭐⭐ §8.5 —— **形态 B 经 AdaptiveCpp 的 omp 后端可以在 CPU 上真跑 kernel**,
   于是这条风险**对形态 B 几乎不成立**。先做形态 B 就是先拿到一条能完整自证的链路。

**残余风险(缩小后)**:仅剩**形态 A(CUDA/HIP)的 kernel 执行**与
R5 的运行期多后端派发,无生产可用的模拟路径,只能上真卡或标为未覆盖。

### RK-2 ⚠️ 旧客户端会把 GPU 产物当成「无约束」而接受(等级已下调,但形状变了)

v4 把这条列为 ⚠️⚠️「旧 mcpp 可能整份 manifest 加载失败」。**读代码后这条不成立。**

`modules/manifest/src/xpkg.cppm`(已发布描述符的读取器)注释逐字:

> the mcpp-segment key vocabulary is a CLOSED whitelist (the parse loop's else-if
> chain). An unrecognised key is collected into `Manifest::xpkgUnknownKeys` and
> **silently skipped**

并且有 `closest_known_xpkg_key` 把未知键映射回最可能的拼写做提示。
⇒ **旧客户端读到 `accel` 会跳过它,不会失败。**「新功能要新 mcpp」这个前提是成立的。

⚠️ **但残余风险换了个形状,而且正是我们要防的那个:**
旧客户端跳过 `accel` 之后,会把一个 `sm_90` 的产物当作**没有加速器约束**而接受,
在 `sm_86` 的机器上装上去 —— **`no kernel image` 又回来了,只是发生在旧客户端上。**

⭐ **缓解不需要任何新机制,只需要产物顺序。** 消费循环是 first-match-wins:

```cpp
auto bad = tag_check(*published, in.current);
if (bad.empty()) { accepted = true; break; }
```

⇒ **把 CPU-only 变体放在 `artifacts` 列表的第一个。**
旧客户端(不看 `accel`)拿到它 —— 安全、能跑、只是慢;
新客户端逐个评估 `accel`,跳过不匹配的,挑到真正合适的那个。

⇒ 这条降级为「发布约定」,写进 §11.2,并由 `mcpp pack` 保证顺序,不靠人记。

### RK-3 ⚠️ 载荷体积

ConanCenter 当年的第二条理由逐字是 *"Binaries for CUDA are huge (several Gb)"*,
这条到今天仍然成立。memory 另记过「GitCode 镜像上传限速 + 载荷瘦身 —— **主因其实是没
strip**」。

⚠️⚠️ 另有一条运维上的硬约束(memory):**GitCode 资产不可替换、不可删除 ⇒
同号不同字节做不到。** 一个发错的多 GB 加速器产物**永远改不回来**,只能 bump 版本。
产物越大、变体越多,这条的代价越高。

**缓解**:(1) 只打可再分发子集,不打整个 toolkit;(2) strip;
(3) ⭐ **先做 AdaptiveCpp**(§8.1)—— 它构建期不需要厂商工具链,可以在不解决体积问题
的前提下先把形态 B 的全链路跑通;(4) 大变体发布前先在沙箱验一遍(§8.4),
因为发出去就改不回来了。

### RK-4 ⚠️ 变体矩阵由谁构建 —— mcpp-index 的 CI 也没有 GPU

一个包 × N 后端 × M 架构家族的产物要有人编出来。**但编译不需要 GPU**(RK-1 的划分),
所以这条比看起来轻:CI 能编,只是不能跑。
**残余风险**:编出来的产物没有被运行验证过就进了索引。

⭐ **但不加标注(决定 16)。** 理由:**mcpp 今天已经在发布交叉编译的产物 ——
一个在 x86 上编出来的 aarch64 产物,同样没有在目标机器上跑过。**
GPU 产物是同一形状,不是新问题。为它单独加一个字段,等于承认交叉编译产物有两个等级,
而这个区分从来没有被建立过,也没有读者。
⇒ 与其加字段,不如把「哪些后端能在 CI 里真跑」这件事(§8.5)做成排期依据。

### RK-5 法律与许可

CUDA EULA 的 Attachment A 允许随应用再分发指定组件,NVIDIA 也发布
`redistrib_<version>.json` 机器可读清单 —— 但清单里只写 `"license": "CUDA Toolkit"`,
**如何解析成实际许可文本仍是开放问题**(NVIDIA/build-system-archive-import-examples#3)。

**缓解**:载荷描述符逐条记录来源 URL 与 sha256(与 Bazel hermetic CUDA 同法),
许可字段照抄清单原文不做解释。

### RK-6 ⚠️ 上游包的身份规则

memory:「上游包的 namespace 与版本都用上游的;**只有内容自己写的才用 mcpplibs**;
⭐『对齐上游』要逐字节 cmp;⚠️ licence 是**集合**,写错一个比不写更坏」。

⇒ `cuda-toolkit`、`rocm`、`adaptivecpp` 用上游身份;
`rules-cuda`、`compat.cublas` 是我们写的内容,用 mcpplibs。

### RK-7 形态 B × modules 未验证(§2.3)

若实测 icpx/AdaptiveCpp 不支持含 SYCL kernel 的 module interface,
形态 B 的目标必须整体退回 header 模式 —— 这会削弱「mcpp 是 modules-first」的卖点。
**缓解**:阶段 0b 先测,再决定形态 B 的排期。

### RK-8 ⚠️ 与 CMake 的易用性差距是设计取舍,不会消失

「我有一个 `.cu`」在 CMake 是 `project(x LANGUAGES CXX CUDA)` 加一行架构;
在 mcpp 要理解 device target、规则包、接缝三个概念。
**根因是 mcpp 用接缝换来了后端可替换(R5/R7),CMake 没有这个能力所以可以更简单。**
**缓解**:三行最小形态(§10.4)、`mcpp new --template cuda`、文档第一例必须是三行那个。

---

## 10. 自我 review

按四个维度,**只写查出来的问题**。

### 10.1 架构设计

**✅ v4 的两处最弱本轮都收口了**:形态 B 的身份改为**产物核验**(决定 13,§7.5),
`backend` 的两趟归属写死为「请求在第一趟、绑定在第二趟、缝上有 satisfied 记录」
(决定 12,§3.5),后者直接套用了 mcpp 已有的「请求侧与答案侧各留拼写」经验。

**⚠️ 现在最弱的一处:两种形态是两套心智模型,而这是真实的,不是包装问题。**
形态 A 加一个 target,形态 B 改 `[toolchain]`。字段名统一了(决定 15)、身份层统一了
(P3/P4 两边都用),但**「我要用 GPU」这句话在两种形态下要做的事仍然不一样**。
我找不到一个不撒谎的统一写法 —— 底层模型确实不同,**强行统一会是假的统一**。
⇒ 处理办法只能是文档把两条路分开讲清楚,而不是假装它们是一条。

### 10.2 简洁优雅

**✅** 相对 v2 少了两个键(`archs-intersect` 合并、`accelerator-any` 去掉);
`backend` 走 capability;工具链沿用既有 `family@version` 而不是新发明一套。

**⚠️** 形态 A 与 B 的 manifest 写法完全不同(一个加 target,一个改 `[toolchain]`)。
这反映底层模型真的不同,**强行统一会是假的统一** —— 但用户要学两套。

**✅ 决定 15 已把字段名统一成 `archs`**(v4 自查发现的不一致)。

**⭐ 本轮新增的一项自查:有没有「乱加专用的东西」。**
把全部新增的用户可见面逐条列出来数一遍:

| 类别 | 新增 | 数量 |
|---|---|---|
| manifest 键 | `kind="device"`、`backend`、`archs`、`device-link`、`[package] accelerators`、`[build] accel` | 6 |
| cfg | `accelerator` 一个 layer key(**复用已有的 `any`/`all`,不加谓词**) | 1 |
| CLI | `--accel` / `--no-accel` | 2 |
| 描述符 | `accel` 字段 | 1 |
| **新命令(`mcpp <verb>`)** | —— | **0** |
| **新机制** | —— | **0** |

**原样复用、一处未改的**:`BuildAction::Role`、`tag_check` + 消费循环、capability 绑定、
`deploy_files`(决定 14)、`mcpp.runtime.elf`(决定 13)、`CommandDialect`、
`[package] platforms` 的形状(§11.1)、`[toolchain] family@version`(决定 8)、
规则包、11 字段指纹。

⭐ `--accel` / `--no-accel` 不是新花样:它与 `[build] accel` 的关系,
和 `--target` ↔ `[toolchain]`、`--features` ↔ `[features]` 完全同形
—— **manifest 声明 + 命令行覆盖**,mcpp 已有的模式。

### 10.3 兼容性

**✅** `.cu` 进分类表但**不进默认 glob**,`builtin_extension_table()` 的
`.ixx` 判例逐字适用:*"a break its author cannot fix, because the tarball for that
version has already shipped."*

**✅ RK-2 由代码给出了答案**:已发布描述符的读取器对未知键是
*"collected into `Manifest::xpkgUnknownKeys` and **silently skipped**"* ⇒
旧客户端不会加载失败。「新功能要新 mcpp」这个前提成立。

**⚠️ 但残余风险换了形状**:旧客户端跳过 `accel` 后会把 `sm_90` 产物当成无约束而接受。
缓解是**产物顺序(CPU-only 排第一)**,零新机制 —— 但它必须由 `mcpp pack` 保证,
**不能靠发布者记住**,否则就是一条没有执行者的约定。

### 10.4 易用性

**✅** R1(数量最大)零改动;错误信息本身就是产品。最小形态可以是三行:

```toml
[targets.kernels]
kind = "device"
sources = ["src/*.cu"]
```

(`backend` 在只有一个 device 规则包时自动绑;`archs` 缺省 ⇒ **报错并列出该 backend
的常用取值**,不猜 —— 理由是报告 §2.2 的 CMake `native` 陷阱,根因就是把
「本机有什么」当成「要编成什么」。)

**⚠️** RK-8:与 CMake 的差距是取舍不是缺陷,但要说出来而不是粉饰。

### 10.5 ⭐ 生态闭环(本轮新增的维度)

**⚠️ 最弱的一处:L3 的「真实可用」目前只有一个候选扛得住。**
ggml 之外,CUTLASS 是 header-only(不验证分发)、oneMath 依赖形态 B(依赖 RK-7)、
onnxruntime 体量太大不适合首发。
⇒ **整个生态验证在第一阶段实际压在 ggml 一个包上**,它若卡住(例如它的
多后端构建对 mcpp 的图有超出预期的要求),闭环就没有第二条腿。
**缓解**:L3 之前先用一个**自建的最小多后端库**做夹具,把风险与 ggml 解耦。

**⭐ 本轮新增:§8.5 让「真实可用」的第一步比预想的近。**
形态 B 经 AdaptiveCpp 的 omp 后端可以在无卡 CI 里**端到端跑完并真的执行 kernel**,
而 §11.1 说明**源码分发不受体积与「谁来编」两条风险影响**。
⇒ **「AdaptiveCpp + 源码分发」是一条能完整自证、且没有运维风险的最短路径**,
应当作为第一条打通的链路;CUDA + 二进制分发排在其后。

---

## 11. 源码分发与二进制分发

加速器把这两条路的差异放大了,所以要分开说清楚。

### 11.1 源码分发 —— 消费者的 `archs` 说了算

源码包被消费时,**是消费者在编**,所以 `archs` / `backend` 由**消费者**决定,
包本身不含任何架构。那么包该声明什么?

⭐ **复用 `[package] platforms` 的形状**:mcpp 已有
`platforms = ["linux","macos","windows"]`,文档说它是
*"the platforms the package supports (a CI matrix hint, shown via `mcpp why`)"*
—— **一个声明,不是强制。**

```toml
[package]
platforms    = ["linux", "windows"]
accelerators = ["cuda", "rocm"]          # [提议] 同一形状:支持面声明,非强制
```

⇒ **两个字段语义严格对称**:`platforms` / `accelerators` 说「我支持什么」(声明),
`accel` 说「这个二进制里有什么」(实测)。**声明面与实测面各有各的字段,不混。**

三条后果:

1. 源码分发时消费者**必须自己有工具链** ⇒ 走 xlings 载荷自动安装,与 gcc/llvm 同一条路。
2. ⚠️ **编译成本落到消费者头上。** CUTLASS 规模的模板实例化,一个错的 `archs`
   可能是一小时。这是决定 11(缺省报错不猜)的第三条理由。
3. ⭐ 源码分发**不受 RK-3(体积)与 RK-4(谁来编)影响** ——
   它是加速器支持成本最低的一条路,也应该是**第一条打通的路**。

### 11.2 二进制分发 —— 粒度、顺序、以及不可撤回

**粒度:一个产物一个家族目标。**
不是每颗芯片一份(组合爆炸,报告 §3.5:Spack 旗舰 buildcache 里 GPU 二进制为零),
也不是全塞一份(体积)。家族目标(`sm_90f` / `gfx10-3-generic`)是 2025 年才有的
中间选项,正好是这个粒度(报告 §1.3)。

**顺序:CPU-only 变体必须排第一。** 见 RK-2 ——
消费循环是 first-match-wins,旧客户端不看 `accel`,谁在前面就拿谁。
⇒ **`mcpp pack` 保证这个顺序,不靠发布者记住。**

**不可撤回:** ⚠️⚠️ GitCode 资产不可替换不可删除 ⇒ 多 GB 的产物发错只能 bump 版本。
⇒ 大变体发布前必须过沙箱(§8.4)。

### 11.3 ⭐ 实际形态是混合的,设计不该假设二选一

生态里真实发生的事:

- **kernel 走源码**(消费者按自己的卡编),
- **厂商库走二进制**(cuBLAS/cuDNN 本来就只有二进制),
- **框架走二进制**(ggml/onnxruntime 按后端出预建产物)。

⇒ 三者在同一个依赖图里共存。这对设计的要求是:
**`accel` 的「缺席=不受约束」语义必须在两条路上都成立** ——
源码包没有 `accel`(它还没被编出来),CPU-only 二进制也没有 `accel`(它不受约束),
两者对 `tag_check` 是同一个读数,而这**正好是对的**:两者确实都不施加加速器约束。

⭐ 这条是「空维=不受约束」这个已有语义的一次意外红利 ——
它让源码包与 CPU 包在身份系统里天然同形,不需要第三种状态。

---

## 12. 分阶段与判据

判据取**只有做对了才会出现的读数**,不取「构建通过」。标注 `[需GPU]` 的按 §8.3 隔离。

| 阶段 | 内容 | 判据 |
|---|---|---|
> ⭐ **排期主线(§8.5 + §11.1 的结论):先「AdaptiveCpp + 源码分发」,再「CUDA + 二进制分发」。**
> 前者能在无卡 CI 里端到端自证(kernel 真跑),且不受体积与「谁来编」两条运维风险影响;
> 后者两样都占。下表的 L0/8 之所以排在 CUDA 相关项之前,就是这个理由。

| **0a** | 兼容性探针(RK-2) | 旧版 mcpp 读手工加了 `accel` 的描述符:**忽略而非失败**(代码已表明是 skip,此处是复核) |
| **0b** | 形态 B 探针(RK-7) | 实测 icpx / AdaptiveCpp 能否编含 SYCL kernel 的 module interface;以及是否需要新 `CompilerId` |
| **0c** | 规则包原型(零引擎改动) | 一个带 kernel 的工程能 `mcpp build` 出可运行二进制 `[需GPU]` |
| **1** | capability 同名符号边界(决定 4) | 两个 `provides=["gpu-blas"]` 同时在图里 ⇒ **绑定期报错**,非链接期重复符号 |
| **2** | `accel` 字段 + `tag_check` | 同一包发两变体,消费者按声明架构选中不同的那个;⚠️ 两变体须在其余各维完全相同 |
| **3** | `SourceKind::Device` + `.cuh` 进 header 轴 | `.cu` 不在 P1689 结果里;**只改一个 `.cuh` 后重建,kernel 对象真被重编**(判据落产物内容) |
| **4** | `accelerator` 多值 layer + 组合子语义 | `cfg(any(accelerator="rocm"))` 在 cuda-only 构建下不合并;拼写错被 `unknown_tokens()` 报出 |
| **5** | 逐 glob 收窄(决定 3) | 空集 ⇒ 报错;非子集 ⇒ 报错 |
| **6** | `-ccbin` 钉定 + 配对校验 | 配一个超出支持表的宿主编译器,**mcpp 在 nvcc 之前报错**,消息含两边版本号 |
| **7** | device link | 跨 TU `__device__` 调用能链上;改 `device-link` 触发重新 prepare |
| **L0** | `adaptivecpp` 载荷 | ⭐ 无 CUDA 载荷、**无 GPU** 的机器上,形态 B 全链路跑通,**且 kernel 真的执行**(`--acpp-targets=omp`) |
| **L1/L2** | `rules-cuda` + `compat.cublas`/`compat.rocblas` | 两个 provider 同时在图 ⇒ 绑定期报错(与阶段 1 同一判据,但这次是**真实的包**) |
| **L3a** | 自建最小多后端夹具(**源码分发**) | 同一份源码,消费者按自己的 `archs` 编出可用产物;不涉及二进制发布 |
| **L3b** | ggml(**二进制分发**) | 同一份源码,`--features cuda` 与 `--features rocm` 产出 `accel` 不同的两个产物;CPU-only 变体排第一(§11.2) |
| **8** | 形态 B(SYCL/OpenMP) | 换 `[toolchain]` 后产物里出现设备段;offload 载荷有 `nvptx-none` 的 `libgomp` 插件 |

#### 2026.9.5.1 实际落地了哪些阶段

| 阶段 | 状态 | 证据 |
|---|---|---|
| 0a 兼容性探针 | ✅ | `xpkg.cppm` 读取器对未知键 skip;`mcpp.toml` 的 `runtime.artifacts` 是封闭白名单,已加 `accel` |
| 0b 形态 B 探针 | ❌ 未做 | 本机无 SYCL 工具链,做了无法真实验证 |
| 0c 规则包原型 | ✅ | `examples/09-cuda-kernel`,RTX 4080 上 `mcpp run` 输出 `12 24 36 48` |
| 1 capability 边界 | ❌ **未做,且需重新设计**(见 §7.4 的补注) |
| 2 `accel` + `tag_check` | ✅ | 单测 12 条 + e2e 600;真实拒绝消息见 §14 |
| 3 `SourceKind::Device` | ✅ | 单测 6 条;`.cuh` 进 header 轴 |
| 4 `accelerator` 多值 layer | ✅ | 单测 7 条;语义按 §3.4 修正后的一行 |
| 5 逐 glob 收窄 | ❌ 未做 | 依赖 device target(阶段外),本轮 device 编译走规则包 |
| 6 `-ccbin` 配对 | ⚠️ 部分 | 引擎侧读上界并在 `mcpp self doctor` 报告;**钉定**发生在规则包里,不在引擎里 |
| 7 device link | ❌ 未做 | 依赖 device target |
| L1/L2 索引包 | ⚠️ 部分 | `compat.cuda-runtime` 已合入并发布并沙箱验证;`rules-cuda` 目前是 `examples/` 里的 path 包,尚未收录进索引 |
| L0 / L3 / 8 | ❌ 未做 | 见上 |

⚠️ **决定 3(逐 glob 收窄第一版就做)与决定 4(capability 边界先修)都没有落地。**
前者依赖 device target 这一原语,而本轮 device 编译走的是规则包路线(设计 §12 阶段 0c
本来就排在引擎 device target 之前);后者见 §7.4 的补注 —— 它需要先被重新设计。
两条都不是「忘了」,但都与拍板时的意图有出入,记在这里而不是让阶段表替它们含糊过去。

⚠️ **0a 是前置不是并行**:它若答「失败」,阶段 2 的形状要改。

---

## 13. 仍未决

v4 的六条已全部拍板(决定 11–16)。真正剩下的:

1. **`archs = ["native"]` 是否提供。** 决定 11 拒绝了「缺省时猜」,但没决定
   「显式写 native 时探测」要不要给。倾向给,且**只在本地,CI 里用它应当告警** ——
   否则就是把 CMake 那个陷阱换个写法搬进来。
2. **形态 B 的 `archs` 取值空间与形态 A 不同**(`spir64` / `nvptx-none` 是 triple 形状,
   `sm_90f` 是 arch 形状)。字段名统一了(决定 15),但**取值该不该也统一**,
   还是由 backend 各自定义值域?倾向后者,但需要写进 §7.1 的 `accel` 结构说明。
3. **`mcpp pack` 一次能不能产多个 `accel` 变体**,还是每个变体一次调用。
   影响 RK-4(谁来编)与 §11.2 的顺序保证。
4. **规则包提供节名表的接口形状**(决定 13)—— 是规则包的模块导出一个函数,
   还是描述符里的一张静态表?后者更适合「引擎不加载规则包也要能核验产物」。

---

## 14. 实施后的自我 review(2026-09-05,PR #559 / mcpp-index #346)

本节只记**实施推翻了设计的地方**,以及生态级的观察。通过的不记。

### 14.1 设计被实施推翻的五处

| # | 设计说 | 实测 | 影响 |
|---|---|---|---|
| 1 | `any(accelerator=…)` 表示成员判定,裸键表示集合相等(§3.4 三行语义) | **错的。** 让组合子改变操作数含义会使 `all(a="cuda", a="rocm")` 变成不可满足而不是「两者都启用」 | 改为**处处成员判定**,组合子保持纯布尔。**用户少学一条规则**,`cfg(accelerator="cuda")` 直接可用 |
| 2 | 「xpkg 已确认 skip 未知键 ⇒ RK-2 等级下调」 | **只对一条路成立。** 已发布描述符的读取器确实 skip;而**打包后的 `mcpp.toml` 的 `runtime.artifacts` 是封闭白名单,未知键直接报错** | RK-2 在 mcpp.toml 这条路上是真的。两个读取器两种策略,设计只查了一个 |
| 3 | 形态 B 需要一个新的工具链家族(§5.0 倾向) | **不需要。** DPC++ 就是 clang:同 driver、同 GNU flag、同 BMI。`Family` 封闭枚举一处都不用动 | 形态 B 的工程量从「加家族」降到「加载荷变体 + 能力位」 |
| 4 | nvc++ 提供 `-stdpar`,看起来属于形态 B | **属于形态 A。** HPC SDK 自述主标准是 C++17,无 modules ⇒ 当不了 `import std` 工程的宿主编译器 | 它只能作设备编译器,与 nvcc 同类 |
| 5 | 宿主编译器上界内置一张按 CUDA 版本的表(§5.2 备选) | **应当读 `crt/host_config.h`。** 表是某一个 release 的拷贝,下一版就静默过时 | 一个 mcpp 从未见过的 toolkit 也能作答;解析不了的头文件不产生断言 |

⭐ 第 1 条值得单独说:它是**实施让设计变简单**的一次,而不是变复杂。

### 14.2 生态强制执行了设计原则,而不是相反

三次实测里,**生态自己拦下了我想走的近路**:

1. **链宿主 `libcudart` 被拒。** mcpp 的运行期闭包校验报
   *"Its PT_INTERP is a private loader, so the host's /usr/lib is NOT consulted"*,
   并指向 `xlings install <pkg>`。**这不是我设计的,是引擎已有的。**
   §5.0b 的「少依赖 host」不是一条建议,是被执行的规则。
2. **`allow_host_libs = true` 不够。** 闭包校验比它更强。只能改成静态链接可再分发的一半,
   把宿主依赖收敛到 `libcuda.so.1` 这一个不可消除的点。
3. **`[xlings] deps` 只对 ROOT 工程 materialize。** 索引里 `riscv-virt-rt` 的注释已经写着
   这条,并给出正解:包级的安装边是 `xpm.<platform>.deps`。

⇒ **生态的既有约束比我的设计更严,而且它们是对的。**

### 14.3 ⚠️⚠️ 我自己犯的两个错,都是本文档反复警告过的形状

**其一,重复了一处被明令禁止重复的探测。** `compat.cuda-runtime` 的第一版自己抄了一份
宿主 `libcuda` 探测,而 xim 的 sentinel recipe 原文写着:

> Single source of truth for "where is host libcuda" → all GPU xpkgs read from
> `pkginfo.dep_install_dir(...)` and **don't reimplement ldconfig probing each**

xim 的 `hostlib.lua` 还记着这条规则的来历:**四处各自探测,三处是错的**,
每一处都是同一个「假设目录布局」的错误。我写了第五处。
**是 review 抓出来的,不是我自己发现的。**

**其二,凭印象写下了一条「为什么需要」的理由,而它是错的。**
第一版还抓了 `libnvidia-ptxjitcompiler`,PR 正文写「没有它前向兼容会静默失效」。
实测:只编 `compute_80`、只让 `libcuda.so.1` 可见,在 sm_89 上 JIT 成功(`jit result=99`)。
驱动通过自己的路径加载兄弟库。**三个 pattern 全是多余的。**

⭐ 这正是 memory `reasons-written-from-memory-kill-good-fixes` 的镜像:
凭印象写的理由既能否掉正确修法,也能**留下多余实现**,而**结论会被复查,理由不会**。

### 14.4 生态级:一个跨工具的缺陷

`xlings install` 会把**由 mcpp 安装的**裸名 shim 从 subos 的 `bin/` 里剪掉,
包括 `mcpp` 自己,即使那次安装是 already-installed 的 no-op。
已受控复现(13 条 → 6 条,删掉 7 条),已提 openxlings/xlings#582。

⭐ 形状是:**两个工具写同一个 shim 目录,其中一个把另一个的条目当作陈旧项。**
这与本文档 §1 的主题同构 —— 一个字段两个写者,而其中一个不知道另一个存在。

### 14.5 本轮的覆盖与未覆盖

**已落地并真机验证**(RTX 4080 / CUDA 12.0 / 驱动 550.144.03):
`SourceKind::Device`、`accelerator` 多值 layer、`accel` 身份维与 `tag_check`、
描述符与构建请求两端接线、宿主/设备编译器配对(`mcpp self doctor`)、
`--accel` / `--no-accel`、`[package] accelerators`、docs 20 章中英双份、e2e 600、
`examples/09-cuda-kernel`(`mcpp run` 输出 `12 24 36 48`)、
`compat.cuda-runtime`(farm 单条链向 sentinel)。

**未做,且理由是「做了无法真实验证」**:形态 B(本机无 SYCL 工具链)、
device link / RDC、含设备代码的静态库、Metal(本机无 macOS)、
xim 的 CUDA 工具链载荷。每一项在 §12 都有独立判据。

### 14.6 §8.2 的环被实际走了一遍,并且闭合了

设计说跨仓库顺序是硬的,而这一轮把它走完了:

1. 引擎侧改动进 PR #559(未发布);
2. `compat.cuda-runtime` 只用现有键,因此**不必等引擎发布**,独立进 mcpp-index #346;
3. #346 合入后,**判据是 `Publish Index Artifact` 在合入的那个 commit 上绿**
   —— 不是「PR 绿」,也不是「main 有这个文件」。实测该 workflow 在 `a6f625e3` 上
   completed/success 之后,`mcpp index update` 才能解析到它;
4. 于是示例把 `LD_LIBRARY_PATH` 的绕法换成一条普通依赖,`mcpp run` 无环境变量直接输出
   `12 24 36 48`。

⭐ 第 3 步是 memory `index-stale-but-marker-fresh` 记的第五层:
**xlings 消费的是 `artifact:<sha>`**,所以「合入了」与「能用了」之间还隔着一个 workflow。
本轮没有踩到它,因为事先按那条记忆等了发布再验证。

⚠️ 仍未闭合的是另一件事:**引擎侧的新键要等 #559 发布之后,索引里的包才能使用**。
`compat.cuda-runtime` 恰好一个新键都没用到,所以这一轮绕开了;
下一个用到 `accel` 字段的索引包不会这么幸运。
