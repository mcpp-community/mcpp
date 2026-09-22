# 05 —— 依赖与解析

**读者：** 构建里已经不只有自己代码的作者。

**本章回答的那一个问题：** 一个依赖从哪里来，一条版本约束是什么意思，以及两条
约束不一致时会发生什么。

**不在这里：** 什么使两个包成为同一个包 —— 那是
[SPEC-001](../specs/package-identity.md)，本章施用它而不复述它；以及怎样发布
一个包，那是 [11 —— 发布一个库](11-publishing-a-library.md)。

在此之前：[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md) 是这些表与其余表同处
的地方。在此之后：[06 —— Feature 与能力](06-features-and-capabilities.md) 讲
一个依赖怎样变成可选的。

## `[dependencies]` —— 运行时依赖

**推荐写法是带精确版本的点式选择器。** 点式形式点名的是**一个身份** —— 最后
一段是包名，之前所有段都是 namespace —— 因此解析到什么，不取决于当前配置了
哪些 namespace。

```toml
[dependencies]
compat.gtest               = "1.15.2"
imgui.core                 = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"
mcpplibs.capi.lua          = "0.0.3"
```

裸名也被接受，它经由默认 namespace（`mcpplibs`）解析 —— 在只用那一个
namespace 的工程里方便，在不是的工程里有歧义：

```toml
[dependencies]
cmdline = "0.0.2"        # resolves to mcpplibs.cmdline
```

<details>
<summary>等价拼法：namespace 子表</summary>

把同一 namespace 下的条目归组。它解析到的身份与点式形式完全相同，在许多依赖
共享一个 namespace 时值得使用。

```toml
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # Explicit namespace; no fallback search
```

</details>

```toml
# Path dependency (local development)
[dependencies]
mylib = { path = "../mylib" }
```

```toml
# Git dependency — pick exactly one of tag / branch / rev
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }
applib = { git = "https://github.com/user/applib.git", branch = "develop" }
```

```toml
# Long-form dep spec: features and backend knobs
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # Request a feature of this dependency
widget = { version = "1.0", backend = "glfw_opengl3" }  # Sugar for: features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` 是**通用的约定糖衣**：它 1:1 脱糖为请求该依赖的
`backend-<impl>` feature（库若支持这个旋钮，应在自己的 `[features]` 中声明
一族 `backend-*`）。若目标包声明了 `[features]` 但不含所请求的 feature
（包括 backend 脱糖的结果），默认给出 warning，`mcpp build --strict` 下报错。

**Git 依赖与 `mcpp.lock`**:`tag` 和 `rev` 已经指向历史中的一个固定点，而
`branch` 是会移动的。首次构建把分支解析成一个 commit 并写进 `mcpp.lock`,
此后每次构建都重新构建**那个** commit —— lock 是权威来源而不是缓存提示，
所以删掉 `~/.mcpp/git` 或换一台机器，都不会静默切换到更新的分支头。要用新的
分支头，必须显式请求：

```bash
mcpp update mylib     # drop the recorded commit; the next build re-resolves it
mcpp update           # same, for every dependency
```

既然记录下来的 commit 已经足以决定构建什么内容，那么在 `~/.mcpp/git` 里已有
克隆的情况下，重新构建完全不发出网络请求，在 `--offline` 下照常工作。只有两件
事需要网络：解析一个在 lock 里没有 commit 的分支，以及克隆一个尚未缓存的
commit。`git =` 指向本地目录（或 `file://` URL）时，这两件事都不需要，因此
离线状态下从不会被拒绝。

**SemVer 约束**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret, default)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # Exact match
qux = ">=1.0, <2.0" # Range combination
```

### `visibility` —— 一个依赖的用法是否跨越本包自己的边界

```toml
[dependencies]
sdk = { version = "1.0", visibility = "private" }
```

每条依赖边都带一个 `visibility`，默认 `public`。它决定该依赖对消费方提出的
要求 ——`provides`/`requires` 之外，头文件目录、宏定义与 flag —— 只到达本包
自己的翻译单元，还是同时到达本包的**消费方**。

| 取值 | 本包自己的翻译单元 | 本包的消费方 |
|---|---|---|
| `public`（默认） | 是 | 是 |
| `private` | 是 | **否** |
| `interface` | 否 | 是 |

`private` 是实现细节的常规情形：一个被 vendor 进来的库，或者一个包为实现
某项功能而需要、但自己的接口并不暴露的平台 SDK。`interface` 是更少见的反向
情形 —— 本包自己的头文件会 `#include` 这个依赖，但本包从不编译链接它的目标
文件。`public` 是大多数依赖想要的：依赖的某个类型出现在本包自己的公开头文件
里，消费方就需要同一条头文件搜索路径才能用到它们。

