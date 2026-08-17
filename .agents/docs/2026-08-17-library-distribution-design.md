# 库分发:`mcpp pack <target>` 与二进制包(2026-08-17)

> **这份是方案。** 发现(现状、实测、file:line)在
> `2026-08-17-distribution-architecture-analysis-and-design.md`;那份文档的
> §2(五轮实测)是本方案每一条判据的证据来源,本文只在需要时回指。
>
> **实施状态(2026-08-17):P0 已实现并开 PR #451(未合入)。**
> 落地与设计的三处差异,以及实现过程中被实测推翻的四条,记在 §11。
>
> 起因:issue #433「预编译 .so + .ixx/.h/.cppm 接口」。
> 讨论过程中我有**四处设计被自己的实测推翻**,都记在 §9,因为**错的那版看起来同样合理**。

---

## 0. 一页纸

**模型三句话:**

1. **二进制包不是新格式** —— 它是一个**自带 `mcpp.toml` 的普通 tarball**,走 mcpp 已有的
   Form A 通路(`prepare.cppm:2764`)。索引条目与源码包**一字不差**。
2. **产出什么由 `[targets.<n>].kind` 决定** —— `mcpp pack <target>`。
   没有 `--lib`,没有 `--artifact`。**新增 manifest 段 0 个、键 0 个** ——
   生成的包就是一个**普通的 `mcpp.toml`**(§2.4)。
3. **能算出来的东西不给字段** —— 接口发布集是**模块闭包**,公开头是 `include_dirs` 全量,
   两者都**不允许**在 pack 期裁剪(裁了会让源码分发与二进制分发语义不同)。

**四条不可让步的判据:**

| # | 判据 | 违反会怎样(实测) |
|---|---|---|
| **J1** | 接口与二进制**原子产出、不可分别替换** | 随包 `.cppm` 改一行结构体字段序 ⇒ 编译链接运行全过、**数据是错的、零诊断** |
| **J2** | 产物里**不含生产机器的绝对路径** | `.so` 烧着 `/home/…/.mcpp/…` 的 RUNPATH,换机器即废 |
| **J3** | 发布集 = **从公开根算的模块闭包**,不是 `.m.o` | 实现分区照样产 `.m.o` ⇒ 按 `.m.o` 选会**泄露闭源源码** |
| **J4** | per-target 的 `ldflags` 用 `cfg(...)` **不用裸三元组** | 裸三元组在原生构建下不匹配 ⇒ **CI 全绿、本机静默失配** |

---

## 1. 模型

### 1.1 两种接口模式,两条规则,由目录决定

```
pkg/
├── mcpp.toml            ← 生成物:一个普通的 mcpp.toml(§2.4),没有新段
├── include/             ← 文本接口:原样全发,消费者 #include,不产生任何对象
├── interface/           ← 模块接口:算闭包,消费者编译它得到 BMI + object
├── lib/<triple>/        ← 链接面:.a / .so / .dylib / .lib(PE 导入库)
├── bin/<triple>/        ← 运行面(仅 PE):.dll,部署到 .exe 旁
├── HOST-REQUIREMENTS    ← 与 mcpp pack 同一份推导(仅当有内容要说)
└── LICENSE / THIRD-PARTY
```

| | `include/` | `interface/` |
|---|---|---|
| 是谁的输入 | 预处理器 | **编译器** |
| 消费者要编译吗 | 否 | **是** |
| 要算闭包吗 | 否 | **是**(J3) |
| 要从归档剔对象吗 | 否 | **是** |
| ABI 闸门 | `extern "C"` ⇒ tag 只有三段(只约束 libc) | tag 全段 |

**两种模式互不干扰、可同时存在** —— 实测:同一个包被「只 `#include`」/「只 `import`」/
「两者同时」三种方式消费 × 静态/动态两种形态,**六格全过**。

**`lib/` 与 `bin/` 分开是机制不是风格**:PE 上链接的文件(`.lib` 导入库)与运行的文件
(`.dll`)是两个;ELF/Mach-O 是同一个。
**按三元组分目录不按 OS 分**:MinGW 与 MSVC 同为 windows,一个产 `lib*.a` 一个产 `*.lib`。

### 1.2 两个兼容量,不是一个

| | `abi_tag` | `build_key` |
|---|---|---|
| 回答 | 「这份二进制能不能链进你的构建」 | 「这份 BMI 能不能直接用」 |
| 谁能算 | **生产者**(消费者存在之前就能枚举) | **只有消费者**(含依赖闭包 Merkle) |
| 来源 | `mcpp build --print-fingerprint` 的 [1][2][4][5][6] 的**纯投影** | `cache_key::key_hex`(已存在) |

```
<arch>-<os>-<env>-<compiler><major>-<stdlib><major>-c++<level>

x86_64-linux-gnu-gcc16-libstdcxx16-c++23
aarch64-macos-none-llvm22-libcxx22-c++23
x86_64-windows-msvc-msvc194-msvcstl194-c++23
```

