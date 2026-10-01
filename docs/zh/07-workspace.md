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

写了 `.workspace = true` 而没有工作空间解析它的条目，在该包进入构建的每个位置（根包、
`-p` 选中的成员、`path`、`git` 与索引依赖）都被拒绝，消息点名所在的表与条目
（mcpp 2026.9.27.1+）。它按 `members` 列出该包的工作空间的 `[workspace.dependencies]`
解析；带自己 `[package]` 的工作空间根以同样方式解析自己的条目。

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

`[toolchain]`、`[target.<triple>]` 与 `[indices]` 为整个依赖图选择编译器、目标行与索引，
因此成员只在作为一次构建的根时从工作空间根继承它们：从工作空间构建、以 `-p` 选中，或作为
另一个包的宿主工具构建（最后一种自 mcpp 2026.9.27.1）。作为依赖到达的成员从该次构建的根
取得它们。

不带 `--target` 的构建以宿主为目标，`[target.<宿主三元组>]` 对它生效，与
`--target <宿主三元组>` 相同（mcpp 2026.9.27.1+）。

根的 `[xlings.workspace]` 条目，包括 `[target.<selector>.xlings.workspace]` 行，同样隐式
继承（mcpp 2026.9.27.1+）：载荷描述的是构建运行的环境，与 `[toolchain]` 相同，不需要
显式声明。成员自己声明的同一个包优先。`[feature-xlings.<f>]` 不被继承，因为特性属于声明
它的包。

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
| 向量（`cxxflags`、`cflags`、`ldflags`、`dialect_cxxflags`、`include_dirs` 等） | 追加，**工作空间在前** |
| `defines` | 按宏名构成的集合：成员对某个继承来的宏名写出的条目替换它，`!NAME` 移除它（2026.9.25.1+） |
| `[workspace.dependencies]` | 逐依赖显式选择加入，`x.workspace = true`（§3） |

"声明了"指的是这个键被写过，而不是它的值与默认值不同。成员在
`[workspace.package] standard = 26` 之下刻意写 `standard = "c++23"`，得到的就是
c++23；什么都不写的成员得到 c++26。这两种情况值相同而意图相反，所以这一点被
记录下来，而不是靠推断。

标量与向量都是**隐式继承**，不需要逐键选择加入。工作空间要消除的正是"某个成员
忘了选择加入"这种漂移，所以继承是默认行为，覆盖才是需要主动写出的动作。依赖保留
显式选择加入，因为依赖是解析图上的一条边：隐式继承一条边，会在成员自己的
manifest 只字未提的情况下改变它解析到什么。

**追加的向量能覆盖什么。** 成员的词在命令行上排在工作空间的词之后。编译器按
"后者胜出"处理的标志因此可以通过重写来覆盖：`-fno-exceptions` 之后的
`-fexceptions`、`-Wx` 之后的 `-Wno-x`、`-O0` 之后的 `-O2`。头文件目录按顺序搜索，
所以工作空间 `include_dirs` 目录中的头文件，先于成员目录中的同名头文件被找到。
宏通过 `defines` 覆盖，每个宏名只产生一个 `-DNAME` 词：

```toml
# 工作空间根
[workspace.build]
defines = ["LOG_LEVEL=1", "TRACE"]

# 成员
[build]
defines = ["LOG_LEVEL=3", "!TRACE"]   # 以 -DLOG_LEVEL=3 编译，且不定义 TRACE
```

`defines` 条目同样替换同一个包的 `cflags` 或 `cxxflags` 中同名的 `-DNAME` 词。
`!NAME` 要求 mcpp 2026.9.25.1 或更新版本；更早的 mcpp 会把它作为 `-D!NAME` 交给
编译器，那是一个错误。

