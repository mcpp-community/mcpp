# 加速器支持 实施计划

> **For agentic workers:** 用 superpowers:subagent-driven-development 或
> superpowers:executing-plans 逐任务实施。步骤用 `- [ ]` 跟踪。

**Goal:** 让 mcpp 能声明、构建、发布并正确选择加速器产物,并在 mcpp-index / xlings
上打通一条真实可用的生态链路。

**Architecture:** 五个原语(device target / 接缝模块 / `accel` 身份字段 /
`accelerator` cfg layer key / 规则包),其中三个复用已有机制。引擎持有身份、指纹、
架构集合与工具链配对;厂商 flag 拼法由规则包持有。

**Spec:** [`2026-09-05-accelerator-support-design.md`](2026-09-05-accelerator-support-design.md)

**Tech Stack:** C++23 modules,mcpp 自举构建,ninja 后端,xmake 无关。

## Global Constraints

- 基线一律 `origin/main`(d18cd8b,2026-09-04)。worktree:
  `/home/speak/workspace/github/mcpp-community/mcpp-accel`,分支 `feat/accelerator-support`。
- **每个仓库一个 PR**,尽量单 PR 全部实现。
- 文档/PR 标题/代码注释:英文、学术体、陈述句、无表情。
- **`.cu` / `.cuh` 不进默认 source glob** —— `builtin_extension_table()` 的 `.ixx` 判例。
- 新增描述符字段必须是旧客户端可跳过的(`xpkg.cppm` 已确认 skip 未知键)。
- 判据取「只有做对了才会出现的读数」,不取「构建通过」。

## 本机验证条件(已实测,2026-09-05)

| 项 | 值 | 意义 |
|---|---|---|
| GPU | NVIDIA RTX 4080,compute capability **8.9**(`sm_89`) | **可以真跑 kernel**,不止编译期验证 |
| nvcc | 12.0 V12.0.140,`/usr/bin/nvcc` | 形态 A 可全链路验证 |
| nvcc 宿主上界 | `host_config.h`:**gcc > 12 报错;clang 必须 < 15** | T6 的判据来源 |
| 可用宿主 | `clang++-14` ✅(实测编译并运行成功);`g++-13` ❌(超界);`g++-12` 二进制不存在 | 配对必须可选择,不只是校验 |
| mcpp 载荷 gcc | 16.1.0 | ⚠️ **比 nvcc 12.0 的上界高四个大版本 ⇒ 默认配对必然失败** |
| 基线构建 | `mcpp build` 53.23s,exit 0 | |

⭐ 最后两行是 T6 存在的理由的**现场证据**:mcpp 的默认载荷与本机 CUDA **不兼容**,
所以 T6 不能只做「校验」,必须能**选择**一个兼容的宿主编译器,或给出可执行的错误。

---

## 仓库与依赖关系

```
mcpp (引擎)                     ── PR A
  │  T1 SourceKind::Device
  │  T2 accelerator cfg layer key
  │  T3 accel 身份字段 + tag_check
  │  T4 [package] accelerators
  │  T5 device target
  │  T6 宿主/设备编译器配对
  │  T7 capability 同名符号边界
  │  T8 --accel CLI
  │  T9 文档   T10 测试
  ↓ (引擎发布后才能被索引消费)
mcpp-index                      ── PR B   规则包 + 示例库描述符
  ↓
xlings / xim-pkgindex           ── PR C   载荷(按需)
  ↓
沙箱真实验证                     ── xlings subos --sandbox --cmd
```

**跨仓库次序是硬的**:引擎的新键必须先发布,索引里的包才能用它 —— 否则索引里的包
在已发布的 mcpp 上加载失败。本计划因此把 PR A 做完并发布,再做 PR B。

## 任务依赖图(mcpp 仓内)

```
T1 ─┬─> T5 ─> T6
    │
T2 ─┴─────────────┐
T3 ─> T8          ├─> T9 文档
T4 ────────────────┤
T7 ────────────────┘
                   └─> T10 测试(每个任务自带,T10 是补齐 e2e)
```

