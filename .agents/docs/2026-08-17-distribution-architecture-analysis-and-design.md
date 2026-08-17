# mcpp 分发架构:全面分析与方案(2026-08-17)

> 覆盖:源码分发 / 二进制分发 / 多平台打包 / 私有分发,以及 issue #433
> (「预编译 .so + .ixx/.h/.cppm 接口」)。
>
> 前半是**发现**(现状是什么、证据在哪、file:line 与实测),后半是**方案**
> (改什么、怎么算通过)。第 7 节是需要 review 的决策点。
>
> **⇒ 方案已独立成文:`2026-08-17-library-distribution-design.md`**
> (落地形态、docs 计划、examples、CI 验证矩阵、分期)。**本文是发现,那份是方案。**
>
> 关联 issue:#433(本文起因)、#290(描述符构建规则不能按版本区分)、
> #304(`runtime.library_dirs` 同时落在链接线上)、#276(嵌入式 SDK 集成)、
> #416(纯 C 包被链进 libstdc++)。

---

## 0. 一页纸结论

**五条结论,按重要性排序:**

1. **#433 想要的东西,在 Linux 上今天就能跑通 —— 我实测跑通了。**
   一个「只有模块接口 `.cppm` + 预编译 `libfoo.so`」的包,通过 `[runtime]`
   的 `libraries` / `link_library_dirs` / `runtime_search_dirs` 被消费者
   `import` 并链接,`std::string` 和异常都能跨边界。**零引擎改动。**
   见 §2。

2. **但它「能跑」不等于「能分发」,而且失败是静默的。** 同一次实测暴露三件事:
   - 产出的 `.so` 里烧着**生产者机器的绝对 RUNPATH**(`/home/speak/.mcpp/...`),
     换台机器就是另一回事 —— 而 `mcpp pack` 的 RUNPATH/INTERP 重写**只服务可执行文件,不服务库**;
   - 进程里同时有**两份 C++ 运行时**(exe 静态 libstdc++ + `.so` 动态
     `libstdc++.so.6`),没有任何一层告警;
   - **接口与二进制之间没有任何绑定**。我把随包发的 `.cppm` 里
     `struct Point { int x; int y; }` 改成 `{ int y; int x; }`(mangling 不变),
     **编译通过、链接通过、运行通过、打印出交换后的错数据**。全程零诊断。

3. **真正的结构性缺口不在「能不能链」,在四处**:
   ① 描述符**强制要求 `sources`**(`xpkg.cppm:2059`),所以「无源包」这个种类不存在
   —— 生态里现有的做法是造一个假的 anchor `.c`(见 `compat.openblas`);
   ② `kind = "shared"` **只有 Linux/ELF**(`plan.cppm:1006` 直接拒绝),
   即 `.dll` / `.dylib` 这两条腿**在生产侧根本不存在**;
   ③ **没有兼容标签**(wheel tag 那种东西)—— `abi.cppm` 有五维模型但太粗
   (无编译器版本、无 stdlib 版本、无标准档位);
   ④ **安装线协议没有 target 轴**(`package_fetcher.cppm:419` 只发
   `{"targets":[...]}`),所以交叉编译一定拿到宿主 arch 的载荷。

4. **能力其实已经攒够了,缺的是把它们接起来。**
   `cache_key.cppm` 已经在算一个覆盖 toolchain × 语言 × profile × 身份 ×
   自身配置 × 上游 Merkle 的**逐包 ABI 完备键**,并且全局构建缓存
   (`$MCPP_HOME/build-cache/v1/pkg/<index>/<pkg>@<ver>/<key16>/`)里**已经躺着
   BMI + obj**。二进制分发格式 ≈ **可搬运的构建缓存条目**。
   xim 的 XPackage Spec V2 也**已经有一等的 arch 轴**(per-arch resource map /
   URL 模板 / per-arch sha256)。

5. **最短路径不是索引,是文件。** issue 作者原话是「类似 `pip install runtime.whl`」。
   `mcpp add ./runtime-0.1.0-<tag>.mpkg` 不需要索引、不需要网络、不需要鉴权、
   **不需要 xlings 改一行**,完全在 mcpp 自己手里。**建议它做 P0**,索引通路做 P1。

7. **包自带一份 `mcpp.toml` —— 这不是新机制,是 mcpp 已有且文档推荐的 Form A**
   (`prepare.cppm:2764`:描述符没有 `mcpp` 字段时,glob 载荷里的 `mcpp.toml`)。
   它一次消掉三样东西:**新描述符键**(⇒ 不需要版本 floor ⇒ 老客户端不会被砖)、
   **新消费代码路径**、以及 —— 通过「胖包 + `[target.'cfg(...)']` 在消费者构建期
   选 arch」—— **整个 G3**(描述符 arch 轴 + 安装线 target 轴 + 动 xlings)。
   实测边界:`[distribution]` 标量段今天被静默接受;但 `artifacts = [{…}]`
   **硬失败**,产物清单必须借用已在白名单里的 `[[runtime.artifacts]]`。

6. **P0/P1 按「形态」切,不按「平台」切**(§4.5.1 / §5)。`kind = "lib"` **没有平台
   限制**,静态形态今天三平台就能产出,而且**消掉了整整四类问题**:RUNPATH 重写、
   DLL 部署、双 C++ 运行时、加载器搜索闭包 —— 上面第 2 条里的三处静默失败,
   静态形态天然只剩「接口↔二进制绑定」那一处。所以
   **P0 = static × 三平台,P1 = shared × 三平台**;按平台切会让 Windows/macOS
   在 P0 结束时拿到一个空盒子。

---

## 1. 今天的分发架构:五条通路

mcpp 今天有五条互相独立的「东西怎么到达另一台机器」的通路。它们从未被放在一张表里
比较过,而 #433 的困惑正来自这里:作者看到的是通路 A 和通路 B,而他要的是通路 C,
通路 C 今天只以「手法」的形式存在,没有名字。

### 1.1 通路 A —— 源码分发(库)

```
你的仓库 → git tag → GitHub 自动生成 tag tarball
   → gitcode 镜像(逐字节相同,CN 区)
   → mcpplibs/mcpp-index 的 pkgs/<x>/<name>.lua(GLOBAL + CN URL + sha256)
   → publish-artifact.yml 推一个内容哈希 artifact
   → 消费者 bump 版本
```
(`docs/10-publishing-a-library.md`)

- 生产侧:`mcpp publish` → `git archive` 出 tarball + sha256 + 生成 `xpkg.lua`
  (`publish/pipeline.cppm:69`),然后**人工**开 PR 到索引。
- 描述符两种形态:**Form A**(tarball 里自带 `mcpp.toml`)与 **Form B**
  (描述符内联 `mcpp = { ... }` 块,`xpkg.cppm:synthesize_from_xpkg_lua`)。
- 关键事实:`make_release_info` 把 **linux / macosx / windows 三个平台块填成同一个
  URL**(`publisher.cppm:295-302`)。源码 tarball 在三个平台上是同一份字节 ——
  **这条通路天生没有多平台问题,因为它根本不区分平台。**

### 1.2 通路 B —— 应用二进制分发(`mcpp pack`)

四个 mode(`docs/02-pack-and-release.md`):`system` / `vendored`(默认) /
`self-contained` / `static`。两条产出族:ELF/Mach-O → `.tar.gz`(`lib/` + 重写
RUNPATH);PE → `.zip`(DLL 与 `.exe` 平铺,因为 PE 没有 rpath)。

- **它的核心价值不是打包,是重写**:每个 mode 都会重写 `PT_INTERP` 与
  `DT_RUNPATH`,因为开发构建产物寻址的是**这台机器**的载荷目录。e2e 215 会扫遍
  bundle 里每个 ELF,发现 `$MCPP_HOME` 下的路径就失败。
- **它的边界:只服务可执行程序。** `mcpp pack` 的输入是 `builtBinary`
  (`pack.cppm:Plan::builtBinary`),整条流水线围绕「一个 exe + 它的闭包」。
  **没有任何 `mcpp` 命令会对一个库做同样的重写。** 这是 §2.2 那条 RUNPATH
  发现的根源。

### 1.3 通路 C —— 预编译库的「事实上」通路(anchor TU 手法)

今天生态里确实有预编译二进制在分发,但它没有名字,是一组手法。样板是
`compat.openblas`(`~/.mcpp/registry/data/mcpplibs/pkgs/c/compat.openblas.lua`):

| 需要表达的事 | 今天怎么写 |
|---|---|
| 「我没有源码要编」 | **造一个假的 `mcpp_openblas_anchor.c`**,因为 `sources` 是强制的 |
| 头文件 | `include_dirs = { "include" }` |
| 链接预编译库 | `ldflags = { "-Llib", "-lopenblas" }`(`-L` 被 mcpp 重写成 `<verdir>/lib`) |
| Windows 导入库 vs 静态库 | **per-OS 块**,`windows = { ldflags = { "-Llib", "-llibopenblas" } }` |
| 运行期 DLL | `windows = { runtime = { library_dirs = { "bin" } } }` → 拷到 `.exe` 旁 |
| 平台差异的入口 | linux/macosx 由 `install()` 钩子**写出** anchor;windows 用 `generated_files` |

这套能工作,但它是**反向表达**:包的意图是「不要编译我,链接我」,而写法是
「编译一个什么都不做的 `.c`,顺便偷偷加几个链接 flag」。后果:

- 「这个包是预编译的」这件事**不可查询** —— 没有字段、没有 lint、没有诊断;
- 没有任何**兼容性检查**:一个用 gcc 13 编的 `libopenblas.a` 和一个 gcc 16 的
  消费者之间,mcpp 无话可说(C 库侥幸没事,C++ 库会炸);
- `runtime.library_dirs` 这个名字**同时管链接和运行**,这就是 **#304**。

### 1.4 通路 D —— 私有分发

今天三种形态,全部是**源码**:

| 形态 | 写法 | 鉴权 | 状态 |
|---|---|---|---|
| path 依赖 | `foo = { path = "../foo" }` | 无需 | ✅ 可用 |
| git 依赖 | `foo = { git = "...", rev/tag/branch = "..." }` | **环境里的 git 凭据** | ✅ 可用 |
| 私有索引 | `[indices] acme = { url = "git@..." }` 或 `{ path = "/srv/index" }` | **环境里的 git 凭据** | ✅ 可用 |

缺口:
- `IndexSpec`(`pm/index_spec.cppm:14`)**没有任何鉴权字段** —— 全靠 ambient
  git credential helper / SSH key。私有 HTTPS + token 只能把 token 写进 URL。
- `IndexSpec::artifact`(内容哈希 artifact 通道)是**为公开 GitHub Actions 设计的**,
  私有场景没有对应机制。
- **没有「离线包」入口**:没有 `mcpp add ./something.mpkg`。这正是 issue 作者
  要的形状。

### 1.5 通路 E —— 工具链与运行时载荷(xim / xlings 层)

mcpp 自己的 payload(gcc/llvm/glibc/ninja/…)走 xim。这条通路**已经有一等 arch 轴**:
XPackage Spec V2 的 per-arch resource map / URL 模板 + per-arch sha256 /
`xpm.source`,并且是 fail-closed 的(`xim-pkgindex/docs/V2/xpackage-spec.md`)。

但它有一个对二进制分发致命的性质:**arch 在安装时按宿主解析**
(spec 原文:"arch is resolved per-host at install time")。而 mcpp 调用它时
只发 `{"targets":[...],"yes":true}`(`package_fetcher.cppm:419-431`)——
**没有 os、没有 arch、没有 triple**。交叉编译时,拿到的是宿主 arch 的载荷。

对源码包无害(源码 tarball 与 arch 无关)。**对二进制包是致命的。**

### 1.6 能力矩阵

| | A 源码库 | B 应用 pack | C 预编译库(手法) | D 私有 | E 载荷 |
|---|---|---|---|---|---|
| Linux | ✅ | ✅ | ⚠️ 手法 | ✅ | ✅ |
| macOS | ✅ | ✅ | ⚠️ 手法 | ✅ | ✅ |
| Windows | ✅ | ✅ | ⚠️ 手法 | ✅ | ✅ |
| arch 轴 | n/a | ✅(`--target`) | ❌ 描述符载荷无 arch | n/a | ✅ V2 |
| 交叉编译 | ✅ | ✅ | ❌ 安装线无 target | ✅ | ⚠️ 按包名绕开 |
| 生产 `.so` | n/a | n/a | **仅 Linux** | n/a | n/a |
| 生产 `.dll`/`.dylib` | n/a | n/a | ❌ **不存在** | n/a | n/a |
| ABI 兼容检查 | n/a | n/a | ❌ | ❌ | ⚠️ 五维粗粒度 |
| 接口↔二进制绑定 | n/a | n/a | ❌ **实测静默错数据** | ❌ | n/a |
| 鉴权 | 公开 | n/a | n/a | ⚠️ ambient git | 公开 |
| 离线包安装 | ❌ | n/a | ❌ | ❌ | n/a |

---

## 2. 实测:#433 要的东西,今天在 Linux 上已经跑通

