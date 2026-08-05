# build.mcpp 机制架构设计：一个 hook，多种节点

> 状态：**步 0–6 全部已实施（2026.8.5.1）**。实施记录见 §9（步 0+1）与 §10（步 2–6）。
> 范围：`build.mcpp` 作为**扩展机制**的长期形态 —— 不是某个具体特性
> 关联：#355（依赖产出的 host 工具，是本路线的第一块地基）、#241、L3 原始设计
> （`.agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md`）
> 涉及（预估）：`src/build/build_program.cppm`、`src/build/hostprogram.cppm`、
> `src/build/prepare.cppm`、`src/build/plan.cppm`、`src/build/ninja_backend.cppm`

---

## 0. 结论摘要

今天的 `build.mcpp` 把**两件本质不同的事**塞进了同一个点：

- **配置**（configuration）——「这次构建长什么样」：探测宿主、选源码、定 flag。
- **施工**（build work）——「把这批输入变成那批输出」：codegen、transpile、静态检查、后处理。

它天生适合前者，被迫承担后者。**所有已知痛点都是这个混淆的推论**——codegen 没有增量、
不能并行、失败无法归因到具体输入、拿不到依赖产出的工具（#355）。用户提到的两个未来
场景（编译前静态检查、把某语言编译成 C++）**都是施工**，直接塞进今天的机制会把这四个
痛点原样继承一遍。

架构主张三条：

| # | 主张 | 反面 |
|---|---|---|
| **A** | **一个 hook，多种节点** —— 不增加生命周期钩子的数量，增加钩子**输出**的表达力。后处理 = 「以最终产物为输入的节点」，不是新钩子 | 加 pre/post/pre-link/post-link：N 个钩子 = N 套生命周期 × N 份缓存语义 × N 种失败模式，且钩子之间的顺序还要再定义一次 |
| **B** | **声明期必须能算出「文件名集合」，不必算出「文件内容」** —— 这条 mcpp 特有的硬约束（来自 plan/fingerprint/模块扫描）把「什么能进图、什么必须 eager」划得一清二楚 | 假装所有 codegen 都能进图；或反过来假装都不能 |
| **C** | **扩展靠包，不靠 DSL** —— 可复用规则（protobuf codegen、某语言 transpiler、lint 预设）以**普通 mcpp 包**分发，`build.mcpp` `import` 它们的 host 模块 | 引入 Lua/Starlark/YAML 规则 DSL：第二门语言，直接违背 mcpp「用 C++ 写构建」的立身之本 |

一个可测量的「不优雅」指标：**今天新增一条 directive 要改 9 处代码**（§1.3 清单）。
架构目标之一是把它降到 **1 处（一张表）**。

结构性发现：**主张 C 要求「build.mcpp 能 import 依赖提供的 host 模块」，而 #355 要求
「build.mcpp 能拿到依赖提供的 host 二进制」—— 这是同一套 host 子构建机制的两种产物
（`lib` 与 `bin`）。#355 不是孤立特性，它是这条架构路线的第一块地基。**

在扩表之前，有 **5 个现存的稳定性/兼容性缺口**必须先补（§4）。

---

## 1. 今天的机制：精确画像

### 1.1 一句话

一个 host 程序，在 prepare 期跑一次，通过**行式 stdout** 输出**对 `buildConfig` 的补丁**。

### 1.2 三张表

**生命周期位置**（`src/build/prepare.cppm`）

| # | 阶段 | 位置 |
|---|---|---|
| 0 | manifest 解析 | — |
| 1 | `[generated_files]` 物化 | `:1664`（**依赖解析之前**，可产出 `build.mcpp` 自身） |
| 2 | 依赖解析 / feature 激活 | `:1725`–`:3830` |
| 3 | **依赖的 build.mcpp** | `:3841` |
| 4 | **root 的 build.mcpp** | `:3985` |
| 5 | modgraph 扫描 / fingerprint | `:4100`–`:4220` |
| 6 | BuildPlan | `:4239` |
| 7 | build.ninja → 编译链接 | `execute.cppm:292` |
| 8 | 分发（`mcpp pack`） | `src/pack/` |