⚠️ **`arch-os-env` 必须用 `triple.cppm` 的规范拼写,不是 fingerprint 的 [4]** ——
[4] 是编译器自报的(`x86_64-w64-mingw32`),与 `[target.'…']` 键的拼写
(`x86_64-windows-gnu`)不同,直接用会让同一个决定有两个拼写。

**纯 `extern "C"` 库发一个更短的 tag** —— `x86_64-linux-gnu`,没有
compiler/stdlib/std 段。闸门按段比对 tag 里**有**的东西,而 `abi_check`
(`abi.cppm:157`)早就是「未指定的维度 = 不关心」。**tag 的形状就是 surface**,
不需要额外的开关;C 库的 tag 组合数因此天然从 N×M 掉回 N。

### 1.3 三层分发,层级由消费端选

| Tier | 包里有什么 | 生效条件 | 失配时 |
|---|---|---|---|
| **S** source | 全部源码 | 永远 | —— |
| **I** interface+binary | 接口 + 预编译库 | `abi_tag` 匹配 | 降级到 S;无 S 则**明确拒绝并列出可用 tag** |
| **B** +BMI | 再加预编译 BMI | `build_key` 精确匹配 | **静默**降级到 I |

Tier B 必须先做跨机器可行性实验才能实施(GCC 把时间戳与源码路径写进 BMI),
**它只能是加速器,永不成为正确性依赖**。

---

## 2. 生产者侧

### 2.1 命令

```bash
mcpp pack                                    # 唯一可打包目标;有歧义 → 报错并列出候选
mcpp pack mathkit                            # kind = "lib"    → 静态库包
mcpp pack mathkit-shared                     # kind = "shared" → 动态库包
mcpp pack myapp                              # kind = "bin"    → 应用 bundle(既有四档 --mode)
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu # 胖包(--target 可重复)
```

**新增 CLI:一个位置参数(与 `mcpp run [target]` 同形)+ `--target` 可重复。**
**新增 manifest 键:0 个。**

| `[targets.<n>].kind` | 产出 | `--mode` 适用 |
|---|---|---|
| `bin` | 应用 bundle | ✅ 既有四档 |
| `lib` | 静态库包 | ✅ **`system`(默认)/ `vendored`** —— 见下 |
| `shared` | 动态库包 | ✅ `system` / `vendored` / `self-contained` |

#### 2.1.1 ⚠️ 依赖跟不跟着走,静态库同样要选(修正)

本文前一版写「静态库的 `--mode` 不适用,归档不携带任何东西」。**那句是错的。**
「归档不携带依赖的代码」恰恰**是**问题所在:消费者必须自己把 `zlib` 拉进来 ——
除非包把它一起带上。这就是 `--mode` 问的那件事,只是机制随形态不同:

| `--mode` | 库包里有什么 | 消费者要什么 | 适合 |
|---|---|---|---|
| **`system`**(默认) | 只有本库 + `[dependencies]` 声明 | 自己解析依赖(公开索引 / 私有索引) | 依赖都是公开包 |
| **`vendored`** | 本库 + **依赖的产物一起进 `lib/<triple>/`**,包内不声明 `[dependencies]` | 什么都不要 | 内网 / 离线 / 依赖本身也是闭源 |
| `self-contained` | 仅 `shared`:再带上 libc 一侧的闭包 | 什么都不要 | 跨发行版 |

**与应用的 `--mode` 是同一个问题的同一批答案**:闭包跟不跟着走。机制不同
(应用问的是运行期 `.so`,静态库包问的是构建期可链接的产物),但问题相同,
所以复用既有词汇而不是发明第二套。

`vendored` 形态下,依赖产物在 `[[runtime.artifacts]]` 里各占一条,
`provenance` 记它**来自哪个包**(`"vendored compat.zlib@1.3.2 by mcpp-pack 2026.8.17.1"`),
生成的 `ldflags` 按拓扑序把它们排在本库之后。

**分期**:`system` 是 P0(当前实现自然就是它),`vendored` 是 P1。

### 2.2 生产者工程示例(全部是既有的键)

```toml
# examples/05-lib-dist/mcpp.toml
[package]
name        = "mathkit"
version     = "0.1.0"
description = "Demo: shipping a prebuilt library with both header and module interfaces"
license     = "Apache-2.0"
platforms   = ["linux", "windows"]   # 既有键。pack 用它做覆盖校验(缺腿告警)

[build]
sources      = ["src/*.cppm", "src/*.cpp", "src/*.c"]
include_dirs = ["include"]           # 公开头。整发,不可裁

# [lib] path 不写 ⇒ src/mathkit.cppm 约定 ⇒ 模块闭包从这里出发

[targets.mathkit]
kind = "lib"                         # → mcpp pack mathkit 产出静态库包

[targets.mathkit-shared]
kind   = "shared"                    # → mcpp pack mathkit-shared 产出动态库包
soname = "libmathkit.so.1"           # 给出正确的运行期名

[pack]
include = ["share/**"]               # 既有键 —— 只作用于 extras
```

