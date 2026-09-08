# 05 —— 依赖与解析

**读者:**构建里已经不只有自己代码的作者。

**本章回答的那一个问题:**一个依赖从哪里来,版本约束是什么意思,以及两个约束
不一致时会发生什么。

**不在这里:**什么使两个包成为同一个包 —— 那是
[SPEC-001](../specs/package-identity.md),本章施用它而不复述它;以及怎么发布一个包,
那是 [11 —— 发布一个库](11-publishing-a-library.md)。

在此之前:[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md) 是这些表与其余表同处的
地方。在此之后:[06 —— Feature 与能力](06-features-and-capabilities.md) 讲一个依赖
怎么变成可选的。

## `[dependencies]` — 运行时依赖

**推荐写法是带精确版本的 dotted selector。** dotted 形式点名的是**一个身份** ——
最后一段是包名,之前所有段都是 namespace —— 因此解析到什么,不取决于当前配置了
哪些 namespace。

```toml
[dependencies]
compat.gtest               = "1.15.2"
imgui.core                 = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"
mcpplibs.capi.lua          = "0.0.3"
```

裸名也被接受,它经由默认 namespace(`mcpplibs`)解析 —— 在只用那一个 namespace 的
工程里方便,在不是的工程里有歧义:

```toml
[dependencies]
cmdline = "0.0.2"        # 解析为 mcpplibs.cmdline
```

<details>
<summary>等价拼法:namespace 子表</summary>

把同一 namespace 下的条目归组。它解析到的身份与 dotted 形式完全相同,在许多依赖
共享一个 namespace 时值得用。

```toml
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # 显式 namespace,不做回退搜索
```

</details>

```toml
# 路径依赖(本地开发)
[dependencies]
mylib = { path = "../mylib" }
```

```toml
# Git 依赖 —— tag / branch / rev 三选一
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }
applib = { git = "https://github.com/user/applib.git", branch = "develop" }
```

```toml
# 长式 dep spec:features 与 backend 旋钮
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # 请求该依赖的 feature
widget = { version = "1.0", backend = "glfw_opengl3" }  # 糖:= features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` 是**通用约定糖**:1:1 脱糖为请求该依赖的 `backend-<impl>`
feature(库若支持该旋钮,应在自己的 `[features]` 中声明 `backend-*` 系列)。
若目标包声明了 `[features]` 但不含所请求的 feature(含 backend 脱糖结果),
默认给出 warning,`mcpp build --strict` 下报错。

**Git 依赖与 `mcpp.lock`**:`tag` 和 `rev` 本身就指向历史中的固定点,而 `branch`
是会动的。首次构建把分支解析成一个 commit 并写进 `mcpp.lock`,此后每次构建都重建
**那个** commit —— lock 是权威而不是缓存提示,所以删掉 `~/.mcpp/git` 或换一台机器
都不会静默切换到更新的分支头。需要新的分支头时,必须显式指定:

```bash
mcpp update mylib     # 丢掉记录的 commit,下次构建重新解析
mcpp update           # 同上,对所有依赖
```

既然记录的 commit 已经足以决定构建什么,那么在 `~/.mcpp/git` 里已有克隆的情况下,
重新构建完全不发网络请求,`--offline` 下照常工作。只有两件事需要网络:解析一个在
lock 里没有 commit 的分支,以及克隆一个尚未缓存的 commit。`git =` 若指向本地目录
(或 `file://` URL),这两件事都不需要网络,因此离线下也绝不会被拒绝。

**SemVer 约束**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret,默认)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # 精确匹配
qux = ">=1.0, <2.0" # 范围组合
```

### 命名空间解析规则

每个包的身份是**命名空间 + 名字**二元组。每个 selector 都只规范化成一个身份:

- `cmdline` → `(mcpplibs, cmdline)`;省略 namespace 只表示默认 `mcpplibs`。
- `compat.gtest` → `(compat, gtest)`。
- `mcpplibs.capi.lua` → `(mcpplibs.capi, lua)`。

不存在有序回退或按短名的全索引模糊搜索:

```toml
# 正确 —— 点式选择器
[dependencies]
chriskohlhoff.asio = "1.38.1"

