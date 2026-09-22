# 07 —— 工作空间

**读者：** 仓库里不止一个包的作者。

**本章回答的那一个问题：** 多个包怎样成为一次构建，以及一个成员与其余成员共享什么。

**不在这里：** 把这些包发布出去，那是 [11 —— 发布一个库](11-publishing-a-library.md)。
在此之前：[06 —— Feature 与能力](06-features-and-capabilities.md)。在此之后：
[08 —— 测试](08-testing.md)。

工作空间在同一个仓库中组织多个相关的 mcpp 包（库或应用程序）。各成员包共享统一
的依赖版本与工具链配置，同时各自保留独立的 `mcpp.toml` 工程文件。

## 1. 概述

工作空间解决以下问题：

- **依赖版本统一管理**——多个子包使用相同版本的第三方依赖，避免重复声明与版本漂移。
- **工具链配置共享**——在工作空间根声明一次工具链，成员继承或按需覆盖。
- **多包协同开发**——库与应用在同一仓库中开发，通过 `path` 依赖相互引用。

工作空间不改变依赖的声明方式。成员之间通过既有的 `path = "..."` 机制相互引用，
与非工作空间工程的用法完全一致。

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

`members` 列出各成员包的相对路径，每个路径下必须包含各自的 `mcpp.toml`。

可选字段 `exclude` 排除特定路径：

```toml
[workspace]
members = ["libs/*"]
exclude = ["libs/experimental"]
```

### 2.2 虚拟工作空间与带根包的工作空间

**虚拟工作空间**：根 `mcpp.toml` 只含 `[workspace]`，不含 `[package]`。根目录不
产出构建产物，只作为管理节点。

```toml
# 虚拟工作空间 —— 只有 [workspace]
[workspace]
members = ["libs/core", "apps/server"]
```

**带根包的工作空间**：根 `mcpp.toml` 同时含 `[package]` 与 `[workspace]`。根目录
本身也是一个可构建的包。

```toml
[workspace]
members = ["libs/core"]

[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
myproject.core = { path = "libs/core" }
```

### 2.3 成员工程文件

各成员维护自己的 `mcpp.toml`，结构与普通工程相同：

```toml
# libs/core/mcpp.toml
[package]
namespace = "myproject"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
```

成员之间通过 `path` 依赖相互引用：

```toml
# libs/http/mcpp.toml
[package]
namespace = "myproject"
name      = "http"
version   = "0.1.0"

[dependencies]
myproject.core = { path = "../core" }

[dependencies.compat]
mbedtls.workspace = true
```

## 3. 依赖版本的继承

在 `[workspace.dependencies]` 中集中声明依赖版本，成员通过 `.workspace = true`
继承：

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

成员可以覆盖继承来的版本：

```toml
[dependencies.compat]
mbedtls = "4.0.0"          # override; does not use the workspace version
```

## 4. 工具链与构建配置的继承

工作空间根的 `[toolchain]` 与 `[target.<triple>]` 配置由全体成员自动继承。成员
可以在自己的工程文件中覆盖。

配置优先级（从高到低）：

1. 命令行参数（`--target`、`--static`）
2. 成员 `mcpp.toml` 中的声明
3. 工作空间根 `mcpp.toml` 中的声明
4. 全局配置（`~/.mcpp/config.toml`）
5. 内置默认值

```toml
# workspace root
[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

```toml
# a member overrides the toolchain
[toolchain]
default = "llvm@20.1.7"
```

### 4.1 `[workspace.package]` 与 `[workspace.build]`

全体成员共享的包元信息与构建标志，在工作空间根声明一次：

```toml
[workspace]
members = ["libs/core", "libs/http", "apps/server"]

[workspace.package]
standard = 26                  # or "c++26"; both spellings are accepted
version  = "0.4.2"
license  = "Apache-2.0"
authors  = ["example"]

