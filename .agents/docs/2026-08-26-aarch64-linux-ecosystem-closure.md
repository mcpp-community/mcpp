# aarch64 Linux:让整个生态可用,并且可测

> 2026-08-26。起因:把 `linux-aarch64` 加进 `ci-target-matrix.yml` 的构建机轴,
> 第一次运行就连挖出三层缺陷,而它们都不在 mcpp 里。

---

## 0. 一句话

**mcpp 为 aarch64 Linux 发布二进制,而它到那里之后能用的东西只有 `musl-gcc`
和 `ninja`。** 其余每一个宿主代码包 —— llvm、glibc、linux-headers、zlib、
libxml2、gcc-runtime、mingw-cross-gcc —— 都只发 x86_64。

⚠️ **这不是「aarch64 没做」,是「aarch64 做了一半而没有任何东西在看」**:
`ci-aarch64-fresh-install.yml` 一直是绿的,因为它验的是
`xlings install mcpp` 装得上、能编一个 hello world,而不是那台机器上的目标矩阵。

---

## 1. 判据:什么叫「aarch64 全生态可用」

不用形容词。四条,每条都能被一条命令回答:

| # | 判据 | 怎么测 |
|---|---|---|
| **J1** | `mcpp toolchain list --format json` 在 aarch64 上列出的每一行,`status != planned` 的都真的装得上 | `tests/matrix/scan.sh` 的 `unsupported/other` 与 `mismatch` 格数为 0 |
| **J2** | aarch64 的 `expected.tsv` 行存在,且 `coverage` job 通过 | 发布集 == 矩阵集 |
| **J3** | 七个 openkal 仓库在 aarch64 runner 上 CI 绿 | 各仓 `ci.yml` 加 aarch64 leg |
| **J4** | `mcpp-index` 的每个包在 aarch64 上能被 `mcpp add` + 构建 | 索引 smoke 加 aarch64 leg |

⭐ **J1 不要求「全部 ok」。** 一个诚实的 `unsupported` 带具名 `reason` 就是通过。
不可接受的是 `mismatch`(解析说行、构建炸了)与 `other`(拒绝了而没有理由)。

---

## 2. 现状:四个仓库各缺什么(实测 2026-08-26)

### 2.1 `xim-pkgindex` —— 主要缺口在这里

| 包 | `archs` 声明 | 实际有 linux-aarch64 资产 | 性质 |
|---|---|---|---|
| `llvm` | `{x86_64, arm64}` | **否** | ⚠️ 声明了却没有;`arm64` 只对 macosx 成立 |
| `gcc` | `{x86_64}` | 否 | 正确 —— 其它架构由 `musl-gcc` 覆盖 |
| `musl-gcc` | `{x86_64, aarch64}` | **是** ✅ | |
| `ninja` | `{x86_64, aarch64}` | **是** ✅ | |
| `glibc` | `{x86_64}` | 否 | 目标代码 |
| `linux-headers` | `{x86_64}` | 否 | 目标代码 |
| `zlib` | `{x86_64}` | 否 | llvm 的宿主侧依赖 |
| `libxml2` | `{x86_64}` | 否 | llvm 的宿主侧依赖 |
| `gcc-runtime` | `{x86_64}` | 否 | llvm 的宿主侧依赖 |
| `mingw-cross-gcc` | `{x86_64}` | 否 | 宿主代码 |
| `picolibc-riscv` | `{x86_64, aarch64}` | 不需要 ✅ | ⭐ 目标代码,一份归档服务所有宿主 |

⚠️ **`llvm.lua` 第 178 行**:`local llvmdir = "llvm-" .. version .. "-linux-x86_64"`
—— 架构写死在解包路径里,与 `archs` 的声明矛盾。

### 2.2 `xlings` —— 发了 aarch64,但形状不同

`xlings-2026.8.17.2-linux-aarch64.tar.gz` 存在。⚠️ 但两个包的**内部布局不一样**:

```
linux-x86_64   subos/default/bin/xlings   513 条目
linux-aarch64  bin/xlings                 494 条目
```

(x86_64 的 `subos/default/bin/xlings` 是指向 `bin/xlings` 的符号链接,同一文件。)

