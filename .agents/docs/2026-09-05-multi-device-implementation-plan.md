# 多设备生态:实施计划与任务表

对应方案 `2026-09-05-multi-device-ecosystem-design.md`。本文只管**做什么、谁依赖谁、
判据是什么、现在到哪一步**。方案改了本文跟着改,反之不成立。

## 0. 状态总览

| 批次 | 仓库 | 状态 |
|---|---|---|
| ⓪ 修已发布的错误示范 | mcpp | ✅ T0.1/T0.2 合入 `1e2137b`;T0.3 在 **#563** |
| ① 载荷 | xim-pkgindex | ✅ **#759 已合入 `a4644a7`**,15 项 CI 全绿,25 个包;⚠️ 13.x 后端不可达由 **#760** 修 |
| ② 引擎 | mcpp | ✅ T2.1–T2.10 全部完成并各有判据 |
| ③ 发布 | mcpp | ✅ **2026.9.5.2 已发布**;两端资产 GET 均 200 且字节数一致;xim bump **#761 已合入 `99554b2`**,`Publish Index Artifact` 绿;bootstrap pin 已前移 |
| ④ 适配面 | mcpp-index | 🟡 T4.1/T4.3/T4.4 ✅(**#347 `8a9ca64`**、**#348 `a607ff2`**);T4.2 待 ① 的 pocl/lavapipe |
| ⑤ 框架 | mcpp-index | 🟡 T5.1 走到「载荷版本不匹配」:见下方记录。它作为 gate 已经交付了它该交付的东西 —— 暴露出 C-6 的引擎缺口 |
| ⑥ 生态验证 | 沙箱 | ✅ 三份脚本全绿(见下);⚠️设备那一半沙箱做不到 —— `--sandbox` 的 `/dev` 只有 14 项且无 `/dev/nvidia*` |

图例:⬜ 未开始 / 🟡 进行中 / ✅ 完成并有判据 / ⛔ 阻塞

---

## 1. 任务表

### ⓪ 修已发布的错误示范(mcpp,无依赖,立刻做)

| # | 任务 | 判据 | 依赖 |
|---|---|---|---|
| T0.1 | 从全部用户文档删除 `[xlings] deps`,`[xlings.workspace]` 为唯一形式 | `git grep -c 'xlings\] deps' docs/` 为 0(中英双份) | — |
| T0.2 | `examples/09-cuda-kernel` 改用 `[xlings.workspace]` | 示例中不出现 `deps =` | T0.1 |
| T0.3 | ✅ 示例去 host 化 | ✅ `mcpp build -v` 的命令行里 `/usr` CUDA 路径 **0 处**;nvcc 与两处 include 全来自 `xpkgs/xim-x-cuda-*`;`mcpp run` → `12 24 36 48` | T1.1 |

### ① 载荷(xim-pkgindex,批内并行)

| # | 任务 | 版本 | 判据 | 依赖 |
|---|---|---|---|---|
| T1.1 | ✅ 24 个 CUDA 组件(编译/运行/调试/分析/算子库) | 12.9 线 + 13.3 线 | ✅ 载荷编 sm_89 并在 4080 上跑出 `12 24 36 48`;两线并存可切换 | — |
| T1.2 | ⬜ `llvm-offload` | 22.1.8 | ⚠️ 已有替代:`dpcpp@7.1.0` 载荷自带全套 offload 工具,需要 RDC 的工程可用它。独立 `llvm-offload` 仍待做 | — |
| T1.3 | ✅ `dpcpp` | 7.1.0 | ✅ `sycl-ls` 报 `[cuda:gpu] NVIDIA CUDA BACKEND` | — |
| T1.4 | `pocl` | 7.2 | `clinfo` 出现 CPU 设备 | — |
| T1.5 | `mesa-lavapipe` | 25.2.8+ | `vulkaninfo` 出现 `PHYSICAL_DEVICE_TYPE_CPU` | — |
| T1.6 | ✅ 随 T1.1 一并落地 | 13.3.x / 2026.x | 静态检查通过;运行判据待 ⑥ | — |
| T1.7 | ✅ `libcublas`+四个算子库(上游归档含 static,未再拆) | 13.5.1.27 | ⚠️ static 分包推迟:上游一个归档同时含 shared 与 static,拆包要重打,先按上游形态发 | — |
| T1.8 | `chipstar` | 1.3.0 | 无卡机器上跑一个 CUDA kernel | T1.4 |
| T1.9 | `adaptivecpp` | 25.10.0 | `--acpp-targets=omp` 跑 kernel | T1.2 |
| T1.10 | `hip-runtime` / `hipcc` | 7.14.0 | `HIP_PLATFORM=nvidia` 跑 kernel | T1.1 |

### ② 引擎(mcpp,有序)

| # | 任务 | 判据 | 依赖 |
|---|---|---|---|
| T2.1 | ✅ **C-1 设备目标原语** —— 以既有 `mcpp::action` 的 `role = "artifact"` 落地,不新增 target kind | ✅ artifact 角色的产物不进链接(ninja_backend 既有);理由见「动态更新记录」 | — |
| T2.2 | ✅ **C-2 二次链接边** —— 以「object 角色的 action 以其它 action 的产物为输入」落地,核心只管顺序与指纹 | ✅ e2e 607:artifact 角色产物被 object 角色消费,中间产物不进链接线,顺序由图保证。⚠️ C9(nvcc `-rdc=true` + `-dlink` 真机)本机做不到 —— 见下方记录 | T2.1 |
| T2.3 | ✅ **C-3 逐 glob 收窄** `sources = [{ glob, accel }]` | ✅ e2e 606 四段:覆盖 ⇒ 编译且设备源到达构建程序;`--no-accel` ⇒ 整条 glob 排除;非子集 ⇒ 拒绝并点名两侧(`accel-mismatch`);空集 ⇒ 拒绝点名 glob。6 条单测 | — |
| T2.4 | ✅ **C-4 `exclusive` 能力声明** | ✅ e2e 601:独占对被拒并点名双方;**对照** —— 不声明的两个提供者照常共存。3 条单测 + 中英文档 + `exclusive-capability` 进机器接口契约页 | — |
| T2.5 | ✅ **C-5 载荷可用性机制**:探针通道 `mcpp::fact` / `mcpp::floor`(协议 v7),核心只比较;根工程的构建程序说完后再查一次 | ✅ e2e 605:根 build.mcpp 陈述的下界被比较并拒绝(两侧取值 + `version-floor-unmet`);对照:满足则构建 | — |
| T2.6 | ✅ **C-6 含设备代码的归档** | ✅ e2e 608:object 角色 action 的产物进 `.a`(判据是 `ar t` 的成员表与 `nm` 的符号,不是退出码 —— 空档案也会成功)。缺陷由 T5.1 暴露:llama.cpp 的 305 个 `.cu` 全被丢弃并只留一条警告 | T2.1 |
| T2.7 | ✅ **C-7 `accel` 维语法开放**(#562) | ✅ 5 条单测 `AccelOpenGrammar.*`;`floor>=` 为中性拼法 | — |
| T2.8 | ✅ **把 CUDA 探针搬进规则包** | ✅ `test_core_vendor_probes`:剥注释后 `src/` 无厂商工具名(自带分母,枚举 < 100 文件即判失败);doctor 的设备节与 `mcpp.toolchain.devicehost` 一并删除;同样的读数由 rules-cuda 产出 | T2.5 |
| T2.9 | ✅ **`accel` 表达驱动下界** | ✅ 实测:`fact=cuda.driver=12.4` + `floor=cuda.driver >= 13.0` ⇒ 13.3 工具包在编译前被拒;PTX 高于驱动 ⇒ 警告(点名架构集合仍可运行) | T2.5, T2.7 |
| T2.10 | ✅ **未指定设备目标的构建期诊断** | ✅ `accel` 不含架构 ⇒ 规则包在提交任何 action 前拒绝并说明;`--no-accel` 走 CPU 实现而非「编了但没设备码」 | T2.7 |

### ③ 发布(mcpp)

| # | 任务 | 判据 |
|---|---|---|
| T3.1 | ✅ 版本号 + CHANGELOG + 发布 | ✅ 四个平台产物在 GitHub 与 GitCode **两端 GET 均 200 且字节数逐个相同**(不用 `gh release view` 的列表,不用 HEAD);`xlings install mcpp@2026.9.5.2` 后 store 路径自述 `mcpp 2026.9.5.2` |

### ④ 适配面(mcpp-index,依赖 ③)

| # | 任务 | 判据 | 依赖 |
|---|---|---|---|
| T4.1 | ✅ `compat.cuda-runtime` → `compat.cuda-driver` 改名 + `repo` 改正 | ✅ 旧条目冻结保留;工作区成员 `tests/examples/cuda-driver` **同时依赖新旧两个名字**,让跳转期这条承诺有判据(此前它只是一句注释) | — |
| T4.2 | `compat.vulkan-icd` / `compat.opencl-icd`(缺失时回落载荷) | 无卡机器上 dlopen 到软件实现 | T1.4, T1.5 |
| T4.3 | 🟡 `rules-cuda` 进索引(**#348**);hip/sycl/spirv 待做 | ✅ 描述符指向发布 tarball 的子路径(与 `grpcgen` 同形,**不新建仓库**);示例去掉 path 依赖、改成一行 `[dependencies.mcpplibs]` 后在 4080 上跑出 `12 24 36 48` | T3.1, T2.8 |
| T4.4 | 🟡 `compat.cudart` + `cublas`/`cufft`/`curand`/`cusolver`/`cusparse` ✅ | ✅ e2e 判据=新成员 `tests/examples/cuda-curand`:无卡机器断言库能加载并应答,有卡再断言 [0,1] 与均值。⚠️ `cudnn`/`nccl`/`onemkl` 仍缺 xim 载荷 | T1.7 |

### ⑤ 框架(mcpp-index,依赖 ④)

| # | 框架 | 判据(一律用上游自带测试) | 依赖 |
|---|---|---|---|
| T5.1 | llama.cpp 多后端(CPU/CUDA/Vulkan/SYCL/HIP) | 各后端推理出正确 token(C4) | T4.3 |
| T5.2 | ncnn | lavapipe 上分类与 CPU 一致(C5) | T4.2 |
| T5.3 | CUTLASS | GEMM 与参考比对(C6) | T4.3 |
| T5.4 | oneDNN | `benchdnn` 两条 lane(C11) | T1.3 |
| T5.5 | Kokkos | 自带 unit test 两份产物都过(C7) | T4.3 |
| T5.6 | OpenCV CUDA 模块 | accuracy test(C13) | T4.4 |
| T5.7 | FAISS | 自带测试 | T4.4 |
| T5.8 | ONNX Runtime | model test + EP 共存(C12) | T4.4 |
| T5.9 | libtorch 载荷消费 | 闭包校验(C14) | T4.4 |

### ⑥ 生态验证(沙箱)

| # | 任务 | 判据 |
|---|---|---|
| T6.1 | ✅ 新建 subos `md-verify`,CN mirror(mcpp 与 xlings 各一份),装发布物 | ✅ **走 store 路径不走 shim** —— `xlings install` 装完仍提示 `mcpp` 解析到旧版;store 路径自述 `mcpp 2026.9.5.2` |
| T6.2 | ✅ 三份脚本:引擎(13 条断言)、索引适配(1 条)、CUDA 全链(3 条) | ✅ 全绿。⚠️ 设备那一半在沙箱里**构造上做不到**:`--sandbox` 的 `/dev` 只有 14 项、无 `/dev/nvidia*`,`cudaMalloc` 必报「no CUDA-capable device」。判据因此拆两处:沙箱断言「链路成立 + 缺设备时干净地说出来」,设备结果在宿主上断言 |
| T6.3 | ✅ 对照 | ✅ 同一份脚本对 **2026.9.5.1** 跑:13 条里 12 条变红,且理由逐条具体(新 API 不存在、`host_config` 在旧引擎里出现 2 次)。这是「判据确实测到了东西」的证据 |

---

## 2. 并行与关键路径

```
⓪ ──────────────────────────────────────────────► (独立,最先完成)

① 载荷 ────┬─ T1.1 CUDA ──────────┐
           ├─ T1.2 llvm-offload ──┤
           ├─ T1.3 dpcpp ─────────┤
           ├─ T1.4 pocl ──────────┼──► ④ ──► ⑤ ──► ⑥
           ├─ T1.5 lavapipe ──────┤
           └─ T1.6-10 ────────────┘
                                  │
② 引擎 ── T2.1 ─ T2.2 ─ … ─ T2.10 ┴─► ③ 发布 ─┘
```

⭐ **关键路径是 ②**(引擎有序,十个任务串行),① 与它完全并行。
⚠️ ⑤ 的九个框架是工作量主体,T5.1 是 gate —— 它若暴露设计问题,后八个都受影响。

---

## 3. 动态更新记录

方案允许按实际情况改,但每次改必须写下理由。

| 日期 | 改了什么 | 理由 |
|---|---|---|
| 2026-09-05 | 建表 | — |
| 2026-09-05 | §3.0 改正:xim **推荐** xlings-res 双镜像,但**允许**第三方 URL template + per-arch sha256 | 读 `xpkg-creater/SKILL.md` §资源选择策略;索引里 247 个配方直连 github。我原先写「xim 不 re-host」过绝对 |
| 2026-09-05 | T0.1/T0.2 完成 | 文档 24 处清零;示例改用 `[xlings.workspace]` |
| 2026-09-05 | T1.1/T1.3/T1.6/T1.7 落地为 xim PR #759(25 包) | 实测通过:载荷编 sm_89 并在 4080 上跑通;两条线并存可切 |
| 2026-09-05 | `cuda-cccl` 用显式 per-version URL 而非模板 | 上游把组件从 `cuda_cccl` 改名为 `cccl`,目录名进 URL,一个模板 404 |
| 2026-09-05 | 配方用 `io.popen` 列文件 | `os.files` 在 `config()` 沙箱里不可用(`attempt to call a nil value`),`llvm.lua` 也用 popen |
| 2026-09-05 | ⚠️⚠️ T2.5 的驱动取数**撤出核心** | 仓库自带的 `test_runtime_contract` 不变量禁止「厂商词 + 探针启动」在 `src/` 共现,抓住了我写的 `nvidia-smi` 调用。**这条规则先于本工作存在且是对的** —— 关系留下并单测,取数改由声明抵达,归规则包通道 |
| 2026-09-05 | T0.3 完成,并暴露一处「几乎为真」 | nvcc 自动加的是**它自己的** `../include`,12.x 那里有 `crt/` 却没有 `cuda_runtime.h`(在 cudart 组件)。不显式传载荷 include ⇒ nvcc 从 `/usr/include` 取头、连带读宿主的 `host_config.h`,**用着载荷的编译器却报宿主工具包的错** |
| 2026-09-05 | ① 全部完成并合入 | 25 个载荷;五个命令(nvcc/cuda-gdb/ncu/nsys/sycl-ls)从 shim 可达 |
| 2026-09-05 | T2.4 完成 | `exclusive` 是列表不是布尔:一个包可提供多项能力而只有部分独占。schema 警告而非报错,因为绑定期那一处才是执行者 |
| 2026-09-05 | ⚠️ 本机 shim 被 #582 剪掉一次 | 25 次 `xlings install` 后 `mcpp` 等 7 个裸名 shim 消失,store 完好。重装即恢复 —— 又一次受控复现,补进 issue |
| 2026-09-05 | `libcublas` 暂不拆 static | 上游一个归档同时含 shared 与 static,拆分需要重打包并 re-host,与「不 re-host」冲突;先按上游形态发,拆分单列 |
| 2026-09-05 | ⚠️⚠️ **C9(RDC 真机)在本机不可测,判据改为通用形状** | nvcc 路线在这台机器上**两个工具包都用不了**:12.9 满足驱动而不满足 C 库,13.3 满足 C 库而不满足驱动。C9 需要 nvcc 才能构造,于是它的判据退回到 e2e 607 所测的**通用链式 action**(与厂商无关的同一形状),真机 RDC 留给有 13.x 驱动的机器 |
| 2026-09-05 | ⚠️⚠️ **12.9 工具包 + glibc ≥ 2.41 是一个不能配的对** | 12.9 的 `crt/math_functions.h` 为宿主重声明 C23 的 `cospi`/`sinpi`/`rsqrt` **不带 `noexcept`**,glibc 2.41+ 带,而 C++17 起它是函数类型的一部分 ⇒ 六条 `exception specification is incompatible`,点名两个头文件而不给结论。13.3 不再重声明,同一份 C 库下干净通过。规则包读 `bits/mathcalls.h` 直接拒这一对 |
| 2026-09-05 | **引擎新增两个环境变量而不是复用 `MCPP_TARGET_SYSROOT`** | 后者是**档位**事实(裸机 C 库载荷,宿主目标为空),已有消费者按它判断档位;第二个编译器要的是**环境**事实。复用会把两个含义压进一个键 —— 正是 [[a-value-acquires-the-receiving-layers-requirements]] 那条 |
| 2026-09-05 | **`-B` 的守卫收敛为 `gcc::binutils_prefix_dir`,并只对 GCC 作答** | 三处副本(registry / gcc / flags),其中一处注释写着「Mirrors the guard in build/flags.cppm」。clang 的命令行本就不带 `-B`,所以对 clang 作答会描述一个没人传的开关 |
| 2026-09-05 | ⚠️ **构建程序 helper 在 Linux 上改用 `DT_RPATH`** | RUNPATH 只对 helper **自己**的 needed 生效;它 `dlopen` 的宿主库的依赖(`libdl.so.2`)按私有 loader 的默认搜索,搜不到。这是规则包能读到驱动版本的前提 |
| 2026-09-05 | ⚠️⚠️ **① 的 13.x 载荷装完不能用** —— xim #760 | nvcc 用 `$(TOP)/nvvm/bin/cicc` 找自己的后端,而 13.x 把 `nvvm/` 与 `crt/` 拆成了独立包=独立载荷根。**载荷完整、`nvcc --version` 正常、组件都装了**,编译时 `exit 127`。修法在 `install()` 里把这两个目录链回来;`os.exists`/`os.ln` 在配方沙箱里都不存在,`os.cp(symlink=true)` 是「保留源里的符号链接」而不是「建一个」 |
| 2026-09-05 | ⚠️⚠️ **「没有加速器」被写成了显示用的 `(none)`** | `accel_str` 为空集打印 `(none)` 是给 ABI 标签读的;`resolvedAccel` 把这个拼法当值传了出去 ⇒ ①`MCPP_ACCEL=(none)` 到达**每一个从未提过加速器的工程**,与手册承诺的空串矛盾;②指纹里 `if (!accel.empty())` 恒真,给所有工程都追加了 `#accel=(none)`。判据只能靠构建程序**写文件**取得 —— 它的 stdout 只在非零退出时才打印。e2e 605 第四段标题写着「变量与 layer 都清空」却只测了 layer,这就是它逃过套件的原因 |
| 2026-09-05 | ④ 的 T4.1/T4.4 落地为 mcpp-index #347 并合入 | 六个新包 + 一次改名;两处上游耦合写进配方(`crt/` 在编译器组件里;NVIDIA 的 `.so` 带 `RUNPATH=$ORIGIN` 会关掉继承的 RPATH ⇒ 要一并 farm glibc 三个存根) |
| 2026-09-05 | ⚠️ **path 索引里包的命名空间由「索引名」决定,而不是描述符里的 `namespace`** | 本地验证时 `[indices] localidx = { path = ... }` 下 `compat.cudart` 解析不到,而诊断说「a package with this name exists under another namespace: compat.cudart」—— 把索引名改成 `compat` 即通。诊断本身值得单独修 |
| 2026-09-05 | ⭐⭐ **T5.1 作为 gate 立刻兑现了**:它暴露了 C-6 的引擎缺口 | llama.cpp 的 CUDA 后端是 305 个 `.cu` 挂在 `kind = "lib"` 上。引擎把 object 角色的 action **只**挂到可执行/共享库/测试上,于是每一个 action 都被丢弃、只留一条警告,而构建**成功**并产出一个不含设备码的归档。修法是把静态库加进那个谓词(归档规则本来就消费 `lu.objects`);判据 e2e 608 断言 `ar t` 的成员表 —— 空档案也会成功 |
| 2026-09-05 | ⚠️⚠️ T5.1 的 CUDA lane 在本机**四维矩阵无解**,而链路本身全通 | 轴 → 收窄的 glob → 设备源清单 → 规则包 → action → **进静态库归档** → 链接,整条链实测走通,48 个设备目标已产出。挡住的是一个与 mcpp 无关的四维矩阵:①CCCL 2.x(12.9 线)的 `cub::LoadDirectWarpStriped` 少一个重载;②CCCL 3.3(13.3 线)同样不匹配;③补进 CCCL 3.2(13.2 线)后换成 **clang 编不动 libcu++**(`string_view` 的推导指引只允许 `__host__ __device__`、`block_load.cuh` 要 placement new);④走 nvcc 则 12.9 撞 glibc 2.44、13.3 撞驱动 12.4。⇒ 结论是**载荷矩阵**,不是设计 |
| 2026-09-05 | ⚠️ clang 路线要带 NVIDIA 自己的 libc++ 逃生开关 | 设备单元只要 include `<cuda_runtime.h>`,`crt/host_defines.h:67` 就以 `"libc++ is not supported on x86 system"` 停下 —— 守卫是 `__CUDACC__ && _LIBCPP_VERSION`,而 clang 编 CUDA 时自己就定义 `__CUDACC__`,于是一条**写给 nvcc 宿主 pass** 的拒绝落到了这条路线上。`-D_ALLOW_UNSUPPORTED_LIBCPP` 只在 clang 路线传。⚠️ 示例自己的 kernel 一直没暴露它:**裸 kernel 一个工具包头都不 include**,示例测的是接线不是头文件 |
| 2026-09-05 | ⚠️ 未提交 `xim:cuda-cccl@13.2.75` | 配方改动做好并解析通过,但它是为一个没走通的用例加的,而索引的 CUDA 线策略是「12.9 + 13.3 两条」;单加一个 13.2 的 cccl 不自洽。撤回 |
| 2026-09-05 | ③ 发布完成,**gtc 没有需要补的** | 四个产物在 GitCode 端全部由 `publish-ecosystem` 传完(历史上 180s 上限会打死最大的两个)。核验仍按记忆里的规矩做:**GET 而不是 HEAD,`gitcode.com` 而不是 `api.gitcode.com`,不信 `gh release view` 的列表** —— 八次 GET 全 200 且字节数两端相同 |
| 2026-09-05 | ⚠️ **沙箱里跑不了设备那一半**,判据因此拆两处 | `xlings subos --sandbox` 的 `/dev` 只有 14 项且**没有 `/dev/nvidia*`**,`cudaMalloc` 必报「no CUDA-capable device is detected」。沙箱断言到「规则包来自索引 + 设备单元编译链接通过 + 缺设备时干净报出」为止,设备结果在宿主断言(`12 24 36 48`)。⭐ 第一版脚本把设备结果写进沙箱判据,红了才发现 —— 这正是判据该做的事 |
| 2026-09-05 | ⚠️ 沙箱 `--cmd` 的引号会把源码改坏 | 用内联 heredoc 传 C++ 源进 `--cmd`,换行被吃掉、`printf` 的格式串断成两行。**记忆里那条(为传输改脚本 ⇒ 假绿)的同族**:修法是把脚本写进 subos 的 home 再执行,不是改脚本 |
| 2026-09-05 | ⚠️ **layer 不能选择依赖,feature 可以** | `[target.'cfg(accelerator = "cuda")'.dependencies]` 被引擎拒绝并说明理由:layer 由依赖图解析而来,用它选依赖会让依赖决定自己被问的问题。⇒ 一个库的设备后端拆两半:**依赖挂 feature,源文件挂 accel 轴** |
| 2026-09-05 | ⚠️ 设备编译要显式指名 CCCL 载荷,否则命中 `/usr/include/cub` | 与 T0.3 的 `cuda_runtime.h` 同一形状,第三次出现。且 12.x 的 `include/cub` 在 13.x 变成 `include/cccl/cub` |
| 2026-09-05 | e2e 317 的等待窗从 2s 放宽到 5s | 到达「五次短失败」下界最少要 1.25s(4×250ms 重启延迟 + 5×50ms 轮询),2s 窗只给每次 spawn 留 150ms;main 上 macOS **连续两次**在此失败而本分支同码两次通过 —— 判据由 runner 负载决定。5s 窗留 750ms |
| 2026-09-05 | e2e 602 声明 `requires: unix-shell`,并以 `MCPP_OFFLINE=1` 运行 | doctor 在 Windows 上整段不产出(载荷只有 linux 构建;Windows 工具包的上界是 `_MSC_VER` 区间,报告尚未读它);隔离 home 下 doctor 会把整套引导 + 工具链装进临时目录:实测 229s / 1.4 GB |
| 2026-09-05 | ⚠️ 核心改动:`--offline` 下跳过首次沙箱引导 | `load_or_init` 在空 home 里克隆索引、经 `xlings install` 装 ninja/patchelf,全部走网络,违反 `--offline`「绝不碰网络」的承诺。实测 offline 空 home 26s / 126 MB → 0.3s;e2e 604 带对照(已引导的 home 不提示);文档中英各补一句 |
| 2026-09-05 | C-1/C-2 不新增 `[[target]] kind = "device"` | 读了引擎:`mcpp::action` 已有四种角色(source/check/object/artifact),artifact 产物不进链接而 object 产物进链接,且 ninja 按路径连边 ⇒ 「不参与常规链接、由某条边消费的产物」就是 artifact 角色,「二次链接」就是以 artifact 为输入的 object 角色 action。再加一种 target kind 是同一个决定写第二遍 |
| 2026-09-05 | `accel` 进构建程序(`MCPP_ACCEL`)与 `cfg(accelerator)` | 后者的 `Ctx.accelerators` 字段**从未被写入**(声明了、文档了、没人填);e2e 605 第四段证明 `--no-accel` 下 layer 为空 |
| 2026-09-05 | ⚠️ `--accel/--no-accel` 进指纹并绕开 fast path | 实测:设备构建成功后 `mcpp build --no-accel` 报 `Finished in 0.00s` 并交回设备构建 —— 判据是 605 第四段先红后绿 |
| 2026-09-05 | ⚠️ 本机 e2e 168 红 | 已发布的 2026.9.5.1 同样红(musl gcc 13.3.0 载荷无 std module 源)⇒ 环境不是回归;CI 绿 |
| 2026-09-05 | ⚠️⚠️ 自构建的指纹目录中途漂移(`de4e07f` → `e45f24c7`) | 工作树 stash 后旧二进制也算出新值 ⇒ 输入在仓库外变了(后台 e2e 套件同时改写共享 `~/.mcpp`);我拿旧目录的二进制测了 606 近一小时。规则:**每次构建后用 `ls -t target/*/*/bin/mcpp` 重新取二进制,且开发期间不并行跑整套 e2e** |
