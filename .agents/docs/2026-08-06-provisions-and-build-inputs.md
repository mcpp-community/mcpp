# 依赖提供物与构建期输入:两个缺口,同一个形状

> 状态：**设计待 review**
> 关联：[#359](https://github.com/mcpp-community/mcpp/issues/359)（由 grpc-m 的真实使用暴露）
> 涉及：`src/modgraph/scanner.cppm`（`UsageRequirements`）、`src/build/prepare.cppm`、
> `src/build/build_program.cppm`、`src/build/directives.cppm`

---

## 0. 目标:用户侧从 4 条依赖 + 逐个列名,降到 1 条 + 1 行

grpc-m 今天要求用户写:

```toml
[dependencies.mcpplibs]
grpc        = "1.83.0"
grpc-plugin = { version = "1.83.0", tools = ["grpc_cpp_plugin"] }
grpcgen     = { version = "1.83.0", host-module = true }
[dependencies.compat]
protobuf    = { version = "35.1", tools = ["protoc"] }
```

```cpp
import mcpp; import grpcgen;
int main() { return grpcgen::generate({"helloworld"}) ? 0 : 1; }
```

后三条**全是为了 codegen**,而且要求用户知道「gRPC 的代码生成需要 protobuf 的 protoc」——这是**库该承担的知识**。目标形态:

```toml
grpc = { version = "1.83.0", features = ["codegen"] }
```
```cpp
import mcpp; import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }   // 扫 proto/**
```

对照业界:xmake 是 `add_requires("grpc")` + `add_files("proto/*.proto")`;CMake+vcpkg 是 1 条依赖 + `protobuf_generate(...)`。达到目标形态后 mcpp **严格更优** —— 因为它还额外保有「版本错配不可表达」与「交叉编译构造上正确」这两条别人没有的性质。

两个缺口各挡住一半,**缺一个都到不了**。

## 1. 关键发现:模型已经存在,新东西没接进去

mcpp 早有一套「依赖能提供什么 × 提供给谁」的模型(`src/modgraph/scanner.cppm`):

```cpp
struct UsageRequirements {
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::filesystem::path> includeDirsAfter;
    std::vector<std::string>           cflags, cxxflags, ldflags, modules;
};

struct PackageRoot {
    UsageRequirements privateBuild;   // 只给自己
    UsageRequirements publicUsage;    // 沿边传给消费者
    UsageRequirements linkUsage;      // 链接期
};
```

include dirs、defines、link flags、modules 全都通过它传播,规则清楚、单点定义。

**#355 引入的两种新提供物没有进入这个模型**:

| 提供物 | 在模型里? | 实际实现 |
|---|---|---|
| include dirs / defines / ldflags / modules | ✅ `UsageRequirements` | 按作用域传播 |
| **host 工具**(`tools = [...]`) | ❌ | `prepare.cppm:4113` 硬编码 `toolEnvByConsumer[edge.consumerPackageIndex]` —— 只给**发出请求的那条边**的消费者 |
| **host 模块**(`host-module = true`) | ❌ | `prepare.cppm:4016` 只遍历 `m->dependencies`,即**只认 root 的**直接依赖 |

于是「库代用户拉起整条 codegen 工具链」在架构上不可能:工具**被构建了**,但环境变量记在库的账上,消费者的 `build.mcpp` 看不见。

> 实测确认(不是推断):一个 path 依赖在自己的 manifest 里写 `compat.protobuf = { tools = ["protoc"] }`,消费者 `mcpp::dep_bin("protobuf","protoc")` 拿到**空串**,`dep_dir` 同样为空。

**根因不是「少了一次传播」,而是:新增一种提供物时,没有任何地方逼你回答「它怎么传播」。** 这与本仓库反复付学费的「同一决策在 N 处推导」是同一形状的镜像——一个必答问题在**零处**被表达。`directives::kTable` 已经用「Scope 是必填字段」解过一次。

## 2. 缺口 B 同构:输入声明的种类是封闭的

`build.mcpp` 的缓存键由**声明过的输入**构成,而输入只有两种形态:

```cpp
// build_program.cppm:283
os << "in " << hash_file(abs_against_root(root, f)) << ' ' << f << '\n';   // 文件内容
os << "env " << hash_string(env_value(e)) << ' ' << e << '\n';             // 环境变量
```

`hash_file` 读的是**文件内容**。于是「我的输出取决于这个目录里有哪些文件」**无法表达**:

- 对目录调用 `rerun_if_changed` 无效(目录没有可读内容);
- 新增一个 `.proto` 不改变任何已声明文件的哈希 → build.mcpp 不重跑 → **新文件静默不生成**。

实测:glob `proto/**` 后新增 `fresh.proto`,`Finished dev in 0.01s`,产物 0 个。这比「要求用户列名字」更坏,所以 grpc-m 最终选了显式列表。

同样的形状:**新增一种输入时,没有地方回答「它的指纹怎么取」。**

## 3. 设计

一条主张:**两个缺口都收敛成「表 + 必答字段」,与 `directives::kTable` 同一范式**,而不是各打一个补丁。

### D1. 提供物进 `UsageRequirements`,传播由作用域决定

```cpp
struct UsageRequirements {
    // …既有字段…
    // #359: host 工具与 host 模块。放在这里而不是旁路,是为了让「它怎么
    // 传播」由所在的作用域回答,与 includeDirs 完全同一条规则。
    std::vector<ToolProvision>       tools;
    std::vector<HostModuleProvision> hostModules;
};
```

- 放进 `privateBuild` → 只有该包自己的 `build.mcpp` 能用;
- 放进 `publicUsage` → 沿 **public 边**传给消费者。

于是 `grpc` 可以在描述符里声明「我的 codegen feature 对外提供 protoc 与 grpc_cpp_plugin」,消费者只写一条依赖。

**三条必须写死的语义**,否则这会变成一个安全与可维护性的洞:

1. **传播的是「可见性」,不是「自动执行」。** `dep_bin()` 只返回路径;跑不跑由消费者的 `build.mcpp` 决定。传播不改变「谁构建了这个工具」,也不改变 tool store 的键。
2. **必须显式声明,不能默认传播。** 默认传播意味着任意深层依赖都能往消费者的工具命名空间里塞东西——那是供应链问题。库要对外提供,必须自己写明(与 `include_dirs` 默认 private、要 public 得显式是同一条纪律)。
3. **命名冲突用包名消歧**,`dep_bin(pkg, tool)` 本来就是两段式,无需新语法。

> 顺带修掉一个相邻缺陷:`dep_dir()` 目前只覆盖**直接**依赖,所以传递依赖的数据文件目录取不到(protoc 的 well-known types 就是这么一个目录)。它应与 tools 走同一条传播规则。

### D2. 输入种类进表,指纹由种类决定

```cpp
enum class InputKind {
    File,       // 内容哈希(现有)
    Directory,  // 递归成员集合:相对路径 + size + mtime,不读内容
    Env,        // 环境变量(现有)
};
```

`Directory` 的指纹**只取集合**,不取内容——内容变化由集合里的 `File` 条目负责。这与 Cargo 的 `cargo:rerun-if-changed=<dir>` 是同一个解。

补上之后 glob 从「结构性不安全」变成一等用法,规则包才能提供 `generate_all()`:

```cpp
mcpp::rerun_if_changed_dir("proto");   // 集合变了就重跑
```

**代价要写明**:目录指纹用 mtime,而 mtime 在某些场景(容器构建、git checkout)不稳定。因此:
- 只把**成员集合**纳入指纹,不把内容纳入 → 误重跑的代价只是一次 build.mcpp 重跑(秒级),不是全量重编;
- 不递归进符号链接(与既有扫描一致)。

### D3. 为什么这两条必须一起做

只做 D1:用户从 4 条降到 1 条,但仍要在 `build.mcpp` 里逐个列 `.proto`。
只做 D2:用户不必列 proto,但仍要写 4 条依赖并知道 gRPC 需要 protobuf 的 protoc。

**两条合起来**才是目标形态,也才是「对齐并超过业界」的那一步。

## 4. 实施步骤

| 步 | 内容 | 风险 |
|---|---|---|
| 1 | `UsageRequirements` 加 tools / hostModules 两个字段,`privateBuild` 行为保持今天不变 | 低,纯新增 |
| 2 | 沿 public 边聚合(复用 features 的边聚合路径,#242/#243 已有先例) | 中——要确认不会把 private 依赖的工具泄漏出去 |
| 3 | 描述符/manifest 侧:声明「对外提供」的语法 | 中——是新的用户可见语法,需按 Schema Ownership Principle 审 |
| 4 | `dep_dir()` 覆盖传递依赖 | 低 |
| 5 | `InputKind` 表 + `Directory` 指纹 + `rerun_if_changed_dir` | 低 |
| 6 | grpc-m 侧改成 1 条依赖 + `generate_all()`,作为真实验证 | —— |

步 1–4 是缺口 A,步 5 是缺口 B,步 6 是端到端证据。

## 5. 验证

- **单测**:传播规则(private 不外泄、public 沿边传、冲突消歧)、目录指纹(增删文件变、改内容不变、mtime 抖动不误伤集合)。
- **e2e**:一个库对外提供工具 + 一个消费者只写一条依赖就能在 `build.mcpp` 里 `dep_bin` 到;新增一个文件后 glob 场景确实重跑。
- **真实场景**:grpc-m 的模板降到 1 条依赖 + 1 行 build.mcpp,且生成产物仍与官方 protoc 逐字节相同(该基线已在 2026.8.5.x 建立)。

## 6. 明确不做

- **不让传播默认开启**。库必须显式声明对外提供,理由见 D1 第 2 条。
- **不把目录内容纳入指纹**。那会把一次 build.mcpp 重跑放大成全量重编,而收益为零(内容变化本来就由 File 条目覆盖)。
- **不引入「工具版本独立于依赖版本」的语法**。单一版本轴正是「错配不可表达」的来源,是本设计要保住的性质。
- **不在本轮解决 windows 的工具子构建失败**(见 mcpp-index 的 compat.protobuf windows 块):那是独立缺陷,原因尚未定位。