### 2.3 `mcpp-index` —— 结构上无缺口

包是**源码分发**(`add_urls` + `add_versions` + sha256),没有 `archs` 字段,
也不需要。缺的是**验证**:没有任何一条 CI 在 aarch64 上构建过它们。

### 2.4 `mcpp` 自己 —— 发布了,但表在说谎

`release.yml` 产出 `mcpp-<v>-linux-aarch64.tar.gz`。而
`available_toolchain_indexes()` 是一张按 OS 分支、**不问架构**的静态表:

```cpp
{ "gcc", ... }, { "musl-gcc", ... }, { llvm::package_name(), ... }
… else if constexpr (is_linux) out.push_back({ "mingw-cross-gcc", ... });
```

⇒ aarch64 上 `toolchain list` 会说 llvm 可装、四个裸机行 `available`,
而 `mcpp toolchain install llvm 22.1.8` 会 404。**声明 ≠ 安装**,又一次。

---

## 3. 分类:宿主代码与目标代码,决定了每一项的工作量

⭐⭐ **这是整份计划里唯一真正的结构判断。** 一个包属于哪一类,决定它需不需要
per-host-arch 的构建:

| 类 | 含义 | aarch64 需要什么 | 本仓涉及的包 |
|---|---|---|---|
| **宿主代码** | 在构建机上运行 | **一份 aarch64 构建** | llvm, musl-gcc, ninja, zlib, libxml2, gcc-runtime, mingw-cross-gcc |
| **目标代码** | 在目标机上运行 | 什么都不用 —— 一份归档服务所有宿主 | picolibc-*, openkal-*(源码) |
| **目标侧 sysroot** | 目标机的 C 库 | 一份 **per-target** 构建,与宿主无关 | glibc, linux-headers |

⚠️ **`glibc` 与 `linux-headers` 在这张表里最容易被归错。** 它们是
`x86_64-linux-gnu` 这个**目标**的 C 库,不是宿主的。aarch64 宿主要它们,是因为
`aarch64-linux-gnu` 这个**目标**需要 —— 那是另一件事,见 §5.4。

---

## 4. 关键决策:aarch64 的 LLVM 怎么建

### 4.1 上游没有

| 来源 | linux-aarch64 |
|---|---|
| `llvm/llvm-project` 19.1.7 | ✅ `clang+llvm-19.1.7-aarch64-linux-gnu.tar.xz` |
| `llvm/llvm-project` 20.1.7 / 21.1.0 | **无** |
| `xlings-res/llvm` 20.1.7 / 22.1.8 | **无** |

⚠️ 上游从 20.x 起停发 linux-aarch64,而 mcpp 钉 20.1.7 / 22.1.8,且需要
C++23 modules + `import std` —— 19.x 顶不上。**只能自己建。**

### 4.2 两个方案

**方案 A:照搬 x86_64 的形状(glibc 动态链接 + 五个依赖包)**

x86_64 的载荷实测:896 MB(bin 756 MB / lib 123 MB / include 16 MB),
`deps = {glibc>=2.39, linux-headers, zlib, libxml2, gcc-runtime}`。

照搬意味着 aarch64 上要先补齐 **zlib / libxml2 / gcc-runtime** 三个宿主包,
外加 aarch64 的 `glibc` / `linux-headers`。⇒ **6 个包的链**。

**方案 B:静态链接 musl(⭐ 推荐)**

用索引里已有的 `aarch64-linux-musl-gcc` 自举,把 clang/lld 静态链到 musl:

- 依赖数 **5 → 0**。没有 glibc、没有 gcc-runtime、没有 zlib/libxml2 的
  运行期问题(编进去)。
- 与 `musl-gcc` 在 aarch64 上已经成立的形状一致 —— 那个包正是这样做的,
  而它是这台机器上**唯一现在就能用的工具链**。
- ⚠️ 代价:体积更大,且要关掉 `LLVM_ENABLE_LIBXML2` 等可选特性。
- ⚠️ 未验:libc++ 的 `std` 模块在 musl-static 的 clang 下是否完整可用。
  **这是方案 B 的第一个判据,必须先于其余工作验证。**

### 4.3 建在哪

