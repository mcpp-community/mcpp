# 多设备生态:实施计划与任务表

对应方案 `2026-09-05-multi-device-ecosystem-design.md`。本文只管**做什么、谁依赖谁、
判据是什么、现在到哪一步**。方案改了本文跟着改,反之不成立。

## 0. 状态总览

| 批次 | 仓库 | 状态 |
|---|---|---|
| ⓪ 修已发布的错误示范 | mcpp | 🟡 T0.1/T0.2 ✅,T0.3 待载荷 |
| ① 载荷 | xim-pkgindex | 🟡 **PR #759**(25 个包,已实测) |
| ② 引擎 | mcpp | 🟡 进行中 |
| ③ 发布 | mcpp | ⬜ |
| ④ 适配面 | mcpp-index | ⬜ |
| ⑤ 框架 | mcpp-index | ⬜ |
| ⑥ 生态验证 | 沙箱 | ⬜ |

图例:⬜ 未开始 / 🟡 进行中 / ✅ 完成并有判据 / ⛔ 阻塞

---

## 1. 任务表

### ⓪ 修已发布的错误示范(mcpp,无依赖,立刻做)

| # | 任务 | 判据 | 依赖 |
|---|---|---|---|
| T0.1 | 从全部用户文档删除 `[xlings] deps`,`[xlings.workspace]` 为唯一形式 | `git grep -c 'xlings\] deps' docs/` 为 0(中英双份) | — |
| T0.2 | `examples/09-cuda-kernel` 改用 `[xlings.workspace]` | 示例中不出现 `deps =` | T0.1 |
| T0.3 | 该示例去 host 化(`-L/usr/...`、`/usr/local/cuda/bin/nvcc`) | 示例与规则包里 `grep -c '/usr'` 为 0 | T1.1 |

### ① 载荷(xim-pkgindex,批内并行)

| # | 任务 | 版本 | 判据 | 依赖 |
|---|---|---|---|---|
| T1.1 | ✅ 24 个 CUDA 组件(编译/运行/调试/分析/算子库) | 12.9 线 + 13.3 线 | ✅ 载荷编 sm_89 并在 4080 上跑出 `12 24 36 48`;两线并存可切换 | — |
| T1.2 | `llvm-offload`(补 slim 载荷缺的 offload 工具) | 22.1.8 | `clang -x cuda -fgpu-rdc` 编链通过 | — |
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
| T2.1 | **C-1 设备目标原语** `[[target]] kind = "device"` | 单测 + e2e:设备目标不参与常规链接 | — |
| T2.2 | **C-2 二次链接边** `role = "device-link"` | 跨 TU `__device__` 调用链接成功(C9) | T2.1, T1.2 |
| T2.3 | **C-3 逐 glob 收窄** | 空集/非子集各报错一次 | T2.1 |
| T2.4 | ✅ **C-4 `exclusive` 能力声明** | ✅ e2e 601:独占对被拒并点名双方;**对照** —— 不声明的两个提供者照常共存。3 条单测 + 中英文档 + `exclusive-capability` 进机器接口契约页 | — |
| T2.5 | **C-5 载荷可用性机制** + 探针通道 | 驱动只到 12.4 时请求 13.x ⇒ 构建前拒绝(C2) | — |
| T2.6 | **C-6 含设备代码的归档** | `.a` 的 `accel` 随包传播(C13) | T2.1 |
| T2.7 | **C-7 `accel` 维语法开放** | `vulkan1.3` / `sycl:spir64` / `hip:gfx1100` 可解析比较 | — |
| T2.8 | **把 CUDA 探针搬进规则包** | 核心 grep 不到厂商名字(C15);卸掉规则包 doctor 安静(C16) | T2.5 |
| T2.9 | **`accel` 表达驱动下界** | PTX 版本高于驱动 ⇒ 构建前拒绝(C20) | T2.5, T2.7 |
| T2.10 | **未指定设备目标的构建期诊断** | 报「没有为任何可用设备编」而非运行期(C19) | T2.7 |

### ③ 发布(mcpp)

| # | 任务 | 判据 |
|---|---|---|
| T3.1 | 版本号 + CHANGELOG + 发布 | GitHub/GitCode 资产 sha256 一致;`ci-fresh-install` 全绿 |

### ④ 适配面(mcpp-index,依赖 ③)

| # | 任务 | 判据 | 依赖 |
|---|---|---|---|
| T4.1 | `compat.cuda-runtime` → `compat.cuda-driver` 改名 + `repo` 改正 | 旧名保留一个跳转期 | T3.1 |
| T4.2 | `compat.vulkan-icd` / `compat.opencl-icd`(缺失时回落载荷) | 无卡机器上 dlopen 到软件实现 | T1.4, T1.5 |
| T4.3 | `rules-cuda` / `rules-hip` / `rules-sycl` / `rules-spirv` 进索引 | 消费者一行依赖即可用 | T3.1, T2.8 |
| T4.4 | `compat.cublas` / `cudnn` / `nccl` / `onemkl` | 闭包校验通过 | T1.7 |

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
| T6.1 | 新建 subos,配 CN mirror,装发布物 | 版本自述正确 |
| T6.2 | 逐条跑 C0–C20 | 全绿,且每条模拟器 lane 有硬件对照 |
| T6.3 | 对照:去掉包后判据必须变红 | 每条判据都测到了东西 |

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
| 2026-09-05 | T2.4 完成 | `exclusive` 是列表不是布尔:一个包可提供多项能力而只有部分独占。schema 警告而非报错,因为绑定期那一处才是执行者 |
| 2026-09-05 | ⚠️ 本机 shim 被 #582 剪掉一次 | 25 次 `xlings install` 后 `mcpp` 等 7 个裸名 shim 消失,store 完好。重装即恢复 —— 又一次受控复现,补进 issue |
| 2026-09-05 | `libcublas` 暂不拆 static | 上游一个归档同时含 shared 与 static,拆分需要重打包并 re-host,与「不 re-host」冲突;先按上游形态发,拆分单列 |