T1/T2/T3/T4/T7 彼此独立,可并行。

---

## File Structure

| 文件 | 职责 | 任务 |
|---|---|---|
| `modules/source-kind/src/source_kind.cppm` | 加 `Device` 档与设备扩展名表 | T1 |
| `src/build/prepare_inputs.cppm` | `accelerator` layer key、多值语义 | T2 |
| `src/pack/abi_tag.cppm` | `AbiTag::accel`、`tag_check` 扩展 | T3 |
| `modules/manifest/src/types.cppm` | `[package] accelerators`、device target 字段 | T4/T5 |
| `modules/manifest/src/toml.cppm` | 解析上述键 | T4/T5 |
| `modules/manifest/src/xpkg.cppm` | 描述符读写 `accel` | T3 |
| `src/build/prepare.cppm` | device target 的计划、架构收窄、配对校验 | T5/T6 |
| `src/toolchain/compat.cppm` | 宿主/设备编译器支持表 | T6 |
| `src/cli.cppm` | `--accel` / `--no-accel` | T8 |
| `docs/05-mcpp-toml.md` + `docs/zh/` | 手册 | T9 |
| `docs/19-accelerators.md` + zh | 新章 | T9 |
| `tests/unit/test_*.cpp` | 单测 | 各任务 |
| `tests/e2e/6xx_*.sh` | e2e | T10 |

---

## Task 1: `SourceKind::Device`

**Files:** `modules/source-kind/src/source_kind.cppm`,
`tests/unit/test_source_kind.cpp`(若无则新建)

**Interfaces:**
- Produces: `SourceKind::Device`;`is_scan_exempt(Device) == true`;
  `affects_graph_shape(DeviceHeader) == true`;`classify(".cu") == Device`;
  `default_source_globs()` **不含** `*.cu`。

- [ ] **Step 1: 写失败的测试** —— `classify("k.cu")` 应为 `Device`;
      `classify("k.cuh")` 应为 `Header` 一类(触发图形状失效);
      `default_source_globs(builtin_extension_table())` 不含 `*.cu`。
- [ ] **Step 2: 跑测试确认失败**(`mcpp test --filter source_kind`)
- [ ] **Step 3: 实现** —— 加 `Device` 枚举值、`kDeviceExtensions[] = {".cu",".hip"}`、
      `kDeviceHeaderExtensions[] = {".cuh",".hiph"}`(后者并入 `Header` 返回值),
      `is_scan_exempt` 加 `Device`,`default_source_globs` **不加**设备扩展名。
- [ ] **Step 4: 跑测试确认通过**
- [ ] **Step 5: 提交** `feat(source-kind): classify device translation units`

**判据(只有做对才出现的读数)**:一个含 `.cu` 的工程 `mcpp build` 后,
`build.ninja` 里没有该文件的 P1689 扫描边;而 `.cuh` 改动后 fast path 回落完整 prepare。

---

## Task 2: `accelerator` 多值 cfg layer key

**Files:** `src/build/prepare_inputs.cppm`,`src/build/prepare.cppm`,
`tests/unit/test_cfg_predicate.cpp`

**Interfaces:**
- Consumes: 已有 `kCfgLayerKeys`、`Ctx::layer_value`、`unknown_tokens`。
- Produces: `Ctx::layers_multi`(`map<string, vector<string>>`);
  `accelerator` 进 `kCfgLayerKeys`;`any/all` 在多值 layer 上的语义。

语义(写死):
- `accelerator = "cuda"` ⟺ 集合恰好是 `{cuda}`
- `any(accelerator = "cuda")` ⟺ `cuda ∈` 集合
- `all(accelerator="cuda", accelerator="rocm")` ⟺ 两者都在