复现材料在 `scratchpad/bindist/`(附录 A 有完整脚本)。

### 2.1 步骤与结果

**① 生产者**(`provider/`):模块接口只有声明,实现在实现单元里。

```cpp
// src/runtimelib.cppm
export module runtimelib;
export namespace rt { int answer(); std::string name(); void boom(); }
// src/impl.cpp
module runtimelib;
namespace rt { int answer(){return 42;} ... }
```
```toml
[targets.runtimelib]
kind = "shared"
```
`mcpp build` → `bin/libruntimelib.so`,导出**恰好两个**符号:

```
T _ZGIW10runtimelib              ← 模块初始化器
T _ZN2rtW10runtimelib6answerEv   ← rt::answer(),带 W10runtimelib 模块附着
```

**② 「二进制包」**(`dist/`):只有接口源 + 预编译库 + 一个 `mcpp.toml`。

```
dist/
├── mcpp.toml
├── src/runtimelib.cppm      ← 只有声明
└── lib/libruntimelib.so     ← 从 ① 拷来
```
```toml
[build]
sources = ["src/runtimelib.cppm"]
[targets.runtimelib]
kind = "lib"
[runtime]
libraries           = ["runtimelib"]
link_library_dirs   = ["lib"]
runtime_search_dirs = ["lib"]
```

**③ 消费者**(`app/`):`runtimelib = { path = "../dist" }`,`import runtimelib;`

```
$ mcpp run
answer=42
name=from-the-prebuilt-so (len=20)
caught runtime_error: thrown inside the .so
```

**跨边界的 `std::string` 和 C++ 异常(含 RTTI 类型匹配)都正常。** 消费者只编译
了那一个 `.cppm`(产出 BMI + 一个近乎空的 object),其余符号解析到 `.so`。
`mcpp` 引擎**一行没改**。

> 顺带一个与本议题无关但会绊人的坑:消费者 TU 里 `import` 必须写在 `#include`
> **之后**。写在之前时 GCC 16.1 把后续 include 的声明卷进了奇怪的作用域,报
> `In function 'int std::main()'` —— 报错点与原因完全不沾边。

### 2.2 三处它没有告诉你的事

**(a) 发出去的 `.so` 烧着生产者机器的绝对路径。**

```
$ readelf -d dist/lib/libruntimelib.so
 (NEEDED)   libstdc++.so.6
 (RUNPATH)  /home/speak/.mcpp/registry/data/xpkgs/xim-x-glibc/2.44/lib64:
            /home/speak/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64:
            /home/speak/.mcpp/registry/subos/default/lib
```

这正是 `docs/02-pack-and-release.md` 关于路线 A 讲的那件事(「烧进去的
`PT_INTERP` 指的是**你**的机器」),只不过对象从可执行文件换成了库 ——
而**库这一侧没有 `mcpp pack`**。手工拷贝出去的 `.so` 是机器绑定的。

**(b) 一个进程里两份 C++ 运行时,零告警。**

```
app  : NEEDED = libruntimelib.so, libm, libgcc_s, libc     ← 没有 libstdc++
       nm -D | grep std | wc -l = 528                      ← 静态 libstdc++ 进来了
.so  : NEEDED = libstdc++.so.6                             ← 动态
```
这是 `distribution.cppm` 里 `Role::SharedLibrary` 那段注释描述的危险,方向反过来:
exe 的静态 libstdc++ 把符号以 GLOBAL 导出,`.so` 绑到了它上面。**这次侥幸对了**
—— 两边是同一个 gcc 16.1 的 libstdc++。生产者换个编译器版本,就是一个进程里
两份不同实现的 `std::string`。

契约模型本身是对的(`Role::Distributable` 默认 SelfContained,
`Role::SharedLibrary` 默认 ToolchainCoupled),问题是 **contract 是根工程的设置,
预编译 `.so` 的 contract 在生产时就冻结了,消费者看不见也检查不到。**

**(c) 接口与二进制之间没有任何绑定 —— 这是最严重的一条。**

把随包发的 `.cppm` 改成结构体字段互换(Itanium 不 mangle 字段顺序,符号名不变):

```cpp
// 生产者编进 .so 的:  struct Point { int x; int y; };  origin() 返回 {111, 222}
// 随包发出的接口:      struct Point { int y; int x; };   ← 只改了这一行
```
```
$ mcpp run
x=222 y=111   (producer meant x=111 y=222)
```

**编译通过、链接通过、运行通过、数据是错的、全程零诊断。** 同样的实验用返回类型
(`int` → `long long`)也一样静默通过。

这不是「mcpp 的 bug」——C++ 语言层面这就是 IFNDR。但它是**分发格式必须解决的问题**:
只要接口和二进制可以被分别替换,这个失败就是可达的,而且它跨越机器和时间
(「上周谁把那个头改了」)。

### 2.4 实验二:自动生成 + 接口闭包 + 跨平台(2026-08-17,`scratchpad/lab/`)

写了一个 176 行的 `mcpp pack <target>` 原型(`lab/mkdist.py`),对一个真实的多单元库
(主接口 + 接口分区 + **实现分区** + 实现单元 + C API + 头文件)跑
**x86_64-linux-gnu / x86_64-linux-musl / x86_64-windows-gnu** 三个 target。
它推翻了我上一轮写进本文的**两条规则**,并找出**一个新缺陷**。

#### 2.4.1 包内 `mcpp.toml` 能自动生成 —— 原料今天全部已暴露

`mcpp build --print-fingerprint` 的 11 个字段里,tag 需要的六个全在:

```
[1] gcc          [2] 16.1.0        [4] x86_64-linux-gnu
[5] libstdc++ 16.1.0               [6] c++23         [11] 0a0ba53e8ca69b41(runtime binding)
```

`abi_tag` 是它们的**纯投影**,不需要任何新推导 —— §4.3 的主张就地验证。

**⚠️ 但 [4] 是编译器自报的 triple,不是 mcpp 的规范 triple。**
原型第一版直接用 [4],windows 那条腿产出的 tag 是
`x86_64-w64-mingw32-gcc16-…`,而同一份 `mcpp.toml` 里 `[target.'…']` 键写的是
`x86_64-windows-gnu` —— **同一个决定,两个拼写**。真实实现必须用
`triple.cppm` 的规范拼写(`cfgpred` 的注释也说 cfg 词汇 **IS** 规范 triple 词汇)。

#### 2.4.2 「接口怎么制定」= 模块闭包,不是 `.m.o`,也不是 grep

工程结构:

```
src/mathkit.cppm   export module mathkit;  export import :api;
src/api.cppm       export module mathkit:api;      ← 接口分区
src/secret.cppm    module mathkit:secret;          ← 实现分区(闭源逻辑)
src/impl.cpp       module mathkit;  import :secret;
```

| 做法 | 结果 |
|---|---|
| 只发 `mathkit.cppm` + `api.cppm` + `.a` | ✅ 消费者构建成功、`add(2,3)=5` |
| 主接口改成也 `import :secret;`,仍不发它 | ❌ **硬失败**:`mathkit:secret: error: failed to read compiled module` |

**判据:发布集 = 从主接口单元出发、沿其 purview 内 `import` 的传递闭包。**
实现分区只被实现单元 import 时**不在闭包里,不发布,源码不泄露**。

**失败的不对称性(这才是必须算而不是猜的理由):**

| | 后果 |
|---|---|
| 发少了 | **响** —— 编译期 `failed to read compiled module`,点名模块 |
| 发多了 | **哑** —— 静默把闭源实现分区的源码发出去 |

**⚠️ `.m.o` 不是判据 —— 用它会泄露源码。** 实测:实现分区 `secret.cppm`
**照样产出 `secret.m.o`**(`.m.o` 的含义是「模块单元的对象」,不是「接口的对象」)。
按 `.m.o` 挑要发布的源,就会把 `secret.cppm` 发出去。

#### 2.4.3 ⚠️ 同一个闭包有第二个用途 —— 我上一轮把它写错了

上一轮我写「打包时剔除归档里的接口对象」,规则给的是**「剔除所有 `.m.o`」**。
原型照做,**三个 target 全部链接失败**:

```
libmathkit.a(impl.o): in function `mk::add@mathkit(int, int)':
   undefined reference to `mk::secret_helper@mathkit()'
```

因为 `secret.m.o` 里是**真代码**。正确规则与 §2.4.2 是**同一个闭包**:

> **剔除的是「已发布接口闭包里那些单元」的对象,不是所有 `.m.o`。**

修正后三个 target 全部构建成功,归档里剩下 `secret.m.o` + `impl.o` + `capi.o`。

#### 2.4.4 平台差异

| 事实 | linux-gnu | linux-musl | windows-gnu(MinGW) |
|---|---|---|---|
| 静态库文件名 | `libmathkit.a` | `libmathkit.a` | **`libmathkit.a`** |
| mangling | `_ZN2mkW7mathkit3addEii` | 同 | **同**(Itanium) |
| `.m.o` 命名 | 一致 | 一致 | 一致 |
| 产物 | ELF | ELF | **PE32+ executable, 18 sections** |

- **产物命名按 `env` 分,不按 OS 分**:MinGW 走 GNU 约定(`lib*.a`),
  只有 MSVC 才是 `*.lib` —— 所以布局里的 `lib/` 必须按**三元组**而不是按 OS 分目录。
- **MinGW 的 mangling 与 ELF 相同**,所以 MinGW 产的库能被 MinGW 消费者链接;
  MSVC 不同 —— 这就是 `cxxabi` 必须是 ABI 模型一维的原因(`abi.cppm:84`)。

#### 2.4.5 ⚠️ 新缺陷:裸三元组谓词在原生构建下不匹配

胖包的每条腿要一个 `[target.<predicate>.build] ldflags`。原型第一版用**裸三元组**,
结果:**显式 `--target` 三个全绿,裸 `mcpp build` 链接失败**。

最小探针(`lab/probe/`,同一台机器、同一个解析出的三元组):

| `[target.<pred>.build] cxxflags = ["-DX"]` | 裸 `mcpp build` | `--target x86_64-linux-gnu` |
|---|---|---|
| `'x86_64-linux-gnu'`(裸三元组) | **0 处命中** | 2 处命中 |
| `'cfg(linux)'` | 2 处命中 | 2 处命中 |

**根因一行**(`src/build/prepare_inputs.cppm:139`):

```cpp
if (triple.empty()) return false;      // ← 裸三元组分支
```

而同一文件的 `context_for()`(:49-53)在 `targetTriple` 为空时**回落到
`triple::host_triple()`**。于是 `cfg(...)` 用宿主事实求值,裸三元组分支却拿着
原始的空串直接返回 false —— **同一个决定,两处推导**。
`types.cppm:665` 的注释明确承诺的是另一种行为:

> *prepare_build evaluates it against the RESOLVED target (**host triple for a
> native build**, the --target triple for a cross build)*

**危险形状:CI 传 `--target` 全绿,开发者本机日常构建静默失配。**
与本方案的关系:**胖包必须用 `cfg(...)`,不能用裸三元组**(见 §4.8)。
修法看起来是一行(空串时与 `host_triple()` 比),但这是独立缺陷,应单独开 issue。

#### 2.4.6 全矩阵验收

胖包改用 `cfg(...)` 后,四种构建全过,且每个 target 的 `build.ninja` 里
**只出现自己那条腿**:

```
(native, 无 --target)   OK
x86_64-linux-gnu        OK   → fatpkg/lib/x86_64-linux-gnu
x86_64-linux-musl       OK   → fatpkg/lib/x86_64-linux-musl
x86_64-windows-gnu      OK   → fatpkg/lib/x86_64-windows-gnu   (PE32+)
```


### 2.5 实验三:两种接口模式 × 两种库形态(2026-08-17,`scratchpad/lab/`)

问题:「动态库 + 接口文件」的结构应该是简单好描述的 —— 能不能同时自动支持
`.cppm`(模块)与 `.h`(头文件)两种接口?

**结论:能,而且结构确实简单。复杂度不在布局,全部集中在「模块接口」这一种模式上。**

#### 2.5.1 支持矩阵 —— 同一个包、同一份 `mcpp.toml`,三种消费方式

一个生产者(`lab/parts`)同时提供两种接口:
`include/mathkit_c.h`(`extern "C"`)+ `interface/mathkit.cppm`(模块)。

| | 只 `#include` | 只 `import` | **两者同时** |
|---|---|---|---|
| **静态库** `.a` | ✅ `hdr: 5` | ✅ `mod: 5` | ✅ `both: c=5 mod=5` |
| **动态库** `.so` | ✅ `hdr: 5` | ✅ `mod: 5` | ✅ `both: c=5 mod=5` |

**六格全过,零特殊处理。** 动态库那一列的消费者 ELF 里
`NEEDED: libmathkit.so` 确实在,`runtime_search_dirs` 进了 RPATH。
静态/动态之间,包描述符只差 `[runtime]` 要不要 `runtime_search_dirs`。