**每个成员在每种位置上都恰好接收一次继承值**（2026.9.25.1+）：作为命令所构建的包
（`-p <member>`，或在成员目录内执行的命令）、作为另一个成员的 `path` 依赖，以及作为
通过 `git` 引用的、托管在 git 上的工作空间的成员（§6）。作为依赖出现的成员同样会解析
它自己的 `x.workspace = true` 条目。

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
mcpp build -p server -p cli # build several members, in one plan
mcpp build --workspace      # build every member explicitly
mcpp build --workspace --exclude legacy   # every member but legacy
mcpp test                   # virtual workspace → tests ALL members; rooted → the root package
mcpp test  -p core          # test a single member
mcpp test  -p core -p http  # test several members: one plan, one build, one report per member
mcpp test  --workspace      # test every member (one report per member; continues past failures)
```

在**虚拟**工作空间根（只有 `[workspace]`、没有 `[package]`）下，裸 `mcpp build` /
`mcpp test` 作用于**全体**成员；在**带根包**的工作空间（`[package]` +
`[workspace]`）下，两者作用于根包；`--workspace` 作用于根包与全体成员。
对多个成员的 `mcpp test` 把它们放在一起规划、只构建一次（§5.4），然后运行每个成员的
`tests/**/*.cpp`——发现按成员隔离，因此两个成员各有一个 `tests/main.cpp` 也不冲突。

### 5.2 从成员子目录构建

```bash
cd libs/http
mcpp build                  # auto-detects the workspace and builds the current member
```

mcpp 从当前目录向上搜索；若发现某个 `mcpp.toml` 含 `[workspace]` 且当前目录在其
`members` 列表中，则自动进入工作空间模式并继承工作空间配置。此时命令等同于在工作空间根
执行 `mcpp build -p <当前成员>`，在工作空间的构建目录中构建（§6）。

### 5.3 `-p, --package` 选项

`-p` 可用于 `build`、`test`、`run`、`mcpp emit build-database` 等命令，指定目标成员。
选项名说的是**包**，参数值按下述顺序解析：

1. 成员的限定名 `<namespace>.<name>`（只有声明了 namespace 的成员才有这个拼法）；
2. 否则，成员裸的 `package.name`——如果两个以上成员共享它，拒绝并点名每一个匹配；
3. 否则，成员在 `[workspace] members` 里写的路径，或其目录名的最后一段（历史拼法，
   作为回落保留）。

```bash
mcpp build -p server        # matches apps/server（按目录或包名）
mcpp test -p core           # matches libs/core
mcpp run -p server -- --port 8080
```

参数值若既是某个成员的包名，又是另一个成员的目录，选中包名所命名的那个成员，并给
出警告点名另一个成员——选项名的是包，包名的精确匹配压过恰好同名的目录。

#### 多个成员（mcpp 2026.10.1.1+）

`-p` 可以在 `build`、`test` 与 `mcpp emit build-database` 上重复。每个值指定一个成员，
按上述顺序解析，命令作用于所有这些成员。选择是一个**集合**，无论 `-p` 怎么排列，都按
`[workspace] members` 的顺序保存；同一个成员被两种拼法各指定一次，只选中一次。

```bash
mcpp build -p server -p cli         # the same members as -p cli -p server
mcpp test  -p core -p http
```

指定不到任何成员的值会在规划任何内容之前被拒绝，拒绝信息列出各成员。`-p` 与 `--workspace` 同时出现也会被拒绝：两者陈述了两种选择，任何一方都不会被默认取代另一方。`mcpp run` 执行
一个程序，所以只作用于一个成员：第二个 `-p` 被拒绝，并点名所有被指定的成员，绝不会被
理解为"取最后一个"。

#### 排除成员：`--exclude`

```bash
mcpp build --workspace --exclude legacy      # every member but legacy
mcpp test  --exclude legacy --exclude bench  # at a virtual root: every member but two
```

`--exclude <name>` 可以在 `build`、`test` 与 `mcpp emit build-database` 上重复。它的值按
与 `-p` 相同的方式解析，从"选中每个成员"的选择中去掉成员：`--workspace`，或不带 `-p` 的
虚拟根。下列情形在规划任何内容之前被拒绝：与 `-p` 同时出现；两种形式都不适用时（在成员
目录中，或在带根包的根下且没有 `--workspace`）；名字不匹配任何成员；以及它去掉了所有成员。

`--workspace`（用于 `build`、`test` 与 `mcpp emit build-database`）是扇出形式：作用于
**每个**成员。`mcpp test --workspace` 逐成员分别汇报，遇失败继续，只要有任一成员失败就
非零退出——很适合作为"一个测试众多库的工作空间"单条、无需 shell 的 CI 步骤。成员无论因
什么失败都只让自己失败：一个测试、它的包的构建，或者它的规划（无法规划的成员被排除在外，
其余成员重新放在一起规划）。

#### 扇出的汇报

```
   Workspace building 97 members: libs/core, libs/http, ...
   Workspace built members libs/core, libs/http, ... in 120.40s; slowest: obj/libs/jsc/tests/jsc.o 88.0s
   Workspace testing member 'libs/core' (3/97)
test_paths ... ok (0.31s)
 test result ok. 7 passed; 0 failed; finished in 121.10s (build 120.40s + run 0.60s)
   Workspace member 'libs/core' (3/97) ok — 7 passed, run 0.60s
...
 workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
    slowest: libs/install 32.2s, libs/http 24.1s
