# 工作空间 (Workspace)

工作空间允许在同一个仓库中组织和管理多个相关的 mcpp 包（库或应用程序）。各成员包共享统一的依赖版本配置和工具链设置，同时保持独立的 `mcpp.toml` 工程文件。

## 1. 概述

工作空间解决以下问题：

- **依赖版本统一管理** — 多个子包使用相同版本的第三方依赖，避免重复声明和版本不一致
- **工具链配置共享** — 在工作空间根目录统一声明工具链，各成员继承或覆盖
- **多包协同开发** — 库与应用在同一仓库中开发，通过 `path` 依赖相互引用

工作空间不改变依赖声明方式。成员之间通过已有的 `path = "..."` 机制声明依赖关系，与非工作空间项目的用法完全一致。

## 2. 工程文件结构

### 2.1 工作空间根

在仓库根目录的 `mcpp.toml` 中声明 `[workspace]`：

```toml
[workspace]
members = [
    "libs/core",
    "libs/http",
    "apps/server",
]
```

`members` 列出各成员包的相对路径，每个路径下须包含独立的 `mcpp.toml`。

可选 `exclude` 字段排除特定路径：

```toml
[workspace]
members = ["libs/*"]
exclude = ["libs/experimental"]
```

### 2.2 虚拟工作空间与根包工作空间

**虚拟工作空间**：根 `mcpp.toml` 仅包含 `[workspace]`，不包含 `[package]`。根目录不产出构建产物，仅作为管理节点。

```toml
# 虚拟工作空间 — 只有 [workspace]
[workspace]
members = ["libs/core", "apps/server"]
```

**根包工作空间**：根 `mcpp.toml` 同时包含 `[package]` 和 `[workspace]`。根目录本身也是一个可构建的包。

```toml
[workspace]
members = ["libs/core"]

[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
core = { path = "libs/core" }
```

### 2.3 成员工程文件

各成员维护独立的 `mcpp.toml`，结构与普通项目一致：

```toml
# libs/core/mcpp.toml
[package]
namespace = "myproject"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
```

成员之间通过 `path` 依赖引用：

```toml
# libs/http/mcpp.toml
[package]
namespace = "myproject"
name      = "http"
version   = "0.1.0"

[dependencies]
core = { path = "../core" }

[dependencies.compat]
mbedtls.workspace = true
```

## 3. 依赖版本继承

在 `[workspace.dependencies]` 中集中声明依赖版本，成员通过 `.workspace = true` 继承：

```toml
# 根 mcpp.toml
[workspace.dependencies]
cmdline = "0.0.2"
mcpplibs.capi.lua = "0.0.3"  # 精确 selector:(mcpplibs.capi, lua)

[workspace.dependencies.compat]
mbedtls = "3.6.1"
gtest   = "1.15.2"
```

```toml
# 成员 mcpp.toml
[dependencies.compat]
mbedtls.workspace = true    # 继承版本 → "3.6.1"

[dev-dependencies.compat]
gtest.workspace = true      # 继承版本 → "1.15.2"
```

成员可以覆盖继承的版本：

```toml
[dependencies.compat]
mbedtls = "4.0.0"          # 覆盖，不使用 workspace 版本
```

## 4. 工具链与构建配置继承

工作空间根的 `[toolchain]` 和 `[target.<triple>]` 配置自动继承到所有成员。成员可在自身的工程文件中覆盖。

配置优先级（从高到低）：

1. 命令行参数（`--target`、`--static`）
2. 成员 `mcpp.toml` 中的声明
3. 工作空间根 `mcpp.toml` 中的声明
4. 全局配置（`~/.mcpp/config.toml`）
5. 内置默认值

```toml
# 工作空间根
[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

```toml
# 某成员覆盖工具链
[toolchain]
default = "llvm@20.1.7"
```

### 4.1 `[workspace.package]` 与 `[workspace.build]`

所有成员共享的包元信息与构建标志,在 workspace 根声明一次:

```toml
[workspace]
members = ["libs/core", "libs/http", "apps/server"]

[workspace.package]
standard = 26                  # 也可写 "c++26",两种拼法都接受
version  = "0.4.2"
license  = "Apache-2.0"
authors  = ["example"]