# 正确 —— 命名空间子表(同一组织有多个包时更推荐)
[dependencies.chriskohlhoff]
asio = "1.38.1"

# 错误 —— 裸名永远到不了 chriskohlhoff 命名空间
[dependencies]
asio = "1.38.1"
```

第三种写法会明确报错,指出实际尝试的 `(mcpplibs, asio)`;若该短名存在于别处,错误信息会给出可直接复制的显式 selector。

#### 裸名过渡期(`2026.8.10.1` 起,`2026.9` 移除)

索引里已发布的 `compat.*` 包与既有 manifest **全部**写成裸名(`gtest = "1.15.2"`)。
升级后直接失败,等于让一次程序发布把**已经发布、且无法追溯修改**的数据作废,
所以有一个版本的过渡期:裸名在 `mcpplibs` 未命中时仍可到达 `compat.<name>`,
不声明 namespace 的 descriptor 也仍可被裸名解析。

但它不再静默:

```
warning: dependency 'gtest' resolved to 'compat.gtest' through the deprecated
bare-name search; namespace omission means `mcpplibs` only. Write the exact
package:
    [dependencies.compat]
    gtest = "1.15.2"
  (or run `mcpp add compat.gtest@1.15.2`). This fallback is removed in 2026.9.
```

写进 `mcpp.lock`、install 与 cache 的是**规范身份**;歧义拼写只存在于工程的
`mcpp.toml` 中,直到被改写 —— `mcpp add gtest@1.15.2` 会完成这次改写。

过渡期**不适用于**写明 namespace 的 selector(`mcpplibs.gtest` 未命中就是未命中),
裸名也**仍然**到不了第三方 namespace。

**为什么只允许一个身份?** 因为依赖解析必须可复现。候选搜索会让同短名包受索引状态影响,新增索引还可能悄悄重定向既有依赖。

**给 xpkg 作者:** 索引描述符里,身份是 `(package.namespace, package.name)` 二元组。命名空间是点分路径,**`name` 是单一原子段**:

```lua
package = {
    namespace = "chriskohlhoff",
    name      = "asio",                 -- 单一段;不是 "chriskohlhoff.asio"
}