**纯头文件包**(连一个 `.cppm` 都没有的传统形态)也通过 —— `include/` + `lib/` +
`ldflags`,消费者 `#include` 即用。

#### 2.5.2 让两种模式自动共存的规则:只有两条,由目录决定

```
pkg/
├── mcpp.toml
├── include/        ← 文本接口:原样全发,消费者 #include,不产生任何对象
├── interface/      ← 模块接口:必须算闭包,消费者编译它得到 BMI + object
├── lib/<triple>/   ← 链接面
├── bin/<triple>/   ← 运行面(仅 PE)
└── LICENSE
```

| | `include/` | `interface/` |
|---|---|---|
| 是谁的输入 | **预处理器** | **编译器** |
| 消费者要编译吗 | 否 | **是** |
| 要算闭包吗 | 否(头的 `#include` 由预处理器解决,而且头本来就全发) | **是**(§2.4.2) |
| 要从归档剔除对象吗 | 否 | **是**(§2.4.3) |
| ABI 闸门强度 | `extern "C"` ⇒ **只约束 libc**(`abi.cppm:12-16`) | 全部维度 |

**这正是「为什么感觉应该简单」的答案:传统 C/C++ 库(头 + `.so`/`.a`)真的简单
—— 没有闭包、没有剔除、闸门也弱。全部复杂度属于模块接口那一种模式,
而它的复杂度是可自动化的(scanner 的模块图现成)。**

两种模式**互不干扰**,可以同时存在:实测同一个包被三种方式消费,六格全过。

#### 2.5.3 ⚠️ `kind = "shared"` 在 musl 上不是「被拒绝」,是「链接期炸掉」

`plan.cppm:1002` 那道守卫的判据是 `os != "linux"`。**musl 是 linux,所以它过闸**,
然后死在链接器里:

```
$ mcpp build --target x86_64-linux-musl        # [targets.x] kind = "shared"
crtbeginT.o: relocation R_X86_64_32 against hidden symbol `__TMC_END__'
             can not be used when making a shared object
ld: failed to set dynamic section sizes: bad value
```

`crtbeginT.o` 是**静态链接**的启动文件(`T` 后缀),因为 musl target 蕴含 `-static`
—— `-static` 与 `-shared` 互相矛盾。三个 target 的真实状态:

| target | `kind = "shared"` | 诊断质量 |
|---|---|---|
| `x86_64-linux-gnu` | ✅ | —— |
| `x86_64-linux-musl` | ❌ 链接失败 | **差** —— 消息里既没有 musl 也没有 shared |
| `x86_64-windows-gnu` | ❌ 守卫拒绝 | **好** —— 点名原因与出路 |

**守卫的判据应该是「这个 target 是否动态链接」,而不是「os 是不是 linux」。**
这条与 G2 是同一处,但比 G2 原来的描述更糟:它是一个**过了闸再炸**的洞。

#### 2.5.4 ⚠️ `sources = []` 今天不生效 —— 与「不写」逐字节等价

G1 的精确判据。在包里放一个 `src/leftover.cpp`,然后:

| `mcpp.toml` | `build.ninja` 里 `leftover` 的出现次数 |
|---|---|
| `sources = []` | **7** |
| (整行删掉) | **7** |

两者**完全一样** —— `toml.cppm:1703` 的 `if (sources.empty()) → 填默认 glob`
把显式空吞掉了。对二进制包的后果:**包里任何遗留在 `src/` 下的文件都会被编进
消费者的构建**,而且可能与预编译库里的符号重复定义。作者没有任何写法能说
「什么都不要编」。

(我一开始误以为这条已经能用 —— 因为测试包里根本没有 `src/` 目录,
默认 glob 也匹配不到东西。**「两条路径给出同一答案」不等于「这条路径生效了」。**)


### 2.3 判据

> **能编译链接运行,不等于能分发。** 三条判据,一条不满足就不叫「支持二进制分发」:
> 1. 产物里不含生产机器的任何绝对路径(e2e 215 已经为 exe 定了这条,库要同一条);
> 2. 接口与二进制**不可分别替换** —— 要么一起来,要么拒绝;
> 3. 消费者的工具链与产物的 ABI **不匹配时必须拒绝**,而不是链上去再赌。

---

## 3. 结构性缺口

按「阻塞面 × 改动成本」排,G1–G3 是硬阻塞。

### G1 —— `sources` 强制:没有「无源包」这个种类

```cpp
// src/manifest/xpkg.cppm:2058
// Validate minimum
if (m.modules.sources.empty()) {
    return std::unexpected(ManifestError{
        "synthesised manifest missing sources (mcpp segment must declare `sources = { ... }`)", ...});
}
```

对 `mcpp.toml` 一侧则是另一种问题:`sources` 缺省会被填成默认 glob
(`toml.cppm:1703`),所以「作者故意没有源」和「glob 一个都没匹配上」**不可区分**
—— 我在 §2 里那个 `dist/` 包之所以要写 `sources = ["src/runtimelib.cppm"]`,
是因为接口确实要编;一个纯 C 头文件 + `.so` 的包今天只能靠 anchor 手法。

**判据:「不存在」与「显式为空」必须可区分。** 仓库里已有这个模式
(`XlingsConfig::subosDeclared`,`types.cppm:632`)。

**实测(§2.5.4):`sources = []` 与整行删掉在 `build.ninja` 里逐字节等价**
(同一个遗留 `src/leftover.cpp`,两种写法都是 7 处命中)。所以作者今天**没有任何
写法**能表达「什么都不要编」—— 对二进制包,这意味着包里任何遗留在 `src/` 下的
文件都会被编进消费者的构建,并可能与预编译库里的符号重复定义。

### G2 —— `kind = "shared"` 只有 Linux

```cpp
// src/build/plan.cppm:1002
if (!targetTriple.empty() && targetTriple.os != "linux") {
    for (auto const& t : manifest.targets) {
        if (t.kind != Target::SharedLibrary) continue;
        return std::unexpected("shared libraries are only supported for Linux (ELF) targets today...");
    }
}
```
注释写得很清楚:PE 消费者需要导入库、Mach-O 需要 install-name,**两者都没建模**,
所以宁可拒绝也不产出没人验证过的东西。每个 shared 相关 e2e 都写着
`# requires: elf`。

**这条直接把 #433 的 `.dll` / `.dylib` 两条腿砍掉了。** 不是分发格式的问题,
是**生产侧根本产不出来**。

**⚠️ 而且守卫的判据是错的(§2.5.3)。** 它问的是 `os != "linux"`,
但 `x86_64-linux-musl` **是** linux —— 于是它过闸,然后死在链接器里:
`crtbeginT.o: relocation R_X86_64_32 against hidden symbol '__TMC_END__'`
(musl target 蕴含 `-static`,与 `-shared` 矛盾)。消息里既没有 musl 也没有 shared。
**正确判据是「这个 target 是否动态链接」,不是「os 是不是 linux」。**

