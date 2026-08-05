# issue #355：依赖产出的 host 工具（codegen 工具链缺口）

> 状态：**已实施（2026.8.5.1）**。§12 行业调研与据此调整的两处见 §12.4;
> 实施与**真实案例验证**（含 §11 那条风险的结论）见 §13。
> 关联：#355（本条）、#241（`MCPP_DEP_<NAME>_DIR`）、#274（显式 ninja 目标）、#344（cache 地址）
> 涉及（预估）：`src/build/prepare.cppm`、`src/build/build_program.cppm`、
> `src/build/hostprogram.cppm`、`src/pm/dep_spec.cppm`、`src/manifest/toml.cppm`、
> `src/manifest/xpkg.cppm`、`src/build/compile_commands.cppm`、`src/home.cppm`、
> 新增 `src/build/tool_store.cppm`
> 生态侧：`mcpp-index` 的 `compat.protobuf`、`mcpplibs/grpc-m`

---

## 0. 结论摘要

issue 的判断是对的：**`build.mcpp` 唯一缺的就是「工具在哪」**。但代码核实后，
「把工具交给消费者」这件事在当前架构里不是加一个环境变量能完成的，它撞上了**四条
彼此独立的结构性约束**，其中一条直接否掉了「把依赖的 bin target 塞进主 ninja 图」
这一最直观的方案：

| # | 约束 | 出处 | 后果 |
|---|---|---|---|
| C1 | **`build.mcpp` 在 prepare 期运行，那时 ninja 图还不存在** | `prepare.cppm:3841`（dep 循环）/ `:3985`（root）；plan 在 `:4239`、`build.ninja` 更在 `execute.cppm:292` 之后 | 任何「由主构建产出工具」的方案都**时序上不成立** |
| C2 | 主构建图**只为 target 编译**；工具必须能在 host 上跑 | `plan.cppm:497` `naming_for(tc)`；`prepare.cppm:1683` `host_tc_for_build_program` | 交叉编译下主图的产物**无法执行** |
| C3 | 依赖包的 `[targets]` **从不进入 link 计划**（唯一例外是 `kind="shared"`） | `plan.cppm:714` 只遍历 root 的 `manifest.targets`；`:922-937` `sharedDepTargets` | 依赖的 bin 目标今天**根本不被构建** |
| C4 | **注册表包根是共享的、可能只读，明文禁止写入** | `build_program.cppm:41-45`、`:233`；docs/07 §「Dependencies' build.mcpp」 | 「把依赖当 root 再构建一次」不能就地写 |

由 C1 + C2 得到本方案的形状：**工具只能由一次「以该依赖包为 root、面向 host 的
嵌套构建」在 prepare 期产出，产物落到全局 tool store，再以环境变量交给
`build.mcpp`。** 这正是 Cargo 的 `[build-dependencies]`（host 图与 target 图分离）
与 Bazel 的 exec configuration 的形状，不是新发明。

由 C4 得到一条**必须先做的前置项**：mcpp 目前有 5 处写入「工程根」的地方
（`target/`、`mcpp.lock`、`compile_commands.json`、`.mcpp/`、`target/.build-mcpp`），
嵌套构建必须能把它们整体外置。这一项独立有价值（只读源码树、CI 缓存、发行版打包）。

三个决策点的回答（对应 issue「需要设计决策的点」）：

1. **触发条件** → 消费方在 `[dependencies]` 里显式声明 `tools = ["protoc"]`。
   默认不构建。成本门（libprotoc 的 157 TU）复用**已有的**
   `[features].sources` + `[targets].required_features`，不需要新机制。
2. **交叉下必须是 HOST 产物** → 是，且比 issue 说的更强：工具子构建**整条链**
   （它自己的依赖、它的工具链）都按 host 解析，且**允许**与主构建用不同工具链——
   因为**可执行文件与主构建之间没有任何 ABI 接触**。这一条是本方案便宜的根本原因。
3. **与 `[xlings] deps` 的边界** → `[xlings] deps` = mcpp 包图**之外**的宿主工具
   （make/cmake/python）；本方案 = 包图**之内**、由包自己产出的工具。两者不重叠，
   `xim:` 预编译工具包仍然是「源码不可构建」时的合法出路。

一句话总结方案：**tool store 是接口，「怎么把条目填满」是 provider 细节。**
本次只实现 `build-from-source` 这一个 provider；`prebuilt-asset`（描述符自带
per-host 预编译资产，把 protoc 从「编 400 TU」降到「下 5MB」）是**设计好的扩展点，
本次不实现**。

---

## 1. 精确机制（代码级）

### 1.1 依赖的 `[targets]` 从不进入 link 计划

`src/build/plan.cppm:713-714`：

```cpp
// 4. Link units (one per [targets.X])
for (auto& t : manifest.targets) {          // ← manifest = ROOT 的 manifest
```

依赖包只贡献 **CompileUnit**（`cu.packageName` 标记归属），它们的 `.o` 被无差别地
灌进 root 的 link unit。依赖自己的 `[targets]` 在整个 plan 里**只有一个例外**被读到——
`kind="shared"`：

`src/build/plan.cppm:922-937`：

```cpp
for (std::size_t i = 1; i < packages.size(); ++i) {
    for (auto const& t : p.manifest.targets) {
        if (t.kind != mcpp::manifest::Target::SharedLibrary) continue;
        sharedDepPackages.insert(qname);
        sharedDepTargets.push_back(SharedDepTarget{...});
```

这是「依赖的 target 被提升为 link unit」的**既有先例**，形状上完全可以照抄给
`kind="bin"`。但它救不了 #355——见 1.2。

### 1.2 时序：`build.mcpp` 跑在 ninja 图存在之前（**这条决定方案形状**）

`prepare_build` 的实际顺序：

```
依赖解析 / feature 激活          prepare.cppm:~3400-3830
  ↓
G2: 依赖的 build.mcpp 循环        prepare.cppm:3841-3903
  ↓
L3: root 的 build.mcpp            prepare.cppm:3985-4040
  ↓
modgraph 扫描 / fingerprint       prepare.cppm:~4100-4220
  ↓
BuildPlan 生成                    prepare.cppm:4239 make_plan()
  ↓
run_build_plan → 写 build.ninja   execute.cppm:292    ← ninja 图到这里才成形
```

`build.mcpp` 要调用 protoc 生成 `*.pb.cc`，而这些生成源必须在 **modgraph 扫描之前**
落盘（docs/07 明文：「BEFORE the modgraph scan (so its generated=/source= sources
are picked up)」）。所以：

> **凡是由主 ninja 图产出的东西，`build.mcpp` 都用不上。**

把依赖的 `kind="bin"` 提升成主图 link unit（1.1 的先例）因此**不能解决 #355**。
它是另一个独立特性（「消费者想把依赖的 bin 当交付物一起装出来」），有价值，
但不在本设计范围内。