package = {
    namespace = "mcpplibs.capi",        -- 层级放这里
    name      = "lua",
}
```

文件名只是提示 —— 描述符按声明的身份被发现,所以 `pkgs/c/chriskohlhoff.asio.lua` 与 `pkgs/z/anything.lua` 解析结果完全相同。推荐 `<name>.lua` 或 `<namespace>.<name>.lua`(命中 mcpp 的快路径),但不强制。

旧的完全限定拼写(`name = "chriskohlhoff.asio"`)仍被接受,已发布的描述符无需改动。`mcpp xpkg parse` 会校验该规则,请在索引 CI 里跑它。描述符身份需要 mcpp >= 0.0.106,精确 selector 需要 mcpp >= 2026.8.10.1,两者使用 xlings >= 0.4.69;规范全文见 `docs/specs/package-identity.md`。

`mcpp new --template` 刻意复用同一身份模型，而不是另造包文法:
`[ns.]name[@version][:tname]`。其中裸名同样只表示 `mcpplibs`，version 与模板名可分别
省略。省略 `tname` 时选择唯一显式 default；若未写 `default = true` 且只有一个模板，
该单模板自动成为默认。多个未标默认的模板会报错，绝不按目录顺序选择。规范表见
`docs/specs/package-identity.md` §4.4。
### mcpp 何时刷新包索引

`mcpp build` / `run` / `test` **只在依赖无法用本地索引解析时**刷新包索引,绝不会
因为"时间到了"就刷。具体地说,只有三种情况会触发:本地根本没有索引、依赖的描述符
不在其中、或 SemVer 约束在本地已知版本里无解。只要所有依赖都能在本地解析出来,
无论本地索引多旧,构建都不会发起任何网络请求。

由此带来的一个需要知道的语义:`^1.2` 这类约束是对**本地索引已知的版本**求解的。
若上游在上次刷新之后发布了 `1.3.0`,需要主动获取:

```bash
mcpp index update     # 同步索引
mcpp update           # 同步索引,并重新解析依赖
mcpp index status     # 看本地现状:状态、年龄、修订号
```

三个开关,优先级从高到低:

| 开关 | 作用 |
|---|---|
| `--offline`(任意命令) | 完全不碰网络——不刷索引、不下载、不自动装工具链,也不发 `git ls-remote`/`clone`。已安装的东西照常构建,包括 commit 已在 `mcpp.lock`、克隆已在缓存里的 git 依赖 |
| `MCPP_OFFLINE=1` | 同上,作用于整个 shell 会话或 CI job |
| `~/.mcpp/config.toml` 里 `[index] auto_refresh = false` | 永不自动刷新索引,但下载仍然可用 |

`MCPP_NO_AUTO_INSTALL=1` 作为 `--offline` 的旧式窄化拼写仍然有效(它只管工具链的
自动安装)。

任意命令加 `-v` 可以看到每个依赖的判定结果与原因。

## `[dev-dependencies]` —— 测试依赖

```toml
[dev-dependencies.compat]
gtest = "1.15.2"
```

`mcpp build` 忽略这些依赖;`mcpp test` 解析并使用它们。`mcpp test` 自动发现
`tests/**/*.cpp` 并把它们编译为测试二进制。运行器与框架无关:每个文件是一个独立的
二进制,以退出码判定 —— 裸 `main`、gtest(经 `[dev-dependencies]` + `gtest_main`)
或任何其他框架的行为完全一致,`-- args` 会转发给每个测试二进制
(例如 `-- --gtest_filter=...`)。注意:合成的测试目标名可能包含 `/`
(`tests/00-a/0.cpp` → `00-a/0`),这与 `[targets.*]` 名不同 —— 两个命名空间是
刻意分开的(测试目标从不进入 manifest,也不参与发布)。测试按其相对 `tests/` 的
路径命名(`tests/00-a/0.cpp` → `00-a/0`),每个测试独立编译(一个测试写坏只让它
自己失败;包或依赖损坏则报告为构建错误),`mcpp test <pattern>` 与
`--message-format json` 分别提供过滤与机器可读输出。

## `[build-dependencies]` —— 构建期依赖(mcpp 2026.8.29.1+)

```toml
[build-dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

段与边上的请求回答的是**两个不同的问题**,把它们混为一谈是建模上的错误,不是写法之争。

- **段**回答:这个包本身进不进目标。`[dependencies]` 进;`[build-dependencies]` 永不进,
  只能经由它到达的东西也一样。
- **边上的请求**回答:要它的哪一种构建期产物。`tools = [...]` 要一个宿主可执行文件,
  `host-module = true` 要一个构建程序可以 import 的模块。

一个包可以同时在两个轴上取值,而 protobuf 正是证明这两个轴必须分开的例子:工程既链接
`libprotobuf`,构建期又需要 `protoc`。它只写一次,写在 `[dependencies]` 里:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

`[build-dependencies]` 用于第一个轴无法表达的那种组合 —— 某个包的库不得进入目标,
而它的工具或规则仍然需要。同一个包同时出现在两张表里不是错误:普通声明胜出,因为
一行 `[build-dependencies]` 不应该悄悄拿掉目标真正需要的库。

与 `[dev-dependencies]` 不同,这些依赖**会**被传递遍历:一个构建期依赖自己的依赖正是
让它能工作的东西,并且继承它「只服务构建」的性质。

feature 可以为构建期请求划定范围而无需第二个声明处 —— `[feature-deps.<name>]` 可以给
一条已经无条件声明的依赖追加 `tools`,所以「按需才要」不需要另开一张表。

> 这个段很早就能被解析,而直到 2026.8.29.1 之前没有任何做决定的代码读它:写下它得到的是
> 一份能加载的清单、零诊断、零效果。

## 当前边界

- **只有两件事需要网络,也只有这两件:**解析一个在锁里没有 commit 的分支,以及克隆
  一个尚未缓存的 commit。指向本地目录或 `file://` URL 的 `git =` 两者都不需要,因此
  离线时从不会被拒绝。
- 索引刷新窗口**不适用于写明了 namespace 的选择器**。`mcpplibs.gtest` 一旦未命中就
  保持未命中,直到下一次刷新。
- `mcpp.lock` 记录并核验一次解析,但不约束解析。见
  [51 —— 受支持的版本与兼容性](51-supported-versions.md)。