```
examples/05-lib-dist/
├── mcpp.toml
├── README.md
├── include/mathkit_c.h        # extern "C" 头接口
└── src/
    ├── mathkit.cppm           # export module mathkit;  export import :api;   ← 闭包根
    ├── api.cppm               # export module mathkit:api;                    ← 闭包内
    ├── secret.cppm            # module mathkit:secret;   ← 实现分区,**不发布**
    ├── impl.cpp               # module mathkit;  import :secret;
    └── capi.c                 # C API 实现
```

### 2.3 `mcpp pack <target>` 做什么

```
1. build 出产物                        ⚠️ 用这次构建的 target/<triple>/<fp>/bin/… 路径,
                                          绝不 glob(会挑到陈旧的 fingerprint 目录)
2. 算接口闭包(从 lib-root 出发)       ⚠️ 走 P1689,不走 M1 文本扫描器(§9-D)
   → 拷进 interface/
   → 打印「将发布 / 不发布」两张清单
   → 闭包里出现实现分区 ⇒ 告警「它的源码会被发布」
3. 拷 include_dirs 全量 → include/
4. 从归档剔除**已发布闭包那些单元**的对象    ⚠️ 不是「所有 .m.o」(J3 同一个闭包的第二个用途)
5. artifact_kind = shared 时额外:
     · RUNPATH / INTERP 重写($ORIGIN 化)   ← J2
     · 第三方闭包收集(ELF: LD_TRACE;PE: 导入表)
     · PE: 导入库进 lib/,DLL 进 bin/
6. 生成包内 mcpp.toml(§2.4)+ HOST-REQUIREMENTS
7. 确定性归档(PE → .zip,其余 → .tar.gz)
```

**计划阶段必须校验的组合**(不能留到链接期;`--mode` 与形态的组合见 §2.1.1):

| target | `kind = "lib"` | `kind = "shared"` |
|---|---|---|
| `x86_64-linux-gnu` | ✅ | ✅ |
| `x86_64-linux-musl` | ✅ | ❌ **musl 蕴含 `-static`,产不出共享库** |
| `x86_64-windows-gnu` / `-msvc` | ✅ | ❌ P1 解锁(PE 导入库) |
| `*-macos` | ✅ | ❌ P1 解锁(install_name) |

### 2.4 包内 `mcpp.toml` —— 一个普通的 mcpp.toml,没有新段

> **本节是第三次重写。** 前两版分别提了 `MCPP-PACKAGE.toml` 和 `[distribution]` 段。
> 逐条追问「这件事别处已经能说了吗」之后,**两者都被删掉**:每一条事实都有既有字段。

#### 2.4.1 每条事实的归属

| 曾经想新增的 | 归属(全部既有) |
|---|---|
| `artifact_kind`(static/shared) | `[[runtime.artifacts]].role` = `static-library` / `shared-library` |
| `abi_tags`(包级列表) | `[[runtime.artifacts]].abi` —— **每条腿一个,比包级列表更准** |
| `abi_surface`(c/cxx) | **tag 自身的形状**(见 2.4.2) |
| `modules` | `[modules] exports`(既有的完备性断言) |
| `cxx_runtime` | `[build] cxx_runtime`(既有) |
| `interface_digest` | `[[runtime.artifacts]]` 里 `role = "interface"` 的条目,**逐文件一条** |
| `built_by` | `provenance = "mcpp-pack <version>"` |
| `build_key` | `host_fingerprint`(docs/05 §2.11 原文:*optional evidence*) |
| 「这是分发包」的标记 | `provenance` 以 `mcpp-pack` 开头 —— **可推导** |
| `schema` | 不需要:没有新 schema 要版本化 |

**实测**(mcpp 2026.8.15.3):`role = "interface"`、任意 `abi` 串、`digest`、
`provenance` 全部原样进 `resolution.json`,并挂上既有的 `identity` 判定
(路径不存在 ⇒ `missing`)。**兼容性比新段更好** —— 新段会被老客户端静默跳过
(等于没有记录),而 `[[runtime.artifacts]]` 是老客户端**已经在读**的段。

#### 2.4.2 `abi_surface` 消失了:tag 的**形状**就是 surface

`abi.cppm:157` 的 `abi_check` 早就是「**未指定的维度 = 不关心**」。所以不需要一个
布尔来说「我只约束 libc」—— **发一个短 tag 就是在说这件事**:

```
纯 extern "C" 库    abi = "x86_64-linux-gnu"                                  ← 只有三段
C++ 模块库          abi = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"          ← 全段
```

闸门按**段**比对 tag 里有的东西。C 库的 tag 组合数因此天然从 N×M 掉回 N,
不需要任何开关。