### 1.3 host ≠ target：主图只为 target 编译

`plan.cppm:497` `const auto naming = naming_for(tc);` —— 整个 plan 的产物命名、
flags、链接都取自解析出的 target 工具链。交叉构建下（`--target x86_64-windows-gnu`）
主图产出的是 PE，protoc 必须是当前机器能跑的 ELF。

mcpp 里**已经有**这条语义的正确实现，就是 `build.mcpp` 自己：

`src/build/prepare.cppm:1683-1723` `host_tc_for_build_program()` —— 「把同一个
toolchain spec **去掉 target 轴**再解析一次」，并且 `build_program.cppm:576-581`
把这条列为 load-bearing：

```
// build.mcpp is compiled AND run on the machine doing the build, so a std BMI
// built for the target would produce a helper that cannot execute — the same
// host≠target mistake the mingw-cross work had to fix in four separate places.
```

**工具子构建复用同一条规则即可**，不需要新的 host 解析逻辑。

### 1.4 依赖包没有 per-target 源码分区

`src/manifest/types.cppm:75-94`：`Target` 只有 `name/kind/main/soname/cflags/
cxxflags/defines/requiredFeatures`——**没有 `sources`**。一个包的全部源码编成一个
对象池，link unit 按 kind 取用。

所以「给 compat.protobuf 加一个 protoc bin target」会把 libprotoc 的 157 TU 灌进
**所有**消费者的库构建里。这正是 issue 决策点 1 担心的成本。

但成本门**已经存在**，不需要新机制：

- `[features.<name>].sources` —— feature 门控源码集（`xpkg.cppm:1364` 起、
  `toml.cppm` 同形；e2e `106_feature_gated_sources_toml.sh` 覆盖）
- `[targets.<name>].required_features` —— target 门（`types.cppm:93`，
  gate 在 `prepare.cppm:4056-4062`）

组合起来：

```toml
[features.protoc]
sources = ["*/src/google/protobuf/compiler/**/*.cc"]   # 157 TU，默认不编

[targets.protoc]
kind              = "bin"
main              = "src/google/protobuf/compiler/main.cc"
required_features = ["protoc"]
```

**engine 侧唯一的缺口**：`src/manifest/xpkg.cppm:1336-1350` 的 targets 解析器只认
`kind` / `main` / `soname`，**不认 `required_features`**（`toml.cppm:473` 认）。
Form B 描述符（`compat.protobuf` 正是 Form B）因此今天写不出这个门。这是必须补的
一行级改动。

### 1.5 注册表包根共享 / 可能只读

`src/build/build_program.cppm:41-45`、`:233-237` 明文：

```
// Dependencies MUST point this into the CONSUMING project's tree — a registry
// package root is shared and may be read-only.
```

而一次完整的 `mcpp build` 会往工程根写 **5 处**：

| 写入物 | 位置 | 出处 |
|---|---|---|
| `target/<triple>/<fp>/` | `root/target/...` | `prepare.cppm:199-210` `target_dir()` |
| `mcpp.lock` | `root/mcpp.lock` | `prepare.cppm:4690` |
| `compile_commands.json` | `plan.projectRoot/...` | `compile_commands.cppm:208` |
| `.mcpp/.xlings.json` | `root/.mcpp` | `prepare.cppm:1782-1790`（仅当有 `[indices]`/`[xlings]`） |
| `target/.build-mcpp/` | `root/target/.build-mcpp` | `build_program.cppm:234-237` |

嵌套构建若就地跑，会**同时违反 1.5 的明文不变量**、污染 registry、并让两个并发
构建在 `<verdir>/target/` 上打架。所以「工作目录外置」是硬前置项，不是优化。

### 1.6 `prepare_build` 以 cwd 为根

`src/build/prepare.cppm:842`：

```cpp
auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
```

工作区 fan-out（`cmd_build.cppm:68-81`）也是靠 `package_filter` 在**同一个 cwd 根**
下选成员，不是靠换根。要以「registry 里的某个包」为根构建，必须给 `prepare_build`
一个显式的根参数。好消息：`prepare_build` 全函数**只有一处 `static`**
（`:4413` 一个 `static const std::string kEmpty`），没有跨调用状态，**递归重入是安全的**。

### 1.7 现状复核：gRPC 这条链到底缺什么

- `mcpplibs/grpc-m` 是 **Form A**（自带 `mcpp.toml`），`compat.protobuf` 是 **Form B**。
- `compat.protobuf` 描述符自己写着（`pkgs/c/compat.protobuf.lua:20-23`）：
  「does NOT build `libprotoc` (a further 157 TUs) and ships no protoc binary」。
- `grpc-m/README.md`「Code generation」明文：「mcpp has no mechanism for handing a
  dependency's built binaries to a consumer. So generated stubs are checked in」，
  并要求用户自备 protoc **35.1** 与 gRPC **1.83.0** 的插件——**版本匹配责任
  今天完全落在用户身上**，且错配是运行期错误。

issue 对现状的描述与代码一致，无需修正。

---

## 2. 设计判据（不变量）

本方案接受下列判据，后文每个决策都能追溯到其中一条：

- **INV-1 依赖图保持声明式。** 「我需要某个包产出的某个工具」是对依赖图提出的
  需求，必须写在 `mcpp.toml`，不能由 `build.mcpp` 在运行期申请。
  （docs/07 已有明文：「It cannot add a registry dependency — keep your dependency
  graph declarative in mcpp.toml … build.mcpp is for *leaf* decisions」。）
- **INV-2 工具永远是 host 产物。** 与 `--target` 无关，与主构建的 linkage/profile
  无关。判据不是「跟主构建一致」，而是「能在这台机器上执行」。
- **INV-3 可执行文件与主构建零 ABI 接触。** 因此工具子构建**可以**用与主构建不同的
  工具链、不同的 profile、不同的依赖版本解析结果——这不是妥协，是本方案便宜的原因。
  （对照：`kind="lib"` 依赖绝不允许这样。）
- **INV-4 不写 registry 包根。** 见 1.5 的既有明文。
- **INV-5 单一版本轴。** 工具的版本 = 产出它的那个包的版本。不引入第二条版本轴
  （这正是 issue 反对 `xim:grpc-tools@X` 的理由，方案必须真正做到）。
- **INV-6 默认零成本。** 没有消费者要工具时，行为与今天**逐字节相同**。

---

## 3. 方案总览