```

`M/N` 进度与逐测试耗时。同一配置的成员只构建一次，因此构建只在组的那一行里报告一次：
它的成员、它的墙钟时间，以及占用时间最多的那些边。这就是原先逐成员拆分所给出的信号：
链接要 90 秒、而不是测试慢的成员，会被组的 `slowest:` 边点名。每个成员自己的那一行写
出它的**运行**耗时，最后的 `slowest:` 一行按这个耗时给成员排名。

`--message-format json` 以 NDJSON 承载同样的数据。每条 test 记录都带成员限定字段
（`"member"`），`group_build` 记录在每个组的第一条测试记录之前说明该组的构建，每个成员的
汇总指明它的组（`build_group`），流的末尾是一条 `workspace_summary` 记录，列出失败成员与
未运行成员——一旦两个成员都有一个叫 `smoke` 的测试，裸测试名就不再能归因。各字段见
[50 —— 机器可读输出](50-machine-output.md#mcpp-test---message-format-json--测试流)。

#### 给扇出设期限

```bash
mcpp test --workspace --timeout 60        # per-test RUN deadline (default 300)
mcpp test --workspace --build-timeout 300 # per-ninja-drive deadline (default 0 = no limit)
mcpp test --workspace --workspace-timeout 1800   # whole fan-out (default 0 = no limit)
```

各成员的运行是串行的，所以一个没有上界的成员会拖住排在它后面的每一个成员。三个期限都是
**汇报而非中止**：测试超时只判该测试失败，扇出继续；构建超时判等待这次构建的成员失败；
`--workspace-timeout` 停止扇出并列出未运行的成员，而不是把进程留给 CI 去 kill——
那样会把进程本该说出的话一并丢掉。

`--workspace-timeout` 限制的是运行，从命令开始时计时：它在每个成员的测试开始前检查，到那
时还没有开始的成员被列为未运行（2026.10.1.1+）。一个组的成员在同一步中构建，期限无法打断
这一步，因此仅构建就超过期限的工作空间会把这次构建做完（由 `--build-timeout` 限制），然后
不再启动任何成员。2026.10.1.1 之前每个成员依次构建、依次运行，期限可以在两次构建之间停止
扇出。

### 5.4 每个配置一张构建图（mcpp 2026.9.29.1+）

对工作空间的命令把成员放在一起规划：被选中的成员及其全部依赖构成一张构建图，只有一个
`build.ninja`，被多个成员使用的成员只编译一次。

- **配置。** 工具链请求、目标、C++ 标准、`dialect_cxxflags`、C++ 运行时、`linkage`、配置档、
  索引，以及其他作用于整张图的 `[build]` 值都相同的成员在同一张图中构建。其中任一项不同的成员
  在各自的图中构建，这些图同时进行，共享命令的并行任务数。成员写下的相对路径（例如它自己的
  `[indices]` 路径）按该成员的目录解析。
- **选择。** `--workspace`，以及虚拟工作空间根下不带 `-p` 的命令，选中全体成员；`-p X` 与在
  X 的目录中执行的命令规划 X 及其所依赖的一切；`-p X -p Y` 把两者放在一起规划，作为一个
  选择（§5.3）。这些选择共用构建目录，同一个编译单元在包含它的每个选择中都由同一条命令编译：
  命令取决于单元所属的包、该包所到达的包、选择为它启用的 feature，以及被选成员作为根所持有的
  声明，而不取决于图中的其他任何内容（2026.10.1.2+）。因此先执行 `mcpp build --workspace`
  再执行 `mcpp build -p X` 不编译任何内容；只有当某个包启用的 feature，或被选成员的根声明在
  两条命令中不同时，它才会被重新编译。2026.10.1.2 之前，另有三个关于整张图的事实也会进入
  其他成员的命令：两个成员提供同一个模块名、两个成员从各自目录之外列出同一个文件、某个成员
  构建共享库。
- **编译参数。** 成员的 `cflags`、`cxxflags`、`ldflags` 与 defines 作用于该成员自己的命令。
  修改它们会重新编译该成员以及导入它的单元，构建目录保持不变。
- **Feature。** `--features f` 在每个声明了 `f` 的被选成员中启用它；没有被选成员声明它时，
  命令被拒绝。同一配置中被多个成员使用的包只编译一次，feature 取它们请求的并集；被两个
  配置的成员使用的包在每个配置中各编译一次。
- **根的声明。** 每个被选成员都像单独规划时的根那样声明依赖：它对某个依赖的 `path` 或
  `git` 覆盖胜过其他包的声明，它依赖边上的 `linkage` 被采纳，它的注册表依赖参与索引刷新
  的判断（2026.9.30.2+）。两个被选成员对同一依赖的 checkout（种类或引用）或链接形态意见不一
  时被拒绝，并点名双方。见 [05 —— 同一依赖两条声明冲突的处理](05-dependencies.md#同一依赖两条声明冲突的处理)。
- **Hooks。** 每个被选成员的 `[hooks]` 按成员顺序在构建前后运行。
- **资源。** 成员的 `[resources]` 与 `windows_code_page` 按该成员的目录与 include 目录编译，
  只嵌入该成员自己的程序与共享库（2026.9.29.2+）。
- **构建程序。** 成员的构建程序按依赖在前的顺序运行；只要程序的输入不变，无论命令选中哪些
  成员，程序的结果都被复用（2026.9.29.5+）。它们的编译同时进行，数量以作业数为上限，只有运行
  遵循这个顺序，因此计划与串行构建写出的相同；多个程序 import 的同一个 host 模块只为它们
  编译一次（2026.10.1.1+；见 [30 — 构建程序](30-build-mcpp.md)）。
- **测试。** 对多个成员的 `mcpp test` 按 `build` 的方式规划：每个配置一次，包含各成员的
  测试，因此成员共用的包只编译一次，它的构建程序只运行一次，它的 feature 是选择所请求的
  并集（2026.10.1.1+）。该配置的包与测试二进制只构建一次；然后每个成员的测试按成员顺序
  运行，使用该成员自己的运行时目录，而不是其他成员的。在 `mcpp build --workspace` 之后运行
  的测试不会编译构建已经编译过的任何内容，除非某个 dev-dependency 改变了包的 feature。
  对一个成员的 `mcpp test` 是该成员的一次规划，一如既往。
- **编译数据库。** `mcpp build --configure-only` 与 `mcpp emit build-database` 按构建的方式
  规划，每个配置一次规划并包含各成员的测试，因此成员共用的包在每个配置中只描述一次。规划了
  多个配置的命令只写一次根目录的 `compile_commands.json`，内容为各配置数据库的并集
  （2026.9.29.5+）。
- **无事可做的构建。** 在没有任何改动时重复执行的命令，每个配置只做一次检查，不重新规划。
- **模块名。** 模块名在一个程序之内唯一，而不是在一张图之内唯一（2026.9.30.2+）。不共享任何
  程序的两个成员可以各自提供同名模块，一条 `--workspace` 命令会把两者都构建出来；同时链接两者的
  成员会被拒绝。见 [05 —— 每个程序里一个名字只对应一个模块](05-dependencies.md#每个程序里一个名字只对应一个模块mcpp-20269302)。

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

工作空间在其根目录构建（2026.9.29.1+）：

```
myproject/
├── mcpp.lock                               # 整个工作空间一份锁文件
└── target/<triple>/<configuration>/
    ├── build.ninja, compile_commands.json  # 每个配置一张图、一份数据库
    ├── obj/<package>/                      # 各包的中间产物
    ├── gcm.cache/<package>/                # 各包的 BMI（clang 为 pcm.cache）
    ├── modmap/                             # 导入它们的编译单元所读的模块映射
    └── bin/
        ├── server/                         # 成员的产物：bin/<包名>/
        │   ├── server
        │   └── libfoo.so                   # 其程序加载的共享库与运行时文件
        └── ...