**用户逻辑今天只能在 3/4。5–8 完全进不去。**

**输出通道与作用域**（`build_program.cppm`）

| directive | 落点 | 作用域 |
|---|---|---|
| `cxxflag` / `cflag` / `cfg` | `buildConfig.{cxx,c}flags` → `privateBuild` | 本包 TU |
| `link-lib` / `link-search` | `buildConfig.ldflags` → 转发到 root | 最终链接 |
| `generated` / `source` | `buildConfig.sources` + `modules.sources` | 编译集 |
| `include-dir[-after]` | `privateBuild.includeDirs[After]` | **本包私有**（Cargo 纪律：构建程序不得拓宽包的公开接口） |
| `rerun-if-changed` / `-env-changed` | 缓存键 | — |

**缓存键**（`build_program.cppm:486-496`）

`hash(build.mcpp 源) × hash(编译器身份 + 链接策略) × hash(契约 env) × 声明的文件 × 声明的 env`
→ 命中则**重放缓存的 directive**，不再运行程序。

### 1.3 新增一条 directive 的成本：9 处

以 `include-dir` 为例：

| # | 位置 | 内容 |
|---|---|---|
| 1 | `build_program.cppm:103` | `Directives` 结构体字段 |
| 2 | `build_program.cppm:157` | `parse_line` 分派 |
| 3 | `build_program.cppm:336` | `write_cache` 序列化 |
| 4 | `build_program.cppm:379` | `read_cache` 反序列化 |
| 5 | `build_program.cppm:432` | `apply` 落到 manifest |
| 6 | `build_program.cppm:401` | `cache_fresh` 的存在性校验（仅产物类） |
| 7 | `prepare.cppm:2613` | `DirectiveMark` 字段 |
| 8 | `prepare.cppm:2617` + `:2628` | `markDirectiveTail` / `foldDirectiveTailIntoPrivateBuild` |
| 9 | `hostprogram.cppm:kMcppModuleSource` | 类型化 API |

外加文档 ×4（en/zh × 05/07）。而 `prepare.cppm:2611` 的注释**自己承认**这还不完整：

```
// Link/source/fingerprint residues stay at the call sites — they
// genuinely differ between root and dep (see each).
```

也就是说 link/source 类的新 directive **还要再改 2 个调用点**。这正是这个代码库反复吃亏
的「同一决策多处推导」形态，只不过这次是**提前**看见它。

---

## 2. 症结：配置 vs 施工

|  | 配置（configuration） | 施工（build work） |
|---|---|---|
| 回答什么 | 这次构建**长什么样** | 把**这些**输入变成**那些**输出 |
| 输出物 | 一份**描述**（对构建计划的补丁） | **文件**，或一个成败判定 |
| 运行频率 | 每次解析一次 | 每个输入一次 |
| 需要的性质 | 全局视野、可缓存重放 | 增量、并行、可归因、可缓存 |
| 归属层 | prepare | ninja |
| 今天在哪 | `build.mcpp` ✅ 天生合适 | `build.mcpp` ❌ 被迫 eager 全量 |

四个已知痛点全是推论：

1. **无增量**：改一个 `.proto` → 整个 build.mcpp 重跑 → 全量重生成
2. **无并行**：一个进程串行做完所有生成
3. **失败无归因**：程序退出非 0，用户看到的是「build.mcpp exited with 1」，不是「foo.proto 第 3 行」
4. **拿不到依赖的工具**（#355）：施工需要工具，而工具由构建产出 —— 配置期还没有构建

用户提到的两个未来场景，按这张表分类：

- **编译前静态代码检查** → 施工（输入=最终源码集，输出=成败）。今天连表达都表达不了：
  build.mcpp 跑的时候源码集还没定，它自己就是源码集的贡献者之一。
- **把某语言编译成 C++** → 施工（N 输入 → M 输出）。今天只能 eager 全量转译，
  改一个文件全量重来。

---

## 3. 架构主张

### 3.1 主张 A：一个 hook，多种节点