```
                        ┌──────────────────────────────────────┐
   mcpp.toml            │  tool store（全局，跨工程共享）        │
   [dependencies]       │  <cache_root>/tool/<idx>/<pkg>@<ver>/ │
   protobuf = {         │        <keyHex>/bin/protoc            │
     version="35.1",    └──────────────────────────────────────┘
     tools=["protoc"] }         ▲                    │
        │                       │ provider 填充       │ 命中即取
        │ INV-1 声明式           │                    ▼
        ▼                 ┌─────┴──────┐      MCPP_DEP_PROTOBUF_BIN_PROTOC
   prepare：工具供给 pass  │ ①build-    │             │
   （在 build.mcpp 之前）  │  from-src  │             ▼
                          │ ②prebuilt  │      build.mcpp:
                          │  (未实现)   │        mcpp::dep_bin("protobuf","protoc")
                          └────────────┘        → 生成 *.pb.cc
                                                → mcpp::generated(...)
```

**tool store 是接口**：条目的形状（`<key>/bin/<tool><exe>` + `entry.json`）与
「谁把它填满」解耦。本次只实现 provider ①。

---

## 4. 分层实施

### Phase 0（前置，必须）：工作目录外置

**目标**：把 1.5 表里的 5 个写入点收敛到**一个决策点**。

新增（建议放 `src/project.cppm` 或新 `src/build/workdirs.cppm`）：

```cpp
struct WorkDirs {
    std::filesystem::path source;   // 包根：只读语义，只用来找源码/manifest
    std::filesystem::path work;     // 所有写入落在这里；默认 == source
    std::filesystem::path targetRoot() const { return work / "target"; }
    std::filesystem::path lockPath() const   { return work / "mcpp.lock"; }
    std::filesystem::path compileDb() const  { return work / "compile_commands.json"; }
    std::filesystem::path projectEnv() const { return work / ".mcpp"; }
    std::filesystem::path buildMcppDir() const { return work / "target" / ".build-mcpp"; }
};
```

改动：

- `prepare.cppm:199` `target_dir(tc, fp, root)` → `target_dir(tc, fp, wd)`
- `prepare.cppm:4690` lock 写入 → `wd.lockPath()`
- `compile_commands.cppm:208` → 由 `BuildPlan` 携带（新增 `plan.compileDbPath`），
  不再从 `plan.projectRoot` 推导
- `prepare.cppm:1786` `ensure_project_index_dir(cfg, root, ...)` → 传 `wd.projectEnv()`
- `build_program.cppm:234-237` `build_dir()` 的默认分支 → `wd.buildMcppDir()`

`BuildOverrides` 新增两个字段：

```cpp
std::filesystem::path project_root;   // 空 = find_manifest_root(cwd)（今天的行为）
std::filesystem::path work_dir;       // 空 = project_root（今天的行为）
```

**默认值让所有既有路径逐字节不变**（INV-6）。同时顺手解决 `prepare.cppm:842` 的
cwd 硬编码（1.6）。

> 副产品：`mcpp build --work-dir <dir>` 对只读源码树、CI 缓存、发行版打包都直接
> 有用。是否暴露成 CLI 由 review 决定；**引擎内部必须有**。

### Phase 1（本次核心）：`build-from-source` provider

#### 1.1 消费方语法

```toml
[dependencies]
protobuf = { version = "35.1",   tools = ["protoc"] }
grpc     = { version = "1.83.0", tools = ["grpc_cpp_plugin"] }
```

- `DependencySpec` 新增 `std::vector<std::string> tools;`（`src/pm/dep_spec.cppm`）
- `manifest/toml.cppm`：`:534` 的 key 白名单加 `"tools"`，`:564` 旁按 `features`
  的同形写法解析数组，`:632` 的错误文案同步
- `manifest/xpkg.cppm:1488` 的 `deps` 解析器**目前只接受字符串值**
  （`name = "version"`）。必须扩成「字符串 **或** 表」：

  ```lua
  deps = {
      ["compat.protobuf"] = { version = "35.1", tools = { "protoc" } },
  }
  ```

  否则 Form B 描述符永远无法请求工具。（注释里说的「richer Lua parser」指的是
  **namespaced 子表**；这里只是值位置的表，`cur.skip_table()` 已有同形能力。）
- `manifest/xpkg.cppm:1336-1350` targets 解析器补 `required_features`（见 1.4）

#### 1.2 工具请求的聚合

`DependencyEdge`（`prepare.cppm` 内部结构）新增：

```cpp
std::vector<std::string> requestedTools;    // 与 requestedFeatures 完全同形
```

聚合规则与 feature 一致：**同一个依赖包的所有入边取并集**。理由与 #242/#243
同源——请求必须来自权威边图，而不是只扫 root 的直接依赖，否则「grpc 的 mcpp.toml
请求 protoc」这类**传递请求会被静默丢掉**。

#### 1.3 供给 pass 的插入点

放在 `prepare.cppm` 的 feature 激活之后（`:3830` `apply(packages[i], ...)` 结束）、
**G2 依赖 build.mcpp 循环之前**（`:3832`）：

```
feature 激活 (:3812-3830)
  ↓
【新】工具供给 pass                    ← 本方案
  ↓
G2: 依赖 build.mcpp (:3841)            ← 拿得到 MCPP_DEP_*_BIN_*
  ↓
L3: root build.mcpp (:3985)            ← 同上
```

伪码：

```cpp
// key: (依赖包 index) → 请求的工具名（去重、排序）
std::map<std::size_t, std::set<std::string>> toolRequests;
for (auto const& edge : dependencyEdges)
    for (auto const& t : edge.requestedTools)
        toolRequests[edge.dependencyPackageIndex].insert(t);

// consumerIndex → (envVarName → 绝对路径)
std::map<std::size_t, std::vector<std::pair<std::string, std::filesystem::path>>> toolEnv;

for (auto const& [depIdx, tools] : toolRequests) {
    for (auto const& tool : tools) {
        auto p = mcpp::build::provision_tool(packages[depIdx], tool, hostAxes, cfg);
        if (!p) return std::unexpected(...);       // 硬失败，绝不静默降级
        // 回填给所有请求了它的消费者
    }
}
```

#### 1.4 一次工具子构建做什么

`provision_tool(pkg, toolName, ...)`：

1. **定位 target**：在 `pkg.manifest.targets` 里找 `name == toolName &&
   kind == Binary`。找不到 → 报错并列出该包所有 `kind="bin"` target 名。
2. **算 feature 集**：`pkg` 的 `[features].default` ∪ 该 target 的
   `required_features` 的闭包。**注意方向反转**，见 §5.2。
3. **算 store key**（§5.5）。命中 → 直接返回路径，**零构建**。
4. **未命中 → 嵌套构建**：

   ```cpp
   BuildOverrides sub;
   sub.project_root = pkg.root;                       // Phase 0
   sub.work_dir     = storeDir / "build";             // Phase 0，INV-4
   sub.target_triple = "";                            // ← host（INV-2）
   sub.profile       = "release";                     // 固定，见 §5.4
   sub.features      = join(featureSet);
   auto ctx = prepare_build(false, false, {}, sub);
   BuildOptions o; o.ninjaTargets = { linkUnitOutputOf(toolName) };  // #274 已有能力
   run_build_plan(*ctx, ...);
   ```