[workspace.build]
cxxflags         = ["-Wall", "-Wextra"]
dialect_cxxflags = ["-fno-exceptions"]
```

成员只声明属于自己的部分：

```toml
[package]
name = "core"
# standard, version, license and authors are inherited;
# [workspace.build] cxxflags are inherited
```

**合并规则。**

| 种类 | 规则 |
|---|---|
| 标量（`standard`、`version`、`license`、`c_standard`、`linkage` 等） | 成员**声明了该键**时成员胜出；否则取工作空间的值 |
| 向量（`cxxflags`、`ldflags`、`defines`、`dialect_cxxflags`、`include_dirs` 等） | 追加，**工作空间在前**——因而成员自己的标志排在命令行更后面，后者胜出 |
| `[workspace.dependencies]` | 逐依赖显式选择加入，`x.workspace = true`（§3） |

"声明了"指的是这个键被写过，而不是它的值与默认值不同。成员在
`[workspace.package] standard = 26` 之下刻意写 `standard = "c++23"`，得到的就是
c++23；什么都不写的成员得到 c++26。这两种情况值相同而意图相反，所以这一点被
记录下来，而不是靠推断。

标量与向量都是**隐式继承**，不需要逐键选择加入。工作空间要消除的正是"某个成员
忘了选择加入"这种漂移，所以继承是默认行为，覆盖才是需要主动写出的动作。依赖保留
显式选择加入，因为依赖是解析图上的一条边：隐式继承一条边，会在成员自己的
manifest 只字未提的情况下改变它解析到什么。

**成员可以省略 `version`**，只要 `[workspace.package]` 提供了它。这个字段整体上
仍是必需的——两边都没有时会被拒绝，同时指出成员文件和本该提供它的那个
workspace 键。

**并非所有键都可继承。** `[workspace.build] allow_host_libs` 会被拒绝：它关闭的
是某个具体产物的 hermetic 链接检查，而工作空间根若能设置一次，就等于替所有后来
加入、可能从未读过根 manifest 的成员一并关闭了这项检查。**描述"如何构建"的键可
继承；描述"不跑哪项安全检查"的键留在产物所属的那个包里。** `[workspace.package]`
与 `[workspace.build]` 里其他不认识的键同样会被拒绝而不是被忽略：一张以"传播"
为唯一目的的表，如果能静默丢弃某个键，产出的就是一个看起来配置好了、实际上没有
的工作空间。

**没有 `[workspace.target.<triple>]`。** 工作空间根里一个普通的 `[target.<triple>]`
块本来就按 triple 逐项被全体成员继承（成员优先）。为同一能力再造一种拼法，只会
增加接口面而不增加功能。

### 4.2 整个模块图只有一个标准

C++ 模块图有且只有一个标准：BMI 跨档位不兼容，因此根包的 `standard` 施加于图中
每一个包，依赖也不例外。依赖自己的 `standard` 不会被应用。

有一类包是例外。供给 C++ 层（即标准库本身）的包陈述了 `standard` 时，它那些既
不提供也不导入模块的翻译单元恰好按该档位编译；它的模块单元仍按图的档位编译
（[22 —— 目标侧](22-target-side.md)「标准库自身的语言级别」）。没有 BMI 穿过这些
单元，因此上面的规则并未被打破；正是这一点让一个 c++20 工程可以使用源码按 C++23
编写的标准库。

当依赖**声明**了高于当前图的档位时，mcpp 在编译前就报出来：

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

这是 warning 而不是 error——这类构建通常仍会成功，`--strict` 会把它提升为
错误。按上文被应用了声明的 C++ 层供给者不会被这样报出。它只对**工程作者自己拥有
的 manifest** 生效（根包、workspace 成员、`path` 依赖）：从索引解析来的包，其
`standard` 是由描述符生成器写的，而不是由读到这条消息的人写的。

## 5. 构建命令

### 5.1 从工作空间根构建与测试

```bash
mcpp build                  # virtual workspace → builds ALL members; rooted → the root package
mcpp build -p server        # build a specific member and its dependencies
mcpp build --workspace      # build every member explicitly
mcpp test                   # virtual workspace → tests ALL members; rooted → the root package
mcpp test  -p core          # test a single member
mcpp test  --workspace      # test every member (one report per member; continues past failures)
```

在**虚拟**工作空间根（只有 `[workspace]`、没有 `[package]`）下，裸 `mcpp build` /
`mcpp test` 作用于**全体**成员；在**带根包**的工作空间（`[package]` +
`[workspace]`）下，两者作用于根包，用 `--workspace` 才纳入全体成员。
`mcpp test --workspace` 独立构建并运行每个成员的 `tests/**/*.cpp`——发现按成员
隔离，因此两个成员各有一个 `tests/main.cpp` 也不冲突。

### 5.2 从成员子目录构建

```bash
cd libs/http
mcpp build                  # auto-detects the workspace and builds the current member
```

mcpp 从当前目录向上搜索；若发现某个 `mcpp.toml` 含 `[workspace]` 且当前目录在其
`members` 列表中，则自动进入工作空间模式并继承工作空间配置。

### 5.3 `-p, --package` 选项

`-p` 可用于 `build`、`test`、`run` 等命令，指定目标成员。参数值可以是成员目录名
的最后一段，也可以是完整相对路径：

```bash
mcpp build -p server        # matches apps/server
mcpp test -p core           # matches libs/core
mcpp run -p server -- --port 8080
```

`--workspace`（用于 `build` 与 `test`）是扇出形式：作用于**每个**成员。
`mcpp test --workspace` 逐成员分别汇报，遇失败继续，只要有任一成员失败就非零
退出——很适合作为"一个测试众多库的工作空间"单条、无需 shell 的 CI 步骤。

#### 扇出的汇报

```
   Workspace testing member 'libs/core' (3/97)