`ubuntu-24.04-arm` 是原生 aarch64 runner,GitHub 单 job 上限 6 小时。
LLVM 完整构建约 1.5–3 小时 —— **放得下,不需要交叉编译**。

⚠️ `xlings-res/llvm` 目前是纯资产仓(只有 README + releases),构建脚本
`build-llvm-subpkg.sh` 不在其中。⇒ 需要在该仓新建一个 workflow,
并把 carve 配方(哪些留、哪些删)显式写进去,而不是继续留在某个人的工作目录里。

---

## 5. 分阶段计划与依赖

```
P0 ──┬── P1(llvm 构建)──┬── P3(索引接线)── P4(mcpp 诚实)── P5(生态 CI)
     │                    │
     └── P2(xlings 形状)──┘                          P6(aarch64-linux-gnu 目标)
```

### P0 — 让 aarch64 引导跑通(⏳ 进行中,已提交)

三层,一层盖住一层,每修一层才露出下一层:

| # | 缺陷 | 症状 | 状态 |
|---|---|---|---|
| ① | `bootstrap-mcpp` 按 `uname -s` 取 x86_64 xlings | `Exec format error` 126 | ✅ 已修 |
| ② | 缓存键无 `runner.arch`,aarch64 命中 x86_64 缓存 | ①的修复一次没跑到 | ✅ 已修 |
| ③ | 两个 tarball 内部布局不同 | `No such file or directory` 127 | ✅ 已修(find 而非硬编码) |

**判据**:`invariants (linux-aarch64)` 绿。

### P1 — 建 aarch64 的 LLVM

- **P1.1 可行性判据(先做)**:在 `ubuntu-24.04-arm` 上用
  `aarch64-linux-musl-gcc` 静态建一个最小 clang+lld,验证
  `import std` 与 libc++ 的 `std.cppm` 可用。⭐ 这一条不过,方案 B 作废,回 A。
- **P1.2**:在 `xlings-res/llvm` 新建 `build-aarch64.yml`,产出
  `llvm-22.1.8-linux-aarch64.tar.xz` + `.sha256`,carve 配方写进 workflow。
- **P1.3**:同样产出 20.1.7(target 表两个版本都在用)。
- **判据**:资产可下载,sha256 匹配,解包后 `bin/clang++ --version` 在
  aarch64 上退 0。

### P2 — xlings 包形状统一

⚠️ 不改 xlings 的发布(那会动别的消费者)。**mcpp 侧已经改成 find 而非硬编码**,
这是正确的方向:消费者不该假设生产者的内部布局。

- **P2.1**:向 xlings 报一个 issue,说明两个 Linux 包布局不一致。
- **判据**:issue 有编号;mcpp 侧的 find 已经不依赖它被修。

### P3 — `xim-pkgindex` 接线

- **P3.1** `llvm.lua`:把第 178 行的 `linux-x86_64` 改成按 `os.arch()` 取,
  并给 linux 分支加 aarch64 的资源条目。
- **P3.2** ⚠️ **`archs` 要按 OS 拆**,或者在 linux 分支缺资产时让 xim 说
  「本架构未发布」而不是 404。⭐ 这是「声明 ≠ 安装」在索引侧的同一条:
  一个包级 `archs` 覆盖三个 OS,而三个 OS 的资产覆盖面不同。
- **P3.3**(方案 A 才需要)补 zlib / libxml2 / gcc-runtime 的 aarch64。
- **判据**:`xlings install llvm@22.1.8` 在 aarch64 上成功;
  失败时的消息点名架构。

### P4 — mcpp 对 aarch64 诚实(✅ 已落地)

⭐ **决定:aarch64 上先只支持 `musl-gcc`,其余显式标记延缓。**

- **P4.1 ✅** `available_toolchain_indexes()` 在非 x86_64 Linux 上不再列出
  `llvm` 与 `mingw-cross-gcc`。⚠️ 这是一句**政策陈述**(mcpp 在这台宿主上支持
  哪些族),不是索引数据的抄本 —— 与目标行的 `tier` 同类。