5. **收口**：把产物 `mv` 进 `<storeDir>/bin/<tool><exe>`，写 `entry.json`
   （记录全部 key 输入，照 `bmi_cache.cppm:55-58` 的「hash 相等也要逐字段比」纪律），
   然后**删除 `<storeDir>/build/`**（protoc 的几百个 `.o` 没有保留价值——key 覆盖了
   全部输入，命中就不会重建）。
6. 全流程走**临时目录 + rename**（照 `stage.cppm` 的纪律），并对 `<storeDir>` 加
   文件锁，避免两个并发 mcpp 撞同一个 key。

#### 1.5 环境契约与 API

新增契约变量：

```
MCPP_DEP_<SANITIZED_PKG>_BIN_<SANITIZED_TOOL>   = 工具可执行文件的绝对路径
```

`src/build/hostprogram.cppm` 的 `kMcppModuleSource` 加：

```cpp
// #355: 依赖产出的 host 工具的绝对路径，未提供则返回 ""。
inline const char* dep_bin(const char* pkg, const char* tool) { ... }
```

`sanitize` 沿用 `has_feature` / `dep_dir` 的同一套（大写、非字母数字 → `_`）。
碰撞处理照抄 `build_program.cppm:289-301`：**保留首个 + 警告**，绝不静默 last-wins。

这些变量与 `depDirs` 一样进入 `contract_env` → `contract_hash`
（`build_program.cppm:308`），所以**工具路径变化（= 工具被重建）自动触发
`build.mcpp` 重跑**，作者无需写 `rerun-if-changed`。这一条是正确性，不是优化。

### Phase 2（**本次不实现**，仅锁定扩展点）：`prebuilt-asset` provider

protoc 的现实是 upstream **为每个平台发布预编译二进制**。允许描述符声明
per-host 预编译资产，就能把「编 400 TU」降成「下 5MB」，而**版本轴仍然是同一条**
（资产写在同一个包版本的描述符里），INV-5 不破。

形状（示意，不在本次定稿）：

```lua
tool_assets = {
    protoc = {
        ["x86_64-linux-gnu"] = { url = "...protoc-35.1-linux-x86_64.zip",
                                 sha256 = "...", path = "bin/protoc" },
    },
}
```

消费方语法、store 布局、环境变量**完全不变**——这就是「store 是接口」的价值。
本次只需保证 Phase 1 的实现不把 provider 语义焊死在 `provision_tool` 里面。

### Phase 3（可选）：`mcpp tool` CLI

```bash
mcpp tool protobuf:protoc -- -I proto --cpp_out=gen proto/helloworld.proto
```

价值：(a) 让 grpc-m README 里那段「你得自己找一个 35.1 的 protoc」直接消失；
(b) 给 e2e 一个不经过 `build.mcpp` 的独立断言面。成本很低（供给 pass 已经写好）。

---

## 5. 关键语义细则

### 5.1 为什么触发条件放在消费方而不是生产方

生产方（包作者）能声明的只有「我有这个 bin target」——`[targets]` 已经是声明式的
公开产物表，再加一个 `export = true` 是纯仪式。而**成本是消费方承担的**
（157 TU 的编译时间落在消费者机器上），所以决定权必须在消费方。

同时 INV-1 要求它写在 `mcpp.toml` 而不是 `build.mcpp` 里：请求一个工具 = 向依赖图
索取一个额外产物，与「加一个依赖」同级。

> **拒绝的写法**：顶层 `[tools] protoc = "compat.protobuf:protoc"` 别名表。
> 它引入第三套命名空间（本地别名 → 包:target），而 `tools = [...]` 挂在依赖上
> 天然复用已有的包名解析（含 namespace-stripped 短名），且 `dep_bin("protobuf",
> "protoc")` 与 `dep_dir("protobuf")` 对称——正是 issue 要的形状。

### 5.2 `required_features` 在子构建里方向反转（**必须写进文档**）

docs/05 今天写的是：「A gate only — it does not activate features」。

在工具子构建里，被请求的 target **就是**构建目标，所以它的 `required_features`
成为**输入**而非门。这不是「同一决策两处推导」——字段的含义只有一个
（「这个 target 需要这些 feature」），只是两个调用方的**解析方向**相反：

| 调用方 | 方向 | 行为 |
|---|---|---|
| 主构建 | feature 集已定 → 问哪些 target 可发射 | 门（`prepare.cppm:4056`） |
| 工具子构建 | target 已定 → 问需要哪些 feature | 激活 |

docs/05 的那句话必须补一个从句，否则读者会认为工具子构建违反了文档。

### 5.3 为什么是「每工具一个路径变量」而不是 issue 提的 `_BIN_DIR`

issue 建议 `MCPP_DEP_<NAME>_BIN_DIR`。实测下来 per-tool 的**路径**更好：

- **消歧**：store 条目的 key 含 target 名（不同 target 的 `required_features`
  不同 → feature 集不同 → 必然是不同条目）。若坚持一个 `_BIN_DIR`，就得让 key
  改按「本次请求的工具集合」聚合，于是 `{protoc}` 与 `{protoc,x}` 变成两次
  完整构建；或者再造一层聚合目录（软链/拷贝）。两者都是纯粹的额外机制。
- **`.exe` 后缀**：`dep_bin` 在 host 上编译，可以自己补后缀；给 dir 则每个
  `build.mcpp` 都要自己拼平台后缀，必然有人写错。
- **工具的相邻数据**（如 protoc 的 well-known `.proto`）本来就在**包源码树**里，
  用已有的 `dep_dir("protobuf")` 拿，不需要 bin 目录。

### 5.4 target / toolchain / profile / linkage 的取值

| 轴 | 取值 | 理由 |
|---|---|---|
| target triple | **host**（`sub.target_triple = ""`） | INV-2 |
| 工具链 | **该包自己的 `[toolchain]`**，缺省才回落到主构建 spec 去掉 target 轴（`host_tc_for_build_program` 的同一规则） | INV-3：可执行文件与主构建零 ABI 接触，作者自己 pin 的工具链才是他验证过的 |
| profile | **固定 `release`** | 不随主构建 dev/release 摆动，否则同一台机器上 store 至少翻倍；工具跑得快对大 `.proto` 集有实际收益 |
| linkage / `--static` | **不继承** | 主构建的 `--static` 是对**交付物**的要求，与工具无关；musl 主机上的 static 默认仍由该包自己的解析决定 |
| 依赖解析 | 子构建**独立解析**（它自己的 `[dependencies]`，按 host） | INV-3。交叉构建下这本来就是必须的 |