**反对增加钩子。** 每个生命周期钩子的真实成本是：一套运行时机 + 一份 env 契约 +
一份缓存/失效语义 + 一种失败模式 + 一节文档 + N 个 e2e。而且 N 个钩子之间的**相对顺序**
还要单独定义一次。这是乘法，不是加法。

**主张：保留唯一的配置钩子，让它声明节点。**

后处理（strip/sign/打包）在这个模型里不是「post 钩子」，而是「一个以最终产物为输入的
节点」——它因此**免费获得**增量（产物没变就不重跑）、并行、以及正确的失效语义。
上一轮讨论里「post 钩子会二次 strip / 二次签名」的自我失效难题，在节点模型下根本不存在，
因为 ninja 的输出不是输入。

统一后的**唯一新原语**：

```
action {
    id       : string            // 诊断与去重
    inputs   : [path]            // 必须存在，或由另一个 action 产出
    outputs  : [path]            // 声明期即确定（见主张 B）
    command  : [argv]            // 工具路径由程序自己给（如 #355 的 dep_bin）
    role     : source | check | artifact
    deps     : [action-id]       // 可选
}
```

**三个 role 不是三种机制，是同一条边的三种接线方式**：

| role | 输出接到哪 | 典型 |
|---|---|---|
| `source` | 进编译集（等价于今天的 `generated=`，但**延迟到 ninja 期执行**） | protoc、transpiler |
| `check` | 输出是一个 stamp；默认挂 `default` 与编译**并行**，`blocking=true` 时成为编译边的前置 | clang-tidy、格式检查、ABI 检查 |
| `artifact` | 输入是 link 产物，输出进 `dist/` | codesign、appimage、size budget |

`role=check` 默认并行而非阻塞：静态检查串行化整个编译是不可接受的代价，而「构建最终失败」
的效果一样。`blocking` 留给「不通过就不该浪费编译时间」的场景。

### 3.2 主张 B：声明期的能力边界

> **INV-D：配置期必须能算出「输出文件名的集合」，不必算出「文件内容」。**

这条不是设计选择，是 mcpp 的**结构性约束**：BuildPlan、fingerprint、
`compile_commands.json`、以及非 dyndep 模式下的模块 topo 序，都在 prepare 期定型，
而它们全都需要知道「有哪些源文件」。

推论表 —— 这张表就是「用 action 还是用 eager」的判据：

| 场景 | 输出名可预测？ | 归属 |
|---|---|---|
| `.proto → .pb.cc/.pb.h` | ✅ 命名规则确定 | **action**（增量） |
| transpiler X→C++（1:1 或规则确定） | ✅ | **action** |
| lint / 静态检查（输出=stamp） | ✅ | **action** |
| strip / sign / 打包 | ✅ | **action** |
| 生成**模块接口** `.cppm` | 文件名 ✅，但模块名与 import 边要**内容** | **eager**（今天的路径）—— 但见下 |
| 输出数量/名字由工具运行时决定 | ❌ | **eager**；或加一个「聚合成单一已知产物」的步骤 |

**这条判据最重要的副作用**：它让今天的 eager 路径（`generated=`/`source=`）从
「历史包袱」变成**有明确适用面的一等路径**。两条路径都有存在理由，文档能一句话讲清
用哪条 —— 而不是「新的更好，旧的别用」。

**`.cppm` 那一行值得单独跟进**（不在本设计定稿）：`ninja_backend.cppm:214-222`
显示 **dyndep 已是默认**（`MCPP_NINJA_DYNDEP=0` 才退回静态 deps），模块依赖边本来就在
ninja 期用 `rule cxx_scan`(`:844`) + `rule cxx_dyndep`(`:486`) 解析。所以「生成的 `.cppm`
必须 eager」的真正阻塞点，是 plan 仍消费 prepare 期的 `topoOrder`（`prepare.cppm:4239`）。
若能确认该 topoOrder 在 dyndep 模式下是冗余的，这条限制可以解除，transpiler 场景就能
完全进图。**这是一个值得单独核实的问题，不要在本设计里假设答案。**

