# 图形栈全面不可用 —— 三层独立故障,和一条没有主人的依赖链

> 日期：2026-08-10
> 基线：mcpp `2026.8.10.1`(main `3724102`)、xlings `2026.8.10.2`、
> mcpp-index main `6b8d77aa`、xim-pkgindex main
> 本文所有结论都有实测或日志出处；本机绝对路径省略为 `<store>` / `<subos>`。

---

## 0. 结论先行

**图形栈不可用不是一个 bug,是三层各自独立的故障叠在一起,任何一层单独都足以致命。**

| 层 | 故障 | 当前影响 |
|---|---|---|
| **L1 装** | `xim:libglvnd@>=1.7.0.1` 解析不到 | mcpp-index 全量跑时 **8/8 图形包 FAIL** |
| **L2 建** | 缓存命中丢掉 std 次序([#405](https://github.com/mcpp-community/mcpp/issues/405)) | **每个图形项目的第二次构建必挂** |
| **L3 跑** | dispatch/vendor 两半无人校验,本机绑定陈旧 | 拿不到 GL context |

而它们能同时存在且长期没人发现,原因只有一个:

> **这条链跨了四方(mcpp / mcpp-index / xim / 宿主驱动),而没有任何一方对「整条链是活的」负责。
> 每一方的 CI 都绿在自己那一片上。**

---

## 1. L1 —— 装不上:一个约束伪装成一个缺包

### 1.1 现象

mcpp-index PR #200 的全量 workspace 跑:

```
error: xlings install_packages failed (exit 1) for 'compat.glx-runtime@2026.08.08'
  xlings reported: E_INVALID_INPUT: package 'xim:libglvnd@>=1.7.0.1' not found
```

`gui-stack` / `imgui-window` / `eui-neo` / `eui-neo-app-main` / `eui-neo-markdown` /
`eui-neo-vulkan` / `eui-neo-window` / `eui-neo-sdl2` —— **八个,全部同一句**。
同一次跑里 `core` / `catch2` / `catch2-main` / `gmp` 都是 ok。

### 1.2 但 xim 确实发布了 `1.7.0.1`

`pkgs/l/libglvnd.lua`:

```lua
["latest"] = { ref = "1.7.0.1" },
-- Same upstream artifact as 1.7.0, new version key.
["1.7.0.1"] = { … }
["1.7.0"]   = { … }
```

版本存在,而且是 `latest`。**所以「not found」是假话。**

### 1.3 真因写在同一个文件里,往上一百行

`xim-pkgindex/pkgs/g/graphics.lua` 第 58–75 行,原文:

> BARE, no range, and that is **measured rather than stylistic**: mesa's version is
> `25.0.7.1` — four components … — and **the resolver's range comparison cannot parse
> a four-component version at all**. Both `@>=25.0.7` and `@>=25.0.7.1` resolve to
> **"package not found"**, which reads as a missing package rather than an
> unparseable constraint.

然后第 157 行:

```lua
"xim:libglvnd@>=1.7.0.1",
```

**四段版本 + 范围约束 —— 正是它自己在一百行前说不可解析的那件事。**
mesa 用裸名规避了,libglvnd 没有。

### 1.4 这条缺陷有三个可独立修的面

1. **数据面(立刻止血)**:`xim:libglvnd@>=1.7.0.1` → `xim:libglvnd`,与 mesa 同治。
2. **诊断面**:不可解析的约束**必须报成不可解析**,不能报成 "package not found"。
   现在这条错误信息会把每一个看到它的人送去找一个并不缺的包 ——
   这一轮我自己就被同类信息误导过两次。
3. **规则面**:一条 lint —— **任何 `@>=` / `@<=` 约束不得指向四段版本**,
   或者反过来把解析器补成能比较四段。二选一,但不能继续靠注释。

> **判据:注释无法强制不变量。** 同一个文件里,知识写在注释里、陷阱掉在一百行后,
> 这不是作者不小心,是**知识放错了地方**。

### 1.5 对 mcpp 的直接牵连

mcpp 自己的版本就是四段(`2026.8.10.1`),而记忆里早有一条:
`version_req` 三段解析静默丢第 4 段。**任何对 mcpp 版本做范围约束的地方,
都有同一个latent 缺陷。** 这次是别人的注册表先炸,下次可能是 `min_mcpp`。

---

## 2. L2 —— 建不了第二次:#405

依赖被缓存命中时,包的编译边被换成 stage 边。`ninja_backend.cppm` 自己的注释
已经写明这会丢掉编译边携带的次序,并为此把每个 staged 产物聚合进
`_mcpp_staged_cache` 作为 order-only 前置 —— **但 std BMI 是另一条更早发出的
stage 边,没被收进那个聚合。**

```
build gcm.cache/std.gcm : stage_file <cache>/std/…/std.gcm      ← 存在
build _mcpp_staged_cache : phony obj/… obj/…                    ← 不含它
build obj/main.o : cxx_object … || _mcpp_staged_cache           ← 只依赖聚合
```

ninja 从不执行那条边,`std.gcm` 在盘上不存在,消费方读被恢复的 BMI 时报
`No such file or directory` / `Bad import dependency`。

**只有当消费方自己不 `import std` 时才现形** —— 而 imgui 模板生成的 `main.cpp`
恰好只 `import imgui.core; import imgui.app;`。所有图形包都 `import std`。

修法 5 行:`staged` 非空时把 std/std.compat 的 BMI 与 object 一并放进聚合。
不动 cache key、不改 entry schema、不使任何人现有缓存失效。
RED 已用 `mcpplibs.cmdline` 复现(`tests/e2e/211_cached_dep_std_is_ordered.sh`)。

---

## 3. L3 —— 跑不起来:一个能力被两个注册表各提供一半

| 半边 | 谁提供 | 落在哪 |
|---|---|---|
| **dispatch** `libGLX.so.0` | **mcpp-index** `compat.glx-runtime` | `<store>/compat-x-glx-runtime/…/glx_runtime/lib` |
| **vendor** `libGLX_nvidia.so.0` | **xim** graphics 栈 | `<subos>/lib/…` → `<store>/xim-x-nvidia-gl-host-link/…` |

dispatch 的 RPATH **已经**包含 `<subos>/lib`,路径是通的;坏的是被指向的东西:

- `xlings install graphics` 明说 rebind 到 `nvidia-gl-host-link@0.1.2`,
  而 `subos/default/lib/libGLX_nvidia.so.0` 仍解析进 **0.1.1** 的 payload;
- 那个 vendor 文件只有 **22 KB** —— `nvidia-gl-host-link` 是一座桥,不是驱动。

而 mcpp 这边 `mcpp why runtime` 的实际输出是:

```
providers:
  - opengl.glx.driver -> compat.glx-runtime@2026.08.08
artifacts:
  (none declared)
```

**`artifacts: (none declared)`** —— mcpp 的模型里本来就有 `RuntimeArtifact` 这一格,
图形这条链一个都没填。**provider 有名无物,所以没有任何东西可以校验。**

顺带一条硬约束:`<subos>/lib` 同时装着 vendor 库**和私有 glibc**。
任何「把这个目录放上搜索路径」的修法都会让宿主二进制立刻死于
`__pointer_chk_guard`([#401](https://github.com/mcpp-community/mcpp/issues/401) 已实测复现)。
**vendor 只能按对象可达(RPATH),永远不能按目录可达。**

---

## 4. 为什么三层能同时存在:四方各自绿

| 方 | 它的 CI 验什么 | 为什么看不见 |
|---|---|---|
| **mcpp** | 构建/链接/运行时闭包 | **没有任何 e2e 创建过 GL context** |
| **mcpp-index** | 改动了的成员 | main 的 `validate` 里 **`workspace` job 是 `skipped`** —— 只测本次 diff 改了的包 |
| **xim** | 包能装 | 它不构建 C++ 消费方 |
| **宿主驱动** | — | 没人测 |

mcpp-index main 最近一次 `validate` 的 job 列表实测:

```
success  select / lint / mirror-cn-reachable / graphics install: no side effects
skipped  workspace (…)          ← 真正会构建图形包的 job
skipped  timings
```

**所以「main 是绿的」和「图形包装得上」之间没有任何关系。**
`compat.glx-runtime` 自己没改过 —— 变的是 xim 那边。而增量 CI 的定义
就是「只测改了的」,它**结构上不可能**发现一个已发布的包因为**别人**的变化而失效。

这次能露头,纯属偶然:PR #200 改的是 `validate.yml`,按它自己的规则触发了 `__ALL__`
全量扫描 —— 一个和图形毫无关系的 CI 配置 PR。

---

## 5. 三个结构性缺口

- **G1 跨注册表的版本约束没有可满足性检查,而且失败信息会撒谎。**
  一份数据引用另一份数据,没有任何一方在对方变化时重新验证自己;
  验证失败时给出的还是一个误导性的原因。
- **G2 关键知识住在注释里,不住在检查里。**
  四段版本不可范围比较 —— 写清楚了,然后同文件掉进去。
- **G3 没有任何一方对「整条链活着」负责。**
  每一方只验自己改了什么,而这条链是全仓唯一同时跨四方的。

---

## 6. 方案

### 6.1 立刻止血(按层,互不依赖)

| | 动作 | 归属 | 代价 |
|---|---|---|---|
| **L1** | `xim:libglvnd@>=1.7.0.1` → 裸名(与 mesa 同治) | xim-pkgindex | 一行 |
| **L1'** | 不可解析的约束报成不可解析,而不是 "not found" | xlings | 小 |
| **L1''** | lint:`@>=`/`@<=` 不得指向四段版本 | xim-pkgindex | 一条脚本 |
| **L2** | #405 | mcpp | 5 行,RED 已复现 |
| **L3** | 见 6.3 dev 档 | mcpp + 索引数据 | — |

### 6.2 分发模式:先把 mcpp 自己的三档定下来

**构建树 ≠ 可分发物**,这是所有成熟工具的共同前提(CMake `BUILD_RPATH`/`INSTALL_RPATH`
安装时重写甚至 relink;Meson 安装时剥 rpath;libtool relink-on-install;
Bazel 输出树从不假装可分发;Cargo `target/` 同理)。
**开发调试期直接吃 xlings 的 store 是正确且最快的做法** —— 前提是那道边界是显式的。

而 mcpp 已经把这条边界**命名过了**,`src/build/distribution.cppm` 原文:

> **KNOWN LIMIT**: `HostCoupled` … does not strip the toolchain rpath … **Removing that
> rpath is a packaging axis of its own and is not part of this contract.**

所以不是发明架构,是**把它自己点名的那条轴建起来**:

| 档 | libc/deps | 驱动 | mcpp 的不变量 | 现状 |
|---|---|---|---|---|
| **dev**(构建树) | xlings store,绝对路径 | 宿主(经 xlings 桥) | requirement 必须落到**物**,且物的解析路径与 provenance **身份一致** | ❌ `artifacts: (none declared)` |
| **pack**(自包含) | 产物旁 + `$ORIGIN` | **必须来自宿主** | 产物里不得残留 store 绝对路径;产出**宿主要求清单** | ❌ 源码已标 KNOWN LIMIT |
| **static**(musl) | 无 | **不适用** | **拒绝**把驱动类依赖静态化,plan 期失败并说明理由 | ❌ 今天链过去,运行时崩 |

实测:当前图形产物的 RUNPATH 是

```
<xlings-store>/xim-x-glibc/2.44/lib64 : <xlings-store>/xim-x-gcc/16.1.0/lib64
: <xlings-store>/compat-x-glx-runtime/…/lib : $ORIGIN
```

三条指向**构建机 store** 的绝对路径。它不是「依赖 xlings 生态」,是
「依赖这一台机器的这一份 store」。dev 档这样完全正确;**pack 档这样是缺陷。**

### 6.3 dev 档要补的那一格(L3 的解)

让「满足一条需求」落到一个**物**,而不是一个名字:

```
requirement   capability:opengl.glx.driver
  satisfied-by  <file / soname>
  provenance    xim:nvidia-gl-host-link@0.1.2
```

mcpp 用纯构建工具的手段就能抓到本机这次的故障:**物在不在;它解析出来的真实路径,
是否落在声明的那个版本的 store 目录里。** 本机答案是「解析进 0.1.1,而声明是 0.1.2」——
**不需要懂 GL 就能判**。

而这条规则 mcpp **已经对 glibc 严格执行了**(`glibc@2.44` 只解析那一个 payload,
陈旧即错误,绝不回退)。套到 runtime artifact 上是**一致性,不是新机制**。

> **明确不做:能力探针。**
> 我先后提过「provider 在描述符里带探针」和「xlings 暴露机器可读能力查询」,两条都收回:
> dev 档的收益大部分被「物 + 身份」覆盖;pack/static 档探针问的是**另一台机器**
> (autoconf `AC_RUN_IFELSE` 三十年的教训);而在构建过程中执行 provider 提供的
> 任意可执行文件,是一大块不必要的安全与复杂度面。
> 真正只能靠跑才知道的那部分(桥有没有搭上宿主驱动),mcpp 就该报 `NOT_EXERCISED`,
> 那是 `xlings doctor` 的事。

### 6.4 G3 的解:谁来对整条链负责

三条,由近及远:

1. **mcpp-index 增量 CI 要有一条不增量的兜底。**
   现在 cron 已经跑全量 —— 但**它红了不拦任何东西**。至少要让「图形成员全红」
   变成一个会被看见的信号(cron 失败通知 / 一个 badge / 一条 issue 自动开)。
   否则全量跑和不跑没有区别。
2. **上游变化要能触发下游重验。**
   `compat.glx-runtime` 依赖 `xim:graphics`;xim 那边动了,mcpp-index 这边应当重跑
   受影响成员。今天是零。
3. **mcpp 侧加一条真的创建 GL context 的 e2e**,三值报告:
   PASS / FAIL / **NOT_EXERCISED**(无 display 或无驱动)。
   没有三值,这条 e2e 只能在「跳过」和「失败」之间二选一,而跳过会被读成没问题。

---

## 7. 分阶段

| 阶段 | 内容 | 判据 |
|---|---|---|
| **S0** | L1 数据面止血(libglvnd 裸名) | index 全量跑里 8 个图形成员由 FAIL 转 ok |
| **S1** | #405 + `211` | 该 e2e 先 RED 后 GREEN;三平台不回归 |
| **S2** | L1 规则面:四段版本 lint + 不可解析约束的正确诊断 | 故意写一条 `@>=x.y.z.w` 会被 lint 拦下 |
| **S3** | dev 档:requirement→artifact 必须有物;物的路径与 provenance 身份一致 | 本机现状必须报错并指出 vendor 绑定陈旧,而不是 `pass` |
| **S4** | GL context e2e(三值)+ `display` 能力轴 | 有 display 的机器 PASS,无 display 报 NOT_EXERCISED |
| **S5** | pack 档:RPATH 重写为 `$ORIGIN` + 宿主要求清单 | 打出来的图形产物在没有 xlings 的机器上能跑 |
| **S6** | static 档:驱动类依赖不可静态化,plan 期硬拒 | `--target *-musl` 拉进 GL 依赖时红,且说明理由 |

S0/S1 互不依赖,都可以立刻走。S3 需要索引侧先填 artifact(数据,不是代码)。
S5 是图形程序真正的分发出路,也是最大的一块。

---

## 8. 明确不做

- 不在 mcpp 里探测 GPU / Mesa / NVIDIA / WSL / ICD / driver。
- 不在构建过程中执行 provider 提供的探针(见 6.3)。
- 不把 `<subos>/lib` 放进任何环境变量或全局搜索路径(#401)。
- 不动 `compat.glx-runtime` 的 dispatch RPATH —— 它已经是对的。
- 不动 #398 冻结的四条产品边界。

---

## 附:这一轮反复出现的同一条判据

> **「我这一片是绿的」从来不等于「它能用」。**

- Wine 重放通过 ≠ Windows 通过(`Z:` 映射)
- 单测断言路径拼写通过 ≠ 索引可读(它在 Windows 全红时一直绿)
- 缓存第一次通过 ≠ 第二次通过(#405,miss 与 hit 是两条路径)
- **index main 绿 ≠ 图形包装得上**(`workspace` job 根本是 skipped)

四次都是同一个形状:**验证的对象比它声称覆盖的范围窄,而窄在哪里没有被说出来。**
本文提的每一条检查,目的都是把「没验」和「验过了」区分开 —— 三值、lint、
身份一致性,全都是这一件事。