#### 2.4.3 生成的包内 `mcpp.toml` —— 描述只有三件事

**一个二进制包的描述应当和源码包一样、甚至更简单。核心就三样:接口 / 库 / 依赖。**
其余的都不是「描述」,是**证据**,而证据挂在既有的 `[[runtime.artifacts]]` 上,
每条腿一条 + 接口一条,不是每个文件一条。

```toml
# ══ 由 `mcpp pack mathkit` 生成。手工编辑会使 interface 的 digest 失配并被拒绝。 ══
[package]
namespace = "acme"
name      = "mathkit"
version   = "0.1.0"

# ── ① 接口 ────────────────────────────────────────────────────────
[build]
sources      = ["interface/mathkit.cppm", "interface/api.cppm"]   # 模块接口:消费者编译它
include_dirs = ["include"]                                        # 头接口:消费者 #include
cxx_runtime  = "self-contained"

[modules]
exports = ["mathkit"]

[targets.mathkit]
kind = "lib"

# ── ② 库(每条腿一段;必须是 cfg(...),不能是裸三元组 —— J4)──────────
[target.'cfg(all(linux, not(env = "musl")))'.build]
ldflags = ["-Llib/x86_64-linux-gnu", "-lmathkit"]

[target.'cfg(all(linux, env = "musl"))'.build]
ldflags = ["-Llib/x86_64-linux-musl", "-lmathkit"]

[target.'cfg(windows)'.build]
ldflags = ["-Llib/x86_64-windows-gnu", "-lmathkit"]

# ── ③ 依赖(从生产者工程原样带过来)────────────────────────────────
[dependencies.compat]
zlib = "1.3.2"

# ── 证据:每条腿一条 + 接口一条 ────────────────────────────────────
[[runtime.artifacts]]
role             = "static-library"
path             = "lib/x86_64-linux-gnu/libmathkit.a"
provenance       = "mcpp-pack 2026.8.17.1"     # 前缀即「这是分发包」的标记
abi              = "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"
digest           = "sha256:…"
host_fingerprint = "aeb4c4d29e437696"          # build_key,Tier B 用

[[runtime.artifacts]]
role       = "interface"
path       = "interface"                       # 目录,一条即可 —— 不是每个文件一条
provenance = "mcpp-pack 2026.8.17.1"
digest     = "sha256:…"                        # 对有序文件集的摘要
```

**⚠️ ③ 依赖是必须的,而且容易漏。** 一个静态库的 `.a` **不携带**它的第三方依赖 ——
消费者链接时必须自己把 `zlib` 拉进来。所以 `mcpp pack` 要把生产者的
`[dependencies]` 原样写进包(**排除 dev-dependencies 与 path 依赖** ——
后者是本地的,发出去解析不了)。这条规则 `emit_xpkg` 已经在用
(`publisher.cppm:186-192`),**同一份推导,第二个投影**。

`shared` 形态另外多一段:

```toml
[runtime]
runtime_search_dirs = ["lib/x86_64-linux-gnu"]   # 进消费者的 RPATH
# deploy_files      = ["bin/x86_64-windows-gnu/mathkit.dll"]   # PE:部署到 .exe 旁
```

#### 2.4.4 为什么接口 digest 只有一条

**它防的是「解开之后有人改了随包的接口」**,不是「生产者一开始就发错了配对」——
后者只有原子产出能防(§0 J1)。索引通路上,整包已经有 sha256;真正裸奔的是
**path 依赖 / 已解开的 store**,而那里一条目录级 digest 就够:
诊断说「`interface/` 与打包时不一致,拒绝构建」已经是可行动的。
逐文件 digest 能多说一句「是哪个文件」,代价是描述里多出 N 行 —— 不值得。

### 2.5 分发包目录里不许直接 build

判据:**任一 `[[runtime.artifacts]]` 的 `provenance` 以 `mcpp-pack` 开头**。
此时在解开的包目录里直接 `mcpp build` **必须拒绝**,并说清这是分发包不是源码树。
今天它会**成功** —— 把 `interface/` 里只有声明的接口单元编出来,产出一个几乎空的库,
实现全在预编译产物里没被链进来。典型的「看起来成功的失败」。

---

## 3. 消费者侧

### 3.1 三种依赖形态,同一条下游路径

```toml
# ① 离线文件 —— P0。不需要索引、网络、鉴权,不需要动 xlings
mathkit = { package = "vendor/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23.tar.gz" }

# ② 索引(公开或私有)—— P1。描述符与源码包同构:只有 url + sha256
mathkit = "0.1.0"

# ③ 本地目录(内部团队最常走的一条)
mathkit = { path = "../mathkit-dist" }
```

```bash
mcpp add ./mathkit-0.1.0-<tag>.tar.gz
```

**⚠️ 闸门必须对 ③ 也生效** —— 这就是闸门字段要放进包内 `mcpp.toml` 而不是旁路文件的理由。