> 这里有一个必须写进文档的推论：**同一个包可能在一次 `mcpp build` 里被构建两次**
> ——一次作为 target 库（主图），一次作为 host 工具（子构建）。它们是两个互不
> 影响的产物，不共享对象。native 构建下这看起来像浪费，但**正确性上无法合并**
> （feature 集不同：主图没有 `protoc` feature），而且合并会让 native 与 cross
> 走两条不同的路径——这正是这个代码库反复吃亏的「同一决策两处推导」。

### 5.5 store 布局与 key

布局（挂在既有 cache root 下，`mcpp cache gc/clean` 可顺带接管）：

```
<mcpp::home::cache_root()>/tool/<indexName>/<pkgFQN>@<version>/<keyHex>/
    bin/<tool><exeSuffix>
    entry.json
```

key 直接复用 `src/build/cache_key.cppm` 的轴，避免第二套 key 语义：

| 轴 | 内容 | 复用 |
|---|---|---|
| A 工具链 | host 编译器 id/version/driver identity、host triple、stdlib | `build_axes()` |
| B 语言 | C++ 标准 + flag、dialect flags、C 标准 | `build_axes()` |
| C profile | 固定 release（仍入 key，便于将来放开） | `build_axes()` |
| D 身份 | index 名、包 FQN、版本、**target 名** | `PackageAxes` + 新增字段 |
| E 自身配置 | 解析后的 feature 集、`[build]` flags、generated_files… | `PackageAxes` |
| F 上游 | 子构建依赖闭包的 key（Merkle，递归） | `cache_key` 已有 |
| G epoch | 新增 `kToolStoreEpoch` | 新增 |

**F 轴必须递归**（照 `cache_key.cppm` 抬头的论证）：protobuf 的某个依赖换版本
而 protoc 不重建，就是一个**静默错产物**。

### 5.6 递归、环、深度

工具包自己的 `build.mcpp` 可以再请求工具（合法：grpc 的 build.mcpp 要 protoc）。
用一个请求栈做环检测，并给一个深度上限（建议 4）+ 明确错误：

```
error: tool provisioning cycle:
  root → mcpplibs.grpc:grpc_cpp_plugin → compat.protobuf:protoc → mcpplibs.grpc:...
```

### 5.7 失败与可观测性

- 目标不存在 / 不是 `kind="bin"` → 报错并列出该包的全部 bin target
- 该包在 host 平台上不受支持（`[package].platforms`）→ 报错点名 host triple 与
  该包声明的 platforms，而不是让它在第 300 个 TU 上炸
- 首次构建必须显式告知代价（否则用户以为 mcpp 卡死）：

  ```
  Building host tool 'protoc' from compat.protobuf@35.1
    (once per (package version × host toolchain); cached at ~/.mcpp/build-cache/v1/tool/…)
  ```

- 子构建输出默认折叠（`ui::set_quiet`），**失败时全量回放**，并前缀标注是哪个
  工具的子构建——否则用户会以为是自己的工程编译失败。

---

## 6. 与 `[xlings] deps` / `xim:` 的边界（issue 决策点 3）

| 机制 | 面向 | 版本轴 | 何时用 |
|---|---|---|---|
| `[xlings] deps` | mcpp 包图**之外**的宿主工具：make、cmake、python、nasm | 独立（xim） | 工具不是任何 mcpp 包的产物 |
| **本方案 `tools = [...]`** | 包图**之内**、由该包源码产出 | **与包同轴**（INV-5） | 工具是某个 mcpp 包的 `kind="bin"` target |
| `xim:` 预编译工具包 + `[xlings] deps` | 源码在 mcpp 里构建不出来的工具 | 独立 | 兜底，仍然合法 |

docs/05 §2.13 与 docs/07 都要补这张表——issue 说得对，「值得写清楚各自的适用面」。

---

## 7. 拒绝的替代方案

| 方案 | 为什么拒绝 |
|---|---|
| **把依赖的 `kind="bin"` 提升为主图 link unit**（照抄 `sharedDepTargets`） | 撞 C1（时序：`build.mcpp` 早于 ninja 图）+ C2（交叉下产物不可执行）。**它是另一个独立特性**，可以单独做，但解决不了 #355 |
| **主图内嵌 host 子图 + ninja 里做 codegen 边** | 生成源必须在 modgraph 扫描（prepare 期）之前存在；ninja 期生成要走 dyndep。工程量数量级更大，而 `build.mcpp` 的机制已经够用 |
| **签入生成产物**（grpc-m 现状） | 对模板/example 正确，对真实用户不成立（`.proto` 会改），且版本匹配责任推给用户 |
| **`xim:grpc-tools` 独立包** | 破 INV-5：第二条版本轴，protoc 与 protobuf 运行时错配是最难查的一类问题。仍作为「源码构建不出来」的兜底保留 |
| **顶层 `[tools]` 别名表** | 第三套命名空间；`tools = [...]` 挂依赖上更省 |
| **`build.mcpp` 运行期申请工具**（`mcpp:need-tool=`） | 破 INV-1，且形成「跑一遍→发现缺→装→再跑一遍」的重入循环 |
| **子构建用子进程 `mcpp build`** | 隔离性确实更好，但需要额外的 CLI 面（`--project-root`/`--work-dir` 必须公开）、重复的工具链探测与索引读取，且错误只能靠文本回传。`prepare_build` 无跨调用状态（1.6），进程内递归更省且错误结构化。**若 review 更看重隔离性，这是可切换的实现细节，接口不变** |

---

## 8. 改动清单（估算）

**引擎**

| 文件 | 改动 | 规模 |
|---|---|---|
| `src/build/prepare.cppm` | `BuildOverrides` 两字段；`:842` 换根；`:199` `target_dir`；`:4690` lock；`:1786` projectEnv；`DependencyEdge.requestedTools`；供给 pass（`:3830` 后） | 中 |
| **新** `src/build/tool_store.cppm` | store 布局、key、entry.json、锁、`provision_tool` | 中 |
| `src/build/build_program.cppm` | `BuildProgramEnv.toolPaths`；`contract_env` 新变量 + 碰撞守卫；`build_dir()` 接 WorkDirs | 小 |
| `src/build/hostprogram.cppm` | `mcpp::dep_bin` 加进 `kMcppModuleSource` | 小 |
| `src/build/cache_key.cppm` | `PackageAxes` 加 target 名；`kToolStoreEpoch` | 小 |
| `src/build/compile_commands.cppm` | 输出路径由 plan 携带 | 小 |
| `src/pm/dep_spec.cppm` | `DependencySpec.tools` | 小 |
| `src/manifest/toml.cppm` | `tools` 解析 + 白名单 + 错误文案 | 小 |
| `src/manifest/xpkg.cppm` | `deps` 值支持表形；targets 补 `required_features` | 小-中 |
| `src/cli/cmd_build.cppm`（可选） | `--work-dir`；Phase 3 的 `mcpp tool` | 小 |