### 3.3 主张 C：扩展靠包，不靠 DSL

「把某语言编译成 C++」这类规则应该**写一次、到处用**，而不是每个包把同一段 build.mcpp
复制一遍。三条路：

| 路 | 做法 | 判断 |
|---|---|---|
| (a) | 每个包自己写 build.mcpp | 不可复用；规则的 bug 要修 N 遍 |
| (b) | 引入规则 DSL（xmake 的 Lua rule、Bazel 的 Starlark） | **拒绝**：引入第二门语言，直接违背 mcpp「用 C++ 写构建、不引入第二语言」的立身之本 |
| (c) | **规则以普通 mcpp 包分发，build.mcpp `import` 它的 host 模块** | ✅ 推荐 |

```cpp
// build.mcpp
import mcpp;
import mcpp.rules.protobuf;      // 一个普通的 mcpp 包，为 HOST 编译

int main() {
    mcpp::rules::protobuf::generate({
        .protos  = "proto/**/*.proto",
        .out_dir = "gen",
        .grpc    = true,
    });
}
```

规则包因此**有版本、能测试、能发布、用 C++ 写、零新语法**，走的是 mcpp 已有的全部
包管理机制（index、语义化版本、feature、lockfile）。

**这条要求一个引擎能力**：build.mcpp 能 `import` 依赖提供的、为 host 编译的模块。
今天 build.mcpp 只能 import 两个东西 —— 内置的 `mcpp` 模块和 `std`
（`build_program.cppm:541-543`）。

而这正是 **#355 的同一套机制**：

| 需要什么 | 产物类型 | 机制 |
|---|---|---|
| #355：调用依赖产出的工具 | `bin` | host 子构建 → tool store → 路径 |
| 主张 C：import 依赖提供的规则 | `lib`（host BMI + 对象） | host 子构建 → tool store → BMI/对象路径 |

**⇒ #355 不是一个孤立特性，它是这条架构路线的第一块地基。**
这是本轮分析最重要的结构性发现：它把 #355 从「gRPC 的权宜之计」提升为「扩展性架构的
必经之路」，并且说明 #355 的 tool store 设计必须留出「条目里不止有 `bin/`」的余地。

---

## 4. 稳定性与兼容性：扩表之前先补 5 个缺口

这些不是未来问题，是**今天就存在**的。在 directive 表变长之前补，成本最低。

| # | 缺口 | 证据 | 后果 | 处置 |
|---|---|---|---|---|
| **S1** | **协议无版本** | `build_program.cppm:162`：未知 directive → `warning` + 忽略 | 为新版 mcpp 写的 build.mcpp 在旧 mcpp 上**静默丢语义**（构建成功，行为不对） | 程序声明 `mcpp:protocol=N`（由内置模块自动发）；引擎见到未知 directive 且程序声明的 N ≥ 自身支持 → **硬错误**并指明「升级 mcpp」 |
| **S2** | **build.mcpp cache 无语义 epoch** | `write_cache` 只写 `program`/`compiler`/`ctx` 三行 | 引擎改了某条 directive 的**解释**，旧缓存条目被按新语义重放 —— 静默错误 | 加 `epoch` 行，纪律照抄 `cache_key.cppm::kCacheEpoch`（「仅当旧条目不可用时才 bump，且与 mcpp 版本号解耦」） |
| **S3** | **build.mcpp 无超时** | 用的是 `capture_exec`；带超时的 `capture_exec_deadline` 就在隔壁（`process.cppm:105`） | 一个死循环 / 等网络的 build.mcpp 让构建**永久挂起**，且没有任何诊断 | 给默认超时（可 `--build-program-timeout` 覆盖），超时错误点名是哪个包的 build.mcpp。对照 `mcpp test` 已有的两档超时纪律 |
| **S4** | **裸 `printf` 是唯一真正的兼容风险** | 内置 `mcpp` 模块与引擎在**同一个二进制里**，永远同步；`#include <cstdio>` + 手写 `mcpp:` 字符串不是 | 用户手写的字符串会随协议演进腐烂，且 S1 的守卫看不见它（它不会发 `protocol=`） | 文档把 `import mcpp;` 定为**唯一演进面**；裸 printf **冻结**在现有 11 条，不再新增。生态现状支持这一步（grpc-m / opencv-m 都已是 `import mcpp` 风格） |
| **S5** | **directive 的作用域在多处推导** | §1.3 的 9 处清单；`prepare.cppm:2611` 注释自认不完整 | 加新语义时**必然漏一处**，且失败在很远的地方 | 收敛成**一张 directive 定义表**：`{ 名字, 落哪个字段, 作用域(private/link/source), 是否进 cache, 是否进 fingerprint }`；parse / write_cache / read_cache / apply / fold 全部**由表驱动** |