### 3.2 闸门表(顺序即诊断顺序)

| 检查 | 失配 |
|---|---|
| arch / os / env | **拒绝** —— 载荷本身不对 |
| `abi_surface == "c"` | 上一行之后全部跳过 |
| compiler 族 / 主版本 | **拒绝**;`--allow-abi-drift` 强制并打印它保护的是什么 |
| stdlib id / 主版本 | **拒绝** |
| C++ 标准档位 | 消费者 < 生产者 → **拒绝**;> → 放行 |
| `cxx_runtime` 契约 | **警告** + 说清双运行时危险;`--strict` 升级为错误 |
| `interface_digest` | **拒绝** |
| 模块符号存在性(ELF `_ZGIW<M>` / PE 导出表) | **拒绝** |
| `build_key`(Tier B) | **静默**降级到 Tier I |

### 3.3 没有匹配 tag 时的诊断

```
error: acme.mathkit@0.1.0 has no prebuilt artifact for this toolchain
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags : x86_64-linux-gnu-gcc15-libstdcxx15-c++23
                   aarch64-linux-gnu-gcc15-libstdcxx15-c++23
  note: this package ships no source tier, so there is nothing to fall back to.
  fix : ask the publisher for a gcc16 build, or pin [toolchain] to gcc@15.
```