**文档**：`docs/05-mcpp-toml.md`（`tools`、`required_features` 的方向反转、
§2.13 边界表）、`docs/07-build-mcpp.md`（`dep_bin`、环境契约表、host 语义、
「同一个包可能被构建两次」）、两份 `docs/zh/` 同步。

**生态**：`compat.protobuf` 加 `[features.protoc]` + `[targets.protoc]`；
`grpc-m` 加 `[features.codegen]`（`src/compiler/**`）+ `[targets.grpc_cpp_plugin]`，
并把 README 的「Code generation」从「手工签入」改成 `tools = [...]` + `build.mcpp`。

---

## 9. 测试计划

**单测**：store key 的稳定性与敏感性（改 feature / 改上游 key / 改 host 工具链
必须换 key；改主构建 `--target` / profile **不得**换 key）；env 变量 sanitize
与碰撞守卫。

**e2e**（新增，遵守 `# requires:` 必须在第 2 行的既有规则）：

1. **最小闭环**：fixture 包 A 有 `kind="bin"` 的 `codegen`（一个把 `.txt` 变成
   `.cpp` 的十行程序）；工程 B 声明 `a = { path = "...", tools = ["codegen"] }`，
   `build.mcpp` 调它生成源并 `mcpp::generated()`。断言构建通过且产物行为正确。
2. **默认零成本**：同一个 fixture，不写 `tools` → 断言 `codegen` **没有**被构建
   （store 目录为空），且构建输出与基线一致（INV-6）。
3. **成本门**：`required_features` 门控的 target，不激活时子构建报错并列出可用
   target；激活时通过。
4. **store 命中**：连跑两次，第二次断言无编译、输出出现 cached 语义。
5. **交叉语义（本方案最重要的一条断言）**：复用 `102_mingw_cross_wine.sh` 的环境，
   `mcpp build --target x86_64-windows-gnu`，断言
   **产物是 PE 而工具是 host ELF**（`file` / 魔数）。这条不过 = INV-2 没落地。
6. **环检测**：两个 fixture 互相请求对方的工具 → 断言报出环而不是挂死。

> 教训回填（`link-argv-max-arg-strlen` / `explicit-ninja-goals-two-regressions`）：
> **CI 全绿不等于生态可用**。合入前必须在本机跑一次真实的 `compat.protobuf`
> protoc 子构建（400+ TU 的真实规模），验证时间、磁盘、并发锁三项。

---

## 10. 风险与未决

| 风险 | 说明 | 处置 |
|---|---|---|
| **首次构建时间** | protoc ≈ libprotobuf + libprotoc；grpc_cpp_plugin 更大。分钟级 | 全局 store 摊薄；显式进度文案；Phase 2 的 prebuilt provider 是真正的解法 |
| **磁盘** | 每个 (包版本 × host 工具链) 一份 | 构建成功即删 `build/`；接入 `mcpp cache gc` |
| **子构建的 `[xlings]`** | 若工具包声明了 `[xlings] deps`，会写 `.mcpp/` —— Phase 0 已把它导向 work dir，但 xlings 侧的沙箱语义需实测 | 实施前先验（记忆里 `HOME=` 不是 xlings 沙箱，必须用 `XLINGS_HOME` 且**先验证再用**） |
| **并发** | 两个工程同时首次要 protoc | store 目录文件锁 + 临时目录 rename |
| **`prepare_build` 重入** | 函数 4784 行；虽无 static，但 `ui` 全局 quiet 是进程级 | 用 RAII 保存/恢复 quiet；若 review 认为风险仍高，切子进程实现（§7 末行） |
| **lockfile 可复现性** | 消费者的 `mcpp.lock` 目前不记录工具 store key | 本次不做，列为后续（工具是可执行文件，不进链接，复现性影响低于库依赖） |
| **Form B 描述符表达力** | `deps` 值改表形是解析器扩展，可能牵出别的 Form B 用法 | 只在值位置支持表，不动 namespaced 子表（保持原注释的边界） |

---

## 11. 落地顺序建议

1. **Phase 0** 单独一个 PR（默认值不变 → diff 可用「构建产物逐字节相同」验证）
2. **Phase 1 引擎** 一个 PR（含最小 fixture e2e 1/2/4/6）
3. **交叉 e2e（第 5 条）** 与 Phase 1 同 PR —— 它是 INV-2 的唯一证明，不能后补
4. **索引侧**：`compat.protobuf` 的 `protoc` target（先验证 protoc 真能被 mcpp
   从源码构建出来——这一步有独立风险，见下）
5. **grpc-m**：`grpc_cpp_plugin` + README 改写 + example 去掉签入产物
6. **Phase 3 `mcpp tool`**（可选）

> **第 4 步的独立风险**：`compat.protobuf` 今天明文「does NOT build libprotoc」。
> libprotoc 能否被 mcpp 无 CMake 构建出来（是否有 `.h.in` / 生成步骤 / 额外依赖）
> **尚未核实**。这一步应先做一次纯手工验证再开 PR；若失败，Phase 2 的
> prebuilt provider 会从「优化」升级为「protoc 唯一可行路径」，需要提前重排。
> —— §12.4 的「逃生舱」把这条风险从**阻塞项降级为优化项**。

---

## 12. 行业调研（2026-08-05）

### 12.1 两条正交的轴

| 轴 | 问题 | 行业状态 |
|---|---|---|
| **A：host/target 分离** | 工具该为哪台机器构建、怎么声明、怎么交付 | **已收敛**，八个系统答案一致 |
| **B：codegen 放在哪一层** | pre-pass（构建前跑一次的程序） vs 图节点（build graph 里的一条边） | **未收敛**；只有 Cargo 站 pre-pass，其余全站图节点 |

本设计站在轴 A 的行业共识上。mcpp 在轴 B 上目前是 Cargo 派（`build.mcpp` = `build.rs`），
这是一个独立的、更长期的方向问题，见 §12.5。

### 12.2 轴 A：各家机制对照

