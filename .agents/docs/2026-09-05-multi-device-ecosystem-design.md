# 多设备加速生态:载荷、适配面、验证 lane 与框架验证

2026-09-05。本文取代 `2026-09-05-accelerator-support-design.md` §8.5 / §12 / §15.8
三处的结论,并把范围从 CUDA 扩到全部主流设备后端。

已落地的引擎侧能力见前文;本文只写**尚未做的部分**,以及做它们的确切路径。

---

## 0. 前一版被实测推翻的六处

| # | 前一版说 | 实测 | 出处 |
|---|---|---|---|
| 1 | 「CUDA 工具链载荷本机无法真实验证」 | **载荷是 xim 的事,不是本机的事。** NVIDIA 的 redist manifest 给出每个组件的 URL/sha256/size/**上游版本号** | `redistrib_13.3.0.json` |
| 2 | 「`compat.cublas` 在索引的两种形态里都不成立」 | 说法错。第三种形态存在:**载荷进 xim,适配进 mcpp-index**,用 `xpm.<platform>.deps` 接线 | 该机制我已在 `compat.cuda-runtime` 用过一次 |
| 3 | 「`rules-cuda` 进索引要新建仓库」 | 错。`grpcgen` 就是 `mcpp = "*/rules/mcpp.toml"` 指向**已有仓库的子路径**;mcpp 自己的 release tarball 里已含 `examples/09-cuda-kernel/rules-cuda` | `pkgs/g/grpcgen.lua` |
| 4 | 「决定 4(capability 同名符号边界)不可实现」 | 不可实现的是「一刀拒」那个实现。加一个 manifest 声明即可,是设计决定 | 见 §6.4 |
| 5 | 「CUDA kernel 的执行只能上真卡」(§8.5 第三档) | 错。**chipStar v1.3.0**(CUDA/HIP → SPIR-V → OpenCL)配 **PoCL v7.2** 的 CPU 设备,这条链是活的 | 两者均在 2026-09 有发布/提交 |
| 6 | 「Metal 本机无 macOS」 | 理由不成立。仓库有四条 macOS CI | `ci-macos`、`ci-macos-e2e`、`bootstrap-macos`、`macOS ARM64 — xlings LLVM end-to-end` |

六条里有五条的形状相同:**我把「我手边没有」写成了「做不到」**,而 xlings 生态
的存在意义正是消除「手边有没有」这个变量。

---

## 0.1 已发布的示例违反了本文的规则,两处,必须先修

`examples/09-cuda-kernel`(2026.9.5.1 已发布)是**教用户怎么做**的东西,
而它教的两件事都是错的。

### 0.1.1 用了已被取代的 `[xlings] deps`

```toml
[xlings]
deps = [{ linux = "libcuda-host-link" }]     # 2026.9.3.1 起已被取代
```