test_paths ... ok (0.31s)
 test result ok. 7 passed; 0 failed; finished in 9.50s (build 8.90s + run 0.60s)
   Workspace member 'libs/core' (3/97) ok — 7 passed in 9.50s
...
 workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
    slowest: libs/jsc 93.5s, libs/install 32.2s, libs/http 24.1s
```

`M/N` 进度、逐测试耗时，以及按 **build** 与 **run** 拆开的成员耗时。拆开才是有用
的部分：一个测试只要几毫秒、但链接要 90 秒的成员，在单个合并数字里与"测试套件本身
很慢"长得一模一样，而这两种情形只有一种值得去查。

`--message-format json` 以 NDJSON 承载同样的数据。每条 test 记录都带成员限定
字段（`"member"`），流的末尾是一条 `workspace_summary` 记录，列出失败成员与未
运行成员——一旦两个成员都有一个叫 `smoke` 的测试，裸测试名就不再能归因。

#### 给扇出设期限

```bash
mcpp test --workspace --timeout 60        # per-test RUN deadline (default 300)
mcpp test --workspace --build-timeout 300 # per-ninja-drive deadline (default 0 = no limit)
mcpp test --workspace --workspace-timeout 1800   # whole fan-out (default 0 = no limit)
```

扇出是串行的，所以一个没有上界的成员会拖住排在它后面的每一个成员。三个期限都是
**汇报而非中止**：测试超时只判该测试失败，扇出继续；构建超时只判该成员失败；
`--workspace-timeout` 停止扇出并列出未运行的成员，而不是把进程留给 CI 去 kill——
那样会把进程本该说出的话一并丢掉。

## 6. 目录布局

工作空间推荐的目录布局：

```
myproject/
├── mcpp.toml               # [workspace] declaration
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

各成员的构建产物存在各自的 `target/` 子目录下。

工作空间之外的工程，以成员身份引用托管在 git 上的工作空间中的一个成员：
`myproject.http = { git = "...", rev = "..." }` 会在根 manifest 的 `members`
中选中 `libs/http`，取同一个提交，而该成员会像在工作空间内部一样继承
`[workspace.package]`（mcpp 2026.9.16.1+；见 [05 —— 依赖](05-dependencies.md)）。

## 7. 与 C++ 模块的关系

工作空间与 C++23 模块机制协同工作：

- **接口可见性由语言控制**——`export module` 与 `import` 语句决定一个模块的
  公开接口，工作空间不施加额外的可见性限制。
- **模块名由库作者决定**——工作空间不要求模块名与包名或命名空间一致。
- **partition 用于内部组织**——通过 `import :internal;`（不带 `export`）导入
  的 partition 对消费者不可见，不需要构建工具介入。

## 8. 完整示例

见 [`examples/04-workspace/`](../../examples/04-workspace/)，一个三成员工作空间
的完整可运行示例。