```

- 成员的程序与共享库位于它的**产物目录** `bin/<包名>/`，其程序加载的共享库、DLL 与部署文件
  放在旁边。两个成员包名相同时使用 `bin/<namespace>.<name>/`。带根包的工作空间中，根包仍使用
  `bin/`。
- 放入多个产物目录的共享库，在支持硬链接的文件系统上是同一个文件的多个名字，其他情况下被复制。
- 工作空间根的 `compile_commands.json` 覆盖所有已构建或已配置的成员。
- 工作空间根的 `mcpp.lock` 记录全体成员的解析结果。`mcpp build --workspace` 写入完整记录；
  `mcpp build -p X` 更新 X 所在图中的条目。
- 成员的构建程序写入 `<member>/target/.build-mcpp/`。各程序 import 的 host 模块编译进
  workspace 自己的 `target/.build-mcpp/host-modules/`，为全体程序只编一次。
- 早期版本的 mcpp 在成员自己的 `target/` 下留下的构建目录不再被读取；`mcpp clean --stale`
  会删除它们。

工作空间之外的工程，以成员身份引用托管在 git 上的工作空间中的一个成员：
`myproject.http = { git = "...", rev = "..." }` 会在根 manifest 的 `members`
中选中 `libs/http`，取同一个提交，而该成员会像在工作空间内部一样继承
`[workspace.package]`（mcpp 2026.9.16.1+；见 [05 —— 依赖](05-dependencies.md)）。
它还继承所在仓库的 `[workspace.build]`，并按该仓库的 `[workspace.dependencies]`
解析自己的 `x.workspace = true` 条目（2026.9.25.1+），因此同一个提交在它自己的
检出中与在使用方的依赖图中以相同方式编译。索引描述符指向 tag tarball 内的成员时，
该成员以同样方式取得 tarball 中的工作空间。用 `mcpp publish` 发布成员时，继承来的值
被写入发布的清单（[11 —— 发布库](11-publishing-a-library.md)）。

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
