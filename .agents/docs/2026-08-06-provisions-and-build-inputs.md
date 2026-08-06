# 依赖提供物与构建期输入:两个缺口,同一个形状

> 状态：**已定案，实施中**
> 关联：[#359](https://github.com/mcpp-community/mcpp/issues/359)（由 grpc-m 的真实使用暴露）
> 涉及：`src/build/provisions.cppm`（新增）、`src/build/prepare.cppm`、
> `src/build/build_program.cppm`、`src/build/directives.cppm`、
> `src/pm/dep_spec.cppm`、`src/manifest/{types,toml,xpkg}.cppm`

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

后三条全部服务于 codegen,并且要求用户知道「gRPC 的代码生成需要 protobuf 的 protoc」——这是库该承担的知识。目标形态:

```toml
grpc = { version = "1.83.0", features = ["codegen"] }
```
```cpp
import mcpp; import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }   // 扫 proto/**
```

对照业界:xmake 是 `add_requires("grpc")` + `add_files("proto/*.proto")`;CMake + vcpkg 是 1 条依赖 + `protobuf_generate(...)`。达到目标形态后 mcpp 严格更优,因为它额外保有「工具与运行时版本错配不可表达」与「交叉编译构造上正确」两条性质。

两个缺口各挡住一半,缺一个都到不了。

## 1. 关键发现:一个必答问题在零处被表达

mcpp 早有一套「依赖能提供什么 × 提供给谁」的模型(`src/modgraph/scanner.cppm`):

```cpp
struct UsageRequirements {
    std::vector<std::filesystem::path> includeDirs, includeDirsAfter;
    std::vector<std::string>           cflags, cxxflags, ldflags, modules;
};
struct PackageRoot {
    UsageRequirements privateBuild;   // 编译自己时用
    UsageRequirements publicUsage;    // 沿边流向消费者
    UsageRequirements linkUsage;      // 链接期
};
```

#355 引入的两种新提供物没有进入任何等价模型:

| 提供物 | 传播规则在哪 | 实际行为 |
|---|---|---|
| include dirs / defines / ldflags / modules | `UsageRequirements` + 不动点(`prepare.cppm:2906`) | 按作用域传播 |
| host 工具(`tools = [...]`) | 无 | `prepare.cppm:4122` 只发给发出请求的那条边的消费者 |
| host 模块(`host-module = true`) | 无 | `prepare.cppm:4016` 只遍历 root 的 `dependencies` |
| 依赖目录(`dep_dir()`) | 无 | `fillDepDirs` 只覆盖直接依赖 |

于是「库代用户拉起整条 codegen 工具链」在架构上不可能:工具被构建了,但环境变量记在库的账上,消费者的 `build.mcpp` 看不见。

> 实测确认:一个 path 依赖在自己的 manifest 里写 `compat.protobuf = { tools = ["protoc"] }`,消费者 `mcpp::dep_bin("protobuf","protoc")` 拿到空串,`dep_dir` 同样为空。

**根因不是少了一次传播,而是新增一种提供物时,没有任何地方逼你回答「它怎么传播」。** 这与本仓库反复付学费的「同一决策在 N 处推导」是同一形状的镜像——一个必答问题在零处被表达。`directives::kTable` 已经用「Scope 是必填字段」解过一次。

## 2. 缺口 B 同构:输入声明的种类是封闭的

`build.mcpp` 的缓存键由声明过的输入构成,而输入只有两种形态:

```cpp
// build_program.cppm:283
os << "in "  << hash_file(abs_against_root(root, f)) << ' ' << f << '\n';   // 文件内容
os << "env " << hash_string(env_value(e))            << ' ' << e << '\n';   // 环境变量
```

`hash_file` 读的是文件内容,于是「我的输出取决于这里有哪些文件」无法表达:新增一个 `.proto` 不改变任何已声明文件的哈希,build.mcpp 不重跑,新文件静默不生成。实测:glob `proto/**` 后新增 `fresh.proto`,`Finished dev in 0.01s`,产物 0 个。这比「要求用户列名字」更坏,所以 grpc-m 最终选了显式列表。

同样的形状:新增一种输入时,没有地方回答「它的指纹怎么取」。

---

## 3. 设计

一条主张:两个缺口都收敛成「表 + 必答字段」,与 `directives::kTable` 同一范式。

### D1. 提供物进一张 `ProvisionKind` 表,传播由 `reexport` 决定

#### D1.1 容器:新模块,不是 `UsageRequirements`

`UsageRequirements` 描述的是消费者**编译**时的命令行,进的是 ninja 边;提供物描述的是消费者的 **build.mcpp 程序**能看见什么,进的是 prepare 期子进程的环境。两者的消费者、时机、失效条件都不同,合并会让 `privateBuild / publicUsage / linkUsage` 这三个作用域名字对新字段失去意义(`linkUsage.tools` 没有含义)。

因此新增 `src/build/provisions.cppm`,持有:

```cpp
enum class ProvisionKind { Tool, HostModule, DepDir };
```

每一种在表里必须给出三件事:环境变量的形态、裸名是否可用、传播是否需要 `reexport`。新增第四种提供物时,这张表逼你回答同样三个问题——这正是 §1 的病所缺的那个位置。

分模块而非并进 `prepare.cppm` 的匿名命名空间,理由与 `directives.cppm` 头部记录的一致:该匿名命名空间在 clang 22 + C++20 modules + `-O2` 下会破坏邻居(PR#332 / PR#334)。

#### D1.2 语法:`reexport = true`,一个布尔,覆盖三种提供物

```toml
# grpc 的描述符
[feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"],            reexport = true }
grpc-plugin       = { version = "1.83.0", tools = ["grpc_cpp_plugin"], reexport = true }
grpcgen           = { version = "1.83.0", host-module = true,          reexport = true }
```

默认 `false`,即今天的行为。选它而不是新开 `[provides]` 表,理由:

1. `tools` / `host-module` 已经是这条边上的请求字段,「是否再导出」是同一条边的属性。另开一张表会让「谁请求 / 谁提供」在两处推导——正是本设计要治的病。
2. 一个词覆盖三种提供物。grpc 的三条依赖各写一次 `reexport = true`,而不是三种不同的键。
3. `reexport` 在 C++20 模块里就是 `export import` 的名字,读者不需要学新概念。
4. 粒度已经是 per-edge:库要只导出 protoc 而不导出规则模块,把 `reexport` 写在那一条边上即可。per-kind 的更细粒度是 YAGNI。

##### 为什么不能复用边的 `visibility`

`parseVisibility`(`prepare.cppm:2701`)与 `DependencyEdge::visibility`(`:2670`)**默认都是 Public**。「放进 `publicUsage`、沿 public 边传」落地后就是默认传播,与「必须显式声明」直接冲突。

同时更正设计初稿里一处对本仓库的事实误述:manifest 里的 `include_dirs` **并非默认 private**——`prepare.cppm:2868` 把 `publicUsage.includeDirs = privateBuild.includeDirs`,它们默认就是 public 的;真正 private-only 的是 `build.mcpp` 注入的那些(`:2742`)。所以「和 include_dirs 同一条纪律」这个类比不成立,`reexport` 必须是独立的、默认关闭的位。

##### 传播规则(不动点,与 include dirs 同构)

```
own(P→D)      = 该边上请求的 tools / host-module,以及 D 本身的目录
exported(P)   = ⋃ {P→D : edge.reexport} [ own(P→D) ∪ exported(D) ]
visible(P)    = ⋃ {P→D}                 [ own(P→D) ∪ exported(D) ]
```

即:一条边写了 `reexport`,就把「这条边提供的东西,以及 D 转手给我的东西」继续交给 P 的消费者;而 P 自己总能看见所有直接依赖提供给它的东西。单调、可用不动点求解,和 `prepare.cppm:2906` 那个循环同形。

三条必须写死的语义:

1. **传播的是可见性,不是自动执行。** `dep_bin()` 只返回路径,跑不跑由消费者的 `build.mcpp` 决定。传播不改变「谁构建了这个工具」,也不改变 tool store 的键。
2. **provision 的存在性绑定 feature 激活**,与 `featureDefines`(`prepare.cppm:3812`)同一时机。feature 关闭时不应有任何工具被构建。
3. **`reexport` 只在依赖侧有意义。** root 写它无害但无效果(root 没有消费者)。

##### feature-dep 与已声明依赖同键时按加法合并

`mergeActiveFeatureDeps` 原本用 `try_emplace`,即「键已存在就丢弃 feature 的
spec」。gRPC 恰好是反例:它**无条件**依赖 `compat.protobuf`,而 `codegen`
feature 需要往**同一条边**加 `tools = ["protoc"], reexport = true`。把这个请求
挪到无条件条目上不可行(那会让每个消费者都构建 protoc),丢弃又会静默丢掉这个
feature 存在的全部理由。

因此:`tools` / `features` 取并集,`host-module` / `reexport` 取或;
`version` / `path` / `git` 这类**身份字段不合并**,「条件段绝不静默覆盖无条件
段」这条纪律保持不变。与逐边 feature 请求本来就遵循的规则一致。

#### D1.3 裸名:走索引那套命名空间阶梯,而不是「谁最后写谁赢」

`env_var_name`(`tool_store.cppm:154`)今天同时发长名与短名:`compat.protobuf` → `MCPP_DEP_COMPAT_PROTOBUF_BIN_PROTOC` 与 `MCPP_DEP_PROTOBUF_BIN_PROTOC`。今天只有 root 亲自声明的工具进环境,撞车在用户眼皮底下;传递传播之后,两条互不相识的库各自提供同名短名工具时,谁赢取决于 vector 的追加顺序,且无任何诊断。这从另一扇门放回了本设计要保住的「版本错配不可表达」性质。

采用与包身份解析同一套机制:**FQN 是标识,裸名是缺省形式,按固定阶梯解析。**

- 全限定变量 `MCPP_DEP_<NS>_<NAME>_BIN_<TOOL>` **总是**发布,永远无歧义;
- 裸名变量 `MCPP_DEP_<NAME>_BIN_<TOOL>` 按阶梯选出唯一归属:

  1. `(mcpplibs, X)` —— `kDefaultNamespace`
  2. `(compat, X)` —— `kCompatNamespace`
  3. 其余候选中恰好只剩一个
  4. 否则不发布裸名变量

- 阶梯在第 1/2 级破除了平局(即同名候选不止一个)时,发一条 `provisions/ambiguous` 诊断,写明胜出者与全部候选的 FQN;
- 第 4 级不发布裸名,并把同一条诊断升级为「请改用全限定名」。

这与索引解析裸包名的阶梯(`(mcpplibs,X) → (compat,X) → (∅,X)`)是同一条规则的同一次应用;第 3 级是必需的补充,否则 `grpc.grpc-plugin` 这类非默认命名空间的包连裸名都拿不到(grpc-m 现在写的正是 `dep_bin("grpc-plugin", ...)`)。`dep_dir()` 沿用同一套。

### D1.4 发布一个新键不能让旧客户端加载失败

`reexport` 要写进**已发布包**的 manifest(grpc 的 `[feature-deps.codegen]`),
于是冒出一个此前没人问过的问题:比它旧的 mcpp 读到这份 manifest 会怎样?

答案曾经是**整份加载失败**,而且报错是误导性的:
`reexport = true` 被告知「must be a string, inline dep table, or nested table」。

根因是**一个谓词兼任两职**:`looks_like_inline_dep_spec` 既判定「这是内联依赖
spec 还是嵌套命名空间表」,又枚举「哪些键有意义」。于是「不认识的键」不会走到
「未知选项」那条路上——表直接判不出是 spec,被当成命名空间。

后果不是提示不友好,而是**任何已发布包都永远无法采用新键**。这与 #349 确立的
性质是同一条:**数据不得决定程序是否可用**。

判据换轴:**内联 spec 的判定是「它是否指名了一个来源」**(`path` / `version` /
`git` / `workspace`)。嵌套命名空间表的键是**包名**,不会有包叫 `version`,所以
这个判据不会误判。识别为 spec 之后,不认识的键**记为降级**(`--strict` 仍拒
绝),而不是让整份 manifest 加载失败——与 xpkg 读取器的 `xpkgUnknownKeys`
(「record rather than swallow」)同一条纪律。

这不能救**已经发布出去的**旧客户端(它们的解析器就是那样),但从这一版起,
这类问题不再复发。

### D2. 输入种类进表,指纹由种类决定

新增一种输入:**glob**。

```cpp
mcpp::rerun_if_changed_glob("proto/**/*.proto");
```

选 glob 而不是「目录 + 可选过滤器」,因为 glob 严格包含目录(`proto/**` 就是目录形态),并且与用户已经在写的 `sources = ["src/**/*.cppm"]` 是同一个概念、同一套匹配器(`scanner.cppm::path_matches_glob` 已支持 `**` 与 `*`)。

**指纹只取排序后的相对路径集合**,不含 mtime、不含 size、不含内容:

- size 变化 ⊂ 内容变化,已由 `File` 条目覆盖,纳入只带来误重跑;
- mtime 在 git checkout、容器构建、rsync 下不稳定,而本仓库已经在 `file_time_type` 的 epoch 上摔过一次;
- 「我依赖这里有哪些文件」的语义正好是路径集合,不多不少。

必须写死的三条:

1. **排序与规范化**:`generic_string()` + 字节序排序,否则不同平台的目录遍历顺序会让同一棵树算出不同指纹。
2. **永不走进构建输出目录与 `.git`。** `mcpp:generated=` 的产物落在 `target/` 下,在项目树内;`rerun_if_changed_glob("**")` 若把它算进去,集合每次都变,build.mcpp 每次重跑。这是 Cargo 的经典坑,必须在引擎侧堵死而不是靠用户写对模式。
3. **不跟随符号链接**,与既有扫描一致。

代价:误重跑的上限是一次 build.mcpp 重跑(秒级),不是全量重编。

配套的版本处理(初稿遗漏):

- `directives::kProtocolVersion` → 2:新增了程序可依赖的 directive;
- `directives::kCacheEpoch` → 2:缓存记录多了一类行,旧引擎读到新条目会忽略它并误判为新鲜。

### D3. B3:让库能按平台裁剪 provision,并让子构建说出真话

D1 把「是否请求 host 工具」的决定权从用户搬给了库。Windows 上 `compat.protobuf` 不声明 protoc 目标(工具子构建在 Windows 上有一个尚未定位的缺陷,见 #359 的相邻条目),于是库一旦无条件声明,Windows 用户会撞上 `prepare.cppm:4104` 那条硬错:`dependency 'compat.protobuf' has no kind = "bin" target named 'protoc'`。这会让 D1 在 Windows 上从「4 条降到 1 条」变成「本来能构建的现在报错」。

采取两条小改动,而不是把 D1 压在一个开放式排查后面:

**D3a — `[target.<sel>.feature-deps.<name>]`。**
`ConditionalConfig` 今天已经带 `dependencies` / `devDependencies` / `buildDependencies`,唯独缺 `featureDeps`。这正是该类型自己的注释记录过的失败形状:「条件读取器维护自己的一份键子集,落后了也没人发现」(#258 修的是 `BuildInputs`)。补上它是在补一个已知形状的洞,而不是为一个 issue 加特性。补上后,库可以写:

```toml
[target.'cfg(not(windows))'.feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"], reexport = true }
```

Windows 上 feature 仍可激活,但不请求工具,于是不触发硬错;用户看到的是 grpcgen 那条自带修复建议的诊断——与今天 Windows 上的处境相同。**D1 因此在 Windows 上是中性的,在其余平台是净收益。**

**D3b — 工具子构建必须透出内层的真实错误。**
Windows 那个缺陷至今未定位的直接原因是内层 ninja 的输出被汇总吞掉,真正的 scan 报错从未进入日志。透出它是小改动,并且是「将来能定位」的前提。缺陷本身仍然单独开 issue。

### D4. 为什么这些必须一起做

只做 D1:用户从 4 条降到 1 条,但仍要在 `build.mcpp` 里逐个列 `.proto`。
只做 D2:用户不必列 proto,但仍要写 4 条依赖并知道 gRPC 需要 protobuf 的 protoc。
不做 D3:D1 在 Windows 上是倒退。

## 4. 实施步骤

| 步 | 内容 | 风险 |
|---|---|---|
| 1 | `src/build/provisions.cppm`:`ProvisionKind` 表、env 命名、裸名阶梯、传播不动点 | 低,纯新增 |
| 2 | `DependencySpec::reexport` + toml/xpkg 两个解析器 | 低 |
| 3 | `prepare.cppm` 接入:tools / host-module / dep_dir 三处改用不动点结果 | 中——须确认未 `reexport` 的边不外泄 |
| 4 | `ConditionalConfig::featureDeps` + prepare 侧合并(D3a) | 低,与既有条件依赖同路径 |
| 5 | 子构建错误透出(D3b) | 低 |
| 6 | `rerun-if-changed-glob` + 指纹 + protocol/epoch bump + `hostprogram` API | 中——缓存格式变更 |
| 7 | grpc-m 侧改成 1 条依赖 + `generate_all()`,作为真实验证 | ——(需要本设计先发布) |

## 5. 验证

- **单测**:传播规则(未 `reexport` 不外泄、`reexport` 沿链传递、feature 关闭时不存在);裸名阶梯(默认命名空间优先、compat 次之、唯一候选兜底、歧义不发布裸名);glob 指纹(增删文件变、改内容不变、输出目录不参与、跨平台排序一致)。
- **e2e**:一个库 `reexport` 工具 + 一个消费者只写一条依赖就能 `dep_bin` 到;两个不同命名空间的同名包同时提供工具时裸名不被静默绑定;新增一个文件后 glob 场景确实重跑;`[target.*.feature-deps]` 在非匹配平台上不引入依赖。
- **真实场景**:grpc-m 的模板降到 1 条依赖 + 1 行 build.mcpp,且生成产物仍与官方 protoc 逐字节相同(该基线已在 2026.8.5.x 建立)。

## 6. 明确不做

- **不让传播默认开启**。理由见 D1.2。
- **不把目录内容或 mtime/size 纳入 glob 指纹**。理由见 D2。
- **不引入「工具版本独立于依赖版本」的语法**。单一版本轴正是「错配不可表达」的来源。
- **不在本轮定位 Windows 的工具子构建失败**。D3b 只让它可被观测,缺陷本身单独开 issue。
- **不做 per-kind 的 `reexport` 粒度**。per-edge 已经够用。