`[xlings.workspace]` 才是**那一张表**(#548)。`deps` 仍被接受,所以不报错 ——
**这正是它危险的地方**:示例不会红,而抄它的人写下的是过时形式。

且这个键在文档里**到处都是**:英文侧 `docs/05` ×5、`07` ×2、`13` ×2、
`15` ×1、`17` ×3、`18` ×1;中文侧 `05` ×3、`07` ×2、`13` ×2、`17` ×2、`18` ×1。
⇒ **从全部用户文档中删除,不留「也可以这么写」** —— 留着就是留一个误用入口。
(`[xlings].subos` 与 `[xlings.workspace]` 不受影响,它们是当前形式。)

### 0.1.2 手写了 host 路径

```toml
[build]
ldflags = ["-L/usr/local/cuda/lib64", "-L/usr/lib/x86_64-linux-gnu",
           "-lcudart_static", "-lrt", "-lpthread", "-ldl"]
```

规则包里同样:

```cpp
c.push_back("/usr/local/cuda/bin/nvcc");
c.push_back("/usr/bin/nvcc");
c.push_back("/usr/include/crt/host_config.h");
```

**示例通篇依赖宿主的 CUDA 工具包**,而本文 §1 的规则说 host 只留
`libcuda.so.1`。**我写了一个示范违反自己规则的示例。**

⇒ 正确形态,在 §3.1 的载荷落地后:

```toml
[xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"

[dependencies.compat]
cuda-driver = "2026.09.05"      # 唯一的 host 面,由包持有

[build]
accel = "cuda12.9+{sm_89} ptx>=89"
# ldflags 里不出现任何绝对路径
```

规则包侧:nvcc 与 `crt/host_config.h` 都从 `mcpp::xpkg_dir("xim","cuda-nvcc")`
得到,**一个 `/usr` 都不出现**。

这两条排在 §8 的最前面 —— 它们不是新功能,是**已发布的错误示范**,
每多一天就多一批照着抄的人。

## 1. 一条规则,以及它的判据

> **凡是能进 xim / mcpp-index 的,一律进;host 只保留物理上无法再分发的那一层,
> 并且那一层的边界必须被测量,不被假设。**

判据不是「我们尽量了」,而是可以逐项核对的两问:

1. **这个东西为什么不在载荷里?** 答案只允许是「许可禁止再分发」或
   「与内核 ABI 锁步」。其余任何答案(体积、麻烦、本机已有)都不成立。
2. **它的版本边界是被问出来的还是被假设的?** 每一条 host 依赖必须有一个
   **运行期可查的问法**,mcpp 用那个问法回答,而不是内置一张表。

这是同一条原则的第三次应用:
- `crt/host_config.h` → 宿主编译器上界(已落地)
- `nvcc --dryrun` → 设备编译器能否够到自己的后端(已落地)
- `cuDriverGetVersion` → **驱动能跑多新的工具包**(本文新增,见 §2.1)

---

## 2. host 必须面的穷举

逐后端列出「哪一层必须来自 host」,以及为什么不能进载荷。

| 后端 | host 必须面 | 为什么不可再分发 | mcpp 怎么问它 |
|---|---|---|---|
| **CUDA** | `libcuda.so.1`(驱动用户态库) | NVIDIA 驱动 EULA 禁止第三方再分发;且与内核模块 ABI 锁步 | `cuDriverGetVersion` |
| **ROCm/HIP** | `libamdhip64` 之下的 KFD/内核驱动接口 | 内核侧 | `hipRuntimeGetVersion` |
| **Vulkan** | ICD(`/usr/share/vulkan/icd.d/*.json` + 厂商 `.so`) | 厂商驱动;**但软件 ICD(lavapipe)可以进载荷** | `vkEnumeratePhysicalDevices` |
| **OpenCL** | ICD(`/etc/OpenCL/vendors/*.icd`) | 同上;**PoCL 可以进载荷** | `clGetPlatformIDs` |
| **Level Zero** | `libze_loader` + 厂商 driver | 厂商驱动 | `zeInit` |
| **Metal** | 系统框架 | 属于 macOS | 平台判定 |

其余一切 —— nvcc、cudart、nvrtc、cuBLAS、hipcc、DPC++、AdaptiveCpp、
PoCL、lavapipe、SPIR-V 工具、chipStar —— **全部进 xim**。

### 2.1 驱动版本决定可用的工具包版本 —— 已实测,失败很晚

**实测(2026-09-05,本机 RTX 4080 / 驱动 550.144.03):**

用 CUDA **13.3.33** 载荷编 `sm_89`,与用宿主 **12.0** 编同一份源码,同一台机器:

| 用什么编 | 编译 | 链接 | 运行 |
|---|---|---|---|
| CUDA 13.3 载荷 | 干净 | 干净 | `runtime=13030 driver=12040`<br>`cudaMalloc: CUDA driver version is insufficient for CUDA runtime version` |
| 宿主 CUDA 12.0 | yes | | `result: 12 24 36 48` |

**编译与链接全干净,失败在第一次 `cudaMalloc`。** 这与 §1 那个「为 sm_90 编的
库被 sm_86 消费」是同一类失败:晚、且消息不指向任何一次人为选择。

⇒ **处置:两条版本线并存,由 xim 的多版本管理承担。** 13.3 线与 12.9 线各自完整,
mcpp 在构建前问驱动要一个数,选能跑的那条。**这不是权变,是 xim 本来就是干这个的。**

### 2.2 载荷是一个闭包,而这个闭包要被**发现**,不能被猜

同一次实测暴露的第二件事:CUDA 13.x 把组件拆得比 12.x 细得多。
只装 `cuda_nvcc`(30.1 MB)会得到:

```
sh: 1: .../bin/../nvvm/bin/cicc: not found
```

`cicc` 在 **`libnvvm`**(46.9 MB),`crt/host_config.h` 在 **`cuda_crt`**,
另有 `libnvptxcompiler`、`libnvfatbin`、`cuda_culibos`、`cccl`。
一个**能编东西的** nvcc 是七个组件的闭包,不是一个包。

**补齐它的办法不是抄一张目录表,而是用已经落地的那条 doctor 检查驱动它** ——
`nvcc --dryrun` 报出第一个解析不到的裸名,装上带它的组件,再问一次。
这与 §15.7 是同一个机制:**那条检查原本用来抓坏掉的宿主,它同样抓不完整的载荷。**

### 2.3 载荷带着自己的 `nvcc.profile`,因此没有沙箱那个病

Debian 把 `nvcc.profile` 做成指向 `/etc` 的符号链接,沙箱替换 `/etc` 就断
(前文 §15.7)。**redist 载荷把 `nvcc.profile` 放在自己的 `bin/` 里** ——
解到任意前缀都自定位。实测 `nvcc --dryrun` 声明的 PATH 指向载荷自己的目录。

顺带量到:**载荷自带的 `host_config.h` 上界是 gcc ≤ 15 / clang ≤ 21**,
而宿主那个 CUDA 12.0 是 gcc ≤ 12。⇒ **载荷不只是「不依赖 host」,它的宿主编译器
上界还宽得多** —— 这是「进生态」的直接收益,不是理念收益。

## 3. xim 载荷表(全部取当前最新,附实数)

版本号一律用上游的。日期版本只留给「内容是本生态自己写的、没有上游产物」的适配器
(与 `compat.glx-runtime` / `compat.vulkan-runtime` 同规)。

### 3.0 载荷边界由 manifest 的 `license` 字段决定,不由我判断

CUDA 13.3.0 的 linux-x86_64 组件共 **43 个、4.19 GB**,manifest 把它们分成三类许可,
而这三类的处置各不相同:

| 许可 | 组件 | 合计 | 处置 |
|---|---|---|---|
| `CUDA Toolkit` | nvcc、cudart、nvrtc、nvjitlink、**cuda_gdb**、**sanitizer**、cuobjdump、nvdisasm、cuxxfilt、nvml、opencl、profiler_api、nvtx… | ~1.9 GB | **全部进 xim** |
| `NVIDIA SLA` | **nsight_systems 1074.8 MB**、**nsight_compute 336.5 MB** | 1.41 GB | **进 xim**,GLOBAL url 直指 NVIDIA 自己的 CDN |
| `NVIDIA Driver` | nvidia_driver 528.9 MB、cuda_compat | 0.53 GB | **唯一留在 host 的那层** |

第二行不是让步,但我原先的说法过绝对,已按 xim 的规范改正:

- xim **推荐**官方二进制走 `xpm.source = "xlings-res"`,那要求
  `github.com/xlings-res/<pkg>` 与 `gitcode.com/xlings-res/<pkg>` 两腿都有同名
  release、资产字节一致 —— 那是 re-host;
- 但规范**明确允许**第三方 release 用 **URL template + per-arch sha256**
  (`source = "https://.../${version}/...${arch}..."`),索引里 247 个配方直连
  github 就是这一形态。

⇒ **CUDA 组件走 URL template 直连 NVIDIA 的 CDN**:manifest 已给出稳定 URL 与
sha256,不产生再分发行为,SLA 与 Toolkit 两类许可都成立。
代价是 CN 用户直连 NVIDIA CDN;若实测过慢,再单独决定是否把第①档
(150 MB)镜像进 xlings-res —— **那是一个可以后加的优化,不是前置条件**。

**「体积」不是任何一条的理由。** 去掉 driver 后 **3.66 GB 全部进 xim**,
拆成约 40 个可独立安装的组件包 —— 一次普通 CUDA 构建只装第①档的 **150 MB**,
要 profile 的人才装 nsight。**这正是组件级拆包存在的意义。**

### 3.1 CUDA 载荷(全部进 xim,版本一律用 manifest 里的上游版本)

**⓪ 编译器本身 —— 已在 xim,不是新包**

| xim 包 | 版本 | 说明 |
|---|---|---|
| `xim:llvm` | **22.1.8**(已装) | **加速器路线的默认编译器**(§6.3.5)。它同时是宿主编译器、CUDA 设备编译器(`-x cuda`)、SPIR-V 前端 |
| `xim:gcc` | 13.3.0 / 15.1.0 / 16.1.0(已装) | 岛形态里设备 TU 的 `-ccbin` 备选;普通 C++ 工程的默认不变 |

主路线的编译器**一个新包都不用加** —— 它已经是 mcpp 生态的一等公民。
这正是「以 LLVM 为主」在工程上便宜的原因。

**① CUDA 编译与运行必需 —— 150 MB**

| xim 包 | 13.3 线 | 12.9 线 | 大小(13.3) |
|---|---|---|---|
| `xim:cuda-nvcc` | 13.3.33 | 12.9.86 | 30.1 MB |
| `xim:cuda-cudart` | 13.3.29 | 12.9.79 | 1.5 MB |
| `xim:cuda-nvrtc` | 13.3.33 | 12.9.86 | 66.4 MB |
| `xim:libnvjitlink` | 13.3.33 | 12.9.86 | 53.5 MB |
| `xim:cuda-cccl` | — | 12.9.27 | 1.0 MB |
| `xim:cuda-nvtx` / `xim:cuda-profiler-api` | 13.3.29 / 13.3.27 | | 0.1 MB |

两条线并存,由 §2.1 的驱动判据选。本机驱动只到 CUDA 12.4 ⇒ 走 12.9 线。

**② 算子库 —— 按 shared / static 拆开**

宿主上实测的 shared 实体大小(这是消费者真正要装的):

| 库 | shared | 对照:static 档案 |
|---|---|---|
| `libcublas.so` | 102.5 MB | `libcublas_static.a` 154.9 MB |
| `libcublasLt.so` | 484.2 MB | `libcublasLt_static.a` **823.6 MB** |
| `libcusolver.so` | 290.4 MB | |
| `libcusparse.so` | 243.4 MB | |
| `libcufft.so` | 147.0 MB | |
| `libcurand.so` | 92.2 MB | |

⇒ `xim:libcublas` 拆成 `libcublas` / `libcublas-static` 两个包:默认装 shared,
需要静态链接的人显式装。**不是砍功能,是让消费者付自己那份。**

**③ 调试与分析 —— 生态完备性要求,全部进**

| xim 包 | 版本 | 大小 | 许可 |
|---|---|---|---|
| `xim:cuda-gdb` | 13.3.27 | 90.1 MB | CUDA Toolkit |
| `xim:cuda-sanitizer` | 13.3.27 | 10.2 MB | CUDA Toolkit(compute-sanitizer) |
| `xim:cuda-nvdisasm` / `cuobjdump` / `cuxxfilt` | 13.3.29 | 4.8 MB | CUDA Toolkit |
| `xim:nsight-systems` | 2026.1.3.243 | 1074.8 MB | NVIDIA SLA |
| `xim:nsight-compute` | 2026.2.0.7 | 336.5 MB | NVIDIA SLA |

少了这一档,mcpp 上的 CUDA 开发只能编不能查 —— **那才是偷工减料**。
`mcpp` 侧接线:`mcpp run --profile nsys` / `--check sanitizer` 走既有的
runner 与 action 机制,不新增专用命令。

### 3.2 SYCL / oneAPI

| xim 包 | 版本 | 大小 | 来源 |
|---|---|---|---|
| `xim:dpcpp` | **7.1.0**(2026-09-02) | 207 MB(linux) | `intel/llvm` release `sycl_linux.tar.gz` |
| `xim:adaptivecpp` | **25.10.0** | 源码构建 | 无 release 资产 ⇒ 依赖 `xim:llvm` 构建 |

**已核实:官方 Linux 资产已含 CUDA 与 HIP 后端。** v7.1.0 的构建配置是
`--cuda --hip`,release notes 的测试矩阵里有 "NVIDIA CUDA BACKEND on NVIDIA
GeForce RTX 3090"。⇒ Linux 不需要自建。

**Windows 不含**:同一份 release notes 写着 "HIP & CUDA plugins on Windows are
not being built"。Windows 上自建,而**版本号仍用上游 tag `7.1.0`** —— 见 §3.5。

### 3.3 CPU 设备与模拟器(这些也进载荷,不依赖 host)

| xim 包 | 版本 | 提供什么 |
|---|---|---|
| `xim:pocl` | **7.2**(2026-09-04) | OpenCL **CPU 设备** |
| `xim:mesa-lavapipe` | 25.2.8+ | Vulkan **软件 ICD**(`PHYSICAL_DEVICE_TYPE_CPU`) |
| `xim:chipstar` | **1.3.0** | CUDA/HIP → SPIR-V → OpenCL/Level Zero |
| `xim:spirv-tools` / `xim:glslang` | 最新 | SPIR-V 汇编与 GLSL→SPIR-V |

**模拟器进载荷,是「无卡 CI 也能跑 kernel」这件事从偶然变成契约的唯一办法。**
本机现在能跑 lavapipe 是因为发行版恰好装了它;进 xim 之后,任何机器都能跑。

### 3.4 ROCm / HIP

| xim 包 | 版本 | 说明 |
|---|---|---|
| `xim:hip-runtime` / `xim:hipcc` | ROCm **7.14.0**(2026-07-16) | HIP 可以 `HIP_PLATFORM=nvidia` 走 CUDA 后端 ⇒ **本机 4080 就能验 HIP 前端** |

### 3.5 版本对齐是硬规则,自建也不例外

> **凡是有上游版本的,一律用上游版本;自己编的不改号。**

- `xim:cuda-nvcc@13.3.33` —— manifest 里的 `version` 字段,逐字。
- `xim:dpcpp@7.1.0` —— 上游 tag。**Windows 自建出来的也叫 7.1.0**,因为它就是
  那份源码;构建差异记在描述符的构建元数据里,不体现在版本号上。
- `xim:adaptivecpp@25.10.0` —— 上游 tag,尽管是源码构建。
- `xim:pocl@7.2`、`xim:chipstar@1.3.0`、`xim:hip-runtime@7.14.0` —— 同理。

日期版本(`2026.09.05` 这种)**只留给内容是本生态自己写的、没有上游产物的适配器**,
与 `compat.glx-runtime` / `compat.vulkan-runtime` 同规。

这条已经被违反过一次:`compat.cuda-runtime` 的名字与内容不符(§4.1),
虽然版本形态是对的。

---

## 4. mcpp-index 表

xim 装载荷,mcpp-index 描述「怎么对它构建」。实测:xim-pkgindex 179+ 个包里
**零个带 `mcpp` 块** ⇒ 分工是硬的,适配器必须住在 mcpp-index。

### 4.1 适配器(host 面,farm + `runtime.library_dirs`)

| 包 | 状态 | 动作 |
|---|---|---|
| `compat.cuda-driver` | 现名 `compat.cuda-runtime`,**名字与内容不符** | **改名**。NVIDIA 词汇里 "CUDA Runtime" 专指 `libcudart`;本包 farm 的是驱动,其 `capabilities`/`provides` 已写作 `cuda.driver` |
| — | `repo` 字段指向 `openxlings/xim-pkgindex` | **改正**。同族两包指向被适配物的上游 |
| `compat.vulkan-icd` | 新增 | farm host ICD;缺失时回落 `xim:mesa-lavapipe` |
| `compat.opencl-icd` | 新增 | 同上,回落 `xim:pocl` |

### 4.2 构建规则包

| 包 | 来源 | 说明 |
|---|---|---|
| `mcpplibs.rules-cuda` | mcpp release tarball 的 `examples/09-cuda-kernel/rules-cuda` | **一个仓库都不用新建**,与 `grpcgen` 同形。搬进索引后示例改成消费索引里那一份,避免两份 |
| `mcpplibs.rules-hip` | 同一 tarball | hipcc 的 `--offload-arch` |
| `mcpplibs.rules-spirv` | 同一 tarball | `glslc` / `slangc` → SPIR-V,产物是数据不是目标文件 |

### 4.3 库适配(载荷在 xim,构建面在这里)

`compat.cublas`、`compat.cudnn`(若许可允许)、`compat.rocblas`、`compat.onemkl` ——
一律 `xpm.<platform>.deps = { "xim:<载荷>@<上游版本>" }`,自己不探测宿主。

---

## 5. 设备矩阵 × 验证 lane

**每一行都有 lane,没有「不做」这一列。** 有硬件用硬件,没硬件用载荷里的模拟器。

| 后端 | 编译期判据 | kernel 真跑的 lane | 本机可用? |
|---|---|---|---|
| **CUDA / nvcc** | 无需设备 | **本机 RTX 4080**(compute 8.9) | 已用过 |
| CUDA(无卡环境) | — | `xim:chipstar` + `xim:pocl` CPU 设备 | 待接 |
| **SYCL / DPC++ (icpx)** | 无需设备 | ② CUDA 后端 → **本机 4080**,**已实测跑通**(§6.4.5,`seam: 11 22 33 44`) | 已验证 |
| **SYCL / AdaptiveCpp** | 无需设备 | `--acpp-targets=omp`,**CPU 上真跑** | yes |
| **Vulkan compute** | 无需设备 | ① **lavapipe(已实测跑通)** ② **本机 4080** | **两条 lane 同机** |
| **OpenCL** | 无需设备 | ① `xim:pocl` CPU ② 本机 `nvidia.icd` | 两条 |
| **HIP / ROCm** | 无需设备 | ① `HIP_PLATFORM=nvidia` → **本机 4080** ② chipStar+PoCL | yes |
| **Metal** | 无需设备 | macOS CI runner | 四条 macOS CI |
| Level Zero | 无需设备 | 无 Intel GPU ⇒ CPU device | 仅 CPU |

### 5.1 Vulkan 那一行已经跑通了,记在这里作为 lane 的样板

本机同时有两个 Vulkan 设备:

```
PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   NVIDIA GeForce RTX 4080
PHYSICAL_DEVICE_TYPE_CPU            llvmpipe (LLVM 20.1.2, 256 bits)
```

lavapipe:Vulkan 1.4、conformance 1.3.1.1、队列含 `QUEUE_COMPUTE_BIT`。
实跑一次 compute dispatch(`data[i] = data[i]*3+1`,256 元素):

```
device: llvmpipe (LLVM 20.1.2, 256 bits)  (type=CPU)
checked 256 elements, wrong=0 ; sample: 1 4 7 766
KERNEL RAN ON CPU AND THE RESULT IS CORRECT
```

**对照也跑了**:只给 `nvidia_icd.json` 时同一程序报 `NO CPU COMPUTE DEVICE` 退 1,
而这台机器确有可用的 4080 ⇒ 对照不是空的,成功确实来自软件设备。

这就是每条 lane 要达到的样子:**一个真的 dispatch、一个真的结果、一个能让它
变红的对照。** 不是「设备列出来了」。

---

## 6. 引擎与插件的分界

> **原则:mcpp 核心只做通用构建架构;凡是能由插件回答的,一律由插件回答。**

「插件」在 mcpp 里已经有名字:**规则包**(`host-module = true` 的包,导出一个只
`import std` 与 `mcpp` 的模块,发 `mcpp::action` 边)。它已经在 `examples/09-cuda-kernel`
里跑通了。本节的全部工作就是把下面这条线画清楚:

| 问题 | 归谁 |
|---|---|
| 图里有没有「设备目标」这种节点 | **核心** |
| 一个目标要不要第二次链接、产物是什么角色 | **核心**(边的形状) |
| 那第二次链接的程序叫 `nvlink` 还是 `amdgcn-link` | **插件** |
| 产物身份里有没有 `accel` 这一维、怎么比较 | **核心** |
| `sm_90f` / `gfx942` / `spir64` 怎么拼、怎么算覆盖 | **插件** |
| 一个载荷能不能在这台机器上用 | **核心**(机制) |
| 用什么函数去问驱动要那个数 | **插件** |

判据:**核心里不出现任何厂商名字。** 现有实现已经违反过一次 —— `mcpp.toolchain.devicehost`
读的是 `crt/host_config.h`、`unreachable_device_stage` 调的是 `nvcc`,两者都写死了
CUDA。⇒ 本轮要把它们改成**由规则包提供探针、核心只负责调用与报告**。

### 6.1 核心要补的(通用,不含厂商知识)

| # | 项 | 通用形式 |
|---|---|---|
| C-1 | **设备目标原语** | `[[target]] kind = "device"`:一种产出「不参与常规链接、由某条边消费」的目标 |
| C-2 | **二次链接边** | 一个目标可声明它需要一次 `role = "device-link"` 的 action,输入是若干对象、输出是一个对象。核心只管顺序与指纹 |
| C-3 | **逐 glob 收窄** | 每条 source glob 可带 `accel` 约束;空集报错、非子集报错 |
| C-4 | **`exclusive` 能力声明** | 包自己声明独占某能力;两个独占同名能力者同时在图里 ⇒ 绑定期报错。**不由核心猜哪些能力互斥** |
| C-5 | **载荷可用性机制** | 载荷描述符可声明「需要宿主某个量 ≥ N」;核心在构建前取那个量并比较,消息给出两边取值与该轴补救。**取值的办法由插件给** |
| C-6 | **含设备代码的归档** | `.a` 携带 `accel` 维并随包传播 |
| C-7 | **`accel` 维的语法是开放的** | 核心只认「后端名 + 版本 + 架构集合 + 可选下界」这个形状,**不内置任何后端的取值表** |

### 6.2 插件(规则包)要提供的

| 规则包 | 提供 |
|---|---|
| `rules-cuda` | nvcc 调用与 `-gencode` 拼装;`sm_XX`/`sm_XXf`/`sm_XXa` 的覆盖判定;`nvlink` 的 device-link 边;`cuDriverGetVersion` 探针;`crt/host_config.h` 的上界读取 |
| `rules-hip` | hipcc 与 `--offload-arch`;`gfx*` 与 generic target 的覆盖判定;`hipRuntimeGetVersion` |
| `rules-sycl` | icpx / acpp 的 `-fsycl-targets` / `--acpp-targets`;形态 B 的整目标编译 |
| `rules-spirv` | `glslc` / `slangc` → SPIR-V;产物是数据不是对象 |
| `rules-metal` | `metal` / `metallib` |

**既有实现的两处要迁走**:`devicehost.cppm` 的 `parse_host_config` 与 doctor 的
`unreachable_device_stage` 现在都写死 CUDA。改法是核心提供
「向规则包要一个探针、执行它、报告结果」的通道,**CUDA 的那两个探针搬进 `rules-cuda`**。
收益不是洁癖:AMD/Intel/Apple 各有自己的上界与后端可达性问题,**核心不该长四次**。

### 6.3 这条分界对本方案其余部分的影响

- §2.1 的驱动配对 ⇒ C-5(机制)+ `rules-cuda`(探针),而不是核心里加一个 CUDA 检查;
- §2.2 的载荷闭包发现 ⇒ 规则包报出缺什么,核心只负责把消息变成一条可执行的建议;
- §7 的九个框架 ⇒ 每一个都只依赖「核心 + 对应规则包」,**不要求核心认识它们**。

## 6.3.5 主路线是 LLVM/clang,不是 gcc —— 这是生态事实决定的

主次不该按偏好定。逐个核实每个后端的编译器**实体**是什么:

| 后端 | 编译器 | 是不是 LLVM |
|---|---|---|
| HIP / ROCm | `hipcc` | **AMD 的 LLVM fork**(`ROCm/llvm-project`) |
| SYCL / DPC++ | `icpx` | **Intel 的 LLVM fork**(`intel/llvm`,仓库自述 "Home for Intel LLVM-based projects") |
| SYCL / AdaptiveCpp | `acpp` | 自述 "a powerful, generic **LLVM JIT compiler**" |
| Vulkan / SPIR-V | clspv / slang / glslang 链路 | yes |
| Metal | `metal` | clang 系 |
| OpenMP offload | clang 的更成熟 | yes |
| **CUDA** | `nvcc`(EDG 前端) | **唯一的例外** |

而**唯一的例外也有两条通向 LLVM 的路,两条都已实测**:

1. nvcc **接受 clang 作 `-ccbin`** —— 本文的编译实验全程用的就是 clang;
2. clang **原生编 CUDA**(`-x cuda`),此时根本没有 nvcc,也就没有宿主编译器上界。

### 结论:主次翻转

> **加速器路线以 LLVM/clang 为主。gcc 保留支持,但不是默认。**

理由不是「clang 更好」,是**六个后端里五个本来就是 LLVM,第六个能被 LLVM 编**。
选 gcc 作主路线意味着为唯一的例外去适配,并在其余五格上一直做转换。

具体影响:

| 处 | 之前 | 改为 |
|---|---|---|
| 加速器工程的默认工具链 | 跟随 mcpp 默认(gcc 16.1.0) | **`llvm@22.1.8` 载荷** |
| CUDA 的默认编译路线 | nvcc + 挑一个上界内的 ccbin | **clang `-x cuda`**(路线 C);nvcc 路线保留给需要 `-gencode` 精细控制或新 CUDA 版本的场合 |
| `rules-*` 的形态 | 每个后端一套厂商驱动 | **五个后端共用 clang 驱动的形状**,差别只在 flag 与 target 拼法 |
| gcc 的位置 | 默认 | **岛形态里设备 TU 的 `-ccbin` 备选**,以及不含设备代码的普通工程照旧 |

**不是弃用 gcc。** mcpp 主体仍以 gcc 自举,普通 C++ 工程不受影响;
改的只是**加速器这条线的默认**。§7.9 场景 1(纯 C++)一个字都不用改。

代价要写明:clang 对**新 CUDA 版本**的支持滞后(实测 clang 22 只部分支持到
CUDA 12.9,clang 18 只到 12.3)。⇒ **需要最新 CUDA 特性的工程走 nvcc 路线**,
由 `rules-cuda` 按「clang 版本 × CUDA 版本」的可用配对来选,
两条路线共存由 xim 的多版本管理承担。

---

## 6.4 宿主编译器上界不是限制,是四条路里的一次选择(已实测)

问题:CUDA 13.3 声明 gcc ≤ 15 / clang ≤ 21,CUDA 12.9 声明 gcc ≤ 14,
而 mcpp 的工具链载荷是 **gcc 16.1.0**。这条上界会不会把混合编程钉死?

**不会。实测了四条路,其中两条把上界整个消掉。**

### 路线 A —— nvcc 的逃生门 `-allow-unsupported-compiler`

存在,但它只是关掉检查;真正的风险是新版 libstdc++ 头文件里的构造 EDG 前端解析不了。
⇒ **可用作最后手段,不作为设计的答案。**

### 路线 B —— 岛形态把上界关进设备 TU 里(已经在用了)

设备 TU 的 `-ccbin` 与项目的宿主编译器**本来就不必是同一个**:

```
项目其余部分   gcc 16.1.0(mcpp 载荷)  →  .o
设备 TU        nvcc -ccbin gcc 15.1.0   →  .o        ← 上界只约束这一格
                        ↑ 也是 xim 载荷
两者在 extern "C" 的缝上相遇 —— C ABI 跨编译器版本稳定
```

mcpp 的 registry 里现成就有 **13.3.0 / 15.1.0 / 16.1.0** 三个 gcc 载荷。
`rules-cuda` 已经在做「挑一个上界内的 ccbin」这件事,只是现在挑的是 host 上的;
改成**从 xim 挑**,这条路就完全在生态内闭合。

代价是缝必须干净:`extern "C"`、不跨缝传 std 类型。
**这正是形态 A 本来就要求的纪律**,不是额外负担。

### 路线 C —— 用 clang 直接编 CUDA,上界消失(已实测通过)

clang 原生支持 `-x cuda`。此时**宿主编译器就是 clang 自己**,
「nvcc 接不接受这个宿主编译器」这个问题不存在。

实测(本机,2026-09-05):

```
$ ~/.xlings/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++ -x cuda t.cu \
      --cuda-path=<CUDA 12.9 载荷> --cuda-gpu-arch=sm_89 -std=c++17 -c
clang++: warning: CUDA version 12.9 is only partially supported
  编译成功
```

**全程没有任何宿主编译器上界。** 产物的动态依赖闭包也是干净的:

```
libc++.so.1    → xim-x-llvm/22.1.8/lib/...
libc++abi.so.1 → xim-x-llvm/22.1.8/lib/...
libc.so.6      → xim-x-glibc/2.39/lib64/...
```

**约束是对称的,不是消失的**:nvcc 限制*宿主编译器*版本,clang 限制 *CUDA* 版本。
实测 clang 18 只到 CUDA 12.3(遇 13.3 报 `fatbinary fatal: Unknown option '-image'`),
clang 22 部分支持到 12.9。⇒ **两条约束都由 xim 的多版本管理承担**,
`rules-cuda` 按「clang 版本 × CUDA 版本」选一对可用的。

### 路线 D —— 全部来自载荷,host 只出一个库(已实测)

上面那个二进制里,glibc、libc++、cudart、nvcc/clang 全部来自载荷;
host 参与的只有 `libcuda.so.1`。

**但不能用 `LD_LIBRARY_PATH` 去够它** —— 实测把宿主 `/lib/x86_64-linux-gnu`
放进 `LD_LIBRARY_PATH`,进程 **segfault(exit 139)**,因为宿主 glibc 与载荷 glibc
在同一个地址空间里撞了。**正解就是 `compat.cuda-driver` 现在做的事**:
farm 出**单独一条** `libcuda.so.1` 符号链接,经 `runtime.library_dirs` 进搜索路径。

### 路线 D 的补充实测:farm 一条符号链接**不够**,闭包要连它的 `DT_NEEDED`

把 `libcuda.so.1` farm 进一个目录、写进 RUNPATH,产物闭包确实干净:

```
$ ldd tC6 | grep -c '/usr/lib\|/lib/x86_64-linux-gnu'
0
libc++.so.1 → xim-x-llvm/22.1.8/...      libc.so.6 → xim-x-glibc/2.39/...
```

**但驱动仍然 dlopen 不到。** 宿主的 `libcuda.so.1` 自己声明五个 `DT_NEEDED`:

```
libm.so.6  libc.so.6  libdl.so.2  libpthread.so.0  librt.so.1
```

私有 loader 必须能在**同一个闭包里**解析这五个 —— farm 一条链接只解决了
「找得到 libcuda」,没解决「libcuda 找得到它自己要的东西」。

`compat.cuda-driver` 之所以在真实工程里能用(`examples/09-cuda-kernel` 实测
输出 `12 24 36 48`),是因为 **mcpp 的链接模型把 glibc 载荷目录也放进了那条搜索
路径**。手搓 `clang -Wl,-rpath,...` 绕过了这个模型,于是缺了那一半。

**由此:判据 C0 必须经 mcpp 跑,不能手搓 clang。** 被测的对象包含 mcpp 的
链接模型本身;绕过它去测,测的是另一个东西。
(旁证:手搓产物的 `INTERP` 请求 glibc **2.39** 却解析到 **2.44** ——
一个 mcpp 不会产生的不一致。)

### 结论

| 路线 | 上界还在吗 | 代价 | 定位 |
|---|---|---|---|
| A `-allow-unsupported-compiler` | 关掉检查 | 可能真的编不过 | 最后手段 |
| B 岛形态 + xim 里挑 ccbin | **只约束设备 TU** | 缝要干净(本来就要) | gcc 工程 / 需要 `-gencode` 精细控制时 |
| C clang 编 CUDA | **消失**,换成 CUDA 版本上界 | clang 对新 CUDA 支持滞后 | **默认路线**(见 §6.3.5) |
| D 全载荷 | — | 无 | 上面三条的共同底座 |

⇒ **不是「混合编程受限」,是引擎不该替用户选。** 核心提供「设备 TU 可以有自己的
宿主编译器」这个通用能力(§6.1 C-1),**由哪个规则包按哪条路线去选,是插件的事。**

---

## 6.4.5 形态 B × modules 已实测:缝形态是**唯一**可行,而它统一了全部后端

这是方案里最后一格「不知道会怎样」。已测完(DPC++ 7.1.0 载荷 + 本机 RTX 4080)。

### 结果

| 形态 | 结果 |
|---|---|
| kernel 写在**模块接口单元**里,SYCL 头进 GMF | **DPC++ 的 driver 产不出 BMI** |
| **缝形态**:模块接口 SYCL-free,SYCL 在普通 TU | **跑通**,`seam: 11 22 33 44`,kernel 在 4080 上执行 |

第一行的判据取自**最简可能的模块**(`module;` + `#include <cstdio>` +
`export module tiny;`),它同样失败:

```
clang-offload-bundler: error: 'pcm': invalid file type specified
clang++: error: clang-offload-bundler command failed with exit code 1
```

⇒ **不是 C++ 语义冲突,是 DPC++ 的 driver 管线不认识 `.pcm`。** 这是上游的一个
洞,不是设计的选择;它会被上游修好,而在那之前形态 B 只能走缝。

### 由此得到的统一结论

> **缝形态不是 CUDA 的权宜,它是全部后端唯一都成立的那一个。**

- CUDA:已发布并实测(`examples/09-cuda-kernel`,输出 `12 24 36 48`);
- SYCL:**今天唯一能用的形态**(上表);
- HIP / Vulkan / Metal:同构 —— 设备代码进自己的 TU,经 `extern "C"` 与 C++ 相遇。

⇒ **一个工程形状覆盖全部后端。** 用户学一次,到处适用;
mcpp 侧也只需要支持一种形状,而不是每个后端一种。

### 缝的三条纪律,每条都是实测撞出来的

1. **缝必须 `extern "C"`。** 先用 C++ linkage 声明,链接期报
   `undefined reference to 'add_inplace@seam(...)'` —— 模块 linkage 把名字改了。
   与 `examples/09-cuda-kernel` 的纪律**逐字相同**,这不是巧合。
2. **必须显式指定设备目标。** 不指定 `-fsycl-targets` 时默认编 spir64 镜像,
   而本机只有 CUDA 后端,运行期报
   `No kernel named _ZTSZZ11add_inplace... was found`。
   ⇒ **`accel` 维要记录的正是这个**:产物为哪个 SYCL target 编的。
3. **PTX 版本受驱动限制** —— 与 §2.1 同一个失败模式的**第三次独立出现**:

   | SYCL TU 用哪个 CUDA 载荷编 | 运行 |
   |---|---|
   | CUDA 12.9 | `CUDA_ERROR_UNSUPPORTED_PTX_VERSION` |
   | CUDA 12.4(驱动上限) | `seam: 11 22 33 44` |

   三次(nvcc 产物、SYCL 产物、载荷选择)都是**编译链接全干净、运行期才说话**。
   这把 §2.1 从「一条 CUDA 的注意事项」升格为**跨后端的通用约束**:
   `accel` 维必须能表达「这个产物要求驱动至少多新」,而不只是「为哪个架构编的」。

### 一条前置条件:mcpp 的 LLVM 载荷缺 offload 工具

实测 `clang -x cuda -fgpu-rdc`(跨 TU `__device__` 调用)在 `xim:llvm@22.1.8` 上失败:

```
clang++: error: unable to execute command: posix_spawn failed: No such file or directory
clang++: error: llvm-offload-binary command failed with exit code 1
```

该载荷是 **slim 构建(36 个二进制)**,不含 `llvm-offload-binary` /
`clang-linker-wrapper` / `clang-offload-bundler`。
对照:**DPC++ 载荷全带**(它就是 LLVM 的一个发行)。
⇒ **「以 LLVM 为主」的前置条件是给 `xim:llvm` 补上 offload 工具**,
或发一个 `xim:llvm-offload` 伴生包。非 RDC 路径不受影响(已实测通过)。

---

## 6.4.6 CUDA 还是不是「唯一的例外」

§6.3.5 的表里 CUDA 是唯一非 LLVM 的一格。综合本节与 §6.4 的实测,答案是:
**在构建层面它已经不是例外了**,理由有三条,每条都已实测:

1. **clang 原生编 CUDA**(`-x cuda`)—— 此时 CUDA 源码就是 clang 的一种输入语言,
   与 C++/SYCL/HIP 走同一个前端;
2. **SYCL 能编到 CUDA**(`-fsycl-targets=nvptx64-nvidia-cuda`)—— 本节实测,
   kernel 真的跑在 4080 上。**LLVM 工具链直达 NVIDIA 硬件**;
3. **chipStar 把 CUDA 编到 SPIR-V**,同样是 LLVM 路径。

⇒ **nvcc 从「CUDA 的定义」降级为「CUDA 的一个后端」。** 它仍然必要,但只在三处:

| 还需要 nvcc 的场合 | 为什么 |
|---|---|
| 最新的 CUDA 版本 | clang 落后(实测 clang 22 部分支持到 12.9) |
| `-gencode` 的精细控制 | `sm_XXf` 家族目标等 clang 未覆盖的拼法 |
| RDC / device link | 在 `xim:llvm` 补齐 offload 工具之前 |

**对用户的意义:项目形状、依赖声明、feature、诊断在所有后端上是同一套;
换后端只换 `[features]` 与载荷,不换开发方式。** 这就是「统一的构建与开发体验」的
具体含义 —— 不是把 nvcc 藏起来,是让它不再决定工程长什么样。

**兜底也写明**:若某个后端将来出现连缝形态都容纳不了的要求,
它由**自己的规则包**单独支持(§6.2),核心不为它变形。**单独支持是有的,
但它是插件的单独,不是架构的分叉。**

---

## 6.5 多设备后端复用既有通用机制,不新增概念

用户体验的关键不是新语法,是**新东西能不能落在已经存在的格子里**。逐项对照:

| 需求 | 复用哪个既有机制 | 是否新增概念 |
|---|---|---|
| 选后端 | **`[features]`** —— `--features cuda` / `vulkan` / `rocm` | 否 |
| 按后端换依赖 | **`[target.'cfg(accelerator="…")'.dependencies]`** | 否(`accelerator` 已是 layer 键) |
| 按后端换源码 | **`sources` 的逐 glob 约束** | 否(glob 已有,加一个键) |
| 调设备编译器 | **规则包 + `mcpp::action` 边** | 否(`examples/09-cuda-kernel` 已在用) |
| 装工具链 | **xim 载荷 + `xpm.<platform>.deps`** | 否 |
| 跑/调试/分析 | **`mcpp run` 的 runner 机制** —— `--profile nsys`、`--check sanitizer` | 否 |
| 产物选变体 | **`pack::AbiTag` 的 `accel` 维 + `tag_check`** | 否(已落地) |
| 声明能力独占 | `provides` + 新增 `exclusive` | **一个布尔键** |

**整套多设备支持只新增了两样东西**:`accel` 这一个身份维(已落地),
和 `exclusive` 这一个布尔键。其余全部是既有机制的取值扩展。

这是本方案对「简洁」的操作性定义:**不是少写字,是不让用户学第二套概念。**
一个已经会用 `--features` 的人,切后端时不需要学任何新东西。

### 6.5.1 feature 与 accel 的关系要说清

它们**不是同一件事**,混淆会出错:

- **feature 是意图**:「我要 CUDA 支持」—— 消费者写的,可组合,可传播。
- **`accel` 是事实**:「这个产物是为 sm_89 编的」—— 构建产生的,进身份,被比较。

```toml
[features]
cuda = { accel = "cuda12.9+{sm_80,sm_89}" }   # feature 决定 accel,不是等于它
```

一个 feature 可以决定 `accel`,但 `accel` 也可以由 `--accel` 直接给,
或由 `[build] accel` 写死。**三个入口一个出口**,与 `--target` / `[toolchain]`
的关系同形。

---

## 7. 工业级生态覆盖

**不选「小而完整」的样例包。** 判据是:**工业上真的有人用它出货**,
并且它的构建复杂到能把本方案的每一处都压出来。分五档,每档回答一个不同的问题。

### 7.A 推理运行时 —— 「能不能承载真实部署」

| 框架 | 版本 | 为什么是它 | 压出什么 |
|---|---|---|---|
| **ONNX Runtime** | 最新 release | 工业推理事实标准;CUDA / TensorRT / oneDNN / OpenVINO / DirectML / CoreML 六个 EP | **多 provider 共存** ⇒ 直接压 §6.4 的 `exclusive`;跨平台 EP ⇒ 压 `cfg(accelerator=)` |
| **llama.cpp / ggml** | llama.cpp `v0.4.0` / ggml `v0.23.0`(均 2026-09-04) | 部署量最大的本地推理栈;**一个代码库里就有 CUDA/Vulkan/SYCL/HIP/Metal/CPU 全部后端** | 整张设备矩阵;变体分发 |
| **libtorch** | 上游 release | C++ 侧工业消费 PyTorch 的实际形态 | **载荷消费**:cuDNN/NCCL/cuBLAS 闭包、私有 loader 下的 RPATH |

索引里现有的 `ggml-org.llamacpp` pin 在 `b10069`,`sources` 里**只有 `ggml-cpu/*`** ——
CUDA/Vulkan/SYCL/HIP 一个都没有。补齐它就是把矩阵走一遍。

**PyTorch 全源码构建不在第一批**,理由不是难,是**顺序**:它依赖 cuDNN/NCCL/
oneDNN/oneMKL 全部就位。第一批做 `libtorch` 载荷消费,把闭包与 RPATH 这条路走通;
源码构建排在算子库档之后。

### 7.B 算子库与 kernel 编写 —— 「能不能生产算子」

| 框架 | 版本 | 压出什么 |
|---|---|---|
| **CUTLASS** | `4.8.0dev`(2026-08-27) | 形态 A 的极限:海量 `.cu` TU、极重模板、**宿主编译器上界**极敏感、`sm_90a` 这类 arch-specific 目标 |
| **oneDNN** | `v3.13.2`(2026-08-26) | PyTorch/TF 底下的那层;CPU + SYCL GPU ⇒ 形态 B 的工业验证 |
| **cuDNN / NCCL / cuBLAS / rocBLAS / oneMKL** | 各自上游 | 纯载荷:不构建,但必须**可被消费**且闭包成立 |

### 7.C HPC 可移植性 —— 「一份源码能不能跑遍设备」

| 框架 | 版本 | 压出什么 |
|---|---|---|
| **Kokkos** | `5.2.1`(2026-08-17) | DOE 级实际用量;CUDA/HIP/SYCL/OpenMP 由构建期选 ⇒ 压 `cfg(accelerator=)` 的成员判定在真实项目里够不够用 |

### 7.D 视觉与检索 —— 「工业 pipeline 的两端」

| 框架 | 版本 | 压出什么 |
|---|---|---|
| **OpenCV** | `5.0.0`(2026-06-06) | 索引已有 `opencv.opencv`;加 CUDA 模块 ⇒ **含设备代码的静态库**(§6.6) |
| **FAISS** | 上游最新 | 工业向量检索;CUDA kernel + 大量模板 |

### 7.E Vulkan 计算的独立见证

| 框架 | 版本 | 压出什么 |
|---|---|---|
| **ncnn** | `20260526` | GPU 路径就是 Vulkan compute,不依赖任何厂商工具链 |

它与 llama.cpp 的 Vulkan 后端**互为独立见证**:两个不相干的工业项目在
**lavapipe** 上都跑出正确结果,那条无卡 lane 才算真的 —— 单个项目可能碰巧绕开了。

### 7.F 这一节的判据不是「编过了」

每个框架的判据都必须是**它自己的正确性判据**,不是构建成功:
ONNX Runtime 跑它自己的 model test、llama.cpp 推理出正确 token、CUTLASS 的 GEMM
与参考实现比对、oneDNN 跑它自己的 benchdnn、Kokkos 跑它自己的 unit test、
OpenCV 跑它自己的 accuracy test、ncnn 的分类结果与 CPU 后端一致。

**上游自带的测试套件是唯一不会被我写偏的判据。**

---

## 7.5 架构横评:与 CMake / xmake / Cargo / Zig / Bazel / Spack / Conan

本节的立场:**逐条给出可核对的事实,而不是宣称领先。** 落后的地方写在同一张表里。

### 7.5.1 核心差异:产物身份里有没有加速器维

这是本方案的中心命题,也是最容易被含糊过去的一条。

| 系统 | 「为哪个设备架构编的」记在哪里 | 消费时会不会被检查 |
|---|---|---|
| **CMake** | `CMAKE_CUDA_ARCHITECTURES` —— **一个构建变量** | 产出的 `.a`/`.so` 不携带它;消费方无从得知 |
| **xmake** | `add_cugencodes()` —— 构建配置 | 同上 |
| **Cargo** | 无。GPU 全在 `build.rs` 里 shell 出去 | target triple 不编码 GPU 架构;feature 是集合不是兼容维 |
| **Zig** | 无。`-target` 不含 GPU 架构 | no |
| **Bazel** | `--config=cuda` + `cuda_archs` | 不进 artifact 身份 |
| **Conan** | `settings` 参与 `package_id` —— **机制在,维度不在** | ConanCenter #11448 就是在要这个维度,未合入 |
| **Spack** | `+cuda cuda_arch=90` 是**一等 variant**,进 hash | **现有系统里最接近的** |
| **Python wheels** | PEP 817/825 Wheel Variants(`namespace::feature::value`) | 正在加,说明这是全行业公认的缺口 |
| **mcpp** | `pack::AbiTag::accel`,与 triple/compiler/stdlib/standard 并列 | `tag_check` 在选变体时比较,**拒绝时点名维度与两侧取值** |

诚实的结论:**这不是 mcpp 独有的想法** —— Spack 早就有,wheels 正在加。
mcpp 的差异在于**它是一个编译器驱动的构建系统**,所以那个维度既进包身份,
**也能在编译前就用它拒绝**;Spack 只在求解依赖图时用它,不驱动编译。

### 7.5.2 逐维度对比

| 维度 | mcpp | CMake | xmake | Cargo | Zig | Bazel |
|---|---|---|---|---|---|---|
| C++20 modules 一等公民 | 自举即模块 | 2023 起,部分 | 部分 | — | — | 实验 |
| 包管理内置 | yes | 需 vcpkg/conan | xrepo | yes | 新 | 需 rules_* |
| 工具链本身是载荷(hermetic) | xim 装 gcc/llvm/glibc | 用 host | 部分 | rustup | 自带 libc | 最强 |
| 产物身份含加速器维 | yes | no | | no | | no |
| 构建程序是真语言 | `build.mcpp` 是 C++ | CMake DSL | Lua | `build.rs` | Zig | Starlark |
| 交叉编译广度 | 追赶中 | 靠工具链文件 | partial | partial | **最强** | partial |
| 生态广度 | **远落后** | **压倒性** | partial | yes | partial | partial |
| 大规模远程缓存 | no | partial | partial | partial | no | **最强** |

### 7.5.3 mcpp 明确落后的三处,以及本方案对它们做了什么

1. **生态广度。** CMake 拥有一切。⇒ 本方案 §7 用**九个工业级框架**换取覆盖面,
   而不是用数量换;判据是这些框架**自己的测试套件**。
2. **交叉编译广度不如 Zig。** Zig 自带多目标 libc。⇒ mcpp 的路线是 xim 载荷
   (已有 aarch64/musl/mingw/picolibc 多条),方向相同、覆盖面待补,不在本方案范围。
3. **远程缓存不如 Bazel。** ⇒ 不在本方案范围,但 §6 的 `accel` 维恰好是远程缓存
   key 的必要成分 —— 没有它,跨机器复用缓存会取到为别的架构编的产物。

### 7.5.4 语义清晰度:一处可以直接对比的例子

同一件事,在 CMake 与 mcpp 里的表达:

```cmake
# CMake:三个互不相关的机制,各自回答问题的一部分
set(CMAKE_CUDA_ARCHITECTURES 80;90)     # 编给谁
find_package(CUDAToolkit REQUIRED)       # 从 host 找
target_link_libraries(app CUDA::cublas)  # 链什么
# 「产物为谁而编」不在任何一处被记录,消费者拿到 .a 无从得知
```

```toml
# mcpp:一处声明,进产物身份,消费时被比较
[build]
accel = "cuda12.9+{sm_80,sm_90f} ptx>=90"

[dependencies]
cublas = "12.9.x"        # 载荷来自 xim,不是 host
```

差别不在语法长短,在**那句声明去了哪里**:CMake 的进了构建目录,
mcpp 的进了产物身份,并在下一个消费者构建时被 `tag_check` 读出来。

## 7.9 场景示例(参考形态,不是最终措辞)

以下是**参考示例**,用来把前面的抽象落到用户真正会敲的东西上。
措辞会随实现调整;不变的是每个场景**只需要哪些概念**。

### 场景 1 —— 纯 C++,没有设备代码

```toml
[package]
name = "app"
version = "0.1.0"
[language]
standard = "c++23"
```

**一个字都不用改,零代价。** `.cu` 不进默认 glob,`accel` 维为空表示不受约束,
`cfg(accelerator=…)` 不匹配任何东西。这是本方案对绝大多数工程的影响面:没有。

### 场景 2 —— C++ + 设备代码(形态 A:岛)

设备代码住在自己的 TU 里,只经 `extern "C"` 头与 C++ 相遇;C++ 侧照常是模块。

```
app/
  src/app.cppm            # 模块接口,import 的人只看见这个
  src/kernels/saxpy.cu    # 设备 TU,永不被扫描 import,不产 BMI
  include/saxpy/saxpy.h   # 缝:extern "C",不含任何 CUDA 类型
```

```toml
[package]
name = "app"
version = "0.1.0"
[language]
standard = "c++23"

[build]
accel = "cuda12.9+{sm_80,sm_89} ptx>=80"

[build-dependencies]
rules-cuda = { version = "0.1.0", host-module = true }

[dependencies]
cuda-driver = { namespace = "compat", version = "2026.09.05" }
```

```cpp
// build.mcpp —— 构建程序是真的 C++
import std;
import mcpp;
import rules_cuda;

int main() {
    return rules_cuda::compile(mcpp::sources("src/kernels/*.cu")) ? 0 : 1;
}
```

```cpp
// src/app.cppm —— 缝模块:头文件只在这里出现一次
module;
#include "saxpy/saxpy.h"
export module app;
import std;

export namespace app {
    std::vector<float> scale(std::vector<float> x, float a) {
        mcpp_saxpy(static_cast<int>(x.size()), a, x.data());
        return x;
    }
}
```

**消费者 `import app;` 即可,永远看不见 CUDA。** 这就是「把设备代码与 C++ 分开
以支持模块」那个问题的答案:不是不用头文件,是**头文件只出现在缝的内侧一次**。

### 场景 3 —— 整目标形态(形态 B:SYCL)

没有单独的设备 TU;整个目标由 SYCL 编译器编,kernel 就写在 C++ 里。

```toml
[toolchain]
compiler = "dpcpp@7.1.0"          # 载荷来自 xim,不是 host

[build]
accel = "sycl:spir64+{cpu,cuda:sm_89}"

[build-dependencies]
rules-sycl = { version = "0.1.0", host-module = true }
```

```cpp
export module vecadd;
import std;
#include <sycl/sycl.hpp>          // 形态 B 里它在 GMF

export std::vector<float> add(std::vector<float> a, std::vector<float> b) {
    sycl::queue q;
    // ... kernel 直接写在这里,同一个 TU
}
```

**已实测(§6.4.5):kernel 不能写在模块接口单元里** —— DPC++ 的 driver 产不出
BMI。形态 B 今天只能走**缝形态**:模块接口 SYCL-free,SYCL 代码在普通 TU 里,
经 `extern "C"` 相遇。上面这个写法要等上游修好 `clang-offload-bundler`。

### 场景 4 —— 一份源码,多后端(消费者选)

```toml
[features]
default = ["cpu"]
cpu    = []
cuda   = { accel = "cuda12.9+{sm_80,sm_89}" }
vulkan = { accel = "vulkan1.3+{spirv1.6}" }
rocm   = { accel = "hip6.4+{gfx1100}" }

[target.'cfg(accelerator="cuda")'.dependencies]
cublas = "13.5.1"

[target.'cfg(accelerator="vulkan")'.dependencies]
vulkan = { namespace = "compat", version = "1.4" }
```

```console
$ mcpp build --features cuda
$ mcpp build --features vulkan
$ mcpp build --no-accel            # 显式要 CPU-only 变体
```

`cfg(accelerator=…)` **处处是成员判定** ⇒ 同时启用两个后端时
`all(accelerator="cuda", accelerator="vulkan")` 表示「两者都启用」,不是不可满足。

### 场景 5 —— 消费预编译的加速库

```toml
[dependencies]
cublas    = "13.5.1"                                  # 载荷在 xim,适配在 mcpp-index
llamacpp  = { namespace = "ggml-org", version = "b10069", features = ["cuda"] }
```

构建时若本地 `accel` 与产物的 `accel` 不相容:

```
error: no prebuilt artifact of `llamacpp` matches this build
       dimension `accel`: the artifact provides cuda12.9+{sm_90f}
                          this build requests cuda12.9+{sm_86}
       fix: build from source, or `--no-accel` to take the CPU-only variant
```

消息点名**维度**、**两侧取值**、**该轴可行的补救** —— 三者缺一,用户就得猜。

### 场景 6 —— 无卡机器上开发与 CI

```console
$ xlings install mesa-lavapipe pocl        # 模拟器也是载荷
$ mcpp test --features vulkan              # kernel 在 CPU 上真跑
```

```toml
# 在 CI 上把设备固定成软件实现,判据仍是「结果正确」
[target.'cfg(accelerator="vulkan")'.env]
VK_ICD_FILENAMES = "${xim:mesa-lavapipe}/share/vulkan/icd.d/lvp_icd.json"
```

**模拟器 lane 的结果必须与硬件 lane 对照过**(§9 C4/C5),否则等于把
「lavapipe 说对了」当成「它是对的」。

### 场景 7 —— 发布多变体

```console
$ mcpp pack --accel 'cuda12.9+{sm_80,sm_89}'
$ mcpp pack --accel 'vulkan1.3+{spirv1.6}'
$ mcpp pack --no-accel                     # CPU-only,必须排第一
```

**CPU-only 变体在描述符里排第一。** 消费方是首次匹配即用的循环,
而不认识 `accel` 键的旧 mcpp 会跳过它 —— 排第一保证它拿到的是能跑的那个。

### 场景 8 —— 混合:C++ 模块 + 设备代码 + 第三方算子库

这是工业项目的实际形状,三样同时出现:

```toml
[package]
name = "infer"
version = "0.1.0"
[language]
standard = "c++23"

[build]
accel = "cuda12.9+{sm_89}"
sources = [
  "src/**/*.cppm",                                   # C++ 模块
  { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" },   # 设备 TU,逐 glob 收窄
]

[build-dependencies]
rules-cuda = { version = "0.1.0", host-module = true }

[dependencies]
cublas   = "13.5.1"                                  # 厂商算子库
cutlass  = { namespace = "nvidia", version = "4.8.0" }
llamacpp = { namespace = "ggml-org", version = "b10069", features = ["cuda"] }
```

```console
$ mcpp build
$ mcpp run --profile nsys          # 走 runner,不新增专用命令
$ mcpp test --check sanitizer      # compute-sanitizer 也是 xim 载荷
```

注意这里**没有一行提到 host**:nvcc、cudart、cuBLAS、CUTLASS、nsys、
compute-sanitizer 全部来自 xim;host 参与的只有 `libcuda.so.1`。

### 场景 9 —— 诊断

```console
$ mcpp self doctor
    Checking device toolkit
warning: cuda will refuse this host compiler: gcc 16 exceeds the bound of 15
         stated in <payload>/include/crt/host_config.h.
warning: this payload requires driver CUDA >= 13.0; this host reports 12.4.
         Device code will build and link, then fail at the first allocation
         with "CUDA driver version is insufficient". Install the 12.9 line, or
         update the driver.
```

第二条正是 §2.1 实测到的那次失败 —— **把一个只在运行期才说话的错误,
搬到构建之前**。

---

## 8. 跨仓库顺序

顺序是硬的:**消费者先发布,索引 `latest` 才能动**。

```
⓪  立即,无依赖 ── 修已发布的错误示范(§0.1)
     ⓪a 从全部用户文档删除 `[xlings] deps`(英文 14 处 / 中文 10 处),
         `[xlings.workspace]` 是唯一形式
     ⓪b examples/09-cuda-kernel 改用 `[xlings.workspace]`
     ⓪c 该示例去 host 化要等 ①a 的载荷 —— 但**文档与字段先改**

①  xim-pkgindex ── 载荷,三批,批内并行
     ①a 编译必需   cuda-nvcc / cudart / nvrtc / nvjitlink / cccl   (150 MB)
     ①b 设备与模拟器 pocl / mesa-lavapipe / chipstar / spirv-tools / glslang
                     dpcpp@7.1.0 / adaptivecpp@25.10.0 / hip-runtime@7.14.0
     ①c 算子库与工具 libcublas(+static 分包) / libcufft / cudnn / nccl
                     cuda-gdb / cuda-sanitizer / nsight-systems / nsight-compute
②  mcpp ── 引擎,有序
     6.1 设备目标 → 6.2 device link → 6.3 glob 收窄 → 6.4 exclusive
     → 6.5 驱动配对 → 6.6 含设备代码的静态库 → 6.7 accel 维扩展
③  mcpp 发布 ── 一个版本,消费者才能用新键
④  mcpp-index ── 适配面(依赖 ③)
     compat.cuda-driver 改名 + repo 改正;compat.vulkan-icd / opencl-icd;
     rules-cuda / rules-hip / rules-spirv;compat.cublas / cudnn / nccl / onemkl
⑤  框架 ── 依赖 ④,按 §7 的档次
     7.A llamacpp 多后端 → 7.E ncnn → 7.B CUTLASS + oneDNN
     → 7.C Kokkos → 7.D OpenCV + FAISS → 7.A ONNX Runtime → 7.A libtorch
```

① 与 ② **完全并行** —— 载荷不依赖引擎的新键。
⑤ 内部也有序:llamacpp 先做,因为它一个项目就覆盖矩阵,能最早暴露设计问题。

---

## 9. 判据表

判据取**只有做对了才会出现的读数**;框架档一律用**上游自带的测试套件**。

| # | 判据 | lane |
|---|---|---|
| C0 | **默认路线**:`xim:llvm@22.1.8` + `xim:cuda-*@12.9.x`,`clang -x cuda` 编出 `sm_89` 并在 4080 上跑出正确结果,**全程零 host 路径**(`ldd` 里不出现 `/usr/lib`) | 本机硬件 |
| C0b | 同一工程切到 nvcc 路线(`rules-cuda` 选 `-ccbin` 为 `xim:gcc@15.1.0`)结果一致 ⇒ **翻转没有废掉 gcc 路线** | 本机硬件 |
| C1 | `xlings install cuda-nvcc@12.9.86` 后 nvcc 可用,且 `mcpp self doctor` **不报** `cicc: not found` —— 全程不碰 host toolkit | 沙箱 |
| C2 | 驱动只到 12.4 的机器上请求 13.x 载荷 ⇒ **构建前拒绝**,消息含两边版本号与该轴补救 | 本机(驱动 550) |
| C3 | `mcpp run --profile nsys` 产出 report;`--check sanitizer` 报出一处故意写的越界 | 本机硬件 |
| C4 | llama.cpp Vulkan 后端在 **lavapipe** 上推理出正确 token | 无卡 |
| C5 | ncnn 在 lavapipe 上分类结果与 CPU 后端一致 | 无卡 |
| C6 | CUTLASS 的 GEMM 与参考实现比对通过 | 本机 4080 |
| C7 | 同一 Kokkos 源码 `--accel cuda…` / `--no-accel` 两份产物**跑它自己的 unit test 都通过**,而 `accel` 不同 | 两条 |
| C8 | 两个声明 `exclusive` 且同名能力的包同时在图里 ⇒ **绑定期报错**,不是链接期重复符号 | 无需设备 |
| C9 | 跨 TU `__device__` 调用链接成功;改 `device-link` 触发重新 prepare | 本机硬件 |
| C10 | llama.cpp 发多变体后,**旧 mcpp 取到 CPU-only 那个**并正常运行 | 无需设备 |
| C11 | oneDNN 的 `benchdnn` 在 DPC++ CPU device 与 CUDA 后端两条 lane 上都通过 | 两条 |
| C18 | 缝形态在**全部**后端上是同一个工程形状:CUDA 与 SYCL 两个工程的 `mcpp.toml` 除 `[features]` 与载荷外**逐行相同** | 无需设备 |
| C19 | 不指定设备目标时 mcpp **在构建期**报出「产物没有为任何可用设备编」,而不是留到运行期的 `No kernel named … was found` | 本机硬件 |
| C20 | `accel` 维能表达「要求驱动至少多新」,且 PTX 版本高于驱动时**构建前拒绝**(§6.4.5 纪律 3) | 本机硬件 |
| C12 | ONNX Runtime 同时启用两个 EP ⇒ 由 `exclusive` 决定报错还是共存,**与它自己的文档一致** | 无卡 + 硬件 |
| C13 | OpenCV 含 CUDA 模块的 `.a` 被消费时,`accel` 维随包传播并被 `tag_check` 读出 | 无需设备 |
| C14 | libtorch 载荷消费后,`mcpp` 的运行期闭包校验通过(cuDNN/NCCL/cuBLAS 全部可达,且**不经 host `/usr/lib`**) | 本机硬件 |
| C15 | **核心里 grep 不到任何厂商名字**(`nvcc`/`cuda`/`hip`/`sycl`/`gfx`/`sm_`),它们只出现在规则包里 | 无需设备 |
| C16 | 卸掉 `rules-cuda` 后,`mcpp self doctor` 的设备一节**安静地不报**,而不是报错或崩 | 无需设备 |
| C17 | 只装 `cuda-nvcc` 不装 `libnvvm` ⇒ doctor 报出 `cicc` 并**指名要装哪个 xim 包** | 沙箱 |

C2 与 C10 是「答错比不答更坏」的两条:一个放行了跑不了的载荷,一个让旧客户端
拿到跑不了的产物。**它们要最先有测试。**

C4 与 C5 必须**各自在 4080 上再跑一次**:模拟器 lane 的意义在于它与硬件
lane 结论一致;只跑模拟器,等于把「lavapipe 说对了」当成「它是对的」。

---

## 10. 风险

| # | 风险 | 处置 |
|---|---|---|
| RK-a | ~~`libcublas` 775 MB 过大~~ | **作废。** 已量:shared 是 `libcublas.so` 102.5 MB + `libcublasLt.so` 484.2 MB,static 单独分包。**体积从来不是理由** |
| RK-b | ~~DPC++ release 可能不含 NVIDIA 后端~~ | **作废。** 已核:v7.1.0 Linux 构建配置为 `--cuda --hip`,release notes 测试矩阵含 NVIDIA CUDA BACKEND。Windows 不含 ⇒ 自建,版本仍为 `7.1.0` |
| RK-c | 模拟器与真硬件行为不同(浮点、竞态) | **每条模拟器 lane 必须有一次真硬件对照**(C4/C5) |
| RK-d | lavapipe 慢 | 用最小模型;判据是「结果正确」不是「快」 |
| RK-e | 载荷总体积 | 组件级拆包:普通构建 150 MB,profile 才装 nsight,static 才装 `-static` 包 |
| RK-f | chipStar 覆盖的 CUDA 子集有限 | 它只作**无卡环境的 kernel 行为**见证;`-gencode`/PTX/fatbin 仍走真卡 |
| RK-g | **框架档工作量远大于引擎档** | 顺序上 llamacpp 先行;它若暴露设计问题,后面七个都会受影响 ⇒ 它是 gate 不是第一个任务 |
| RK-h | ONNX Runtime / libtorch 依赖 cuDNN、NCCL,二者许可与 CUDA Toolkit 不同 | 逐个读它们自己的 manifest/EULA,**按 §3.0 的方法由许可字段决定**,不由判断决定 |
| RK-i | ~~形态 B × modules 未实测~~ | **已测,见 §6.4.5。** kernel 进模块接口单元 (DPC++ driver 产不出 BMI);缝形态 (4080 上跑出 `seam: 11 22 33 44`)。⇒ 缝形态是全部后端唯一都成立的形状 |
| RK-l | **`xim:llvm@22.1.8` 是 slim 构建,缺 offload 工具** ⇒ `-fgpu-rdc` 不可用 | 「以 LLVM 为主」的前置条件:补 offload 工具或发 `xim:llvm-offload` 伴生包。非 RDC 路径不受影响 |
| RK-k | **主路线翻转到 clang 的代价是 CUDA 版本滞后** —— 实测 clang 22 部分支持到 12.9、clang 18 只到 12.3 | 两条路线共存,由 `rules-cuda` 按「clang 版本 × CUDA 版本」可用配对选;需要最新 CUDA 特性的走 nvcc。判据 C0b 保证 gcc/nvcc 路线不被废掉 |
| RK-j | **把 CUDA 探针从核心搬进规则包会动已发布的行为** | `mcpp self doctor` 的两条消息是已发布的契约。迁移期核心保留一份回落,规则包在场时以规则包为准;两条路径的消息逐字相同 |

---

## 11. 与前文的关系

前文 `2026-09-05-accelerator-support-design.md` 的 §1-§7、§9-§11、§13-§15 仍然成立。
本文取代的是:

- **§8.5** 的三档表 —— 第二档补齐(lavapipe 已实测、PoCL/chipStar 已核),
  第三档的「CUDA 只能上真卡」删除,代之以 §5 的矩阵;
- **§12** 的阶段表 —— 「未做」的理由全部作废,代之以 §5 的 lane 与 §9 的判据;
- **§15.8** —— 两条「缺授权」的结论作废,代之以 §4.2 与 §3.1。

---

## 12. 实施后自我 review(2026-09-05)

方案写完之后由实施推翻或补上的地方。**只记与本文所写不同的**;相符的部分见
实施计划表的判据列。

### 12.1 被实施推翻的

| 本文写的 | 实测 | 处置 |
|---|---|---|
| C-1「设备目标原语」是新的 `[[target]] kind = "device"` | `mcpp::action` 已有四种角色,artifact 的产物不进链接、object 的进链接,且 ninja 按路径连边 —— 「不参与常规链接、被某条边消费的产物」**就是** artifact 角色 | 不新增 target kind。再加一种是同一个决定写第二遍 |
| C-5 的驱动取数由核心完成 | 仓库自带的 `test_runtime_contract` 禁止「厂商词 + 探针启动」在 `src/` 共现,抓住了写下的 `nvidia-smi` 调用 | 关系留在核心并单测,**取数改由声明抵达**;`doctor` 的整节与 `mcpp.toolchain.devicehost` 一并删除 |
| C9(RDC 真机)在本机可测 | nvcc 路线**两端同时被挡**:12.9 满足驱动而不满足 C 库(C23 `cospi`/`rsqrt` 的 `noexcept` 冲突),13.3 满足 C 库而不满足驱动(要 ≥13.0,本机 12.4) | 判据退回 e2e 607 所测的**通用链式 action**;真机 RDC 留给有 13.x 驱动的机器,并在示例 README 里写明为什么 |
| ① 已完成 | 13.x 的 `cuda-nvcc` **装完不能用**:nvcc 用 `$(TOP)/nvvm/bin/cicc` 找后端,而 13.x 把 `nvvm/` 与 `crt/` 拆成了独立包=独立载荷根 | xim #760。修法的形状:**无条件写链接**,不要求嵌套安装成功 —— 它失败无声且两种拼法都不可靠 |
| T1.5「lavapipe 载荷」是新增包 | 现有 `xim:mesa` 载荷里**只有 RADV**,没有 `libvulkan_lvp.so`,也没有 rusticl | 仍待做,且不是新增包而是**重打 mesa 载荷**(`-Dvulkan-drivers=…,swrast` + `-Dgallium-rusticl=true`)。T1.4 同理 |

### 12.2 实施自己造出来又修掉的

- **「没有加速器」被写成了显示用的 `(none)`。** `accel_str` 为空集打印 `(none)`
  是给 ABI 标签读的;`resolvedAccel` 把这个拼法当值传了出去,于是
  `MCPP_ACCEL=(none)` 到达**每一个从未提过加速器的工程**,而指纹里
  `if (!accel.empty())` 恒真。e2e 605 第四段标题写着「变量与 layer 都清空」
  却只测了 layer —— 这就是它逃过套件的原因。判据只能靠构建程序**写文件**取得:
  它的 stdout 只在非零退出时才打印。
- **设备源的映射按裸包名索引。** 同一张图里两个包可以同名不同命名空间;
  改按包根索引。合入前重读 diff 时发现,没有测试覆盖它。

### 12.3 生态级 review:这一轮之后,一个消费者看到的是什么

一个要用 GPU 的工程现在写三样东西,各自答给不同的所有者:

```toml
[xlings.workspace]                  # 载荷:工程自己选版本
"xim:cuda-nvcc"   = "12.9.86"

[dependencies.compat]               # 机器:驱动由机器决定,包只负责够到它
cuda-driver = "2026.09.05"
cublas      = "12.9.1.4"            # 算子库:载荷 + 构建面,两个仓库各管一半

[build]                             # 轴:写一次,规则包据此推导自己的开关
accel = "cuda12.9+{sm_89} ptx>=89"
```

**核心不认识其中任何一个厂商名字**,这条由 `test_core_vendor_probes` 在剥掉
注释的源码上执行,并自带分母。

三条已被生态执行的规则在本轮各验证一次:
GPU 索引包不自己探测宿主(委托 xim sentinel);链宿主 `libcudart` 会被闭包
校验拒掉(所以 `compat.cudart` farm 的是载荷而不是宿主);打包后的
`runtime.artifacts` 是封闭白名单(新增产物字段两个读取器都要查)。

### 12.4 仍然没有做的,以及理由

| 项 | 理由 |
|---|---|
| T1.2 `llvm-offload` | `dpcpp@7.1.0` 载荷自带全套 offload 工具,需要 RDC 的工程可用它;独立包仍待做 |
| T1.4 / T1.5(pocl / lavapipe)与 T4.2 | 需要**重打 mesa 载荷**(见 12.1 末行),或新建 pocl 源码构建配方。两者都是多小时的载荷工程 |
| T1.8/T1.9/T1.10(chipstar / adaptivecpp / hip) | 依赖 T1.2/T1.4 |
| T4.3 规则包进索引 | 依赖 ③ —— 描述符指向 mcpp 的**源码 tarball**(`grpcgen` 同形),tag 不存在则算不出 sha256。规则包已改名到 `mcpplibs` 命名空间,就是为了让它可被引用而不是被复制 |
| ⑤ 九个框架 | 依赖 ④ 的规则包条目。`ggml-org.llamacpp` 与 `opencv.opencv` 已在索引里,多后端是改**它们各自的 `-m` 仓库**而不是索引条目。T5.1 已做到「链路全通、卡在载荷矩阵」—— 见 12.5 |
| T2.6 的端到端判据 | `accel` 已是 `pack::AbiTag` 第四维并进指纹;「`.a` 随包传播」还缺一条跨包的判据 |

### 12.5 T5.1 作为 gate 的实际读数

**它兑现了 gate 的作用**:第一个真实框架就暴露了 C-6 的引擎缺口 —— object 角色的
action **只**挂到可执行/共享库/测试上,而 llama.cpp 的 CUDA 后端是 305 个 `.cu`
挂在 `kind = "lib"` 上,于是每个 action 都被丢弃、只留一条警告,**构建成功**并产出
一个不含设备码的归档。修好并有判据(e2e 608 断言 `ar t` 的成员表 —— 空档案也会
成功退出)。

**链路本身全通,实测到 48 个设备目标**:`[build] accel` → 带 `accel` 的 glob →
`MCPP_DEVICE_SOURCES` → 规则包 → `mcpp::action` → 归档 → 链接。

**挡住的是一个四维载荷矩阵,四条边没有一条是 mcpp 的:**

| 组合 | 读数 |
|---|---|
| CCCL 2.x(12.9 线)+ clang | `cub::LoadDirectWarpStriped` 少一个四参重载 |
| CCCL 3.3(13.3 线)+ clang | 同一个调用,候选是三参与五参 |
| CCCL 3.2(13.2 线)+ clang | 换成 **libcu++ 编不动**:`string_view` 的推导指引只允许 `__host__ __device__`;`block_load.cuh` 要 placement new |
| 任一 CCCL + nvcc | 12.9 撞 glibc 2.44 的 C23 `cospi`;13.3 撞驱动 12.4 |

**这不是「没做完」,是「本机构造上无解」**,与 §12.1 里 C9 那条同一性质。
需要的是一台驱动 ≥ 13.0 的机器(nvcc 13.3 路线),或一个 ggml 与 CCCL 版本匹配的
上游 checkpoint。

顺带三条通用读数,都写进了规则包与文档:
**layer 不能选择依赖**(依赖挂 feature,源文件挂 accel 轴);
**设备编译必须指名 CCCL 载荷**否则命中 `/usr/include/cub`(与 §12.1 的
`cuda_runtime.h` 同一形状,第三次);
**clang 路线要带 `-D_ALLOW_UNSUPPORTED_LIBCPP`**,因为 NVIDIA 那条 `libc++ is not
supported` 的守卫看的是 `__CUDACC__`,而 clang 编 CUDA 时自己就定义它。