**S5 是「优雅简洁」这个诉求的量化目标：9 处 → 1 处。** 它也是引入 `action` 原语的
**前置条件** —— 在一个要改 9 处的结构上加一个字段数是 6 的新 directive，是自找麻烦。

另外两条**已经做对、要保持**的纪律，写下来以免将来被推翻：

- **契约 env 无条件进缓存键**（`build_program.cppm:305-312`）：target/profile/feature 变了
  必须重跑，这条正确性**不能**依赖作者记得写 `rerun-if-env-changed`。
- **include-dir 永远私有**（`build_program.cppm:99-102`）：构建期程序**不得**拓宽包的
  公开接口。这是供应链面的判据，不是风格偏好。任何新 directive 都要先回答
  「它会不会拓宽公开接口」。

---

## 5. 目标架构：分层视图

```
L0  声明层（mcpp.toml）
      静态可解析：依赖图、feature、target 条件、targets
      ↑ 程序不可改写 —— lockfile / LSP / 审计的前提
L1  配置层（build.mcpp）—— 唯一的 hook
      跑一次；输出 = 「对 L0 的补丁」+「对 L2 的节点声明」
      能力：探测宿主 / 选源码 / 定 flag / 声明 action / import 规则包
L2  图层（ninja）
      增量、并行、可缓存、可归因
      节点：compile / link / action(source | check | artifact)
L3  分发层（mcpp pack）
      与构建解耦
```

一条方向性规则，是既有 INV-1 的一般化：

> **L1 只能向下写（声明 L2 的节点），不能向上写（改 L0 的依赖图）。**

它同时解释了为什么 #355 的工具请求必须写在 `mcpp.toml`（L0）而不是 build.mcpp（L1）：
「我需要某个包产出的某个工具」是对依赖图提出的需求。

---

## 6. 演进路径

每一步都是**加法**，没有任何一步需要改变现有 build.mcpp 的行为 ——
这是把兼容性当一等约束的直接结果。

| 步 | 内容 | 前置 | 破坏性 | 状态 |
|---|---|---|---|---|
| 0 | 补 S1–S4：协议版本 / cache epoch / 超时 / 冻结裸 printf | — | 无（S4 是文档 + 停止扩表） | **已实施 2026.8.5.1** |
| 1 | S5：directive 定义表收敛 | — | 无（纯内部重构，可用「产物逐字节相同」验证） | **已实施 2026.8.5.1** |
| 2 | #355：host 工具 + 工作目录外置 | 0, 1 | 无 | **已实施 2026.8.5.1** |
| 3 | `action` 原语，先只做 `role=source`（即 codegen 进图） | 2 | 无（新增） | **已实施 2026.8.5.1** |
| 4 | `role=check`（静态分析）、`role=artifact`（后处理） | 3 | 无（新增） | **已实施 2026.8.5.1** |
| 5 | build.mcpp 可 `import` 依赖提供的 host 模块 → **规则包生态** | 2 | 无（新增） | **已实施 2026.8.5.1** |
| 6 | 核实并（若成立）解除「生成的 `.cppm` 必须 eager」限制 | 3 | 无 | **已核实并解决（§10.4）** |

步 0/1 值得优先，因为它们**成本最低而收益随时间递增**：directive 表越长，补的代价越大。

---

## 7. 明确不做