| 系统 | 机制 | 声明位置 | 版本轴 | 交付方式 |
|---|---|---|---|---|
| **Nix** | `depsBuild{Build,Host,Target}` / `depsHost{Host,Target}` / `depsTargetTarget` 六格 + splicing；`nativeBuildInputs` = `depsBuildHost` | 消费方依赖列表（**按类型分格**） | 同一 nixpkgs 求值 | `pkgsBuildHost.*` 进 PATH |
| **vcpkg** | 依赖上 `"host": true`；`VCPKG_CROSSCOMPILING`；host-only port 用 `"native"` supports 表达式 | 消费方 `vcpkg.json` 的依赖项 | 同一 registry baseline | `CURRENT_HOST_INSTALLED_DIR`；`VCPKG_USE_HOST_TOOLS` 加进 `CMAKE_PROGRAM_PATH` |
| **Conan 2** | `requires` (host context) + `tool_requires` (build context)，双 profile `-pr:b`/`-pr:h` | 消费方 recipe | **`protobuf/<host_version>` 占位符锁同版本** | 环境 / `VirtualBuildEnv` |
| **Cargo** | 稳定：`[build-dependencies]` 恒为 host；不稳定：artifact deps（RFC 3028，**仍 nightly `-Z bindeps`**） | 消费方 `Cargo.toml` 的依赖项 | 同一 lock | `CARGO_BIN_FILE_<DEP>_<NAME>` / `CARGO_BIN_DIR_<DEP>` |
| **xmake** | `add_deps("pkgconf", {host = true})`；**`if package:is_binary() then requireinfo.host = true`**（二进制包自动是 host 包） | 依赖边 | 同一 repo | `package:addenv("PATH","bin")` |
| **Bazel** | attribute 上 `cfg = "exec"`（原 `cfg="host"`，迁移中）；genrule 的 `tools` 属性 | **依赖边（attribute）级** | 同一 WORKSPACE | 直接作为 action 的 executable |
| **Meson** | 全 API 一个 `native: true`（`executable` / `find_program` / `dependency` / compiler）；cross file 分 build/host machine | 调用点 | — | `custom_target` 直接引用 |
| **CMake** | **无一等机制**：`add_executable(t IMPORTED)` + `if(NOT CMAKE_CROSSCOMPILING)`；各项目自造（Qt `QT_HOST_PATH`、LLVM `LLVM_NATIVE_TOOL_DIR`、protobuf `Protobuf_PROTOC_EXECUTABLE`） | 每项目一套变量 | 用户自己保证 | 变量指路径 |

### 12.3 提炼出的最佳实践，及本设计的符合度

| # | 实践 | 本设计 |
|---|---|---|
| 1 | host 工具是**依赖边的属性**，声明在消费方 | ✅ `tools = [...]` |
| 2 | **单一版本轴**（Conan `<host_version>` 是这条的成名解法） | ✅ INV-5，且天然成立（tools 挂在同一条依赖上） |
| 3 | **binary 包/target 默认即 host**（xmake） | ⚠️ 见 §12.4 采纳 |
| 4 | **native 不特殊化**（同一条代码路径，不因 host==target 分叉） | ✅ INV-2 |
| 5 | 工具通过**路径/env** 交付，不通过链接 | ✅ `MCPP_DEP_<PKG>_BIN_<TOOL>` |
| 6 | **给一个逃生舱**：允许指定现成的 host 工具，跳过构建 | ❌ 见 §12.4 采纳 |
| 7 | codegen 应是**图节点**而非 pre-pass | ❌ mcpp 是 Cargo 派，见 §12.5 |

**最有信息量的两个反面数据点：**

- **Cargo 的 artifact dependencies（RFC 3028）自 2021 年 accept 至今仍未稳定**，只能
  `-Z bindeps`。说明「让依赖交出二进制」的语义细节极多（多 artifact 类型、profile
  继承、feature 统一、跨 target 的重复构建）。mcpp 应当**抄它的结论，不抄它的规模**：
  只做 `bin`，不做 `cdylib`/`staticlib`；不做多 target 请求。
- **xmake 有 host 包机制，但 protoc 在交叉下依然是断的**：
  `xmake-repo/packages/p/protobuf-cpp/xmake.lua:99` 只在 `not package:is_cross()`
  时把 `bin` 加进 PATH，`:218` 交叉时直接 `os.tryrm(installdir("bin/*.exe"))`；而
  `xmake/rules/protobuf/proto.lua:36` 用 `find_tool("protoc", {envs})` 从 **PATH 查找**，
  不是图依赖。说明难的不是机制本身，而是**把机制真的接到 codegen 规则上**——
  mcpp 若按本设计落地，这一点上会领先 xmake。

### 12.4 据此对本设计做的两处调整

**调整 A：`kind="bin"` 的 target 在工具语境下默认按 host 解析（xmake 实践 3）。**
本设计原本就把「工具永远是 host 产物」写进 INV-2，这里只是把 xmake 的措辞采纳为
文档表述：`tools = [...]` 请求的必然是 host 产物，**不提供 target 侧的变体**
（对照 Cargo 的 `target = "target"` 逃生舱——那是 Cargo 因为 artifact deps 还要服务
「把二进制打进产物」这个用例才需要的；mcpp Phase 1 不做那个用例，就不需要这个轴）。

**调整 B（新增，实践 6）：逃生舱 —— 允许指定现成的 host 工具。**

```toml
[tools.overrides]                       # 或等价的 CLI / 环境形式
"compat.protobuf:protoc" = "/usr/bin/protoc"
```

以及等价的环境变量 `MCPP_TOOL_<PKG>_<TOOL>=<path>`（CI / 发行版打包友好）。
命中覆盖时 **完全跳过子构建**，直接把该路径填进 `MCPP_DEP_*_BIN_*`。

为什么这一条必须进 Phase 1 而不是以后再说：

- 它是**全行业统一的逃生舱**（vcpkg `VCPKG_HOST_TRIPLET`、CMake
  `LLVM_NATIVE_TOOL_DIR`、Qt `QT_HOST_PATH`、Cargo `target = "target"`），
  没有它，用户在「工具编不出来 / 编太慢 / 我已经有一个」时无路可走；
- 它把 §11 第 4 步那条**未核实风险**（libprotoc 能否被 mcpp 无 CMake 构建）
  从**阻塞项降级为优化项**：即使 libprotoc 一时构建不出来，
  `[tools.overrides]` + upstream 官方 protoc 也能让整条链先跑通；
- 它同时是 Phase 2（prebuilt-asset provider）的**手动版本**——两者共用同一个
  store 出口，验证了「store 是接口」这个分层是真的。

代价：override 的路径不进 store key，因此**不参与可复现性**。必须在文档里点名
这是逃生舱而不是常规路径，且 `mcpp doctor` 应报告当前生效的所有 override。

### 12.5 轴 B：mcpp 的长期方向（**不在本设计范围**，仅记录评估）

Zig 的形态是这条轴的标杆，而 `build.mcpp` 的祖宗正是 `build.zig`：

```zig
const tool = b.addExecutable(.{ .root_module = b.createModule(.{ .target = b.graph.host }) });
const run  = b.addRunArtifact(tool);
run.addFileArg(b.path("input.json"));
const out  = run.addOutputFileArg("generated.zig");   // → LazyPath
exe.root_module.addAnonymousImport("generated", .{ .root_source_file = out });
```