在其中一个方向上写错是静默的（不必要的 `public` 把没人要的头文件广播
出去），在另一个方向上则是构建失败 —— 发生在第一个需要 `private` 所隐藏之物
的消费方那里 —— 这是更安全的失败方式，也是为什么 `[feature-deps.<feature>]`
（见 [06 ——「平台 SDK 依赖保持私有」](06-features-and-capabilities.md#平台-sdk-依赖保持私有)）
要显式写出 `private`，而不是依赖一个恰好能用、直到有消费方加入才会露馅的
默认值。

### 同一依赖两条声明冲突的处理

依赖图里的两条边可能指向同一个身份 —— 同一个 `(namespace, name)` 二元组 ——
却来自图中两个不同的位置：根 manifest 与某个依赖自己的 `[dependencies]`，
或者两个互不相干的依赖。一条声明有一个**种类**（`version`、`git` 或
`path`）和该种类下的一个**引用**（一条 SemVer 约束、一个 `git` URL 加
`rev`/`tag`/`branch`，或一个文件系统路径）。mcpp 只解析这个身份一次；每个
请求方的边都会被记录，其中一个请求方的声明决定了同一身份下所有其他请求方
拿到的是什么。

| 第一条声明 | 第二条声明 | 结果 |
|---|---|---|
| 任意 | 同种类、同引用 | 不变：第二条声明成为指向已解析身份的一条边，不报告任何信息。 |
| `version` | `version`，不同的约束 | 按上文所述，用 SemVer 对两个约束做 AND 合并；无法满足的一对被拒绝，并点名两个约束与两个请求方。 |
| `git` | `git`，不同的 `rev`/`tag`/`branch` | 根是两个请求方之一时，根的声明胜出；否则先解析出来的声明胜出。`dependency/source-override` 警告点名两个请求方与两个引用，说明哪一个被采用、原因是什么，以及如何取用另一个。这个警告绝不会被吞掉。 |
| `path` | `path`，不同的目录 | 规则与警告同上一行，只是比较的是规范化后的绝对目录，而不是 git 引用。 |
| 根的 `path` 或 `git` 声明 | 某个依赖的 `git` 或 `version` 声明（种类冲突） | 根的声明胜出，警告同上。若败下阵的声明是一条 `version` 需求，它会被拿去与根已解析出的那份 checkout 的 `[package] version` 核对；需求被违反就拒绝，并点名那个 pin、请求方与需求本身。 |
| 某个依赖的 `path`/`git` 声明 | 另一个依赖的不同种类的声明（种类冲突，且双方都不是根） | 拒绝："requested as both a … dep … and a … dep …. Pick one."。消息多出一句：在根里声明该身份即可解决。 |

这里根所拥有的特权，与 `linkage` 只在根 manifest 自己的边上生效（见上文
`[dependencies]`）是同一条边界：一个身份最终解析到*哪一份 checkout*，是一个
只有构件自己的 manifest 才能替某个依赖悄悄做出的整图级决定。两个依赖互相
冲突、且都不是根的情形，绝不会靠猜哪个先被声明来解决 —— 那正是本节要替换掉
的"队列顺序的意外"。

### `path` 或 `git` 依赖的身份（mcpp 2026.9.14.2+）

`path` 或 `git` 依赖就是它的 manifest 所声明的那个包，与指向它的键无关。一个
键规范化后的身份，若不同于 manifest `[package]` 的 `namespace` 与 `name`,
就采用 manifest 声明的身份；mcpp 对每条声明边告警一次，点名请求方、键、键所指
的身份，以及 manifest 声明的身份：

```toml
# comp/mcpp.toml; fw/mcpp.toml declares namespace = "huxdemo"
[dependencies]
fw = { path = "../fw" }            # names mcpplibs.fw; huxdemo.fw is used
```

```
warning: 'huxdemo.comp@path' declares the dependency 'fw', which names mcpplibs.fw; the manifest '.../fw/mcpp.toml' declares huxdemo.fw, and that identity is used.
  hint: write 'huxdemo.fw' in 'huxdemo.comp@path' to state the identity the manifest declares.
```

因此，同一目录上分别写作 `fw` 与 `huxdemo.fw` 的两条边是同一个包，只编译
一次，`mcpp why deps` 在它下面列出两个键。未声明命名空间的 manifest 取键的
命名空间，于是在这样一个目录上使用两个不同命名空间的键，就是同一来源上的两个
身份：构建在扫描之前被拒绝，点名二者；修正方式是在该 manifest 中声明
`namespace`，或两处写同一个键。`version` 依赖不受影响，它的身份就是键。

### git 仓库中的一个包（mcpp 2026.9.16.1+）

`git` 依赖指向一个仓库，键说明指的是仓库里的哪一个包。根 manifest 的包是
其一；根 manifest `[workspace] members` 的每一项是另一个。当键的身份不是
根包时，选中 manifest 声明该身份的那个 member，取同一个提交：

```toml
# repo/mcpp.toml declares spike.fw and [workspace] members = ["tool"];
# repo/tool/mcpp.toml declares spike.fw-installer
[dependencies]
spike.fw           = { git = "https://example.org/fw.git", rev = "cc3c74c5" }
spike.fw-installer = { git = "https://example.org/fw.git", rev = "cc3c74c5", tools = ["fw-installer"] }
```

- member 继承仓库的 `[workspace.package]`，与从仓库自己的检出构建时相同。
- member 中留在克隆目录之内的 `path` 边（例如 `spike.fw = { path = ".." }`）
  指向同一 git 源的同一提交，因此它就是根的键解析到的那个包，而不是第二条
  以 path 为源的声明。
- 既不指根包、也不指任何 member 的键，沿用上一节的规则：使用根 manifest 的
  身份，并给出警告。

键按身份选择，不存在 `subdir` 键：较旧的客户端会忽略这样的键，并不声不响地
构建根包。

### 命名空间解析规则

每个包的身份是**命名空间 + 名字**二元组。每个选择器都只规范化成一个身份：

- `cmdline` → `(mcpplibs, cmdline)`；省略 namespace 只表示默认 `mcpplibs`,
  不表示别的。
- `compat.gtest` → `(compat, gtest)`。
- `mcpplibs.capi.lua` → `(mcpplibs.capi, lua)`。

不存在有序回退，也不存在按短名的全索引模糊搜索：

```toml
# Correct — dotted selector
[dependencies]
chriskohlhoff.asio = "1.38.1"

# Correct — namespace sub-table (preferred for several packages from one org)
[dependencies.chriskohlhoff]
asio = "1.38.1"

# Wrong — a bare name never reaches the `chriskohlhoff` namespace
[dependencies]
asio = "1.38.1"
```

第三种写法会明确报错，指出实际尝试过的 `(mcpplibs, asio)` 身份；该短名若存在
于别处，错误信息会给出一个可直接复制的显式选择器。

#### 裸名过渡期（`2026.8.10.1` 起，`2026.9` 移除）

索引里每一个已发布的 `compat.*` 包，以及精确身份出现之前写下的每一份
manifest，都把它的依赖写成裸名 —— `gtest = "1.15.2"`。升级时让它们直接失败，
等于让一次程序发布，把已经发布、且无法追溯修改的数据作废，所以有一个版本的
过渡期：一个在 `mcpplibs` 未命中的裸名，仍可到达 `compat.<name>`，一个完全不
声明 namespace 的描述符，也仍可响应它的裸名。

但它不再是静默的：

```
warning: dependency 'gtest' resolved to 'compat.gtest' through the deprecated
bare-name search; namespace omission means `mcpplibs` only. Write the exact
package:
    [dependencies.compat]
    gtest = "1.15.2"
  (or run `mcpp add compat.gtest@1.15.2`). This fallback is removed in 2026.9.
```

写进 `mcpp.lock`、install 层与 cache 的是**规范身份**，因此歧义的拼法只存在
于一个地方 —— manifest 本身 —— 直到它被改写为止。`mcpp add gtest@1.15.2`
执行的正是这次改写。

这个过渡期**不适用于**写明了 namespace 的选择器（`mcpplibs.gtest` 未命中就
保持未命中），裸名也**仍然**永远到不了第三方 namespace。

**为什么只允许一个身份?** 依赖解析必须可复现。候选搜索会让两个短名相同的
namespace，由索引当时的状态来裁决，而新增一个索引就可能悄悄改变一个既有依赖
指向的目标。

**给 xpkg 作者：** 在一个索引描述符里，身份是 `(package.namespace,
package.name)` 这一对。namespace 是点分路径,**`name` 是单一的原子段**:

```lua
package = {
    namespace = "chriskohlhoff",
    name      = "asio",                 -- one segment; NOT "chriskohlhoff.asio"
}

package = {
    namespace = "mcpplibs.capi",        -- depth belongs here
    name      = "lua",
}
```

文件名只是一个提示 —— 描述符按它声明的身份被发现，所以
`pkgs/c/chriskohlhoff.asio.lua` 与 `pkgs/z/anything.lua` 解析结果完全相同。
`<name>.lua` 或 `<namespace>.<name>.lua` 是推荐写法（它们命中 mcpp 的快
路径），但不是强制要求。

较旧的完全限定拼法（`name = "chriskohlhoff.asio"`）仍被接受，因此已发布的
描述符不需要改动。`mcpp xpkg parse` 会校验这条规则，索引 CI 里应当运行它。
描述符身份要求 mcpp >= 0.0.106，精确选择器要求 mcpp >= 2026.8.10.1，两者都
使用 xlings >= 0.4.69。规范全文在 `docs/specs/package-identity.md`。

`mcpp new --template` 刻意复用同一套身份模型，而不是另造一套包文法：
`[ns.]name[@version][:tname]`。这里的裸名同样只表示 `mcpplibs`;version 与
模板名可以分别省略。省略 `tname` 会选中唯一的显式 default；若没有一个
`default = true` 而只有一个模板，该模板自动成为默认。多个未标默认的模板
是一个错误，绝不会按目录顺序选择。规范化的模板行见
`docs/specs/package-identity.md` §4.4。

### mcpp 何时刷新包索引

`mcpp build` / `run` / `test` **只在依赖无法用本地副本解析时**刷新包索引，
绝不会仅仅因为时间流逝就刷新。具体地说：本地根本没有索引、依赖的描述符不在
其中，或一条 SemVer 约束在本地已知版本里无解，这三种情况会触发刷新。只要
所有依赖都能在本地解析出来，不论本地索引多旧，构建都不会发出任何网络请求。

由此带来一个值得知道的推论：`^1.2` 这类约束，是对**本地索引已知的那些版本**
求解的。上游在上一次刷新之后发布的 `1.3.0`，在被主动取回之前不可见：

```bash
mcpp index update     # sync the index
mcpp update           # sync, then re-resolve dependencies
mcpp index status     # local state: state, age and revision
```

三个开关，按优先级从高到低：

| 开关 | 效果 |
|---|---|
| `--offline`（任意命令） | 完全不碰网络 —— 不刷索引、不下载、不自动安装工具链，也不发出 `git ls-remote`/`clone`。已安装的东西照常构建，包括 commit 已在 `mcpp.lock`、克隆已在缓存里的 git 依赖 |
| `MCPP_OFFLINE=1` | 同上，作用于整个 shell 会话或一次 CI job |
| `~/.mcpp/config.toml` 中的 `[index] auto_refresh = false` | 永不隐式刷新索引：依赖未命中时不刷新，安装本地索引没有的包之前不刷新，工程自定义索引的首次同步也不做（该次构建停止，并指出 `mcpp index update`）。下载仍然可用 |

`MCPP_NO_AUTO_INSTALL=1` 作为 `--offline` 更早、更窄的拼法，仍然被接受
（它只约束工具链的自动安装）。

一次刷新有时限。`[index] refresh_timeout`（单位秒，默认 120）是一次刷新
可用的最长时间；超过它就被终止，一条警告指出这个设置，构建像在任何一次刷新
失败之后一样，继续使用本地索引。经由 xlings 的安装，在 xlings 连续 300 秒
没有任何输出（包括心跳）时被终止。终止 mcpp 会一并终止它启动的 xlings 进程。

对任意命令加 `-v`，可以看到每个依赖的判定结果与原因。

## `[dev-dependencies]` —— 测试依赖

```toml
[dev-dependencies.compat]
gtest = "1.15.2"
```

`mcpp build` 忽略这些依赖；`mcpp test` 解析并使用它们。`mcpp test` 自动
发现 `tests/**/*.cpp` 并把它们编译为测试二进制。运行器与框架无关：每个文件
是一个独立的二进制，由退出码判定 —— 裸 `main`、gtest（经 `[dev-dependencies]`
加 `gtest_main`）或任何其他框架，行为完全一致，`-- args` 会转发给每一个测试
二进制（例如 `-- --gtest_filter=...`）。注意：合成出来的测试目标名可能包含
`/`(`tests/00-a/0.cpp` → `00-a/0`)，这与 `[targets.*]` 的名字不同 ——
两个命名空间是刻意分开的（测试目标从不进入 manifest，也不参与发布）。测试
以它相对 `tests/` 的路径命名（`tests/00-a/0.cpp` → `00-a/0`），每个测试
独立编译（一个测试写坏，只让它自己失败；包或依赖本身损坏则报告为构建
错误），`mcpp test <pattern>` 与 `--message-format json` 分别提供过滤与
机器可读输出。

## `[build-dependencies]` —— 构建期依赖（mcpp 2026.8.29.1+）

```toml
[build-dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

段与边上的请求回答的是**两个不同的问题**，把它们混为一谈是建模上的错误，
不是写法之争。

- **段**回答：这个包本身进不进目标。`[dependencies]` 表示进；
  `[build-dependencies]` 表示永不进，只能经由它到达的东西同样不进。
- **边上的请求**回答：想要它的哪一种构建期产物。`tools = [...]` 要一个宿主
  可执行文件，`host-module = true` 要一个构建程序可以 import 的模块。

一个包可以同时在这两个轴上取值，而 protobuf 正是证明这两个轴必须分开的
例子：一个工程既链接 `libprotobuf`，构建期又需要 `protoc`。它只写一次，
写在 `[dependencies]` 里：

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

`[build-dependencies]` 用于第一个轴无法表达的那种组合 —— 某个包的库不得
进入目标，而它的工具或规则仍然需要。同一个包同时出现在两张表里不是错误：
普通声明胜出，因为一行 `[build-dependencies]` 不应该悄悄拿掉目标真正需要
的库；两条声明各自请求的内容（`tools`、`features`、`host-module`、
`reexport`）都作用在同一条边上（mcpp 2026.9.16.1+；该版本之前，第二条声明
的请求会被丢弃）。

**只含程序的包，只贡献它的程序（mcpp 2026.9.16.1+）。** 一个声明的
`[targets]` 全部是程序（`bin`、`app`、`test`）的依赖，没有可链接的东西。
它的工具由工具子构建构建出来，子构建把这个包当作自己的根来解析；在消费方的
图里，它只提供它的工具与它的目录，别无其他：它自己的依赖不在那里解析，它的
源码不在那里编译，它的 `ldflags` 不进入消费方的链接。因此，一个程序可以
依赖请求它的那个包，SDK 正是这样提供一个针对自身构建出来的程序。没有
`[targets]` 表的包不受影响，即使某个 `src/main.cpp` 为它推断出一个程序。

包之间的环，在解析依赖图的地方就被拒绝，并列出环上的边，与缓存模式无关；
一个工具子构建再次请求正在构建的同一个工具时，在第一次重复处被拒绝，并给出
请求链。

与 `[dev-dependencies]` 不同，这些依赖**会**被传递遍历：一个构建期依赖自己
的依赖，正是让它能工作的东西，并且继承它"只服务构建"的性质。

一个 feature 可以为构建期请求划定范围，而无需另开一个声明处。
`[feature-deps.<name>]` 的条目可以用相同的源，重述一条已经无条件声明的
依赖，并为它追加 `tools`，所以"只在需要时才要"不需要另开一张表：

```toml
[dependencies]
spike.installer = { path = "../installer" }

[feature-deps.installer]
spike.installer = { path = "../installer", tools = ["installer"] }
```

这次重述要写出它的源，因为每一张依赖表都是如此：一个没有 `path`、`git`、
`version` 或 `workspace` 的条目会被当作命名空间表读取并被拒绝，拒绝消息会
说明要重述源。重述中的 `tools`、`features`、`host-module` 与 `reexport`
会加到这一行当前生效的声明上。重述若写了另一个源，会被拒绝，并列出两个源
(mcpp 2026.9.16.1+)；该版本之前它会被忽略。

> 这个段很早就能被解析，而直到 2026.8.29.1，没有任何做决定的代码读过它：
> 写下它得到的是一份能加载的 manifest、零诊断、零效果。

## 当前边界

- **只有两件事需要网络，而且只有这两件：** 解析一个在 lock 里没有 commit 的
  分支，以及克隆一个尚未缓存的 commit。指向本地目录或 `file://` URL 的
  `git =`，两者都不需要，因此离线时从不会被拒绝。
- 索引刷新窗口**不适用于写明了 namespace 的选择器**。`mcpplibs.gtest` 一旦
  未命中，就保持未命中，直到下一次刷新。
- `mcpp.lock` 记录并核验一次解析，但不约束一次解析。见
  [51 —— 受支持的版本与兼容性](51-supported-versions.md)。