| 不做 | 理由 |
|---|---|
| 引入第二门语言的规则 DSL（Lua / Starlark / YAML） | 违背 mcpp「用 C++ 写构建」的立身之本；主张 C 用包机制拿到了同样的复用性 |
| 增加第二个生命周期 hook（pre / post / pre-link / post-link） | 主张 A：钩子数是乘法成本；节点模型覆盖同样的用例且自带增量 |
| 让 build.mcpp 改依赖图 | L1 不向上写；静态可解析性是 lockfile / LSP / 审计的前提 |
| build.mcpp 升级为返回 LazyPath 的完整构建图 DSL（Zig 形态） | `action` 原语已拿到主要收益；完整形态要把行式协议换成结构化图协议，与既有生态不兼容 |
| 包自定义 manifest 键 | 既有 Appendix A「closed syntax, open vocabulary」原则 |

---

## 8. 需要 review 决定的开放问题

1. **`action.command` 的表达力边界**：只允许 argv + **封闭的引擎变量词表**
   （`$in` / `$out` / `${mcpp.compile_db}` / `${mcpp.out_dir}` / `${mcpp.target_file:<name>}`），
   还是允许任意 shell？封闭词表更可移植（Windows 无 shell 假设）、更可缓存，
   但会有人抱怨不够用。**倾向封闭。**
2. **结构化 directive 的线格式**：`action` 有 6 个字段，行式 `key=value` 会很难看。
   建议在既有平坦协议上**扩展**一条 `mcpp:action={json}`（引擎侧已有 `mcpp.libs.json`），
   而内置模块负责编码 —— 这与 S4「`import mcpp;` 是唯一演进面」自洽。**是否接受 JSON 载荷？**
3. **`role=check` 的默认挂载**：默认并行（本文建议）还是默认阻塞？
   是否需要一个 `mcpp check` 只跑 check 节点？
4. **规则包的命名空间与稳定性承诺**：`mcpp.rules.*` 是保留前缀吗？谁来维护第一批
   （protobuf / clang-tidy）？
5. **步 6 的前置核实**：dyndep 已是默认，那么 `prepare.cppm:4239` 传给 `make_plan`
   的 `topoOrder` 在 dyndep 模式下是否已经是冗余的？这决定 transpiler 场景能否完全进图。

---

## 9. 实施记录（步 0 + 步 1，2026.8.5.1）

**范围决策**：本次只实施步 0 与步 1。步 3–5 会新增**公开协议面**（`mcpp:action=`），
一经发布即成为兼容承诺，而 §8 的开放问题（命令表达力边界、结构化载荷格式、
`role=check` 默认挂载）尚未 review —— 在未定案的情况下把它发进生态，正是本文
§4 想要防的那类债。

### 9.1 落地清单

| 项 | 位置 |
|---|---|
| directive 定义表（S5） | **新增** `src/build/directives.cppm` —— `kTable` 一行即一条指令；解析 / 缓存读写 / apply / 声明产物契约 / 私有折叠全部表驱动 |
| 协议版本（S1） | `directives.cppm::kProtocolVersion`、`protocol_error()`；内置模块在 `hostprogram.cppm` 里以 `@PROTOCOL@` 占位符**替换**注入（不硬编码，杜绝漂移） |
| cache epoch（S2） | `directives.cppm::kCacheEpoch`；`build_program.cppm` 的 `write_cache`/`read_cache`/`cache_fresh` |
| 运行上限（S3） | `directives.cppm::run_timeout()`；`build_program.cppm` 改用 `capture_exec_deadline` |
| `capture_exec_deadline` 补 `cwd` | `src/platform/process.cppm` |
| mark/fold 迁出 | `prepare.cppm` 的 `DirectiveMark`/`markDirectiveTail`/`foldDirectiveTailIntoPrivateBuild` → `directives.cppm` 的 `Mark`/`mark()`/`fold_private_tail()` |
| 文档 | `docs/07-build-mcpp.md` + `docs/zh/07`：演进面对照表、协议、epoch、运行上限 |
| 测试 | `tests/unit/test_build_directives.cpp`（21 例）、`tests/e2e/186_build_mcpp_protocol_and_bound.sh` |