关键差异：`LazyPath` 让产物路径**在执行时才解析**，于是 codegen 是**图里的一条边**，
自动获得增量、并行、缓存。mcpp 抄了 build.zig 的「用同语言写构建程序」，
**没抄「构建程序产出的是图节点，而不是副作用」**。

对 mcpp 而言，中间态是可行的，且**可行性已在代码里核实**：

- mcpp 已有 **ninja 期** 的 P1689 扫描（`ninja_backend.cppm:844` `rule cxx_scan`、
  `:486` `rule cxx_dyndep`），模块依赖边本来就在 ninja 期解析；
- `.pb.cc` / `.pb.h` **不是模块**，它们只需要「文件名集合在 prepare 期可预测」，
  而这正是一条声明式 codegen 规则能给的（输入 glob + 命名规则 → 输出名集合）；
- 只有生成的 `.cppm`（模块接口）需要 prepare 期就有内容 —— 这类保持走
  `build.mcpp` / `generated_files` 现有路径。

因此中期方向是 **`[codegen]` 声明式规则**：prepare 期只算文件名集合（不执行），
ninja 期执行、增量、并行；工具从本设计的 tool store 取。收益是 pre-pass 永远给不了的
（改一个 `.proto` 只重生成一个）。

再往前一步（`build.mcpp` 升级为构建图 DSL，返回 LazyPath）需要把行式 stdout 协议
换成结构化图协议，与现有 directive 协议不兼容。**评估结论：不做**——
声明式 `[codegen]` 已经拿到其中 ~80% 的收益。

### 12.6 参考

- Nix：<https://nixos.org/manual/nixpkgs/stable/#chap-cross>（`depsBuild*` 六格与 splicing）
- vcpkg：<https://learn.microsoft.com/en-us/vcpkg/users/host-dependencies>
- Conan 2：<https://docs.conan.io/2/examples/graph/tool_requires/using_protobuf.html>
- Cargo RFC 3028：<https://rust-lang.github.io/rfcs/3028-cargo-binary-dependencies.html>；
  跟踪 issue <https://github.com/rust-lang/cargo/issues/9096>
- Cargo build scripts：<https://doc.rust-lang.org/cargo/reference/build-scripts.html>
- xmake：`xmake/modules/private/action/require/impl/package.lua:656-660`、
  `xmake/rules/protobuf/proto.lua:36`、`xmake-repo/packages/p/protobuf-cpp/xmake.lua:99,218`
- Bazel exec transition：<https://github.com/bazelbuild/proposals/blob/main/designs/2019-02-12-execution-transitions.md>
- Meson cross：<https://mesonbuild.com/Cross-compilation.html>
- CMake cross：<https://cmake.org/cmake/help/book/mastering-cmake/chapter/Cross%20Compiling%20With%20CMake.html>；
  LLVM `LLVM_NATIVE_TOOL_DIR` <https://reviews.llvm.org/D131052>
- Zig build system：<https://ziglang.org/learn/build-system/>

---

## 13. 实施与真实案例验证（2026.8.5.1）

### 13.1 §11 第 4 步的「未核实风险」——已核实，结论是**能**

设计里写着：`compat.protobuf` 描述符明文「does NOT build libprotoc」，而
**libprotoc 能否被 mcpp 无 CMake 构建出来尚未核实**；若不能，Phase 2 的
prebuilt provider 就从「优化」升级为 protoc 的唯一可行路径。

拿真实的 mcpp-index + protobuf 35.1 源码实测：

- 上游 `src/file_lists.cmake` 的 `libprotoc_srcs` = **138 项**，与 libprotobuf 的
  源码集**零重叠**（`importer.cc` / `parser.cc` 早已在 libprotobuf 里）
- 源码树里**没有** `.h.in` / `.cmake.in` —— 不需要任何 configure 步骤
- 138 个 TU 全部编过；第一次链接失败，缺 `upb_*` 符号 —— libprotoc 的 upb 生成器
  需要 upb 运行时，而那正是 compat.protobuf **已有**的 `upb` feature
- 把 target 写成 `required_features = { "protoc", "upb" }` 后**链接通过**
  —— 这正是成本门机制该起的作用，**零引擎改动**

端到端：`protoc` 从源码建出 → `mcpp::dep_bin("protobuf","protoc")` 拿到路径 →
`action` 调用它生成 `demo.pb.cc` / `demo.pb.h` → 编译链接 → 程序输出 `NAME=mcpp`。
store 命中实测：第二次 `rm -rf target` 后构建 **1.41s**，不重建 protoc。

**⇒ Phase 2（prebuilt-asset provider）确认为纯优化，不是必需路径。**

### 13.2 真实案例暴露的三个 bug（合成 e2e 全部漏掉）

四个新 e2e 都用 path 依赖，也就是 **Form A**（包自带 mcpp.toml）。真实索引里绝大
多数是 **Form B**（compat 描述符，manifest 由 `.lua` 合成），protobuf 就是。这个
差异一次性暴露了三个 bug：

| # | 问题 | 后果 |
|---|---|---|
| 1 | 子构建从 `<root>/mcpp.toml` 读 manifest，**Form B 包没有这个文件** | Form B 包**完全不能**当工具提供方 —— gRPC 那条链整个走不通 |
| 2 | `targets.<x>.main` **不展开** `*/` 包装 glob（`sources` 一直会展开） | Form B 包的任何 bin target 都拿不到入口源码 |
| 3 | `role=source` 的**全部**输出都被当成翻译单元 | protoc 的 `.pb.h` 与 `.pb.cc` 撞同一个对象路径 |

修法分别是：`BuildOverrides::preloaded_manifest`（由调用方交出**未经 feature 激活**
的那份 manifest —— `packages[i].manifest` 是被 `apply()` 改过的副本，从它出发会把
同一批 feature 源码折两次）、在 manifest 定稿后解析 `main` 的 glob、
`is_compilable_output()` 按扩展名过滤。

**方法论**：合成测试测的是「我想到的形状」，真实案例测的是「现实的形状」。这三个
bug 没有一个能靠再多写几个 path-依赖 e2e 发现。

### 13.3 一个 GCC modules 约束

`preloaded_manifest` 初版写成 `std::optional<Manifest>`，而 `BuildOverrides` 是
**导出**结构体 —— GCC 写不出 module cluster：

```
mcpp.build.prepare: error: failed to read compiled module cluster 529: Bad file data
src/build/execute.cppm:52:45: fatal error: failed to load pendings for 'std::pair'
```

`rm -rf target` 无效（不是陈旧 BMI）。换成 `shared_ptr<const Manifest>` 让导出布局
保持 trivial 后即通过，顺带省掉每次工具构建的一次 manifest 拷贝。
**导出结构体里不要放大的值类型。**
