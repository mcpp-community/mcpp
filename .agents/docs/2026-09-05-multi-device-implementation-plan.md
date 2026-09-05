# 多设备生态:实施计划与任务表

对应方案 `2026-09-05-multi-device-ecosystem-design.md`。本文只管**做什么、谁依赖谁、
判据是什么、现在到哪一步**。方案改了本文跟着改,反之不成立。

## 0. 状态总览

| 批次 | 仓库 | 状态 |
|---|---|---|
| ⓪ 修已发布的错误示范 | mcpp | 🟡 T0.1/T0.2 ✅,T0.3 待载荷 |
| ① 载荷 | xim-pkgindex | 🟡 **PR #759**(25 个包,已实测) |
| ② 引擎 | mcpp | 🟡 进行中:C-3/C-4/C-5/C-7 ✅,C-1/C-2 以 action 角色落地,C-6/T2.8–T2.10 进行中 |
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
| T2.1 | ✅ **C-1 设备目标原语** —— 以既有 `mcpp::action` 的 `role = "artifact"` 落地,不新增 target kind | ✅ artifact 角色的产物不进链接(ninja_backend 既有);理由见「动态更新记录」 | — |
| T2.2 | 🟡 **C-2 二次链接边** —— 以「object 角色的 action 以其它 action 的产物为输入」落地,核心只管顺序与指纹 | e2e 待补:通用链式 action(无厂商);C9 在 4080 上用 nvcc `-rdc=true` + `-dlink` 实测 | T2.1 |
| T2.3 | ✅ **C-3 逐 glob 收窄** `sources = [{ glob, accel }]` | ✅ e2e 606 四段:覆盖 ⇒ 编译且设备源到达构建程序;`--no-accel` ⇒ 整条 glob 排除;非子集 ⇒ 拒绝并点名两侧(`accel-mismatch`);空集 ⇒ 拒绝点名 glob。6 条单测 | — |
| T2.4 | ✅ **C-4 `exclusive` 能力声明** | ✅ e2e 601:独占对被拒并点名双方;**对照** —— 不声明的两个提供者照常共存。3 条单测 + 中英文档 + `exclusive-capability` 进机器接口契约页 | — |
| T2.5 | ✅ **C-5 载荷可用性机制**:探针通道 `mcpp::fact` / `mcpp::floor`(协议 v7),核心只比较;根工程的构建程序说完后再查一次 | ✅ e2e 605:根 build.mcpp 陈述的下界被比较并拒绝(两侧取值 + `version-floor-unmet`);对照:满足则构建 | — |
| T2.6 | **C-6 含设备代码的归档** | `.a` 的 `accel` 随包传播(C13) | T2.1 |
| T2.7 | ✅ **C-7 `accel` 维语法开放**(#562) | ✅ 5 条单测 `AccelOpenGrammar.*`;`floor>=` 为中性拼法 | — |
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
| 2026-09-05 | e2e 317 的等待窗从 2s 放宽到 5s | 到达「五次短失败」下界最少要 1.25s(4×250ms 重启延迟 + 5×50ms 轮询),2s 窗只给每次 spawn 留 150ms;main 上 macOS **连续两次**在此失败而本分支同码两次通过 —— 判据由 runner 负载决定。5s 窗留 750ms |
| 2026-09-05 | e2e 602 声明 `requires: unix-shell`,并以 `MCPP_OFFLINE=1` 运行 | doctor 在 Windows 上整段不产出(载荷只有 linux 构建;Windows 工具包的上界是 `_MSC_VER` 区间,报告尚未读它);隔离 home 下 doctor 会把整套引导 + 工具链装进临时目录:实测 229s / 1.4 GB |
| 2026-09-05 | ⚠️ 核心改动:`--offline` 下跳过首次沙箱引导 | `load_or_init` 在空 home 里克隆索引、经 `xlings install` 装 ninja/patchelf,全部走网络,违反 `--offline`「绝不碰网络」的承诺。实测 offline 空 home 26s / 126 MB → 0.3s;e2e 604 带对照(已引导的 home 不提示);文档中英各补一句 |
| 2026-09-05 | C-1/C-2 不新增 `[[target]] kind = "device"` | 读了引擎:`mcpp::action` 已有四种角色(source/check/object/artifact),artifact 产物不进链接而 object 产物进链接,且 ninja 按路径连边 ⇒ 「不参与常规链接、由某条边消费的产物」就是 artifact 角色,「二次链接」就是以 artifact 为输入的 object 角色 action。再加一种 target kind 是同一个决定写第二遍 |
| 2026-09-05 | `accel` 进构建程序(`MCPP_ACCEL`)与 `cfg(accelerator)` | 后者的 `Ctx.accelerators` 字段**从未被写入**(声明了、文档了、没人填);e2e 605 第四段证明 `--no-accel` 下 layer 为空 |
| 2026-09-05 | ⚠️ `--accel/--no-accel` 进指纹并绕开 fast path | 实测:设备构建成功后 `mcpp build --no-accel` 报 `Finished in 0.00s` 并交回设备构建 —— 判据是 605 第四段先红后绿 |
| 2026-09-05 | ⚠️ 本机 e2e 168 红 | 已发布的 2026.9.5.1 同样红(musl gcc 13.3.0 载荷无 std module 源)⇒ 环境不是回归;CI 绿 |
| 2026-09-05 | ⚠️⚠️ 自构建的指纹目录中途漂移(`de4e07f` → `e45f24c7`) | 工作树 stash 后旧二进制也算出新值 ⇒ 输入在仓库外变了(后台 e2e 套件同时改写共享 `~/.mcpp`);我拿旧目录的二进制测了 606 近一小时。规则:**每次构建后用 `ls -t target/*/*/bin/mcpp` 重新取二进制,且开发期间不并行跑整套 e2e** |