### 9.2 与设计的三处偏差

1. **`Def` 多了 `missingPrefix`/`missingSuffix` 两列。** 初版把两条产物型指令的
   缺失诊断收敛成一句通用文案，`143_build_mcpp_source_directive.sh` 立刻变红 ——
   它断言的是 `selected source`。这个断言是对的而不是过时的：`generated=` 说的是
   「我**写了**这个文件」，`source=` 说的是「我**选中了**这个已存在的文件」，
   两者是不同的契约，用户需要被告知自己违反了哪一个。**表驱动不等于文案统一**，
   所以把差异也放进表里。

2. **`run_timeout()` 放在 `directives.cppm` 而不是 `build_program.cppm`。**
   后者的匿名 namespace 在 clang 22 下会误编译邻居（PR#332/#334），
   「别再往那个匿名 ns 加代码」是硬约束。于是该模块的定位是「**build.mcpp 契约**
   ——指令表 + 协议版本 + 缓存 epoch + 运行上限」，四者都是 mcpp 与构建程序之间的
   约定，放在一起是自洽的。

3. **未知缓存记录也作废整条条目。** 设计只写了 epoch。实施时补上：一条本 mcpp
   不认识的 `d` 记录（更新的 mcpp 写的）若只跳过它，等于应用了程序所要求的一个
   **真子集** —— 与 S1 拒绝未知指令是同一条判据。

### 9.3 验证

- 单测 56/56 通过（新增 `BuildDirectives` 21 例）
- e2e：12 个 build.mcpp / 生成源相关用例全绿，新增 186 号通过
- 三条协议分支实测：`import mcpp;` + 未知指令 → 硬错误；裸 printf + 未知指令 →
  警告并继续；`protocol=999` → 拒绝并提示升级
- 缓存四条失效路径实测：干净命中 / 缺 epoch / epoch 不符 / 未知 `d` 标签
- 超时实测：3 秒上限对死循环程序在 3.1s 内触发，错误点名包名与 env 覆盖方式

> **一个测试方法论坑**：`build.mcpp` 的缓存命中被工程级 fast path（`try_fast_build`）
> 挡在前面 —— 无变更的第二次构建走 fast path，**根本不会读 build.mcpp 缓存**。
> 验证缓存行为必须先 `touch` 一个源文件把 fast path 打掉，否则会把「fast path 生效」
> 误读成「缓存未命中」。我第一次就是这么误判的。

---

## 10. 实施记录（步 2–6，2026.8.5.1）

### 10.1 §8 五个开放问题的定案

| # | 问题 | 定案 | 理由 |
|---|---|---|---|
| 1 | `action.command` 的表达力 | **封闭词表**：argv + `${mcpp.out_dir}` / `${mcpp.bin_dir}` / `${mcpp.compile_db}` / `${mcpp.target_file:<name>}` | 不假设存在 shell（Windows 没有可依赖的那个），且没有东西能夹带环境状态进来 —— 可移植性与可缓存性同一个理由 |
| 2 | 结构化载荷格式 | **在既有平坦协议上扩展 `mcpp:action={json}`** | action 有六个字段，平坦 `key=value` 表达不了；内置模块负责编码，与 S4「`import mcpp;` 是唯一演进面」自洽 |
| 3 | `role=check` 默认挂载 | **默认并行**，`blocking = true` 才前置 | 把整条编译串行化在 linter 后面是没人接受的代价，而「构建最终失败」的效果一样 |
| 4 | 规则包命名空间 | `mcpp.rules.*` 作为约定前缀，机制上不特殊 | 规则包就是普通包，特殊化它只会多一套规则 |
| 5 | dyndep 下 topoOrder 是否冗余 | **问题问错了** —— 见 §10.4 | |

### 10.2 host 工具（步 2）：实现要点与两个坑

- **工作目录外置是硬前置**，且必须**五处一起搬**（`target/`、`mcpp.lock`、
  `compile_commands.json`、`.mcpp/`、`target/.build-mcpp`）。只搬一部分比一处都
  不搬更糟：那等于照样写进共享的注册表包根，只是更不显眼。