- [ ] Step 1–5 同 TDD 循环。
**判据**:`cfg(any(accelerator="rocm"))` 段在 cuda-only 构建下**不被合并**;
`cfg(acclerator="cuda")`(拼写错)被 `unknown_tokens()` 报出并给出最近词。

---

## Task 3: `accel` 身份字段与 `tag_check`

**Files:** `src/pack/abi_tag.cppm`,`modules/manifest/src/xpkg.cppm`,
`src/pack/prebuilt.cppm`,`tests/unit/test_abi_tag_accel.cpp`

**Interfaces:**
```cpp
struct AccelReq { std::string backend, version, arch; };          // 消费侧请求
struct AccelSet { std::string backend, version;                    // 产物侧集合
                  std::vector<std::string> archs;
                  std::string ptxFloor; };                         // 空 = 无 PTX
struct AbiTag { /* 已有四维 */ std::vector<AccelSet> accel; };
bool accel_accepts(const std::vector<AccelSet>& published, const AccelReq& want);
```
比较语义:`want.backend` 在集合里,**且**(`want.arch ∈ archs` **或**
`ptxFloor` 非空且 `arch >= ptxFloor`)。`published.accel` 为空 = 不受约束。

- [ ] Step 1: 测试 —— 空 accel 接受一切;`{sm80,sm90f}` 拒 `sm86`;
      带 `ptx=80` 时接受 `sm86`;backend 不同直接拒。
- [ ] Step 2–5:实现 + `tag_check` 增加 accel 维的 `TagMismatch`。

**判据**:同一包发两变体,消费者按声明架构选中不同的那个;
诊断里出现 `accel  needs …, this build has …` 行。

---

## Task 4: `[package] accelerators` 声明

**Files:** `modules/manifest/src/types.cppm`,`modules/manifest/src/toml.cppm`

与 `[package] platforms` **严格同形**(声明/CI 矩阵提示,非强制)。
**判据**:`mcpp why` 里能看到它;写了不支持的 backend 时 `mcpp build` 给出提示而非报错。

---

## Task 5: device target

**Files:** `modules/manifest/src/types.cppm`、`toml.cppm`、`src/build/prepare.cppm`、
`src/build/ninja_backend.cppm`

新增 target 字段:`kind = "device"`、`backend`、`archs`、`device-link`,
以及 `[targets.<n>.sources]` 逐 glob 的 `archs` 收窄。

规则:
- glob 级 `archs` 必须是目标级子集,否则配置错误;
- 收窄成空集是**错误**;
- `archs` 缺省是**错误**,消息列出该 backend 的已知取值(决定 11)。

**判据**:空集与非子集都报错;`.cu` 的编译边出现在 `build.ninja` 且带正确的 `-gencode`。

---

## Task 6: 宿主/设备编译器配对(本机可真机验证)

**Files:** `src/toolchain/compat.cppm`,`src/build/prepare.cppm`

nvcc 的上界从 `crt/host_config.h` 读(本机实测:`__GNUC__ > 12` 报错,
clang 必须 `< 15`),或内置一张按 CUDA 版本的表。

行为:
1. 若 mcpp 当前宿主编译器超界 ⇒ **在调用 nvcc 之前**报错,消息含两边版本;
2. 若存在可用的兼容宿主(本机的 `clang++-14`)⇒ 提示可用 `-ccbin`;
3. 都没有 ⇒ 明确说「这套 CUDA 与 mcpp 可提供的宿主编译器都不兼容」。

**判据(本机现成)**:mcpp 载荷是 gcc 16.1.0,nvcc 是 12.0 ⇒
**必须报错且消息里同时出现 `16.1.0` 与 `12.0` 与上界 `12`**,
而不是把命令发给 nvcc 让它报 `unsupported GNU version`。

---

## Task 7: capability 同名符号边界

**Files:** `src/build/prepare.cppm`(绑定处)

同一 capability 的多个 provider 同时在图里 ⇒ **绑定期报错**,
而不是等到链接期报重复符号。

