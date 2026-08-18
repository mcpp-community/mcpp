# 四项遗留的统一分析:判据挂错轴,以及 cl.exe 到底怎么办

> 2026-08-18 · 针对 2026.8.18.2 发布后列出的四项遗留
> 状态:**分析与方案,代码已就绪但未合入(分支 `fix/cl-exe-consumption`)**

先说结论,因为它比四项本身重要:

> **四项里有三项是同一个形状** —— 一个决定被挂在了错误的轴上。
> 而第四项(`.md` 硬失败)是刻意的破坏性变更,它的正确性恰恰来自
> 「不要再让分类不出的东西静默通过」。

---

## 一、`cl.exe` 消费:它已经能用了,是文档没跟上

### 1.1 为什么曾经不行

生成的 manifest 用 GNU 拼写描述每条腿:

```toml
[target.'cfg(…, env = "msvc")'.build]
ldflags = ["-Llib/x86_64-windows-msvc", "-lmathkit"]
```

mcpp 用到的每个**编译器驱动**都吃这一套 —— 包括 Windows 默认的、面向 MSVC ABI
的 clang。**原生 `cl.exe` 不吃**:它在第一个 `-L` 上就停下。

### 1.2 工业界怎么做

调研的结论出奇一致:**没有任何一个包管理器分发「一条命令行」。**
它们分发的是**抽象的链接意图**,由消费方的构建系统渲染成命令行。

| 方案 | 包里存的是什么 | 谁负责渲染 |
|---|---|---|
| **pkg-config** | `.pc` 文件:`Libs: -L${libdir} -lfoo` | `pkg-config --libs` 展开变量;Unix 生态,MSVC 上要靠 pkgconf + 手工桥接 |
| **CMake config-file package** | `fooConfig.cmake` + imported target,`INTERFACE_LINK_LIBRARIES` 里是**绝对路径或 target 名** | CMake 按当前 generator/toolchain 生成 `link.exe` 或 `ld` 的命令行 |
| **vcpkg / Conan** | 上游自己的头与库 + **generator** 产物 | 按消费方生成 `.props`(MSBuild)、`.cmake`、`.pc` —— 一份意图,多种渲染 |
| **MSBuild `.props`** | `<AdditionalLibraryDirectories>` / `<AdditionalDependencies>` | MSBuild 渲染成 `/LIBPATH:` + `x.lib` |

**共同点:库名与库目录是两个字段,不是两个 flag。** 谁把它变成 flag,取决于
最终调用的是哪个程序。

### 1.3 mcpp 的答案(上一轮已实现)

同一条腿再写一遍,这一遍不带方言:

```toml
[target.'cfg(…, env = "msvc")'.runtime]
link_library_dirs = ["lib/x86_64-windows-msvc"]
libraries         = ["mathkit"]
```

`render_link_intent_flags` 按 flavor 渲染成 `/LIBPATH:` + `<n>.lib` 或
`-L` + `-l<n>`。**这不是新词表** —— `[runtime]` 顶层一直有这两个键,这里只是让
它们可以按 target 给,与 CMake 的 imported target、vcpkg 的 generator 是同一个思路。

**两种拼写都写出来**:旧版 mcpp 只读 `ldflags` 并静默忽略新段,去掉它会让所有
旧客户端一个链接 flag 都拿不到;新版读到中立形式时**丢掉同腿的库引用**而不是叠加。

### 1.4 所以「❌」是文档缺陷,不是产品缺陷

docs/12 的边界表仍写着 `consuming a package with native cl.exe ❌ see below`,
而同一份文档的正文已经在描述实现好的方案。**正文更新了,表格没有** ——
这正是 `mcpp-docs-style` 里「断言强度必须与证据相符」要防的那一类。

### 1.5 ⚠️ 一个反直觉的发现:这一处**方言才是对的轴**

上一轮我修了三个「挂错轴」的 flag(`-fPIC`、`--out-implib`、`/DEF:`),
它们都应该按**目标 ABI** 判定。于是很容易顺手认为 `LinkIntentFlavor` 的选择
(`if (isMsvcDialect) return PeMsvc;`)是第四处同样的错误。

**它不是。** 区别在于这些 flag 交给谁:

| flag | 交给谁 | 因此判据是 |
|---|---|---|
| `-fPIC` | 编译器,但语义属于目标格式 | **目标格式**(PE 不需要) |
| `--out-implib` / `/IMPLIB:` | 链接器(经 `-Wl,`) | **目标 ABI**(lld-link vs ld) |
| `/DEF:` | 链接器 | **目标 ABI** |
| `-L` / `/LIBPATH:` | **mcpp 直接调用的那个程序** | **方言**(= 是否 `SeparateLinker`) |

clang 面向 MSVC ABI 时,产物是 MSVC ABI 的,但**它自己是一个编译器驱动**,
只吃 `-L`。所以按 ABI 判会把它错误地喂成 `/LIBPATH:`。

**判据:先问「这个 flag 最终被谁解析」,再决定挂哪根轴。**
已把这条写成单测(`test_link_intent_spelling.cpp`),并在其中点名 clang-on-MSVC
是分开这两个问题的那个反例。

> 顺带:写这条单测时我自己先断言错了 —— 找 `/LIBPATH:` 而实际输出是
> `/LIBPATH$:`(**ninja 转义**,不是 shell 命令行)。渲染是对的,断言是错的。

---

## 二、第三处同族缺陷(本次分析中发现,已控制对照证实)

上一轮修好了 `mcpp pack` 的 lib-root 解析(按声明的扩展名探测),但
**非探测版本还有两个调用点,而它们都应该探测**:

| 位置 | 后果 |
|---|---|
| `prepare.cppm:4488` — host-module 依赖 | 依赖的接口若是 `.ixx`,解析到不存在的 `src/<tail>.cppm`,消费方的 `build.mcpp` 拿到一个指向空的路径 |
| `validate.cppm:134` — lib root 存在性检查 | `.ixx` 工程每次构建都收到**虚假警告** |

**控制对照(用刚发布的 2026.8.18.2 二进制,对同一个 `.ixx` 工程):**

```
warning: src/mathkit.cppm: lib target without conventional lib root
         'src/mathkit.cppm' (create the file or set [lib].path)
```

修复后该警告消失,构建不变。

**这说明「修了主路径」不等于「修了这个决定」** —— 同一个问题有 N 个调用点时,
只改自己正在测的那一个,剩下的会在别人的工程里显形。

---

## 三、四项遗留,逐项分析

### 3.1 `cl.exe` 端到端未验证 → **可关闭**(方案已就绪)

- 渲染侧:`test_link_intent_spelling.cpp`,4 条,**三平台都跑**;
- 端到端:`e2e 262`,`# requires: msvc`,消费方**钉死 `msvc@system`** ——
  这一点是关键:如果中立形式被忽略而 ldflags 生效,clang 消费者**照样能过**,
  只有 cl 会因为一个 `-L` 而失败,所以只有它能证明这件事。
- 262 还断言**生成的图里没有该腿的 `-L`**:「跑通了」也可能是 cl 恰好容忍。

### 3.2 数据符号需要 `dllimport` → **应做成可复现,而不是继续写在文档里**

现状:docs/12 记录了这条限制(与 CMake 为同一机制记录的一致),但没有测试。
问题在于**它是一条关于「什么不工作」的断言**,而这类断言最容易随实现漂移 ——
哪天自动 `.def` 学会了给数据符号加 `DATA`(它已经加了),没人会想起来复核
这条限制是否仍然成立、以及**成立到什么程度**。

方案:在 `e2e 258` 里加一对 fixture:

| 消费方声明 | 期望 |
|---|---|
| 不写 `__declspec(dllimport)` 读导出变量 | **失败或读到错值** —— 把限制钉住 |
| 写了 `dllimport` | 通过 |

⚠️ 这条要小心写:「失败」的具体形态(链接错 vs 读到桩地址)取决于工具链版本,
断言必须钉**可观测的差异**(两者行为不同),而不是钉某一条错误文本。

### 3.3 `.md` 在 `sources` 里现在硬失败 → **保留,但这是需要明说的破坏性变更**

三个选项:

| 选项 | 代价 |
|---|---|
| **硬失败(现状)** | 把 `.md` 放进 `sources` 的工程会报错。消息点名文件、扩展名与该写的键 |
| 警告并忽略 | **回到原点** —— 「编译出一个没人链接的对象」正是被警告忽略掉的那种失败 |
| 只对已知非编译扩展名(`.md`/`.txt`)放行 | 需要维护一张「哪些扩展名可以被静默忽略」的表,而这张表永远不完整 |

**保留硬失败。** 理由是这条缺陷的形状:它不是「多编了一个文件」,而是
**「编了但没链」**,报错落在一个模块修饰过的 `undefined reference` 上。
警告在这里没有力量 —— 构建仍然会失败,只是失败得更晚更远。

补充动作:CHANGELOG 已标注 ⚠️;**建议再在 docs/05 的 `sources` 一节写明**
「`sources` 的每一项都必须能产出被链接的对象」,把它变成一条可引用的规则。

### 3.4 探测式解析器放在 `mcpp.manifest.toml` → **接受,并且它本来就更对**

事实:给 `mcpp.manifest.types`(几乎所有东西都依赖的低层模块)加一条
`import mcpp.source_kind` 之后,GCC 16.1 在编译**与改动无关的 `src/main.cpp`**
时 ICE,清 gcm.cache 无效。

但把这件事只记成「被编译器逼的」是不完整的:

- `mcpp.manifest.types` 的职责是**数据模型**,注释里写着「No parsing lives here」;
- **探测文件系统不是数据模型的职责**。

所以这个位置在架构上本来就更对,编译器只是先一步告诉了我们。
**留下的真实代价是这一族被拆成两个模块**,而 §2 里那两个漏网的调用点正是
这种拆分容易漏人的证据 —— 已修,并在两处都写明了为什么用探测形式。

---

## 四、优化方案(按依赖排序)

| # | 内容 | 状态 |
|---|---|---|
| **P-A** | `render_link_intent_flags` 的方言判据单测(含 clang-on-MSVC 反例) | **已写,三平台通过** |
| **P-B** | `e2e 262`:原生 `cl.exe` 消费打包库,并断言图里没有该腿的 `-L` | **已写,待 Windows CI** |
| **P-C** | docs/12 边界表改 ✅(中英) | **已改** |
| **P-D** | `prepare.cppm` / `validate.cppm` 两处改用探测式解析 | **已改,控制对照证实** |
| **P-E** | `e2e 258` 增加 data-symbol 的 `dllimport` 对照 | 待做 |
| **P-F** | docs/05 写明「`sources` 的每一项都要能产出被链接的对象」 | 待做 |
| **P-G** | 把「flag 挂哪根轴」写成一张表,放进 docs/08 §7.4 | 待做 |

**P-G 是这份分析里最有复用价值的一条**:本轮四个 flag 分别属于三根不同的轴,
而每一次挂错都表现为「在某一个平台上莫名其妙地失败」。把轴写下来,
下一个加 flag 的人就不必重新踩一遍。