- **`${mcpp.target_file:NAME}` 必须解析成 build-dir 相对路径，不是绝对路径。**
  ninja 用「边声明的那个字符串」标识文件，link 边声明的是 `bin/app`；指向同一份
  字节的绝对路径是**另一个节点**，ninja 报
  `missing and no known rule to make it`。第一次实现取了绝对路径，e2e 立刻炸。
  顺带确认了 action 命令的 cwd 是 build dir。
- 子构建通过 `mcpp.build.ninja` 的 backend 驱动，**不能**走 `execute.cppm`
  （它 import 了 prepare，反过来会成环）。`prepare_build` 全函数只有一处 `static`，
  递归重入是安全的。

### 10.3 `action`（步 3+4）：为什么占位文件是对的

role=source 的产物在 prepare 期不存在，而 modgraph 扫描要 glob 磁盘。选择是
**播下占位文件**而不是凭空合成 CompileUnit：这样 glob 找得到它、scanner 读得到它、
plan 给得出对象路径、ninja 在编译边之前用真实内容覆盖它（因为那条编译**依赖**
action 的输出）。整条链路复用现有机制，没有一处特判。

占位文件**绝不截断已存在的文件** —— 第一次构建之后那里是真实内容，重写它会让 ninja
以为输入每次 prepare 都变了。

### 10.4 步 6 的核实结论：问题问错了

设计里问的是「dyndep 模式下 `prepare.cppm:4239` 传给 `make_plan` 的 `topoOrder`
是否冗余」。核实后：`topoOrder` 在 `plan.cppm` 有两处用途 —— `:708` 的名字消歧
普查（**与顺序无关**，只是遍历全部已扫描单元）和 `:829` 的 CompileUnit 发射次序。

**但真正的阻塞点根本不是顺序**，而是：一个没有被扫描过的文件**根本没有
`graph.units` 条目**，于是没有 CompileUnit，于是不会被编译。顺序是不是冗余，与
它无关。

解法用代码库**已有的**答案 ——「声明而非发现」，即 `[modules].scan_overrides` 早就
做过的那个取舍：action 用 `.provides()/.imports()` 声明生成模块的接口，mcpp 播下
带该声明的占位文件，prepare 期的扫描因此与生成器将要产出的内容一致，而 build 期
由编译器自己的 P1689 复核这条声明。**限制解除，且没有动 topoOrder 一行。**

### 10.5 规则包（步 5）：为什么不走 tool store

最初的直觉是「像 host 工具一样，用子构建产出 BMI + 对象，放进 store」。那是**错的**：
BMI 只对「在 standard / dialect / 编译器身份上与它一致」的编译可用，而两次独立解析
的构建**没有理由**一致 —— 消费者的 `standard` 与规则包自己的 `standard` 就可以不同。
不一致的表现是 `module X CRC mismatch`，不是一条清楚的错误，而这个代码库为这一族
问题付过多次学费。

改成**与 build.mcpp 同一条命令、同一套 flag 编译**，一致性就从「需要验证的性质」
变成了**结构性事实**。代价是规则接口单独编译，只能 import `std` 与内置 `mcpp` 模块
—— 一个规则包按构造就是叶子，这个限制写进文档而不是藏起来。

### 10.6 验证

- 单测 56/56
- e2e 19/21；`07_static_library`（本机 binutils payload 的 `ar` 跑不起来）与
  `09_path_dependency`（`ninja missing dep BMI`）在**已发布的 2026.8.4.1 上同样
  失败** ⇒ 环境性，非回归（判定回归前先跑已发布二进制做对照）
- 新增 3 个 e2e：**187**（端到端 / 成本门 / 默认关闭 / 错名列出可用 target /
  override 生效）、**188**（三种 role + 输入变则重生成 + 无关重建不重跑 +
  失败的 check 让构建失败 + 畸形 action 被拒）、**189**（规则包导入生效 / 编辑规则
  触发 build.mcpp 重跑 / 缺 lib root 的诊断）