**判据**:两个都 `provides = ["gpu-blas"]` 且定义同名符号的包同时在图里,
错误发生在绑定阶段,消息列出两个候选包名。

---

## Task 8: `--accel` / `--no-accel`

**Files:** `src/cli.cppm`,`src/build/prepare.cppm`

与 `[build] accel` 的关系,和 `--target`↔`[toolchain]` 同形:manifest 声明 + 命令行覆盖。

---

## Task 9: 文档(英文学术体)

- `docs/19-accelerators.md` + `docs/zh/19-accelerators.md`(新章)
- `docs/05-mcpp-toml.md` + zh:新键
- `docs/11-machine-output.md` + zh:`accel` 进机器接口契约(**取值变了要写进契约页**)

## Task 10: e2e 补齐

`tests/e2e/6xx_accel_*.sh`。需要 GPU 的用例**单独成文件并在 job 一级守卫**,
不与其余混在同一套件里靠 `# requires:` 跳过(memory:守卫住在 job 里)。

---

## 跨仓库

**PR B(mcpp-index)** —— 在 PR A 发布之后。按此顺序:

| # | 包 | 类型 | 验证 |
|---|---|---|---|
| B1 | `rules-cuda` | 规则包 | P5;`backend` 经 capability 解析 |
| B2 | `compat.cccl` | header-only | 源码分发最短路径上的第一个真实库 |
| B3 | `compat.cudart` / `compat.cublas` | 能力包 | capability 绑定;**同名符号夹具(决定 4)** |
| B4 | `compat.cutlass` | header-only | 逐 glob 架构收窄(R4)的压测 |
| B5 | `llama.cpp-m` 加 CUDA 后端 | 真实框架 | ⭐⭐ R1+R3+R5;**它已在 `mcpplibs/` 里** |
| B6 | `compat.onnxruntime` / `compat.opencv` | 后续 | 真实体量的多变体选择 |

**PR C(xim-pkgindex)** —— 载荷。按此顺序:

| # | 包 | 验证 |
|---|---|---|
| C1 | ⭐ `adaptivecpp` | 形态 B **在无卡机器上 kernel 真执行** |
| C2 | `cuda-nvcc` | 设备工具链脱离 host(§5.0b 的原则) |
| C3 | `cuda-cudart` | 依赖已有的 `libcuda-host-link` sentinel |
| C4 | `dpcpp` / `cudnn` / `nccl` | 后续 |

⚠️ 按组件取,不打整个 toolkit。`cuda-nvcc` + `cuda-cudart` 是百 MB 级不是 GB 级。

**沙箱验证**:`xlings subos <name> --sandbox --cmd "..."`,先配 CN mirror。

---

## Self-Review(计划对规范的覆盖)

| 设计决定 | 任务 |
|---|---|
| 1 accel 并列字段 | T3 |
| 2 kind="device" | T5 |
| 3 逐 glob 收窄 | T5 |
| 4 capability 边界先修 | T7 |
| 5 组合表达 | T2 |
| 6 不产静态库 | T5(不实现该路径即可) |
| 7 C++ 运行时核验 | T3(延后:见下) |
| 8 工具链 family@version | 形态 B,本轮不做 |
| 9 小众后端不做 | 范围 |
| 10 生态闭环 | PR B |
| 11 archs 缺省报错 | T5 |
| 12 backend 两趟 | T5 |
| 13 节名核验 | 形态 B,本轮不做 |
| 14 deploy_files | T5(Metal 路径本轮不做) |
| 15 字段统一 archs | T5 |
| 16 不加标注 | 无需实现 |

⚠️ **本轮不做**:形态 B(SYCL/OpenMP,决定 8/13)、Metal(决定 14 的 Artifact 路径)、
决定 7 的 C++ 运行时核验。理由:本机没有 SYCL 工具链与 Metal,做了无法真实验证,
而**未经真实验证的实现正是本设计反对的东西**。这三项在 PR A 里留出接口位置但不实现。