[workspace.build]
cxxflags         = ["-Wall", "-Wextra"]
dialect_cxxflags = ["-fno-exceptions"]
```

成员只声明属于它自己的部分:

```toml
[package]
name = "core"
# standard / version / license / authors 继承自 workspace
# [workspace.build] 的 cxxflags 也继承
```

**合并规则。**

| 类别 | 规则 |
|---|---|
| 标量(`standard`、`version`、`license`、`c_standard`、`linkage` 等) | 成员**声明了该键**时成员优先;否则取 workspace 的值 |
| 向量(`cxxflags`、`ldflags`、`defines`、`dialect_cxxflags`、`include_dirs` 等) | 追加,**workspace 在前** —— 成员自己的标志排在命令行后面,后者生效 |
| `[workspace.dependencies]` | 逐依赖显式选择加入,`x.workspace = true`(§3) |

"声明了"指的是**这个键被写过**,而不是它的值与默认值不同。成员在
`[workspace.package] standard = 26` 之下刻意写 `standard = "c++23"`,得到的就是
c++23;什么都不写的成员得到 c++26。这两种情况的值相同而意图相反,所以这个事实是被
**记录**下来的,而不是推断出来的。

标量与向量是**隐式继承**,不需要逐键选择加入。workspace 要消除的漂移正是"某个成员忘了
选择加入",所以继承是默认行为,覆盖才是需要主动表达的动作。依赖保留显式选择加入,因为
依赖是解析图上的一条**边**:隐式继承一条边,会在成员自己的 manifest 只字未提的情况下改变
它解析到什么。

**成员可以省略 `version`**,只要 `[workspace.package]` 提供了它。这个字段整体上仍是必需的
—— 两边都没有时会被拒绝,并同时指出成员文件和本该提供它的 workspace 键。

**并非所有键都可继承。** `[workspace.build] allow_host_libs` 会被拒绝:它关掉的是某个具体
产物的 hermetic 链接检查,而 workspace 根若能设置一次,就等于替所有后来加入的成员也关掉了
这项检查 —— 而那些成员的作者可能从没读过根 manifest。**描述"如何构建"的键可继承;描述
"不要跑哪项安全检查"的键留在产物所属的那个包里。** `[workspace.package]` /
`[workspace.build]` 中其他不认识的键同样会被拒绝而不是忽略:一个以"传播"为唯一目的的表,
若能静默丢弃某个键,产出的就是"看起来配置好了、实际没有"的 workspace。

**没有 `[workspace.target.<triple>]`。** workspace 根里一个普通的 `[target.<triple>]` 块
本来就会按 triple 逐项被所有成员继承(成员优先)。为同一能力再加一种拼法,只会增加接口面
而不增加功能。

### 4.2 整个模块图只有一个标准

C++ 模块图有且只有一个标准:BMI 跨档位不兼容,因此根包的 `standard` 会施加到图中每一个包,
依赖也不例外。依赖自己的 `standard` 不会被应用。

当依赖**声明**了高于当前图的档位时,mcpp 在编译前就报出来:

```
warning: dependency `render` declares standard = "c++26", and this graph is
         built at c++23
  impact: a C++ module graph has one standard, so the dependency's declaration
          is not applied and its sources are compiled at the graph's level
  hint:   raise the consumer's standard to "c++26", or declare it once for
          every member:

            [workspace.package]
            standard = "c++26"