- **P4.2 ✅ 延缓的前提每轮重测**:`.github/tools/check_aarch64_llvm_deferral.sh`
  查 `xlings-res/llvm` 的 20.1.7 / 22.1.8 是否出现了 `linux-aarch64` 资产。
  ⚠️ **它在理由不再成立时变红**,与一般的检查方向相反。
  ⚠️ 网络故障不得被读成「出现了」:读不到资产表就说读不到,保持前提不动。
- **P4.3 ✅ 生态 e2e 的豁免按理由给**:298 在 aarch64 上跳过,理由是
  `llvm is not installed here`。按理由给而不是按宿主给,llvm 落地那天它自动
  从「跳过」变回「断言」,workflow 一行都不用改。

⚠️ **`host_can_serve()` 对裸机仍无条件 `true`** —— 那条理由(clang/lld 按构造
就是交叉编译器)预设了 clang 在这台机器上存在。P4.1 的门让四个裸机行不再被
列出,所以症状已经消失;这一处的**根因**留到 P1 之后再处理,因为届时它自然成立。

### P5 — 生态 CI 加 aarch64

- **P5.1** 七个 openkal 仓库的 `ci.yml` 各加一条 `ubuntu-24.04-arm` leg。
  ⚠️ `openkal-llvm-runtime` 要求 `mcpp:compiler=llvm`,**依赖 P1**;
  `openkal-musl` 只 `provides c-abi=musl`,**现在就能跑**。
  ⇒ 分两批:musl 层先行,llvm 层等 P1。
- **P5.2** `mcpplibs-index` smoke 加 aarch64 leg(源码分发,只需验证能建)。
- **P5.3** 回填 `tests/matrix/expected.tsv` 的 `linux-aarch64` 行。
- **判据**:J2 / J3 / J4。

### P6 — `aarch64-linux-gnu` 作为**目标**(独立,可后置)

今天它是 `planned`。要让它 `verified` 需要 aarch64 的 `glibc` 与
`linux-headers` **目标侧**载荷。⚠️ 与 P1–P5 无依赖关系 —— 那是「aarch64 作为
构建机」,这是「aarch64 作为目标」。**两件事不要混。**

---

## 6. CI 验收

| 层 | 在哪 | 加什么 |
|---|---|---|
| 构建机轴 | `ci-target-matrix.yml` | ✅ 已加 `linux-aarch64` |
| 分母 | `coverage` job | ✅ 已加:发布集 == 矩阵集,双向 |
| 期望表 | `tests/matrix/expected.tsv` | P5.3 回填 aarch64 行 |
| 生态 | 七个 openkal 仓 | P5.1,分两批 |
| 索引 | `mcpplibs-index` | P5.2 |
| 引导 | `ci-aarch64-fresh-install.yml` | ⚠️ 它验的是装得上,**不是**目标矩阵。保留,但不要把它的绿读成覆盖 |

⚠️⚠️ **`expected.tsv` 里不写 `mismatch`。** 写下它就是把缺陷声明成期望,
矩阵会在那一格恒绿。aarch64 的行要么 `ok`,要么带具名 `reason` 的
`unsupported` —— llvm 缺席期间,四个裸机行应当是后者。

---

## 7. 已知陷阱

⚠️ **一层修复会被上一层盖住。** P0 的三条就是这样:缓存键的缺陷让架构修复
一次都没执行。改完一层一定要看下一层的读数,而不是假定它通了。

⚠️ **改共享路径前先查那三台正在工作的宿主。** P0③ 的 find 在 x86_64 上解析到
另一个路径;查过才知道两者是符号链接、同一文件。一个悄悄把 macOS 和 Windows
挪到别的二进制上的修复,比它修的缺陷更糟。

⚠️ **`ci-aarch64-fresh-install.yml` 一直是绿的。** 它走
`quick_install.sh`(自己读架构),所以从来没碰到 P0①。**一条绿的 CI 不证明
另一条路径可用** —— 这正是为什么构建机轴要按 mcpp **发布**的那一组来定,
而不是按「手头有哪几台 runner」。

⚠️ **不要把索引的架构覆盖面抄进 mcpp。** P4.1 的诱惑是写一张
「llvm 没有 aarch64」的表。那张表会在 P1 落地的当天变成错的,而没有任何东西
会提醒你。要问索引,不要记住索引。