**「不可用」不能被拼成「不存在」** —— 被拼成「找不到包」的失败会让客户端自己驱动
重复刷新索引(#349 的教训)。

### 3.4 兼容性:老客户端拿到这个包会怎样

**能构建,只是没有闸门。** 实测(mcpp 2026.8.15.3):
包内 mcpp.toml 用的 `sources` / `[build]` / `[modules] exports` /
`[target.'cfg(…)']` / `[[runtime.artifacts]]` **全部是已发布能力** —— 老客户端
逐字读得懂,只是不会**执行闸门**(它不知道 `provenance = "mcpp-pack"` 意味着要校验
digest 与 abi tag)。这是**降级**而不是变砖 ——
但**闸门只保护新客户端,必须写进发布说明**。

这也是不新增段的第二个好处:一个**新**段会被老客户端静默跳过,连记录都没有;
而 `[[runtime.artifacts]]` 是老客户端**已经在读、并且会写进 `resolution.json`** 的段,
所以即使闸门不执行,证据仍然落盘、仍然可审计。

---

## 4. `docs/` 文档计划

| 文件 | 动作 | 内容 |
|---|---|---|
| `docs/02-pack-and-release.md` | **改** | 标题改为「打包:应用与库」。新增「§库分发」:`mcpp pack <target>` 的 kind 表、两种接口模式的目录规则、包布局、跨平台合法组合表。既有的应用四档 `--mode` 一字不改 |
| `docs/12-binary-distribution.md` | **新增** | 库作者的完整链路:写工程 → `mcpp pack` → 检查两张清单 → 分发(文件/私有索引/公开索引)→ 消费者怎么用。含 §2.2 与 §3.1 的完整示例 |
| `docs/05-mcpp-toml.md` | **改** | ① `[pack]` 一节补「只作用于 extras,裁不到头与接口」;② §2.11 补 `role = "interface"` 与 `provenance = "mcpp-pack"` 的约定用法(生成物,不是手写配置);③ Appendix A 补一条准入判据:「能从别处推出来的不给字段」 |
| `docs/10-publishing-a-library.md` | **改** | 新增「发布二进制包」小节:与源码包**同构**的索引条目、`platforms` 覆盖校验、老客户端降级说明 |
| `docs/03-toolchains.md` | **改** | `abi_tag` 的六个成分与 `--print-fingerprint` 的对应关系 |
| `docs/zh/*` | **同步** | 上述五处的中文版 |

---

## 5. `examples/` 具体示例

沿用既有编号与「每目录一个 README.md」的约定。

### `examples/05-lib-dist/` —— 生产者(库作者)

结构见 §2.2。README 要点:

- 一个工程**同时**提供头接口与模块接口,消费者可以只用其中一种;
- `secret.cppm` 是实现分区,**不会被发布** —— 跑 `mcpp pack mathkit` 看两张清单;
- 两个目标(`lib` + `shared`)如何同时发布,以及 `soname` 给出的正确运行期名;
- **反面演示**:把 `src/mathkit.cppm` 改成 `import :secret;`,再 pack,
  观察闭包里多出 `secret.cppm` 并触发告警。

### `examples/06-lib-consume/` —— 消费者

```
examples/06-lib-consume/
├── mcpp.toml            # mathkit = { path = "../05-lib-dist/dist" }
├── README.md
└── src/
    ├── main_header.cpp  # 只 #include <mathkit_c.h>
    ├── main_module.cpp  # 只 import mathkit;
    └── main_both.cpp    # 两者同时
```

README 要点:三种消费方式 × 静态/动态两种包 = 六格,全部可跑;
以及**篡改 `interface/` 后必须被拒绝**的演示。

### `examples/07-lib-dist-fat/` —— 胖包与交叉

一个 `mcpp.toml` + 一条命令产出三条腿,消费者用 `--target` 选。README 要点:

- 为什么每条腿的 `ldflags` 是 `cfg(...)` 而不是裸三元组(J4,附最小探针);
- `lib/` 为什么按三元组分目录而不按 OS 分。

---

## 6. CI:验证矩阵就是 e2e 集合本身

> **相对初稿的修订。** 初稿提议新建 `pack-dist-matrix.yml`。**不需要。**
> `tests/e2e/run_all.sh` 已经有能力探测(`elf` / `gcc` / `mingw-cross` /
> `fresh-sandbox` / …)并按 `# requires:` 分流,而 `ci-linux-e2e.yml` 已经在跑
> 整个 `tests/e2e/`。再建一条并行流水线就是**同一个决策的第二处推导**。
>
> 矩阵是 e2e 集合 + 每个测试头部那行 `# requires:`。

### 6.1 落地的测试与它们钉住的判据

| e2e | `# requires:` | 钉住 |
|---|---|---|
| **242** `pack_library_interface_and_headers` | `gcc` | 两种接口模式共存;包布局;两张清单都被打印 |
| **243** `pack_library_interface_closure` | `gcc` | **J3** —— 实现分区源码不外发 **且** 它的对象留在归档里 |
| **244** `pack_library_gate` | `gcc` | **J1** 接口篡改被拒 + tag 失配被拒(且列出可用 tag)+ 包内 build 被拒 |
| **245** `pack_library_fat_target_selection` | `gcc` | **J4** —— 胖包每条腿只被自己的 target 看到,**含原生构建** |
| **246** `explicit_empty_sources` | `gcc` | §8-B2 —— `sources = []` 与「不写」可区分 |
| **247** `bare_triple_conditional_native` | `gcc` | §8-B1 —— 裸三元组谓词在原生构建下命中 |
| **248** `pack_library_fat_pe_leg` | `gcc mingw-cross` | 跨 OS 边界的腿(PE);tag 用规范三元组而非编译器自报 |

### 6.2 ⚠️ 一处差点造出来的假绿

245 最初写成 `# requires: gcc mingw-cross`(gnu + musl + windows 三条腿)。
**`ci-linux-e2e.yml` 只预热 gcc 与 musl,不装 mingw-cross** ——
那条测试会在每一次普通 CI 上**静默跳过**,于是胖包这一整套机制
(以及 J4)在绿色的套件里**从未被验证过**。

拆法:**核心机制用 CI 一定有的工具链**(245:gnu + musl,`requires: gcc`),
**只把新增的二进制格式覆盖单列**(248:PE,`requires: mingw-cross`)。
判据:**一个测试的 `# requires:` 必须是它所验证机制的真实下限,不是它能跑的上限。**

### 6.3 单元测试

| 文件 | 覆盖 |
|---|---|
| `tests/unit/test_pack_abi_tag.cpp` | tag 的投影 / 规范三元组 / C 表面短 tag / 从尾解析 / 档位下限 / 一次报全部失配(15 例) |
| `tests/unit/test_pack_interface.cpp` | 闭包 / 剔除集不是「所有 `.m.o`」/ 依赖模块不越界 / 未解析分区报错(8 例) |

### 6.4 仍需在 CI 上补的(P1)

- macOS 与 Windows 的 e2e 分片会自动跑 242/243/244/246/247(它们只 `requires: gcc`),
  但**尚未在这两个平台上人工确认过**;
- `kind = "shared"` 的库包 e2e(P1,随 PE 导入库 / Mach-O install_name 一起);
- `--target` 覆盖与 `[package].platforms` 的比对告警(P1)。

## 7. 分期

### P0 —— `static` 形态,三平台一次做完(不依赖 xlings、不依赖 shared)

1. `sources = []` 的三态(§8-B2)+ §2.5 的 build 守卫(判据 = `provenance` 前缀)
2. 包布局 + 生成包内 `mcpp.toml`
3. `mcpp pack <target>` 位置参数 + `--target` 可重复;`kind = "lib"` ⇒ 静态库包
4. 接口闭包(P1689)+ 两张清单 + 实现分区告警
5. 从归档剔除已发布闭包的对象
6. `abi_tag` 计算 + 闸门表 + `interface_digest` + 模块符号存在性
7. `mcpp add ./x.tar.gz` + `{ package = "…" }` 依赖形态
8. e2e 242 / 244 / 245 / 246 / 248 / 250 / 251;examples 05 + 06

**为什么 static 能在 P0 覆盖三平台**:`kind = "lib"` 无平台限制,
所以 G2、RUNPATH 重写、闭包收集、DLL 部署这一期全部不需要。

### P1 —— `shared` 形态解锁三平台 + 索引通路

9. PE 导入库(`--out-implib` / `/IMPLIB:`)+ Mach-O `-install_name @rpath/…`
10. `kind = "shared"` 守卫的判据改成「该 target 是否动态链接」(musl 走计划期拒绝)
11. `shared` 形态的 pack:RUNPATH 重写 + 闭包收集 + `bin/` 部署面
12. 胖包上索引(描述符与源码包同构)+ `platforms` 覆盖校验
13. e2e 243 / 247 / 249;examples 07;`pack-dist-matrix.yml`

### P2 —— 交叉与私有鉴权

14. 瘦包(按 tag 分资产)+ `install_packages` 加 target 轴(跨仓库,与 xlings 同步)
15. `IndexSpec` 鉴权(值从环境变量读,永不落盘)
16. `target_cfg` / `[target.'cfg(…)']` 承载 LinkIntent(顺手修 #258 同形状的债)

### P3 —— Tier B 与生态收尾

17. Tier B:**先做跨机器可行性实验**,做不到就不做
18. `.pc` / CMake config 产出
19. #304(`library_dirs` 的 link/runtime 分离收口)、#290(按版本区分构建规则)

---

## 8. 需要先单独修的既有缺陷(不属于本方案,但阻塞它)

| # | 缺陷 | 位置 | 对本方案的影响 |
|---|---|---|---|
| **B1** | `[target.'<三元组>'.build]` 在原生构建下**永不命中** | `prepare_inputs.cppm:139` `if (triple.empty()) return false;`,而同文件 `context_for()` 对 `cfg()` 回落到 `host_triple()`;`types.cppm:665` 的注释承诺的是回落 | 直接阻塞胖包的裸三元组写法(J4)。**任何用该写法的工程都中招**,形状是「CI 全绿、本机静默失配」 |
| **B2** | `sources = []` 与整行删掉逐字节等价 | `toml.cppm:1703` 的默认 glob 吞掉显式空 | 二进制包无法表达「什么都不要编」;遗留在 `src/` 的文件会被编进消费者的构建 |
| **B3** | M1 扫描器不把实现分区建模为 provider | `scanner.cppm:641-650` 对非 `export` 的 `module X;` 从不设 `u.provides` | 「接口够到实现分区」的告警产不出来 ⇒ 闭包必须走 P1689 |
| **B4** | `kind = "shared"` 的守卫判据是 `os != "linux"` | `plan.cppm:1002` | musl 过闸后死在 `crtbeginT.o`,消息里既没有 musl 也没有 shared |

B1 / B2 建议**先单独开 issue 并各带一条回归测试**,不要埋进这个大特性里。

---

## 9. 四处被自己的实测推翻的设计(不要重新提出来)

| # | 我写过的 | 被什么推翻 | 正确的 |
|---|---|---|---|
| **A** | 新增描述符键 `kind = "prebuilt"` + 版本 floor | 它属于「不降级」的那一类,老客户端会被砖 | 走 **Form A**(包自带 `mcpp.toml`)⇒ 无新描述符键、无 floor、兼容几乎免费 |
| **B** | 打包时「剔除所有 `.m.o`」 | **三个 target 全部链接失败**(`undefined reference to mk::secret_helper@mathkit()`)—— 实现分区照样是 `.m.o` 且里面是真代码 | 剔除集 = **已发布闭包里那些单元**的对象;与发布集是**同一个闭包的两个用途** |
| **C** | 胖包每条腿用裸三元组 `[target.'x86_64-linux-gnu'.build]` | 显式 `--target` 三个全绿,**裸 `mcpp build` 链接失败**;最小探针:裸三元组 0 命中 / `cfg(linux)` 2 命中 | 一律用 `cfg(...)`(J4);并把根因(B1)单独开 issue |
| **D** | `[pack]` 加五组键(`default_kind` / `default_artifact` / `targets` / `[pack.interface]` / `[pack.headers]`) | 逐条问「别处已经说过了吗」之后全部落空 | **新增 0 个键**:kind → `[targets.<n>].kind`;接口根 → `[lib]`;头目录 → `[build].include_dirs`;平台 → `[package].platforms` 升格为断言 |

外加一处我自己踩的坑:原型用 `rglob` 找产物,**挑到了陈旧的 fingerprint 目录**
(少一个 `capi.o`,症状是消费者 undefined reference,看起来完全像 mcpp 的问题)。
**打包器绝不能 glob 产物** —— 这与仓库既有的「`ls | head -1` 会自查到旧二进制」是同一形状。

---

## 10. 证据索引

全部实测记录在 `2026-08-17-distribution-architecture-analysis-and-design.md`:

| 判据 | 那份文档的位置 |
|---|---|
| J1(接口↔二进制无绑定,静默错数据) | §2.2(c) |
| J2(`.so` 烧生产机器 RUNPATH) | §2.2(a) |
| J3(闭包 vs `.m.o`;剔除集) | §2.4.2 / §2.4.3 |
| J4(裸三元组不匹配 + 最小探针) | §2.4.5 |
| 六格矩阵(两种接口 × 两种形态) | §2.5.1 |
| 两条目录规则 | §2.5.2 |
| musl + shared 链接期炸 | §2.5.3 |
| `sources = []` 不生效 | §2.5.4 |
| 新段的兼容边界 + array-of-tables 白名单(**为什么最终不新增段**) | §4.2 |
| `[[runtime.artifacts]]` 能承载 role/abi/digest/provenance + identity 判定 | 本文 §2.4.1 实测 |
| `[pack]` 推导审计 | §4.6.1 |
| 复现脚本(`scratchpad/lab/`) | 附录 A |
| file:line 索引 | 附录 B |


---

## 11. 实施记录(PR #451,2026-08-17)

### 11.1 落地与设计的差异

| 设计说的 | 实际做的 | 为什么 |
|---|---|---|
| `[distribution]` 段(4 个字段) | **一个字段都没加** | 追问「这件事别处能说吗」之后,`role`/`abi`/`digest`/`provenance`/`host_fingerprint` 全在 `[[runtime.artifacts]]` 上;`provenance` 前缀就是「这是分发包」的标记 |
| `abi_surface = "c"` 开关 | **tag 的形状本身** | `abi_check` 早就是「未指定 = 不关心」,短 tag 就是那句声明 |
| 新建 `pack-dist-matrix.yml` | **复用既有 e2e 通路** | `run_all.sh` 已有能力探测与 `# requires:` 分流,再建一条是同一决策的第二处推导 |
| `shared` 排 P1 | **Linux/ELF 已可用** | 实测发现只差一件事:包里要同时带链接名与 SONAME |

### 11.2 实现过程中被实测推翻的四条

**(a) `mcpp.pack.library` 里叫 `Error`。** `mcpp.pack` 已经有一个
`mcpp::pack::Error`,而一个名字只能归属一个模块。**GCC 接受了,clang 直接拒绝**
(*cannot be attached to other modules*)—— Windows 与 macOS 全红、Linux 全绿,
这正是三平台矩阵存在的理由。

**(b) `mcpp pack` 在 workspace 根上不能用了。** 路由要在构建前读 manifest,
而 workspace 根**没有自己的目标**(虚拟 workspace 连 `[package]` 都没有),
于是新路由读到空列表、判定「无可打包」。**用上一版发布的二进制跑
`examples/04-workspace` 对照才发现** —— e2e 249 就是这次对照的固化。

**(c) 位置参数没传给应用通路。** 两个 `bin` 目标的工程接受 `mcpp pack app2`
却打包 app1 —— **命令成功、答案错误**。e2e 250 两个方向都钉。

**(d) 动态库包只带了构建出来的文件名。** `-lmathkit-shared` 链的是
`libmathkit-shared.so`,而对象记的是 `SONAME libmathkit.so.1` —— 两个不同的文件名。
包链得上、**起不来**。是 **mcpp 自己的运行期闭包检查**报出来的
(`libmathkit.so.1 not found on the search path this artifact will actually use`),
而不是加载器错误。e2e 251 用 `run` 而不是 `build`:这里链接过了什么都不证明。

外加一条 e2e 卫生:六个 fixture 把包路径按 **shell 拼写**写进了 mcpp.toml。
`00_fixture_path_hygiene.sh` 在 macOS 那条腿上抓到 —— 规则是 Windows 的
(MSYS 只转 argv 不转文件内容),而 lint 在所有平台跑,正是为了让 Linux 上的
reviewer 先于 Windows CI 发现。

### 11.3 我在验证里自己踩的坑

**`ls -t target/*/bin/mcpp | head -1` 挑到了陈旧/别的工具链的 fingerprint 目录 ——
三次。** 一次让 B1 的修复看起来没生效,一次让全部新 e2e 报段错误(实际是拿了
clang 构建的二进制,而 clang 构建的 mcpp 在本机会段错误)。
**这与打包器「绝不 glob 产物」是同一条判据**,只是发生在验证侧。

### 11.4 P0 交付清单

| | |
|---|---|
| 新模块 | `src/pack/{abi_tag,digest,interface,library,library_pipeline,manifest_emit,prebuilt,route}.cppm` |
| 改动 | `prepare_inputs`(B1)、`toml`+`types`(B2)、`source_kind`(`object_filename_for` 归位)、`prepare`(图 + 两道闸门)、`plan`、`cli` |
| 单测 | `test_pack_abi_tag`(15)、`test_pack_interface`(8) |
| e2e | 242–251(10 个) |
| 文档 | `docs/12-binary-distribution.md` + zh;`docs/02`/`05`/README 索引 |
| 示例 | `examples/05-lib-dist`、`examples/06-lib-consume` |