```

这是 warning 而不是 error —— 这类构建通常仍然成功;`--strict` 会把它提升为错误。它只对
**工程作者自己拥有的 manifest** 生效(根包、workspace 成员、`path` 依赖):从索引解析来的
包,其 `standard` 是描述符生成器写的,不是读到这条消息的人写的。

## 5. 构建命令

### 5.1 从工作空间根目录构建与测试

```bash
mcpp build                  # 虚拟工作空间 → 构建所有成员；带根包 → 构建根包
mcpp build -p server        # 构建指定成员及其依赖
mcpp build --workspace      # 显式构建每个成员
mcpp test                   # 虚拟工作空间 → 测试所有成员；带根包 → 测试根包
mcpp test  -p core          # 测试单个成员
mcpp test  --workspace      # 测试每个成员（逐成员汇报；遇失败继续）
```

在**虚拟工作空间**根(只有 `[workspace]`、无 `[package]`)下,裸 `mcpp build` /
`mcpp test` 作用于**所有**成员;在**带根包工作空间**(`[package]` + `[workspace]`)下作用于
根包,用 `--workspace` 纳入全部成员。`mcpp test --workspace` 独立构建+运行每个成员的
`tests/**/*.cpp`——测试发现按成员隔离,因此两个成员各有一个 `tests/main.cpp` 也不会冲突。

### 5.2 从成员子目录构建

```bash
cd libs/http
mcpp build                  # 自动检测工作空间，构建当前成员
```

mcpp 从当前目录向上搜索，若发现包含 `[workspace]` 的 `mcpp.toml` 且当前目录在 `members` 列表中，则自动进入工作空间模式，继承工作空间配置。

### 5.3 `-p, --package` 选项

`-p` 可用于 `build`、`test`、`run` 等命令，指定构建的目标成员。参数值为成员路径的最后一段目录名或完整相对路径：

```bash
mcpp build -p server        # 匹配 apps/server
mcpp test -p core           # 匹配 libs/core
mcpp run -p server -- --port 8080
```

`--workspace`(用于 `build` 和 `test`)是扇出形式:作用于**每个**成员。
`mcpp test --workspace` 逐成员独立汇报、遇失败继续,只要有任一成员失败即非零退出——
非常适合作为「一个测试众多库的工作空间」的单条、无 shell 的 CI 步骤。

#### 扇出汇报什么

```
   Workspace testing member 'libs/core' (3/97)
test_paths ... ok (0.31s)
 test result ok. 7 passed; 0 failed; finished in 9.50s (build 8.90s + run 0.60s)
   Workspace member 'libs/core' (3/97) ok — 7 passed in 9.50s
...
 workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
    slowest: libs/jsc 93.5s, libs/install 32.2s, libs/http 24.1s
```

`M/N` 进度、逐测试耗时,以及**按 build / run 拆开**的成员耗时。拆开才是有用的那部分:
一个测试只要几毫秒、但链接要 90 秒的成员,在单个合并数字里和「测试套件很慢」长得一模一样,
而两者里只有一个值得去查。

`--message-format json` 承载同样的数据。每条 test 记录都带 `"member"` 限定,流末尾是一条
`workspace_summary`,列出失败成员与未运行成员 —— 一旦两个成员都有名为 `smoke` 的测试,
裸测试名就不再可归因。

#### 给扇出设期限

```bash
mcpp test --workspace --timeout 60        # 单测试**运行**期限(默认 300)
mcpp test --workspace --build-timeout 300 # 单次 ninja 驱动期限(默认 0 = 不限)
mcpp test --workspace --workspace-timeout 1800   # 整条扇出(默认 0 = 不限)
```

扇出是串行的,所以一个没有上界的成员会拖住它后面的所有成员。三个期限都是**汇报而非中止**:
测试超时只判该测试失败、扇出继续;构建超时只判该成员失败;`--workspace-timeout` 停止扇出并
列出未运行的成员 —— 而不是把进程留给 CI 去 kill(那会把它想说的话一并丢掉)。

## 6. 目录布局

工作空间推荐的目录布局：

```
myproject/
├── mcpp.toml               # [workspace] 声明
├── libs/
│   ├── core/
│   │   ├── mcpp.toml       # [package] namespace="myproject" name="core"
│   │   └── src/
│   │       └── core.cppm   # export module myproject.core;
│   └── http/
│       ├── mcpp.toml
│       └── src/
│           └── http.cppm   # export module myproject.http;
└── apps/
    └── server/
        ├── mcpp.toml
        └── src/
            └── main.cpp    # import myproject.http;
```

各成员的构建产物位于各自的 `target/` 子目录下。

## 7. 与 C++ 模块的关系

工作空间与 C++23 模块机制协同工作：

- **接口可见性由语言控制** — `export module` 和 `import` 语句决定模块的公开接口，工作空间不做额外的可见性限制
- **模块名由库作者决定** — 工作空间不强制模块名与包名或命名空间一致
- **partition 用于内部组织** — `import :internal;`（不带 `export`）的 partition 对消费者不可见，无需构建工具介入

## 8. 完整示例

参见 [`examples/04-workspace/`](../../examples/04-workspace/)，包含一个三成员工作空间的完整可运行示例。