好消息:相邻机械已经在了 —— `ArtifactNaming::sharedNeedsImportLib` 这个字段存在
(`plan.cppm:467`),Windows 运行期 DLL 部署也已经有(e2e 84 / #299)。

### G3 —— 载荷侧的 arch 轴与 target 轴

分成两个子问题,状态完全不同:

| 子问题 | 状态 |
|---|---|
| 描述符的**构建规则**能否按 arch 分叉 | ✅ **已经可以** —— `target_cfg = { ["cfg(arch=\"aarch64\")"] = {...} }`(`xpkg.cppm:1315`),按**解析后的 target** 求值 |
| 描述符的**载荷**能否按 arch 分叉 | ⚠️ xim V2 有(per-arch map / 模板 / sha256),但**按宿主 arch 解析** |
| mcpp → xlings 的安装调用能否带 target | ❌ **不能**,`make_targets_args` 只有 `{"targets":[...],"yes":true}` |
| 描述符的 `mcpp` 块 per-OS 分叉 | ✅ linux/macosx/windows,且已按 **TargetPlatform** 而非宿主(#254 已修) |

另外 `target_cfg` 只承载 `BuildInputs`(cflags/cxxflags/ldflags/sources/defines/
flags/include_dirs/include_dirs_after,`xpkg.cppm:1359-1377`),**不含
`runtime` / LinkIntent** —— 所以「per-arch 的 `link_library_dirs`」写不出来。
这是 #258 同一形状的债:条件通道自己维护了一份子集。今天可以用
`ldflags = {"-Llib/aarch64", "-lfoo"}` 绕过。

### G4 —— 没有兼容标签

`toolchain/abi.cppm` 建模了五维(libc / cxxStdlib / arch / os / cxxAbi),
并且有 `abi:<dim>=<value>` 的约束语言。对 **C 库**够用(它的注释原文就是
"A C library only constrains libc")。对 **C++ 模块库不够**,缺:

- 编译器**族与主版本**(mangling、libstdc++ 符号版本、模块实现细节)
- stdlib **版本**(`Toolchain::stdlibVersion` 有,但 `AbiProfile` 里没有)
- **C++ 标准档位**(`c++23` vs `c++26` 会改变 `std::` 的可见面与部分 ABI)
- **C++ 运行时契约**(`self-contained` / `toolchain-coupled` / `host-coupled`)

而这些**全都已经在 `cache_key::BuildAxes` 里了**(`cache_key.cppm:76-95`)。
不是没有,是没有对外的、可读的、可发布的投影。

### G5 —— 接口与二进制没有绑定

§2.2(c) 实测。没有 digest、没有 provenance、没有符号存在性检查。

### G6 —— 没有面向库的 pack

`mcpp pack` 的输入是一个 exe。库的 RUNPATH 重写、`$ORIGIN` 化、
第三方闭包收集、`HOST-REQUIREMENTS` 生成 —— 这些逻辑全在
`pack.cppm` / `binfmt.cppm` / `host_requirements.cppm` 里,**只差一个入口**。

### G7 —— 私有分发没有鉴权轴、没有离线入口

见 §1.4。`IndexSpec` 无鉴权字段;没有 `mcpp add ./x.mpkg`。

### G8 —— `runtime.library_dirs` 的 link/runtime 混淆(#304,已有 issue)

新键(`link_library_dirs` / `runtime_search_dirs`)已经把两件事分开了
(`docs/05-mcpp-toml.md` §2.11 的表),legacy `library_dirs` 仍然两边都进。
**二进制分发会把这个坑放大**:预编译包必然要声明库目录,而符号farm 式的包
(`compat.vulkan-runtime`)会因此污染链接线。

### G10 —— 裸三元组谓词在原生构建下不匹配(新发现,应单独开 issue)

`prepare_inputs.cppm:139` 的 `if (triple.empty()) return false;` 让
`[target.'<triple>'.build]` 在**没有 `--target`** 时永不命中,而同文件的
`context_for()`(:49-53)对 `cfg(...)` **回落到 `host_triple()`` ` ——
同一个决定两处推导。`types.cppm:665` 的注释承诺的是回落那一种。
**危险形状:CI 全绿、本机静默失配。** 详见 §2.4.5(含最小探针)。
直接阻塞胖包的裸三元组写法。

### G9 —— 描述符构建规则不能按版本区分(#290,已有 issue)

对二进制分发直接相关:同一个包的 `0.1.0` 和 `0.2.0` 可能有不同的
库文件名 / soname / 依赖集。今天 `mcpp = {}` 块对所有 `xpm` 版本一视同仁。

---

## 4. 架构方案

### 4.1 先定一件事:分发层级是**消费端**选的

这是整个方案的形状来源。生产者**发布多个层级**,消费者**按自己的工具链挑一个**,
挑不到就降级到源码。生产者不能替消费者决定,因为「你的编译器是什么」只有消费端知道。

| Tier | 包里有什么 | 生效条件 | 失配时 |
|---|---|---|---|
| **S** source | 全部源码 | 永远 | —— |
| **I** interface+binary | 接口源(`.cppm`/`.h`)+ 预编译库 | **abi-tag 匹配** | 降级到 S;S 不存在则**明确拒绝并列出可用 tag** |
| **B** +BMI | 再加预编译 BMI | **build-key 精确匹配** | **静默**降级到 I |

三条纪律:

- **Tier B 只能是加速器**,永不成为正确性依赖。失配必须静默降级,不得报错。
- **Tier I 失配必须响**。这是 §2.3 判据 3。
- **「因为客户端太老而不可用」必须报告成「不可用」,不能报告成「不存在」**
  —— 这条是 #349 索引 floor 那次的教训(被拼成「不存在」的客户端会自己驱动
  重复刷新索引)。

### 4.2 核心原语一:`[distribution]` 段 —— 一条可查询的事实

> **相对初稿的修订。** 初稿提议 `[targets.<n>] kind = "prebuilt"`。
> 它有一个致命性质:**描述符里的新键不降级**(`docs/10` 点名的那一类),
> 老客户端会被砖。改成 `[distribution]` 段之后,老客户端读到的仍是
> `sources` + `[runtime]`(**已发布能力**),而 `[distribution]` 被**静默跳过**。
> **兼容性从「要版本 floor」变成「几乎免费」。**
>
> **⚠️ 实测钉死的两条边界(mcpp 2026.8.15.3):**
> 1. `[distribution]` 里放**标量 + 字符串数组**:**接受,且连警告都没有** ✅
> 2. `artifacts = [{ … }]`(array-of-tables):**硬失败** ❌
>    ```
>    error: [[distribution.artifacts]] (array-of-tables) is not allowed for
>    section 'distribution.artifacts'; array-of-tables syntax is only supported
>    for [[build.flags]], [[features.<name>.flags]], [[runtime.requirements]],
>    and [[runtime.artifacts]]
>    ```
>    **所以产物清单不能自己造 —— 必须用已在白名单里的 `[[runtime.artifacts]]`**
>    (它的字段恰好就是 `role`/`path`/`provenance`/`abi`/`digest`/
>    `host_fingerprint`,`docs/05-mcpp-toml.md` §2.11 已经文档化)。实测:
>    `[distribution]` 标量段 + `[[runtime.artifacts]]` 一起,今天的 mcpp 直接通过。
>
> **诚实的代价:静默跳过意味着老客户端拿到预编译包时「一道闸门都没有」**,
> 拿到的是今天的行为(能用、但不安全)。这是**降级**而不是变砖,方向是对的;
> 但它也意味着闸门只保护新客户端 —— 这一点必须写进发布说明,不能假装没有。

```toml
[build]
sources = ["interface/runtimelib.cppm"]   # 只有接口;实现在预编译产物里
# 或 sources = []                          # 纯 C 头 + .a 的包:显式为空 ≠ 缺省

[distribution]
artifact_kind = "static"
abi_tag       = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
...
```

**两件事,不要合并:**

| | 解决什么 | 为什么不能只要一个 |
|---|---|---|
| `sources = []` **显式为空** | 「不要填默认 glob」 | 今天缺省会被填成 `src/**`(`toml.cppm:1703`),于是「作者故意没有源」与「glob 一个都没匹配上」不可区分 |
| `[distribution]` 段 | 「这个包的产物不来自编译」——**一条可查询的事实** | 闸门、`mcpp why`、lint、以及 §4.5.0(d) 的 build 守卫都要读它;挂在 `[build]` 上会让普通源码包也能声明 `abi_tag`,那就成了一个可以说谎的地方 |

**Appendix A(schema ownership)检验:** mcpp 定义**机制**(产物来自文件而非编译边、
以及一组闸门),**词汇留在值里**(路径、tag 字符串、`static`/`shared`)。键封闭。✅

#### 4.2.1 `[pack]` 字段的取舍

一句话:**能从 mcpp.toml 别处推出来的,就不给字段。**
按这条逐条审计之后,`[pack]` **新增 0 个键** —— WHAT/HOW 由 `[targets.<n>].kind`
回答(`mcpp pack [target]`),接口根由 `[lib]` 约定回答,头目录由
`[build].include_dirs` 回答,平台由 `[package].platforms` 回答(升格为**断言**)。
完整的推导审计、以及「什么不允许裁剪」的一致性论证在 **§4.6.1**。

### 4.3 核心原语二:两个兼容量,不是一个

**必须是两个,因为它们回答两个不同的问题。**

| | `abi-tag` | `build-key` |
|---|---|---|
| 回答 | 「这份二进制能不能链进你的构建」 | 「这份 BMI 能不能直接用」 |
| 谁能算 | **生产者**(消费者存在之前就能枚举) | **只有消费者**(含依赖闭包 Merkle) |
| 粒度 | 粗、可读、可发布 | 精确、16 hex、不可读 |
| 用途 | 决定**下载哪个载荷** | 决定 **Tier B 命中** |
| 来源 | `AbiProfile` + `Toolchain` + `CppStandardConfig` | `cache_key::key_hex`(**已存在**) |

**判据:如果试图只用一个,就会发现你无法发布 build-key** —— 它含依赖闭包,
每个消费者的图都不同,生产者得为每种图各发一份,不可能。

**`abi-tag` 的组成(6 段,全部来自已有字段):**

```
<arch>-<os>-<env>-<compiler><major>-<stdlib><major>-c++<level>

x86_64-linux-gnu-gcc16-libstdcxx16-c++23
aarch64-macos-none-llvm22-libcxx22-c++23
x86_64-windows-msvc-msvc194-msvcstl194-c++23
```

- `arch`/`os`/`env`:`triple.cppm` 已有的规范拼写。
  **⚠️ 不是 `--print-fingerprint` 的 [4]** —— 那是编译器自报的
  (`x86_64-w64-mingw32`),与 `[target.'…']` 键的拼写(`x86_64-windows-gnu`)
  不同,直接用会让同一个决定有两个拼写(§2.4.1 实测踩到过);
- `compiler<major>`:`Toolchain::compiler_name()` + `version` 的主段;
- `stdlib<major>`:`Toolchain::stdlibId` + `stdlibVersion` 的主段;
- `c++<level>`:`CppStandardConfig::level`。
- `cxxAbi` 不入 tag —— 它由 (os, compiler) 唯一确定,放进去是第二个答案。

**不入 tag、但必须单独声明并检查的两项:**

| 字段 | 为什么不入 tag | 检查语义 |
|---|---|---|
| `cxx_runtime`(契约) | 它可以**协商**:toolchain-coupled 的 `.so` 是能被 self-contained 的 exe 消费的(§2.2b 实测),只是有危险 | 不匹配 → **警告并说清危险**,`--strict` 升级为错误 |
| `abi_surface = "c" \| "cxx"` | 纯 `extern "C"` 接口只约束 libc 这一维 —— `abi.cppm` 的注释原文就是这个规则 | `"c"` → 只比 arch/os/env,忽略 compiler/stdlib/std |

`abi_surface = "c"` 这个逃生口很重要:它让**绝大多数传统 C 库**(zlib、openblas、
ffmpeg)只需要一个 tag 就覆盖所有编译器,tag 组合数从 N×M 掉回 N。

### 4.4 核心原语三:接口↔二进制的绑定

三道闸,由弱到强,**全都要**:

1. **interface digest** —— 包元数据里记随包接口文件的 sha256。消费者编译前重算,
   不符即拒。挡住「有人改了随包的头」。
2. **符号存在性交叉检查** —— 编完接口后 mcpp 知道模块名 `M`,去预编译库里查
   `_ZGIW<len><M>`(ELF/Mach-O)/ 导出表(PE)。挡住「配错了库」。便宜,值得做。
3. **原子产出 + provenance** —— **接口副本与二进制必须由同一次 `mcpp pack <target>`
   产出**,元数据里记 `built_by` / `build_key` / `source_digest`。
   手工拼装的包 mcpp 拒绝消费。

**必须诚实说清楚:三道闸都挡不住「结构体字段顺序变了但两边都自洽」**——
只要接口和二进制**能被分别替换**,§2.2(c) 就是可达的。唯一充分的保护是
**它们一起产出、一起分发、不可分别替换**。所以第 3 条才是根,前两条是防御。

### 4.5 包格式:**不是新格式 —— 是一个自带 `mcpp.toml` 的 Form A 包**

> **本节相对初稿是重写。** 初稿提议一个新格式 `.mpkg` + 一份并列的
> `MCPP-PACKAGE.toml`。**那是错的形状**,理由见 §4.5.0:mcpp 早就有
> 「包自带 `mcpp.toml`」这条通路,而且它是文档推荐的那一种。

#### 4.5.0 为什么自带 `mcpp.toml` 是对的形状

**它已经是既有机制。** 索引描述符没有 `mcpp` 字段时,mcpp 在解开的载荷里
glob `mcpp.toml` / `*/mcpp.toml` 并当作该依赖的 manifest 加载
(`prepare.cppm:2764-2787`)。`docs/10-publishing-a-library.md` 原话:
*"A repo that ships its own `mcpp.toml` needs no `mcpp` field in the index entry."*
而 LinkIntent 是**逐包聚合**的(`plan.cppm:630-656`,路径按 `package.root`
转绝对),所以 path / git / 索引 tarball 三条路吃的是同一套 —— §2 的实测走的是
path 依赖,索引 tarball 汇合到同一处。

**三个后果,第一个是决定性的:**

**(a) 不需要新描述符键 ⇒ 不需要版本 floor ⇒ 老客户端不会被砖。**

初稿的 `kind = "prebuilt"` 正好属于 `docs/10` 点名警告的那一类**不降级**的键
(和 `module_extensions` 同类):老 mcpp 读不懂它,不是「警告后忽略」,而是硬失败
或者更糟 —— 把 `interface/` 当普通源编译然后链不上。走 Form A 就完全不需要它:
老客户端读到的是 `sources = ["interface/…"]` + `[runtime] libraries /
link_library_dirs`,**全部是已发布能力** —— §2 的实测就是在 **2026.8.15.3** 上
跑通的。**兼容性几乎免费。这是 Form A 相对新描述符键的决定性优势。**

**(b) 胖包(fat package)可以绕开整个 G3。**

包里带的是 `mcpp.toml`,于是它可以写 `[target.'cfg(...)'.build]` ——
而这个条件轴**按解析后的 target 求值**,也就是在消费者那边、在 `--target`
已知之后。所以一个包同时装 x86_64 / aarch64 / linux / windows 的库,
**选择发生在构建期**:

- 不需要描述符 arch 轴;
- 不需要安装线 target 轴;
- **不需要动 xlings 一行。**

这把「交叉编译 + 二进制包」从 P2(跨仓库)搬到了 **P0 就能表达**。
代价是下载体积(可用「胖包默认 + 瘦包按 tag」两种发布方式并存来缓解)。
今天的拼写限制:条件轴只承载 `BuildInputs`(**无 LinkIntent**),所以 per-arch
只能写 `ldflags = ["-Llib/<triple>", "-lfoo"]`,写不了 `link_library_dirs`。
能用,但丑;干净的修法仍是让 LinkIntent 过条件轴(G3 收尾)。

**(c) 闸门字段必须放进 `mcpp.toml`,不能放旁路文件。**

理由不是省一个文件,是 **path 依赖也需要闸门**。如果 abi-tag / interface digest
只存在于 `mcpp pack <target>` 写的旁路文件里,那么「把一个目录拷给同事」这条最常见的
内部路径就没有任何检查 —— 而 §2 的实测证明这恰恰是人们会走的路。
所以:**`MCPP-PACKAGE.toml` 取消,内容并入 `mcpp.toml` 的 `[distribution]` 段。**

**(d) 必须加的守卫 —— 一个字面叫 `mcpp.toml` 的文件躺在解开的二进制包里,
会引诱人在里面 `mcpp build`。**

今天它会**成功**:把 `interface/` 里只有声明的接口单元编出来,产出一个几乎空的
库,而实现全在预编译产物里、根本没被链进去。典型的「看起来成功的失败」。
**`[distribution]` 存在时,在该目录直接 `mcpp build` 必须拒绝**,并说清这是分发包
不是源码树。

#### 4.5.1 布局

```
runtimelib-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23.tar.gz   (PE: .zip)
└── runtimelib-0.1.0/
    ├── mcpp.toml            ← 生成物,含 [distribution] 段;消费端零新代码路径
    ├── interface/           ← 消费者要编译的 .cppm / .ixx
    ├── include/             ← 非模块消费者的头
    ├── lib/                 ← 链接面:.a / .so / .dylib / **.lib(PE 导入库)**
    ├── bin/                 ← 运行面(仅 PE):.dll,必须部署到 .exe 旁
    ├── bmi/                 ← 可选 Tier B
    ├── HOST-REQUIREMENTS    ← 与 mcpp pack 同一份推导
    └── LICENSE / THIRD-PARTY
```

**归档格式与源码包同构**(tar.gz / zip),索引条目形状**一字不改**
(url + sha256 + 三平台块)。文件名带 tag 只是**人读的命名约定**,不是新格式 ——
`mcpp add ./<file>` 靠 `[distribution]` 段识别,不靠扩展名。

**两条规则,由文件所在的目录决定 —— 这就是全部**(§2.5.2 实测):

| | `include/` | `interface/` |
|---|---|---|
| 是谁的输入 | 预处理器 | **编译器** |
| 消费者要编译吗 | 否 | **是** |
| 要算闭包吗 | 否 | **是** |
| 要从归档剔除对象吗 | 否 | **是** |
| ABI 闸门 | `extern "C"` ⇒ 只约束 libc | 全部维度 |

两种模式**互不干扰、可同时存在**:实测同一个包被「只 `#include`」/「只 `import`」/
「两者同时」三种方式消费,× 静态库/动态库两种形态,**六格全过**。
**传统 C/C++ 库(头 + `.so`/`.a`)因此真的简单** —— 没有闭包、没有剔除;
全部复杂度属于模块接口那一种模式,而它是可自动化的。

**`lib/` 与 `bin/` 分开是机制,不是风格。**

| 类别 | ELF | Mach-O | PE |
|---|---|---|---|
| 链接面 | `lib/libfoo.so` | `lib/libfoo.dylib` | **`lib/foo.lib`(导入库)** |
| 运行面 | 同一个文件 | 同一个文件 | **`bin/foo.dll`(另一个文件)** |

PE 上「链接的东西」和「运行的东西」是两个不同的文件,ELF/Mach-O 上是同一个。
一个只分 `interface/` + `lib/` 两层的布局在 Windows 上表达不出这件事 —— 这正是
`compat.openblas` 的 windows 块必须同时写 `ldflags = {"-Llib","-llibopenblas"}`
**和** `runtime = { library_dirs = { "bin" } }` 的原因。

#### 4.5.2 `artifact_kind = "static" | "shared"` —— 一个布局,两种形态

**不要为动态库单开一个命令。** 形态是布局里的一个字段,理由是静态那条腿
**今天三平台就能走通,而且把一整类问题消掉了**(实测,见下)。

```
$ mcpp build                   # kind = "lib",无平台限制
   → bin/libstatlib.a
$ ar t libstatlib.a            → statlib.m.o   impl.o
$ nm --defined-only
   statlib.m.o:  T _ZGIW7statlib               ← 接口单元对象:只有模块初始化器
   impl.o:       T _ZN2slW7statlib6answerEv    ← 实现单元对象:真正的符号
$ readelf -d libstatlib.a      → 无 RUNPATH(归档没有动态段)
```

| | `static` | `shared` |
|---|---|---|
| 三平台产出 | ✅ **今天就有**(`kind="lib"` 无平台限制) | ❌ **仅 Linux**(G2) |
| RUNPATH 重写(§2.2a) | **不需要** —— 归档没有动态段 | 必须 |
| 双 C++ 运行时(§2.2b) | **不会发生** —— 由消费者自己的契约统一 | 会,需检查 |
| 运行期部署 | 无 | PE 需部署 `.dll` |
| 加载器搜索闭包 | 无 | 需要 |
| 代价 | 消费者产物体积;不能热替换 | —— |

**对「闭源库内部分发」这个场景,`static` 往往是更好的默认。**

**一处必须做成结构性保证、而不是巧合的事:打包时剔除已发布接口单元的对象。**
上面的实测显示归档成员切分是干净的 —— 消费者自编接口后会定义 `_ZGIW7statlib`,
于是 `statlib.m.o` 这个成员没有任何未定义符号需要它,**根本不会被拉进来**。
但这依赖「归档成员只在解析未定义符号时才被拉取」这条链接器行为;
`--whole-archive`、或该成员里恰好还有别的被引用符号,都会让它被拉进来并与
消费者自编的接口对象重复定义。打包器把它剔掉,这条风险就不存在了。

> **⚠️ 剔除集是「已发布闭包里那些单元的对象」,不是「所有 `.m.o`」。**
> 本文上一版写的是后者,**实测三个 target 全部链接失败**(§2.4.3):
> `.m.o` 的含义是「模块单元的对象」,实现分区照样是 `.m.o` 且里面是真代码。
> 剔除集与发布集是**同一个闭包的两个用途**(§2.4.2)—— 这正是它们必须由
> 同一次推导给出、而不是各算各的的理由。

#### 4.5.3 包内 `mcpp.toml`:既有的键 + 一个新段

**上半部全是今天就能跑的键**(§2 实测,mcpp 2026.8.15.3):

```toml
# ══ 生成物。手工编辑会使 [distribution].interface_digest 失配并被拒绝。 ══
[package]
namespace = "acme"; name = "runtimelib"; version = "0.1.0"

[build]
sources = ["interface/runtimelib.cppm"]      # 只有接口,消费者编译它得到 BMI

[targets.runtimelib]
kind = "lib"

[runtime]
libraries           = ["runtimelib"]
link_library_dirs   = ["lib"]
runtime_search_dirs = ["lib"]                 # shared 形态才需要
# deploy_files      = ["bin/runtimelib.dll"]  # PE

# 胖包:选择在消费者的构建期发生,按解析后的 target 求值
[target.'cfg(all(linux, arch = "aarch64"))'.build]
ldflags = ["-Llib/aarch64-linux-gnu", "-lruntimelib"]
```

**下半部是闸门 + 证据。⚠️ 只能是标量与字符串数组 —— 产物清单必须借用已在
array-of-tables 白名单里的 `[[runtime.artifacts]]`(见 §4.2 的实测边界):**

```toml
[distribution]
schema         = 1
artifact_kind  = "static"                     # static | shared
abi_tag        = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
abi_surface    = "cxx"                        # cxx | c(纯 extern "C" 只约束 libc)
cxx_runtime    = "toolchain-coupled"
modules        = ["runtimelib"]
interface_digest = "sha256:…"                 # 覆盖 interface/ 下每个文件

# ── 以下是证据,不是旋钮。用户手写 = 错误,不是「覆盖」。 ──
built_by      = "mcpp 2026.8.17.1"
build_key     = "aeb4c4d29e437696"            # cache_key::key_hex,Tier B 用
source_digest = "sha256:…"

# 产物清单 —— 复用既有的、已文档化的段(docs/05 §2.11),不新造。
# 它的字段恰好就是需要的:role / path / provenance / abi / digest / host_fingerprint。
[[runtime.artifacts]]
role       = "static-library"
path       = "lib/libruntimelib.a"
provenance = "mcpp-pack"
abi        = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
digest     = "sha256:…"
```

**实测(mcpp 2026.8.15.3):上面这整份 —— `[distribution]` 标量段 +
`[[runtime.artifacts]]` —— 今天的 mcpp 原样接受并构建成功。**
(顺带一个可观察的证据:加上 `[[runtime.artifacts]]` 后 fingerprint 变了,
说明它确实被读进 manifest 并折进了构建身份,不是被丢掉。)

**为什么证据字段也放这里、而不是旁路文件:**多一个文件就多一个能和主文件漂移的
地方 —— 这是仓库反复出现的那条教训(`publisher.cppm:194-216`:
「THE SAME DERIVATION … 分开推导就是它们漂移的方式」)。代价是必须有一条
**「用户手写了生成字段 = 错误」**的规则,而这条规则本来也需要
(`built_by` 被人改成别的值,比没有它更糟)。

**一份推导,三个投影**(比初稿少一个 —— 包内文件与消费契约合并了):

```
                     ┌→ xpkg.lua 描述符(索引;Form A ⇒ 只有 url + sha256)
一次 resolve+build ──┼→ 包内 mcpp.toml(消费契约 + [distribution] 闸门/证据)
                     └→ HOST-REQUIREMENTS(bundle)  →  消费者 mcpp.lock 条目
```

### 4.6 生产侧:`mcpp pack <target>`

```bash
mcpp pack                                    # 唯一可打包目标;有歧义 → 报错并列出候选
mcpp pack mathkit                            # [targets.mathkit].kind = "lib"  → 静态库包
mcpp pack mathkit-shared                     # kind = "shared"                 → 动态库包
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu # 胖包(--target 可重复)
mcpp pack mathkit --tier bmi                 # 可选 Tier B(默认 interface)
```

> **没有 `--lib`,也没有 `--artifact`。** 产出什么由目标的 `kind` 决定 ——
> §4.6.1(b):三个旋钮塌缩成零。

做的事(**全部复用 `mcpp pack` 已有机械**):
1. build 出库产物;
2. 拷贝接口源、**从归档中剔除接口单元的对象**(§4.5.2)、算 digest;
3. 生成包内 `mcpp.toml`(消费契约 + `[distribution]`)+ `HOST-REQUIREMENTS`;
4. 打包,**确定性归档**(PE 侧 `zip.cppm` 已经是无时间戳的了)。

`--artifact shared` 额外多两步,**这两步是 static 形态根本不需要的**:

5. **RUNPATH/INTERP 重写** —— §2.2(a) 的解药,`pack.cppm` 已有,只差入口;
6. 收第三方闭包(ELF 走 `LD_TRACE_LOADED_OBJECTS`,PE 走导入表 —— `binfmt.cppm` 已有)。

**「三平台」拆成两件事,不要排在一起:**

| | 打包器 / 布局 / 元数据 | 产出动态库本身 |
|---|---|---|
| 平台相关性 | **无关** —— 就是文件布局 + toml + digest | **强相关** |
| 现状 | 两条产出族已在(ELF/Mach-O→tar.gz、PE→zip),且 **PE 路径任何宿主都能跑**(读导入表,不执行产物) | **仅 Linux**(G2) |
| 工作量 | 小,可一次覆盖三平台 | PE 导入库 + Mach-O install_name,**真正的工作量在这里** |

没有右列,Windows/macOS 的包是**空盒子**。这就是把 static 提到 P0 的理由:
它让 P0 结束时三平台都拿到能用的包。

**验收判据(直接复用 e2e 215 的形状):** 扫遍包里每个二进制,
出现 `$MCPP_HOME` 下的路径即失败。static 形态天然满足(归档无动态段),
但**判据照样要跑** —— 它是对 `include/`、`interface/`、元数据里残留绝对路径的守卫。

#### 4.6.1 `[pack]` 的架构 —— 推导审计的结果:一个新键都不加

> **本节是第二次重写。** 第一版提了 `default_kind` / `default_artifact` /
> `targets` / `[pack.interface]` / `[pack.headers]` 五组键。
> 按「**能从 mcpp.toml 别处推出来的,就不给字段**」逐条审计之后,**全部删掉**。
> 留下的是既有键 + 一个 CLI 位置参数。

##### (a) 推导审计

对每一个候选键问同一个问题:**这件事 mcpp.toml 别处已经说过了吗?**

| 第一版的键 | 已经在哪说过了 | 结论 |
|---|---|---|
| `default_kind`(app/lib) | `[targets.<n>].kind` | **删** |
| `default_artifact`(static/shared) | 同上(`"lib"` vs `"shared"`) | **删** |
| `targets = [三元组…]` | `[package].platforms` 问的是同一件事(OS 级) | **删**,改成**校验** |
| `[pack.interface] modules` | lib-root 约定 / `[lib].path` | **删** |
| `[pack.headers] dirs` | `[build].include_dirs` | **删** |
| `[pack.headers] exclude` | 不可推导 —— 但**不允许**(见 (c)) | **删** |
| `include` / `exclude`(extras) | 不可推导 | **保留既有键,语义一字不改** |
| `default_mode` | 不可推导 | **保留既有键** |

**结果:`[pack]` 新增 0 个键。**

##### (b) WHAT / HOW 根本不是 pack 的设置 —— 是 `[targets.<n>].kind`

`mcpp pack [target]`,与既有的 `mcpp run [target]` 同形。**目标的 `kind` 决定一切:**

| `[targets.<n>].kind` | `mcpp pack <n>` 产出 | `--mode` 适用吗 |
|---|---|---|
| `bin` | 应用 bundle(既有四档) | ✅ |
| `lib` | **静态库包** | ❌ 归档不携带任何东西,闭包深度无意义 |
| `shared` | **动态库包** | ✅ `lib/` 要不要带上第三方 `.so` |

`--lib` 这个 flag 不需要了,`--artifact` 不需要了,`default_kind` / `default_artifact`
也不需要了 —— **三个旋钮塌缩成零,因为 `kind` 本来就是答案。**

**同时发布静态与动态 = 声明两个目标**(实测可行):

```toml
[targets.mathkit]         kind = "lib"
[targets.mathkit-shared]  kind = "shared"   soname = "libmathkit.so.1"
```
```
bin/libmathkit.a
bin/libmathkit-shared.so
bin/libmathkit.so.1 -> libmathkit-shared.so     ← soname 别名(既有机制)
```

**已知的 wart:`.so` 的文件名带了 `-shared`。** `soname` 给出了正确的运行期名,
打包器发布 soname 那个名字即可。长期干净的修法是让 `kind` 接受列表
(`kind = ["lib", "shared"]`,Cargo `crate-type` 的先例)——那是一次独立的改动。
**但绝不要用一个 pack 期的 `--artifact` 覆盖去补救**:那会立刻变成 `kind` 的第二个答案。

##### (c) 头与接口在 pack 期**不允许**裁剪 —— 一致性论证

这是唯一一组「不可推导、但仍然不给」的键。理由不是「用不上」,是**给了就破坏不变量**:

> **源码分发的包把 `include_dirs` 的全部内容暴露给消费者**(usage requirements,
> `scanner.cppm:700-716`)。二进制包若裁掉一部分,**同一个库会因为分发形式不同
> 而拥有不同的公开面** —— 破坏「分发形式不改变语义」。

而且「哪些头是公开的」**布局已经回答了**:`include/` 是公开的(它在消费者的
include 路径上),`src/` 是私有的。**一个私有头放在 `include/` 下是工程布局的错误,
不是打包问题。** 给 pack 一个裁剪键 = 允许用打包配置去补救布局错误,
而补救的结果是两种分发形式不一致。

接口 `.cppm` 同理,而且更硬:§2.4.2 的不对称性(发少了响、发多了哑)。

##### (d) 既有键升格为**断言**,而不是新增选择器

`[package].platforms` 的词汇是**封闭的**(`linux | macos | windows`,
`prepare.cppm:1075-1085`,未知值告警/`--strict` 报错)。不要为了 pack 去扩它。

改成:**`mcpp pack` 把 `--target` 的集合与声明的 `platforms` 做覆盖比对,
缺一条腿就告警。** 与 `[modules] exports` 同一形状 —— **既有键做断言,不做选择器**。
好处:不新增键,而且「0.1.0 声称支持 windows 却没打 windows 那条腿」变成可发现的。

##### (e) 最终形态 —— 全部是既有的键

```toml
[package]
platforms = ["linux", "windows"]     # 既有:声明支持的平台。pack 用它做覆盖校验

[build]
include_dirs = ["include"]           # 既有:公开头。整发,不可裁

[lib]
# path = "src/mathkit.cppm"          # 既有:接口根(不写 = src/<tail>.cppm 约定)
                                     #        模块闭包从这里出发

[targets.mathkit]
kind = "lib"                         # 既有:决定 `mcpp pack mathkit` 产出什么

[targets.mathkit-shared]
kind   = "shared"                    # 既有
soname = "libmathkit.so.1"           # 既有:给出正确的运行期名

[pack]
default_mode = "vendored"            # 既有键,语义不变(仅 bin / shared 适用)
include      = ["share/**"]          # 既有键,语义不变 —— 只作用于 extras
exclude      = ["**/*.tmp"]          # 既有键,语义不变 —— 只从 include 里剔除
```

```bash
mcpp pack                                    # 唯一可打包目标;有歧义 → 报错并列出候选
mcpp pack mathkit                            # kind="lib"    → 静态库包
mcpp pack mathkit-shared                     # kind="shared" → 动态库包
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu # 胖包
```

**新增 manifest 键:0。新增 CLI:一个位置参数 +`--target` 可重复。**
(`mcpp pack` 今天只有 `--mode` / `--target` / `--format` / `-o`,没有位置参数 ——
`cli.cppm` 的 `cl::App("pack")`。)

##### (f) 成文规则:什么不允许、什么不推荐

| 类别 | 规则 |
|---|---|
| **不允许** | ① 任何已被别处回答的问题(产物形态 / 接口根 / 头目录);② 任何会让**源码分发与二进制分发语义不同**的裁剪(头、接口) |
| **保留但不推荐** | `[pack].include/exclude` —— 只用于 **extras**(LICENSE / docs / 数据)。**不要拿它裁头或接口:裁不到,而且今天会静默无效**(`types.cppm:743` 的语义是 *drop from `include`*) |
| **必须写** | 无。一个库工程不写任何 `[pack]` 也能打出正确的包 |

##### (g) 唯一保留的纪律:三态

删掉了所有新键,但「不写 = 默认」这条原则本身要求一件事:

> 每个键必须区分 **不写**(默认)/ **写了有值**(覆盖)/ **写了但为空**(显式什么都不要)。

**实测 `sources = []` 与整行删掉在 `build.ninja` 里逐字节等价**(§2.5.4)——
「不写=默认」在没有三态时会退化成「无法表达空」。这条对 G1 是硬要求
(二进制包必须能说「什么都不要编」),对未来任何新键也是。
仓库已有正确的模式:`XlingsConfig::subosDeclared`(`types.cppm:632`)——
**一个 `<key>Declared` 布尔,而不是靠容器的 `empty()`**。

##### (h) 跨平台收口(不变)

- `lib/<triple>/` **按三元组分,不按 OS 分** —— MinGW 与 MSVC 同为 windows,
  一个产 `lib*.a` 一个产 `*.lib`(§2.4.4)。
- PE 的链接面与运行面是两个文件 ⇒ `lib/<triple>/` 与 `bin/<triple>/`。
- per-target `ldflags` 必须生成 `cfg(...)` 谓词,**不能生成裸三元组**(§2.4.5)。
- `kind = "shared"` × target 的合法性要在**计划阶段**校验:musl 是静态链接的,
  今天会一路走到链接器才炸(§2.5.3)。
- 打包器**绝不 glob 产物**,用刚跑那次构建的 `target/<triple>/<fp>/bin/…`(附录 A ⑬)。


### 4.7 消费侧:解析、闸门、降级

**依赖形态(三种,同一个下游路径):**

> `.mpkg` 只是**人读的命名约定**(文件名里带 tag),不是新格式 —— 里面就是一个
> 根目录带 `mcpp.toml` 的 tar.gz / zip。识别靠 `[distribution]` 段,不靠扩展名。

```toml
# ① 离线文件 —— P0,不需要索引、网络、鉴权、xlings 改动
runtimelib = { package = "vendor/runtimelib-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23.mpkg" }

# ② 索引(公开或私有)—— P1
runtimelib = "0.1.0"

# ③ CLI 直装
$ mcpp add ./runtimelib-0.1.0-<tag>.mpkg
```

**闸门表(顺序即诊断顺序):**

| 检查 | 失配 |
|---|---|
| arch / os / env | **拒绝** —— 载荷本身就不对 |
| `abi.surface == "c"` | 只查上一行,以下全跳过 |
| compiler 族 / 主版本 | **拒绝**;`--allow-abi-drift` 强制,并打印它保护的是什么 |
| stdlib id / 主版本 | **拒绝** |
| C++ 标准档位 | 消费者档位 < 生产者 → **拒绝**(接口可能用到更新的语法);> → 放行 |
| `cxx_runtime` 契约 | **警告** + 说清双运行时危险;`--strict` 升级为错误 |
| interface digest | **拒绝** |
| 模块符号存在性 | **拒绝** |
| build-key(Tier B) | **静默**降级到 Tier I |

**没有匹配 tag 时的诊断形状**(这条比机制本身还重要):

```
error: acme.runtimelib@0.1.0 has no prebuilt artifact for this toolchain
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags : x86_64-linux-gnu-gcc15-libstdcxx15-c++23
                   aarch64-linux-gnu-gcc15-libstdcxx15-c++23
  note: this package ships no source tier, so there is nothing to fall back to.
  fix : ask the publisher for a gcc16 build, or pin [toolchain] to gcc@15.
```

**「不可用」不能被拼成「不存在」。** 一个被拼成「找不到包」的失败会让客户端
自己驱动重复刷新索引 —— #349 的教训。

### 4.8 分发侧:索引与安装线

> **相对初稿是重写。** 初稿在描述符里加 `kind = "prebuilt"` + `abi_tags`,
> 并因此要求一个 mcpp 版本 floor。**走 Form A 之后这一整块消失了。**

**首选:胖包 + Form A —— 描述符一字不改。**

```lua
-- 与一个源码包的描述符完全同构:没有 mcpp = {} 块,只有 url + sha256。
-- 包内自带 mcpp.toml,里面的 [target.'cfg(...)'] 在消费者构建期选 arch/OS。
xpm = {
    linux   = { ["0.1.0"] = { url = { GLOBAL = "…", CN = "…" }, sha256 = "…" } },
    macosx  = { ["0.1.0"] = { url = { … }, sha256 = "…" } },
    windows = { ["0.1.0"] = { url = { … }, sha256 = "…" } },
}
```

- **没有新描述符键 ⇒ 没有版本 floor ⇒ 老客户端不会被砖**(§4.2 的实测边界);
- **⚠️ 每条腿的谓词必须是 `cfg(...)`,不能是裸三元组** —— 裸三元组在**原生构建下
  不匹配**(§2.4.5,根因 `prepare_inputs.cppm:139`),失败形状是
  「CI 传 `--target` 全绿、开发者本机静默失配」。实测可用的写法:
  ```toml
  [target.'cfg(all(linux, not(env = "musl")))'.build]
  ldflags = ["-Llib/x86_64-linux-gnu", "-lmathkit"]
  [target.'cfg(all(linux, env = "musl"))'.build]
  ldflags = ["-Llib/x86_64-linux-musl", "-lmathkit"]
  [target.'cfg(windows)'.build]
  ldflags = ["-Llib/x86_64-windows-gnu", "-lmathkit"]
  ```
  四种构建(native / gnu / musl / windows)全过,且每个 target 的 `build.ninja`
  里只出现自己那条腿(§2.4.6);
- **arch 选择在消费者的构建期**,`cfg()` 按解析后的 target 求值 ⇒
  **交叉编译天然正确,不需要安装线的 target 轴**;
- 镜像/CN 分流/sha256/artifact 通道**全部沿用源码包那一套**。

**可选优化(P2+):瘦包,按 tag 分资产。** 只有当胖包体积成为真问题时才做:

```lua
xpm = {
    linux = {
        ["0.1.0"] = {
            x86_64  = { url = "…-x86_64-…-gcc16-libstdcxx16-c++23.tar.gz", sha256 = "…" },
            aarch64 = { url = "…-aarch64-…", sha256 = "…" },
        },
    },
}
```
这用的是 **xim V2 已有的 per-arch resource map**,不是新语法。但它把 arch 选择
挪回**安装期**,于是重新撞上「安装线没有 target 轴」:

```
install_packages  {"targets":[...], "yes":true}
              →   {"targets":[...], "yes":true, "target":{"os":"linux","arch":"aarch64"}}
```
这是**跨仓库改动**(xlings)。两个不改 xlings 的退路,都不推荐但要写下来:

| 退路 | 代价 |
|---|---|
| 把 arch 编进包名(`aarch64-runtimelib`) | 违反 SPEC-001 的身份模型;#290 的「同一个库不同版本」问题再犯一次 |
| mcpp 自己下二进制载荷,绕过 xlings | 镜像/CN 分流/校验/断点全要重做一遍;`fetcher/progress.cppm` 只是起点 |

**决策建议:胖包做默认。** 它让「交叉编译 + 索引二进制包」在 **P1 就可用**,
而不是等 P2 的跨仓库协调 —— 瘦包只是体积优化,不是能力前提。

### 4.9 私有分发:三种形态

| 形态 | 机制 | 需要新增 | 阶段 |
|---|---|---|---|
| **离线包** | `mcpp add ./x.mpkg` / `{ package = "..." }` | 一个依赖形态(格式已有:自带 `mcpp.toml` 的 tar.gz) | **P0** |
| **私有索引(git)** | `[indices] acme = { url = "git@..." }` | 文档 + `.mpkg` URL 支持;鉴权仍走 ambient git 凭据 | P1 |
| **私有 artifact 源** | `IndexSpec::artifact` | `IndexSpec` 加鉴权(header / netrc / helper) | P2 |

**关于鉴权的立场建议:不要发明 mcpp 自己的凭据存储。** 走两条既有轨道:
① git 的 credential helper / SSH(索引侧,今天已经在用);
② 一个 `[indices.<n>] auth = { header_env = "ACME_TOKEN" }` 形状 —— **值从环境变量读,
永不落盘**。理由:mcpp 的 manifest 是要进版本库的,任何能写 token 的字段都会
被写进版本库。

**关于「团队内部统一编译器」——issue 作者说「对于 mcpp 来说统一编译器和版本不是难事」,
这句话是对的,而且这正是 mcpp 相对 CMake/vcpkg 的结构性优势:**
`[toolchain] default = "gcc@16.1.0"` 是可以进版本库的一行,payload 由 xim 保证
逐字节相同。所以团队内部的 tag 组合数常常是 **1**。这让 Tier I 在私有场景里
极其实用 —— 也让 Tier B(BMI)在私有场景里第一次变得**可能**(见 §6)。

### 4.10 非 mcpp 消费者(issue 场景 4)

`mcpp pack <target>` 顺带产出:

- **`.pc`(pkg-config)** —— 便宜,覆盖传统 `#include` + `-lfoo` 消费者;
- **CMake package config** —— 同上;
- **模块接口的互操作:诚实地说,没有好路。** CMake 的 C++20 modules 支持要求
  消费方自己扫描并编译 `.cppm`,能做但很脆。**建议:对非 mcpp 消费者,
  `mcpp pack <target>` 额外产出一份「非模块外观」(头文件 + `extern "C"` 或普通
  C++ 声明),由包作者显式声明,而不是自动生成。** 自动从模块接口生成头文件
  是一个独立的、很大的题目,不该混进这个方案。

---

## 5. 分期与验收判据

> **P0/P1 的切分线是「形态」,不是「平台」。** 初稿按平台切(P0 只做 Linux),
> 那是错的:它让 Windows/macOS 用户在 P0 结束时拿到一个**空盒子**。
> 按形态切之后,P0 三平台都产出能用的包,P1 才去解锁最难的动态形态。

### P0 —— `static` 形态,三平台一次做完(不依赖 xlings、不依赖 G2)

1. `[distribution]` 段 + `sources = []` 的显式表达(G1、§4.2),
   以及 §4.5.0(d) 的「分发包目录里不许直接 build」守卫
2. 包布局:根目录 `mcpp.toml` + `[distribution]` 段 + `[[runtime.artifacts]]`,
   `lib/` 与 `bin/` 分离(§4.5)。**归档格式与源码包同构,索引条目一字不改。**
3. `mcpp pack <target>`(位置参数;`kind = "lib"` ⇒ 静态库包)——**三平台**,
   含「从归档剔除已发布闭包的对象」(§4.5.2)
4. `mcpp add ./x.mpkg` + `{ package = "..." }` 依赖形态
5. abi-tag 计算 + 闸门表(仅 arch/os/env/compiler/stdlib/std)(G4)
6. interface digest + 模块符号存在性检查(G5)

**为什么 static 能在 P0 覆盖三平台:**`kind = "lib"` 没有平台限制(实测产出
`bin/libstatlib.a`,`ar t` 显示接口对象与实现对象分离,`readelf -d` 无 RUNPATH),
所以 G2、RUNPATH 重写、闭包收集、DLL 部署**这一期全部不需要**。

**验收:**
- [ ] e2e:**三平台**各能 `mcpp pack <target>` 出一个包 并被消费者 `import` + 链接
- [ ] e2e:`.mpkg` 里不含任何 `$MCPP_HOME` 路径(照抄 e2e 215;含元数据与 `interface/`)
- [ ] e2e:**篡改随包接口 → 构建必须失败**(直接把 §2.2(c) 那个 struct 互换
      case 变成回归测试 —— 它今天是静默通过的,这是最有价值的一条)
- [ ] e2e:abi-tag 不匹配 → 拒绝,且诊断里**列出可用 tag**
- [ ] e2e:同一个 `.mpkg` 在**清空 build cache 的第二台 MCPP_HOME** 上可消费
      (防止「只在写它的机器上能用」)
- [ ] unit:打包器确实剔除了接口单元对象(`ar t` / PE 等价物断言),
      而不是依赖「归档成员只在解析未定义符号时被拉取」这条巧合
- [ ] `mcpp why` 能说出「这个依赖是 prebuilt / 形态是什么 / tag 是什么 / 为什么选了它」

### P1 —— `shared` 形态解锁三平台 + 索引通路

7. PE 导入库(`--out-implib` / `/IMPLIB:`)+ Mach-O `-install_name @rpath/…`,
   解除 `plan.cppm:1002` 的拒绝(G2)
8. `mcpp pack <target>`(`kind = "shared"`):RUNPATH/INTERP 重写 + 闭包收集 + `bin/` 部署面
9. **胖包**走 Form A 上索引:描述符与源码包同构(url + sha256),
   `[target.'cfg(...)']` 在消费者构建期选 arch/OS —— **无新描述符键、无版本 floor**
10. 私有索引发布的文档与端到端验证

**验收:**
- [ ] 三平台各有一个 shared 库 e2e(今天全部 `# requires: elf`)
- [ ] Windows:消费者链 `lib/foo.lib`、`bin/foo.dll` 部署到 `.exe` 旁、**直接跑 `.exe`**
      (不能用 `mcpp run`,它会塞 PATH 掩盖部署问题 —— 这是既有教训)
- [ ] shared 形态的 `.mpkg` 在第二台机器上可用(§2.2a 的回归守卫)
- [ ] `cxx_runtime` 契约不匹配时**有告警**(§2.2b 今天是静默的)
- [ ] **老 mcpp(2026.8.15.3)拿到这个包能构建成功** —— Form A 的兼容性主张必须
      有一条对着**已发布二进制**跑的测试,不能只在新 mcpp 上验证
- [ ] 胖包 + `--target aarch64-linux-gnu` 选到正确的那条腿(交叉编译回归)

### P2 —— 交叉编译 + 私有鉴权

10. 瘦包(按 tag 分资产)+ `install_packages` 加 target 轴(跨仓库,与 xlings 同步)
    —— **仅当胖包体积成为真问题时才做**,它不是能力前提
11. `IndexSpec` 鉴权(值从环境变量读)
12. `target_cfg` 承载 LinkIntent(顺手修 #258 同形状的债)

### P3 —— Tier B(BMI)与生态收尾

13. Tier B:导出/导入构建缓存条目,**必须**先做跨机器可行性实验(见 §6)
14. `.pc` / CMake config 产出
15. 修 #304(`library_dirs` 的 link/runtime 分离收口)、#290(按版本区分构建规则)

---

## 6. 明确不做 / 需要先证伪的事

**(a) 不要自动从模块接口生成 C 头文件。** 独立的大题目,混进来会把这个方案拖死。

**(b) 不要为二进制包发明第二套身份模型。** `(namespace, name)` + SPEC-001 不变,
tag 是**载荷选择器**,不是身份的一部分。把 arch 编进包名是明确的错误形状(§4.8)。

**(c) Tier B 不能凭推理设计进去 —— 必须先做实验。** 已知的三个坑:
- GCC 把**时间戳**写进 BMI(实测过),所以「BMI 逐字节相同」不能作为判据;
- GCC 把**源码路径**写进 BMI,跨机器路径不同;
- clang `--precompile` 发的是 **full BMI**(体积 16 倍且会让下游 TU 编错),
  两阶段必须 `-Xclang -emit-reduced-module-interface`。

**判据:在两台不同 `$MCPP_HOME`、不同用户名、不同路径的机器上,同一个
build-key 的 BMI 能被对方直接使用并产出可运行程序。** 做不到就不要做 Tier B,
它是纯加速,不值得为它引入正确性风险。

**(d) 不要把 `cxx_runtime` 契约折进 abi-tag。** 它可以协商,tag 不可以。
折进去会让 tag 组合数翻三倍,而且会把一个「警告」变成「找不到载荷」。

**(e) 不要指望闸门能挡住 ODR/布局漂移。** §4.4 已经说了。**唯一充分的保护是
原子产出。** 任何「我们检查得够多了所以可以允许手工拼包」的说法都是错的。

---

## 7. 需要 review 的决策点

| # | 决策 | 我的建议 | 反方理由 |
|---|---|---|---|
| **D1** | P0 走**离线 `.mpkg` 文件**,还是直接做索引通路? | **离线文件**。不碰 xlings、不碰网络、不碰鉴权,完全在 mcpp 手里,而且正好是 issue 作者要的形状(`pip install x.whl`) | 索引通路才是「生态」,文件通路会不会变成永久的旁路 |
| **D2** | `kind = "prebuilt"` **新增目标种类**,还是只让 `sources = []` 合法? | **两个都要**,语义不同(§4.2) | 只加一个键更省;但 tag/digest 需要一个不说谎的挂点 |
| **D3** | abi-tag 的**粒度**:主版本 还是 完整版本? | **主版本**(`gcc16`)。完整版本会让 tag 数量爆炸,而 gcc 的 mangling/ABI 在主版本内稳定 | 主版本内也可能有 ABI 变化;可用 `--allow-abi-drift` 兜 |
| **D4** | `abi_surface = "c"` 逃生口要不要? | **要**。它让传统 C 库的 tag 数从 N×M 掉回 N,而且 `abi.cppm` 已经建模了这个规则 | 会被误用在实际有 C++ 接口的包上 → 需要 lint |
| **D5** | P0/P1 按**平台**切,还是按**形态**切? | **按形态**(修订自初稿)。P0 = `static`,三平台一次做完(`kind="lib"` 无平台限制,实测已验证);P1 = `shared`,解锁 PE 导入库 / Mach-O install_name | issue 明确写了 `.so/.dll/.dylib`,先给 static 会被读成「答非所问」—— 需要在回复里说清 static 消掉了哪三类问题 |
| **D9** | `artifact_kind` 是**布局里的字段**,还是**两个命令**? | **一个字段、一个命令、一个布局**。动态是静态的超集(多两步:RUNPATH 重写 + 闭包收集) | 两个命令更容易分别演进;但会产出两套布局与两套元数据,正是「一个决策两处推导」 |
| **D22** | `[pack]` 到底加几个键? | **0 个**(§4.6.1 推导审计)。WHAT/HOW → `[targets.<n>].kind` + `mcpp pack [target]`;接口根 → `[lib]`;头目录 → `[build].include_dirs`;平台 → `[package].platforms` 升格为断言 | 同时发布静态+动态要声明两个目标,`.so` 文件名带后缀(soname 别名给出正确运行期名)。干净修法是 `kind` 接受列表(Cargo `crate-type`),独立改动 |
| **D23** | 头 / 接口允不允许在 pack 期裁剪? | **不允许**。源码分发把 `include_dirs` 全量暴露给消费者;二进制包裁掉一部分 ⇒ **同一个库因分发形式不同而公开面不同**。而且「哪些头公开」布局已经答了(`include/` vs `src/`)—— 私有头放在 `include/` 下是布局错误,不是打包问题 | 有作者确实想 curate;但那应该改布局,不是加打包旋钮 |
| **D18**(已被 D22 取代) | `[pack]` 要不要能「自定义接口文件」? | **部分要**(完整设计见 §4.6.1):WHAT/HOW 三个标量键(`default_kind` / `default_artifact` / `default_mode`)+ `targets` 列表;CONTENT 按**四个集合各给各的键**(`[pack.interface] modules`、`[pack.headers] dirs/exclude`、既有 `include/exclude` 保留给 extras)。**不加 `.cppm` 文件清单** | 作者会希望有「万能逃生口」;真要给,必须做成 `scan_overrides` 那种**断言+校验**,不是选择器 |
| **D20** | CONTENT 用**一对全局 `include`/`exclude`**,还是**每个集合一个键**? | **每个集合一个键**。四个集合的裁剪风险完全不同(interface 不可裁 / headers 可裁但必须 `-MM` 校验 / artifacts 不可裁 / extras 自由),共用一对键会让「删 README」和「删公开头依赖的私有头」看起来是同一个操作 | 键更多;但既有 `include`/`exclude` 语义**一字不改**(它们本来就只作用于 extras),所以不是破坏性变更 |
| **D21** | 「不写 = 默认」要不要配套**三态**(缺席/有值/显式空)? | **要**。实测 `sources = []` 与整行删掉逐字节等价(§2.5.4)——「不写=默认」的原则在没有三态时会退化成「无法表达空」。用 `XlingsConfig::subosDeclared` 那个模式 | 每个键多一个 `Declared` 布尔;但这是仓库已有的、被验证过的形状 |
| **D19** | 接口闭包建在 M1 文本扫描器还是 P1689? | **P1689**。实测 M1 不把 `module X:part;` 建模为 provider(`scanner.cppm:641-650`),导致「接口够到实现分区」的告警产不出来 | P1689 要跑编译器,更慢;但只在 `pack --lib` 时跑一次 |
| **D16** | 两种接口模式(`include/` 文本 / `interface/` 编译)用**目录**区分,还是用 manifest 里的键区分? | **目录**。规则由文件所在位置决定,不需要第二处声明,也不会漂移;实测两种模式互不干扰、可同时存在 | 目录名成为约定的一部分;作者若把 `.h` 放进 `interface/` 会被当成要编译的东西 —— 需要 lint |
| **D17** | `kind="shared"` 那道守卫的判据改成「是否动态链接」,单独开 issue 还是并进 P1? | **并进 P1**。它就是 G2 的一部分,而且不修的话 musl 用户拿到的是一条读不懂的链接器错误 | 也可以先只补诊断(便宜),真正支持留到 P1 |
| **D14** | §2.4.5 的裸三元组缺陷,单独开 issue 还是并进本方案? | **单独开**。它与二进制分发无关(任何用 `[target.'<triple>'.build]` 的工程都中招),修法看起来是一行,但需要自己的回归测试 | 并进来能少一个 PR;但会把一个通用缺陷埋进一个大特性里 |
| **D15** | 接口发布集用**闭包**,还是让作者在 manifest 里手写清单? | **闭包**(mcpp 的 scanner 已有模块图,是免费的)。手写清单会漂移,而漂移的方向是**静默泄露源码** | 闭包对作者不可见;需要 `mcpp pack <target>` 打印「将发布 / 不发布」两张清单,并对「接口够到实现分区」告警 |
| **D11** | 包内自带 `mcpp.toml`(Form A),还是并列一份 `MCPP-PACKAGE.toml`? | **自带 `mcpp.toml`**(采纳,已重写 §4.5)。它消掉新描述符键 / 新消费路径 / 版本 floor,并让 path 依赖也吃到闸门 | 一个字面叫 `mcpp.toml` 的文件在二进制包里会引诱 `mcpp build` —— 需要 §4.5.0(d) 的守卫,否则是「看起来成功的失败」 |
| **D12** | 闸门+证据放同一段,还是拆两个文件? | **同一段**(`[distribution]`)。多一个文件就多一个能漂移的地方;代价是需要一条「用户手写生成字段 = 错误」的规则,而这条规则本来也要有 | 生成字段与用户字段混在一段里,靠规则而非结构区分 |
| **D13** | 胖包(一包多 arch,构建期选)做默认,还是瘦包(按 tag 分资产)? | **胖包**。它让交叉编译在 P1 就正确,**完全不需要动 xlings**;瘦包是 P2 的体积优化,不是能力前提 | 大型闭源 runtime × 4 tag 的体积;可两种并存 |
| **D10** | `static` 做默认形态? | **是**。对闭源内部分发,它消掉 RUNPATH 重写、DLL 部署、双 C++ 运行时、加载器闭包四类问题 | 大型 runtime 库可能就是要动态(热替换、体积);默认可被 `[pack] default_artifact` 覆盖 |
| **D6** | 安装线 target 轴(跨仓库)排 P2,可接受吗? | **可接受**,因为 P0/P1 用离线文件与显式 tag 绕开了它 | 交叉编译 + 索引二进制包在 P2 之前不可用 |
| **D7** | 鉴权:环境变量 header,还是接入 git credential helper? | **两条都走**:索引 clone 用 git 凭据(今天已经如此),artifact/URL 用 `header_env` | 有人会希望 mcpp 自己存 token —— 建议明确拒绝 |
| **D8** | §2.2(c) 那个静默错数据,要不要**先单独开 issue 并立刻加回归测试**? | **要**,不等整个方案。它今天就可达(path 依赖 + `[runtime] libraries` 是已发布能力) | 它需要 digest 机制才能真正修;但先把测试写成「预期失败」也有价值 |

---

## 附录 A. 复现脚本

材料在 `scratchpad/bindist/`,三个工程:

```bash
# ① 生产者:模块接口只有声明,实现在实现单元
provider/src/runtimelib.cppm   export module runtimelib; export namespace rt { ... }
provider/src/impl.cpp          module runtimelib;  namespace rt { ... }
provider/mcpp.toml             [targets.runtimelib] kind = "shared"
mcpp build                  →  bin/libruntimelib.so

# ② 「二进制包」:接口源 + 预编译库 + mcpp.toml
dist/src/runtimelib.cppm       (从 ① 拷贝)
dist/lib/libruntimelib.so      (从 ① 拷贝)
dist/mcpp.toml                 [runtime] libraries / link_library_dirs / runtime_search_dirs

# ③ 消费者
app/mcpp.toml                  runtimelib = { path = "../dist" }
app/src/main.cpp               #include <...>   然后   import runtimelib;
mcpp run                    →  answer=42 / name=... / caught runtime_error

# ④ skew 实验(本方案要挡的那个)
#    只把 dist/src/runtimelib.cppm 里的 struct Point { int x; int y; }
#    改成                              struct Point { int y; int x; }
mcpp run                    →  x=222 y=111   ← 编译链接运行全过,数据是错的,零诊断

# ⑤ 静态形态(scratchpad/statictest/):同样的两个源,[targets.x] kind = "lib"
mcpp build                  →  bin/libstatlib.a
ar t libstatlib.a           →  statlib.m.o   impl.o
nm --defined-only              statlib.m.o:  T _ZGIW7statlib             ← 只有模块初始化器
                               impl.o:       T _ZN2slW7statlib6answerEv  ← 真正的符号
readelf -d libstatlib.a     →  (无动态段 ⇒ 无 RUNPATH)

# ⑥ 兼容性边界(用已发布的 mcpp 2026.8.15.3 跑,不是用新构建)
#    在 statictest/mcpp.toml 末尾追加:
[distribution] schema/artifact_kind/abi_tag/abi_surface/interface_digest/modules
                            →  mcpp build 成功,连警告都没有            ✅
artifacts = [{ path = "…", role = "…" }]   （array-of-tables 于新段）
                            →  硬失败:"array-of-tables syntax is only supported
                               for [[build.flags]], [[features.<name>.flags]],
                               [[runtime.requirements]], and [[runtime.artifacts]]"  ❌
[distribution] 标量段 + [[runtime.artifacts]]（既有白名单段）
                            →  成功,且 fingerprint 变化(证明它被读进 manifest) ✅
```


### 实验二(§2.4):`scratchpad/lab/`

```
lab/mkdist.py            176 行的 `mcpp pack <target>` 原型(采 tag / 算闭包 / 剔对象 / 生成 toml)
lab/parts/               生产者:主接口 + 接口分区 + 实现分区 + 实现单元 + C API + 头
lab/fatpkg/              产出的胖包:interface/ include/ lib/<triple>/ mcpp.toml
lab/consumer/            消费者(path 依赖 fatpkg)
lab/probe/               §2.4.5 的最小探针:裸三元组 vs cfg(linux)

# ⑦ 三条被实验推翻/确立的规则
python3 mkdist.py parts fatpkg x86_64-linux-gnu x86_64-linux-musl x86_64-windows-gnu
    接口闭包 (2 个): mathkit.cppm, api.cppm
    未发布   : secret.cppm                      ← 实现分区不外发(但它也有 .m.o)
    剔除归档成员: ['api.m.o', 'mathkit.m.o']     ← 不是所有 .m.o(第一版剔全部 → 三平台链接全挂)

# ⑧ 全矩阵(胖包用 cfg(...) 谓词)
cd consumer && mcpp build [--target …]
    (native)  OK      x86_64-linux-gnu OK      x86_64-linux-musl OK      x86_64-windows-gnu OK(PE32+)
    build.ninja 中各自只出现自己那条腿的 fatpkg/lib/<triple>

# ⑨ 最小探针:[target.<pred>.build] cxxflags = ["-DX"] 的命中次数
                                裸 mcpp build   --target x86_64-linux-gnu
    'x86_64-linux-gnu'(裸三元组)       0                 2        ← 缺陷
    'cfg(linux)'                        2                 2
```

### 实验三(§2.5):两种接口模式 × 两种库形态

```
lab/parts/      同时提供 include/mathkit_c.h(extern "C") 与 interface/*.cppm(模块)
lab/fatpkg/     静态库包    lab/sopkg/  动态库包    lab/hdrpkg/  纯头文件包(无 .cppm)
lab/c_hdr/      只 #include        lab/c_mod/  只 import        lab/c_both/  两者同时

# ⑩ 六格矩阵 —— 同一个包,三种消费方式 × 两种库形态
               只 #include    只 import    两者同时
  静态库 .a      hdr: 5        mod: 5      both: c=5 mod=5
  动态库 .so     hdr: 5        mod: 5      both: c=5 mod=5
  消费者 ELF:  NEEDED libmathkit.so + runtime_search_dirs 进 RPATH ✅

# ⑪ musl + kind="shared":过了守卫,死在链接器
mcpp build --target x86_64-linux-musl
    crtbeginT.o: relocation R_X86_64_32 against hidden symbol `__TMC_END__'
                 can not be used when making a shared object
    (守卫判据是 os != "linux";musl 是 linux ⇒ 过闸。真正的判据应是「是否动态链接」)

# ⑫ sources = [] 不生效 —— 与「不写」逐字节等价
    包里放一个遗留的 src/leftover.cpp:
        sources = []   → build.ninja 里 leftover 出现 7 次
        (整行删掉)      → build.ninja 里 leftover 出现 7 次      ← 完全一样
    ⚠️ 我一开始误以为它能用,因为测试包没有 src/ 目录 —— 默认 glob 也匹配不到东西。
       「两条路径给出同一答案」不等于「这条路径生效了」。

# ⑬ 我自己踩的坑(仓库已知形状):mkdist 用 rglob 取产物 → 挑到陈旧 fingerprint 目录
    target/x86_64-linux-gnu/0f8ab572…/  缺 capi.o   ← rglob 取到的
    target/x86_64-linux-gnu/d02fd93d…/  有 capi.o   ← 刚构建出来的
    ⇒ 打包器绝不能 glob 产物,必须用它刚跑那次构建的 target/<triple>/<fp>/bin/… 路径。
```

环境:mcpp 2026.8.15.3,gcc@16.1.0,x86_64-linux-gnu;
交叉 target 的载荷:`xim-x-musl-gcc/16.1.0`、`xim-x-mingw-cross-gcc/16.1.0`。

## 附录 B. 证据索引

| 事实 | 位置 |
|---|---|
| 描述符强制 `sources` | `src/manifest/xpkg.cppm:2058-2063` |
| 描述符 `mcpp` 段封闭键表(无 arch) | `src/manifest/xpkg.cppm:228-234` |
| 描述符 `target_cfg`(**已有** arch 条件构建输入) | `src/manifest/xpkg.cppm:1315-1400` |
| `target_cfg` 只承载 BuildInputs(无 LinkIntent) | `src/manifest/xpkg.cppm:1359-1377` |
| 描述符 `runtime` 子键(LinkIntent) | `src/manifest/xpkg.cppm:1925-1966` |
| `kind = "shared"` 仅 Linux | `src/build/plan.cppm:1002-1017` |
| `kind = "lib"` **无**平台限制,产出 `bin/lib<n><staticLibExt>` | `src/build/plan.cppm:411-423`(`target_output`) |
| **Form A:载荷自带 `mcpp.toml` 时 glob 加载它** | `src/build/prepare.cppm:2764-2787`(及 `2470` 的存在性探测) |
| LinkIntent **逐包**聚合(path/git/索引同一条路) | `src/build/plan.cppm:630-656` |
| legacy `library_dirs` 已**不**进 `linkLibraryDirs`(#304 的一半) | `src/build/plan.cppm:653-656` |
| array-of-tables 白名单(闸门段不能自造产物清单) | 实测,附录 A ⑥ |
| **裸三元组谓词在原生构建下不匹配**(新缺陷) | `src/build/prepare_inputs.cppm:139` vs `:49-53`;承诺见 `src/manifest/types.cppm:665` |
| cfg 词汇 = 规范 triple 词汇(os/arch/family/env + 别名) | `src/build/prepare_inputs.cppm:39-148` |
| MSVC 与 Itanium 的 cxxabi 分叉 | `src/toolchain/abi.cppm:84` |
| 接口闭包 / `.m.o` 不是判据 / 剔除集 | 实测,附录 A ⑦ |
| `[modules] exports` 是**完备性断言**(写错硬失败),不是选择器 | `src/modgraph/validate.cppm:93-117` |
| 既有 `[pack]` 键(新键的先例) | `src/manifest/types.cppm:736-745`;解析 `src/manifest/toml.cppm:1337-1347` |
| **`[pack].exclude` 只从 `include` 里剔除**(裁不到 headers) | `src/manifest/types.cppm:743` 注释原文 |
| `[pack.<sub>]` 子表先例 | `docs/02-pack-and-release.md` §Configuration(`[pack.bundle-project]`) |
| 三态模式(缺席 vs 显式空) | `src/manifest/types.cppm:632`(`subosDeclared`) |
| 「断言 + 校验」的先例(`scan_overrides`) | `src/manifest/types.cppm:62-70` |
| **M1 扫描器不把实现分区建模为 provider** | `src/modgraph/scanner.cppm:641-650`;告警 `:984` |
| P1689 扫描后端(闭包应建在它上面) | `src/modgraph/scanner.cppm:121-124` |
| 两种接口模式 × 两种库形态 六格全过 | 实测,附录 A ⑩ |
| `kind="shared"` 在 musl 上过闸后链接失败 | 守卫 `src/build/plan.cppm:1002`;实测 附录 A ⑪ |
| `sources = []` 被默认 glob 吞掉 | `src/manifest/toml.cppm:1703`;实测 附录 A ⑫ |
| 静态归档:接口对象与实现对象分离、无 RUNPATH | 实测,附录 A ⑤ |
| PE 需导入库(字段已存在) | `src/build/plan.cppm:452-479` |
| 平台轴只有 OS,无 arch | `src/platform/axis.cppm:63-87` |
| 安装线协议无 target/arch | `src/pm/package_fetcher.cppm:419-431` |
| 五维 ABI 模型 + `abi:<dim>=<v>` | `src/toolchain/abi.cppm:31-145` |
| C 库只约束 libc(逃生口的依据) | `src/toolchain/abi.cppm:12-16` |
| ABI 完备的逐包 key(tag 的原料) | `src/build/cache_key.cppm:20-46, 76-133` |
| 构建缓存布局(BMI+obj 已在盘上) | `src/bmi_cache.cppm:1-45` |
| C++ 运行时契约三层模型 | `src/build/distribution.cppm:1-120` |
| SharedLibrary 双运行时危险 | `src/build/distribution.cppm:51-64`(role 注释)、`154-180`(`default_contract`) |
| 三平台同一份源码 tarball | `src/pm/publisher.cppm:289-303` |
| 「一份推导,多个投影」的既有纪律 | `src/pm/publisher.cppm:194-216` |
| 私有索引无鉴权字段 | `src/pm/index_spec.cppm:14-57` |
| lock 不 pin 索引依赖、不记 ABI | `mcpp.lock` 头部注释 / `src/pm/lock_io.cppm:29-36` |
| pack 的输入是一个 exe | `src/pack/pack.cppm:Plan::builtBinary` |
| anchor TU 手法样板 | `~/.mcpp/registry/data/mcpplibs/pkgs/c/compat.openblas.lua` |
| xim V2 的 arch 轴(按宿主解析) | `~/.mcpp/registry/data/xim-pkgindex/docs/V2/xpackage-spec.md` §"三种新版本条目形状" |
| LinkIntent 的 link/runtime 分离表 | `docs/05-mcpp-toml.md` §2.11 |
| 新键需要版本 floor 且必须降级 | `docs/10-publishing-a-library.md` §"Manifest keys that need a version floor" |
| schema 准入原则 | `docs/05-mcpp-toml.md` Appendix A |
