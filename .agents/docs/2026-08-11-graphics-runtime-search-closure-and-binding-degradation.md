# 图形栈剩下的那一半:链接期看得见、运行期看不见

> 日期:2026-08-11
> 基线:mcpp `main` `1bc6074`(源码版本 `2026.8.11.1`,本机已装 `2026.8.10.3`)、
> xlings `2026.8.11.2`、本机 x86_64-linux-gnu / gcc 16.1.0 / NVIDIA + X11
> 来源:xlings [#532](https://github.com/openxlings/xlings/issues/532)(构建侧标签,mcpp 侧已完成一半)、
> [#537](https://github.com/openxlings/xlings/issues/537)(subos info 低报)、
> [#540](https://github.com/openxlings/xlings/issues/540)(E2b 落地前的接口谈判)、
> [#543](https://github.com/openxlings/xlings/issues/543)(Windows 上 `mcpp build|test` 直接失败)
> 前置阅读(只需结论):`2026-08-10-graphics-closure-and-distribution-tiers-design.md`
>
> 本文所有"已验"结论都在本机跑过,命令与输出在正文里。凡未实测一律标 **未验**。
> file:line 全部核于上述基线。
>
> **实施状态(2026-08-11):全部已实施,版本 `2026.8.11.2`。**
> 实施计划见 `2026-08-11-graphics-runtime-search-closure-implementation-plan.md`。
> **与本文的三处出入,以实施为准:**
> - **§2.6 的"`system` 档剥离"不需要写代码** —— 实测 `--mode system` 早就把 rpath
>   清空并把 `PT_INTERP` 改回 `/lib64/ld-linux`,产物里没有任何 `$MCPP_HOME` 路径。
>   真正的缺口是 §2.6 那条**已有守卫抓不到它**,所以这一项从"实现"变成"补测试"。
> - **§2.6 关于"从 farm 解析到的 NEEDED 升级为 host requirement"没有做**。
>   `HOST-REQUIREMENTS` 存在的理由是记录**产物本身看不出来**的东西(经 dlopen 链
>   到达的驱动);而一个 `NEEDED` 本来就写在产物里,再抄一遍是冗余。
> - **§2.1 说 farm 走 `linkIntent.runtimeSearchDirs`,实施改成了 `BuildPlan` 上的
>   独立字段**。原因见实施计划 §4:那个字段还喂 `runtimeLibraryDirs`,而后者会变成
>   `mcpp run` 的 `LD_LIBRARY_PATH` —— **farm 绝不能进环境变量**,那正是本文
>   §6"不做什么"里禁掉的东西。差点自己踩进去。

---

## 0. 结论先行

这一轮要修的是**两个缺陷**,它们看起来一个属于图形、一个属于 Windows,底下是同一句话:

> **一个决策在一处做了,在另一处没做 —— 而"没做"的那一处不报错。**

| | 缺陷 A(图形) | 缺陷 B(xlings#543) |
|---|---|---|
| **一句话** | subos 在**编译/链接**期是 sysroot,在**运行**期不是任何东西 | subos **没有声明**,被当成 subos **有矛盾** |
| **症状** | `mcpp build` rc=0,`./bin/app` → `libGL.so.1: cannot open shared object file` | `mcpp build\|test` 直接 error 退出,文案在讲 GL 驱动 —— 在一台 Windows 上 |
| **判据行** | `flags.cppm:797`(运行期路径只有载荷目录) | `runtime_binding.cppm:217`(`!info.present` ⇒ 硬失败) |
| **谁掩盖了它** | `elf_runtime.cppm:282` 回落到**宿主**默认目录 ⇒ `validation: pass` | 无 —— 它很响,只是响错了地方 |
| **回归窗口** | 一直如此(不是回归) | **PR #400**,首次随 `2026.8.10.2` 发布 |

两条修法也是同一句话的两面:

> **A:运行期的搜索集合必须与链接期的视图同源(同一个 binding 推导一次)。**
> **B:缺声明必须降级为"未验证",不得升级为"构建失效"。**

本轮**不新增任何用户可见语法**。用户侧的可见变化只有一条:原来链得上、跑不起来的图形程序,现在跑得起来。

---

## 1. 实测基线(先看现场,再谈设计)

### 1.1 复现:零 flag 的 GL 程序

```toml
# mcpp.toml
[package]
name = "glprobe"
version = "0.1.0"
standard = "c++23"

[build]
ldflags = ["-lGL", "-lX11"]
```

```console
$ mcpp build
    Finished dev [unoptimized + debuginfo] in 0.05s          # rc=0 ✅

$ readelf -d target/x86_64-linux-gnu/*/bin/glprobe
 (NEEDED)  [libGL.so.1]
 (NEEDED)  [libX11.so.6]
 (NEEDED)  [libm.so.6] [libgcc_s.so.1] [libc.so.6]
 (RPATH)   [<mcpp-home>/data/xpkgs/xim-x-glibc/2.39/lib64:
            <mcpp-home>/data/xpkgs/xim-x-gcc/16.1.0/lib64]     ← 没有 <subos>/lib

$ ./target/.../bin/glprobe
error while loading shared libraries: libGL.so.1: cannot open shared object file   # rc=127 ❌

$ mcpp run          # rc=1,同样的错误
$ mcpp why runtime
validation: pass (source post_link)                          # ⚠️ 假绿
  - …/bin/glprobe: pass
```

**顺带记一条**:`[build] libraries = ["GL"]` 不是合法键(`warning: unsupported key 'libraries' (ignored)`),
今天唯一的写法是 `ldflags = ["-lGL"]`。这属于用户语法层,**本轮范围外**(见 §6)。

### 1.2 为什么链接期通得过:subos 已经是 sysroot 了

`build.ninja` 头部(原文):

```ninja
cxxflags = -std=c++23 -fmodules -O0 -g --sysroot=<mcpp-home>/subos/default -B<…>/xim-x-binutils/2.42/bin
ldflags  = --sysroot=<mcpp-home>/subos/default \
           -Wl,--dynamic-linker=<…>/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2 \
           -L<…>/xim-x-glibc/2.39/lib64  -Wl,-rpath,<…>/xim-x-glibc/2.39/lib64 \
           -L<…>/xim-x-gcc/16.1.0/lib64  -Wl,-rpath,<…>/xim-x-gcc/16.1.0/lib64 \
           -B<…>/xim-x-binutils/2.42/bin -specs=<…>/mcpp-clean-link.specs -lGL -lX11

build bin/glprobe : cxx_link obj/main.o
  unit_ldflags = -static-libstdc++ -Wl,--disable-new-dtags        ← 标签契约,已生效 ✅
```

`-lGL` 之所以解析得到,是因为 `--sysroot=<subos>` 让 ld 把 `<sysroot>/lib` 当默认库目录。
**mcpp 今天已经把整个 subos 当作自己的根**,只是这个事实没有传到运行期。

| 阶段 | 对 subos 的视图 | 机制 | 状态 |
|---|---|---|---|
| 编译 | 整个 subos | `--sysroot=<subos>`(`linkmodel.cppm:83`) | ✅ |
| 链接 | 整个 subos | `--sysroot=<subos>`(`linkmodel.cppm:109`) | ✅ |
| **运行** | **两个载荷目录** | `flags.cppm:800-803` 只遍历 `plan.toolchain.linkRuntimeDirs` | ❌ |

**这不是"缺一个 flag",是同一个决策在两处独立推导,而其中一处的输入集合小了一整个 farm。**

### 1.3 假绿的机制:闭包模型用错了加载器

`elf_runtime.cppm:262-288` 的 `resolve_needed()` 搜索顺序:

```cpp
for (auto const& raw : requester.runpaths)      append_unique_path(dirs, expand_origin(raw, …));
for (auto const& dir : additionalSearchDirs)    append_unique_path(dirs, dir);
for (auto const& dir : binding.libraryDirs)     append_unique_path(dirs, dir);
for (auto const& dir : host_library_dirs())     append_unique_path(dirs, dir);   // :282
```

`host_library_dirs()`(`:245`)返回 `/lib/x86_64-linux-gnu`、`/usr/lib64`、`/usr/lib`……
本机实测宿主确实有 `/usr/lib/x86_64-linux-gnu/libGL.so.1` ⇒ `libGL.so.1` **在模型里解析成功**,
`resolution.unresolved` 为空,判决落到干净的 `Pass`。

**但产物跑在私有 loader 下**(`PT_INTERP` = 载荷 2.39 的 `ld-linux`),它的内建默认路径里没有宿主目录。

> **闭包模型模拟的是宿主加载器,产物用的是私有加载器。两者在"默认目录"这一项上不同,
> 而这一项恰好是唯一没被 binding 覆盖的输入。**

即使 `unresolved` 非空,后果也只是 `inconclusive`(`elf_runtime.cppm:759-768`),
而 `has_proven_mismatch()`(`runtime_validation.cppm:41-47`)只对 `ProvenMismatch` 失败。
**⇒ 今天没有任何一条路径能让"这个产物加载不了"变成红色。**

这条要和 §2.1 一起修:**只修路径不修判据,等于把新行为建在一个不会响的报警器上面。**

### 1.4 ⚠️ 为什么不能直接用 `$XLINGS_SUBOS_LIB`

这是本轮最硬的一条实测,它直接否掉"加两个 flag 就完事"的写法:

```console
$XLINGS_SUBOS_LIB               = ~/.xlings/subos/current/lib
  └─ libc.so.6 → ~/.xlings/data/xpkgs/xim-x-glibc/2.39/lib/libc.so.6

mcpp 的 RuntimeBinding          = ~/.mcpp/registry/subos/default/lib
  └─ libc.so.6 → ~/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib/libc.so.6
```

**两个 home、两份物理 glibc 载荷、两个不同的 subos。** mcpp 有自己的 registry home,
它解析的 subos 与 shell 的 active subos **不是同一个**,并且没有任何机制保证它们同步。

把环境变量里的 farm 塞进 DT_RPATH,等于给产物挂上一个 mcpp 不管理、不 pin、随时会被
`xlings use` 换掉的第二套 libc / libstdc++ —— 正是 rule B 存在的那句话:
*"one process cannot mix runtime payloads"*。
本机两边版本号恰好都是 2.39,所以今天侥幸能跑;**版本一旦不同就是静默错**。

> **⇒ farm 路径只能从 mcpp 自己已解析的 `RuntimeBinding.subosDir` 推导。环境变量在这里是噪声,不是契约。**

### 1.5 xlings#543:Windows 上直接失败

报告(xlings 2026.8.11.1 + mcpp 2026.8.10.3,Windows 11):

```
error: selected SubOS 'default' cannot provide a RuntimeBinding:
       subos 'C:\Users\…\.mcpp\registry\subos\default' does not describe itself
       (no `subos_info` block), so programs run from here get no environment it
       declares — a GL application will not find its drivers. …
```

三处都错:

1. **它是硬失败。** `runtime_binding.cppm:216-221`,`resolve_runtime_binding` 在
   `!info.present` 时返回 `unexpected`;`prepare.cppm:1292-1294` **无平台条件地**调用它并
   `return std::unexpected(...)`。⇒ Windows 上每一次 `mcpp build` / `mcpp test` 都停在这里。
2. **它在 Windows 上讲 GL 驱动。** 这段文案是给 Linux 图形场景写的,被一个跨平台调用点复用了。
   Windows 既没有 ELF、没有 `PT_INTERP`、没有私有 libc,`validate_runtime_artifact` 自己第一句就是
   *"non-Linux platform; ELF/glibc rules are not applicable"*(`elf_runtime.cppm:629-633`)。
3. **回归窗口已确认。** `git log -S "cannot provide a RuntimeBinding"` 只命中 `0006ce6`(PR #400),
   且 `git merge-base --is-ancestor 54d29fc 0006ce6` 为真 ⇒ #400 在 `2026.8.8.4` 之后落地,
   **首次随 `2026.8.10.2` 发布**。与报告"降级到 2026.8.8.4 就能跑"完全吻合。

**同一位置还埋着第二颗雷**(尚未爆,爆了就是全平台):

```cpp
// runtime_binding.cppm:222
if (info.schema != mcpp::xlings::subos::kSupportedSchema) { /* hard error */ }
```

而它的**上游读取器**明写着相反的规矩(`subos_info.cppm:45-49`):

> *"A HIGHER one on disk is still read — we take the fields we know and say so.
> Refusing outright would let a newer xlings break an older mcpp, which is the failure
> shape the index-floor incident already paid for once: publishing data must not
> invalidate the program that reads it."*

**读取器按"上限"设计,消费者按"相等"检查。** xlings 写出 schema 2 的那一天,
所有平台上的所有 mcpp 构建同时停摆。这是 index-floor 事故的第二次转生,必须一起修。

---

## 2. 缺陷 A:运行期搜索闭包

### 2.1 A1 — farm 进 `runtimeSearchDirs`,从 binding 推导,排在末位

**做什么**:`RuntimeBinding` 增加一个字段 `searchDirs`(farm 目录,通常是 `<subosDir>/lib`),
由 `resolve_runtime_binding` 在**已有的那次探测**里顺手产出 —— 就是
`runtime_binding.cppm:253-284` 里已经在走的 `{subosDir/"lib64", subosDir/"lib"}` 循环。
**不新增一处布局知识**:今天那个循环是为了找 `libc.so.6`,顺路把命中的目录本身记下来即可。

然后经**已有的专用通道**下发:

```
RuntimeBinding.searchDirs
  → BuildPlan.linkIntent.runtimeSearchDirs           (plan.cppm,与包描述符的同一个字段汇合)
  → render_link_intent_flags → "-Wl,-rpath,<dir>"    (flags.cppm:304-310)
```

选这个通道不是随意的,`flags.cppm:804-806` 的注释已经把语义写死了:

> *"runtimeSearchDirs contributes RUNPATH only; it must never become a link-time `-L` path."*

**正是我们要的**:链接期的解析已经由 `--sysroot` 覆盖(§1.2 实测),这里只补运行期。
**不发 `-L`** 还额外省下链接行长度(128KiB 上限,真实 workspace 已用掉 43%)。

**排序不变式(硬要求)**:

> **farm 目录必须排在所有载荷目录之后。**

理由是**可变性**,不是风格:

| 目录 | 可变性 | 谁在写 |
|---|---|---|
| `<store>/xim-x-glibc/2.39/lib64` | **不可变** | 装一次就不再动 |
| `<subos>/lib` | **可变** | 每次 `xlings install` / 重解析都重写符号链接 |

载荷目录在前 ⇒ `libc.so.6` / `libm.so.6` / `libstdc++.so.6` 永远从被 pin 的载荷解析,
farm **只补没人提供的那些**(libGL / libX11 / libEGL / libwayland / libvulkan…)。
farm 若在前,一次 `xlings install` 就能在事后悄悄换掉一个**已经构建好**的产物的 libc。

**这一条要被断言,不能只被写下来**(§5.1)。

### 2.2 A2 — 与 2026-08-10 那份设计的分歧:不是推翻,是"不继承 ≠ 不使用"

上一份设计明确写过两条,必须正面回应:

> §1.3.1 表格:**`-rpath <subos>/lib`(路径)| mcpp 必须不继承**
> §不做什么:**不把 `<subos>/lib` 放进任何环境变量或全局搜索路径。……vendor 只能按对象可达(RPATH)。**

逐条对齐:

| 上一份的原话 | 本轮的位置 |
|---|---|
| "不把 `<subos>/lib` 放进**任何环境变量或全局搜索路径**" | **完全保留**。本轮不碰 `LD_LIBRARY_PATH`,不碰任何全局路径。§1.4 又给它加了一条独立证据 |
| "**vendor 只能按对象可达(RPATH)**" | **本轮正是在执行这一条** —— 把 farm 放进产物**自己的 DT_RPATH**,per-object,不外溢 |
| "`-rpath <subos>/lib` mcpp 必须**不继承**" | **保留,并加强**:mcpp 不接受**别人注入**的那条路径(§2.5 声明式退出),而是发**自己推导**的那条 |
| 理由:"一条**未经审查的**、带 libc 的目录" | 关键词是**未经审查**。而 `<subos>/lib` 恰恰是 mcpp **解析自己 libc 时穿过的那个目录**(`runtime_binding.cppm:257-262` 就是从 `<subos>/lib/libc.so.6` 跟到载荷的)。它不是第二个 libc,是**同一个 libc 的第二条路径**;再叠上排序不变式,连"第二条路径"都不会被走到 |

还有一条更硬的、上一份没有的证据:

> **链接期已经在信任 farm 了。** `--sysroot=<subos>` 意味着 `-lGL` 就是从 `<subos>/lib` 解析出来的。
> 运行期拒绝同一个目录,不构成任何安全边界 —— 只构成"链得上、跑不了"。

**⇒ 分歧点只有一处、且是措辞层面的:"不继承"被读成了"不使用"。本轮把它读回"不继承"。**

### 2.3 A3 — 闭包判据必须能变红

两处改动,缺一不可:

**(a) 默认目录按 binding 分档。** `resolve_needed`(`elf_runtime.cppm:282`)的
`host_library_dirs()` 回落**只在 binding 就是宿主运行时**(非 hermetic、无私有 `PT_INTERP`)时才加入。
hermetic binding 的默认集合 = 产物自身 RPATH/RUNPATH + binding 载荷目录 + binding farm。
今天这个回落把宿主的 `libGL.so.1` 冒充成答案,是 §1.1 那个 `pass` 的直接成因。

**(b) "解析不到"要有自己的名字。** 今天 hermetic 产物的一个无法解析的 `NEEDED`
是**可证的失败**(私有 loader 一定打不开),把它归到 `Inconclusive` 是**把可证的事说成没查过**。
`RuntimeVerdict::Status` 增加 `UnresolvableNeeded`,并让 `runtime_validation` 的失败门收下它。

> 三值的用途是区分"没查"和"查了没事"。**"查了,而且证明它跑不起来"是第四种,不该被前两种吸收。**

分档保留了 `gcc@system` 一类非 hermetic binding 的正确行为:那里宿主目录**确实**是加载器的默认值。

### 2.4 A4 — 两条护栏

| 护栏 | 规则 | 为什么 |
|---|---|---|
| **交叉目标** | 目标三元组与 binding 的 platform/arch 不一致时,**不发 farm** | farm 是宿主 subos 的产物。aarch64-musl / mingw / wasm 目标拿到一条 x86_64-glibc 的路径,轻则无效重则误解析 |
| **非 ELF** | 与标签契约同构:Mach-O / PE **无此概念**,不进任何分支 | `loader_contract` 已经用 `NotApplicable` 立过这个形状,照抄 |

### 2.5 A5 — 对 xlings E2b 的声明式退出:mcpp 主动关掉,不是被动容忍

xlings#540 的 E2b 会让 `ld` 包装器追加 `-rpath "$XLINGS_SUBOS_LIB" --disable-new-dtags`。
它落地之后,mcpp 链出来的产物会同时拿到**两条** farm 路径:mcpp 推导的那条,和包装器注入的那条 —— 而后者
在 §1.4 已被实测证明**可能指向另一个 home**。

**mcpp 在它落地之前就把退出声明出来**:mcpp 驱动的每一次链接,环境里带上
`XLINGS_SUBOS_LD_PATHS=0`(键名以 xlings#540 最终敲定的为准)。

- 今天设它 = 无操作(变量还没人读),**零风险**;
- E2b 落地当天自动生效,mcpp 的 DT_RPATH **仍然只包含 mcpp 决定的内容**;
- 与标签那一半的处理**同构**:mcpp 覆盖大量不经过那个 `ld` 的链路(交叉 musl / mingw、
  `-fuse-ld=lld`、`gcc@system` / `msvc@system`、host 工具子构建),**必须处处一个答案**。

> **这一条是本设计对 xlings 唯一的跨仓依赖,而且是"先声明后生效"式的 —— 不阻塞本轮实施。**
> 若 xlings 最终选择了别的退出形式,只改这一个常量。

### 2.6 A6 — `mcpp pack` 三档行为

`pack` 的依赖解析走的是同一个闭包(`elf_runtime`),所以 A1 落地后
**pack 才第一次有能力看见 libGL / libX11** —— 今天它连这些依赖都找不到,更谈不上打包或申报。

| 档位 | farm 路径 | 从 farm 解析到的 `NEEDED` |
|---|---|---|
| `vendored` / `self-contained` | **天然消失**:`set_search_path`(`pack.cppm:408-426`)整体重写为 `$ORIGIN/../lib` | 随 bundle 走(vendor 驱动除外,见下) |
| `static` | 不适用 | — |
| **`system`** | **剥掉**(本轮新增) | **升级为 `HOST-REQUIREMENTS` 条目**(`host_requirements.cppm` 已有机制) |

`system` 档的语义是"依赖由目标机器提供",而一条构建机绝对路径**不是**目标机器提供的东西 ——
它是一条会跟着 tarball 走、到别的机器上指向不存在或不同内容的目录的路径。剥掉之后信息不丢:
它变成 `HOST-REQUIREMENTS` 里可读的一行。

#### ⚠️ 已有的守卫抓不到它 —— 这一条是核验时发现的,必须一并修

`tests/e2e/215_pack_has_no_build_machine_paths.sh` 正是为"产物不得残留构建机路径"存在的,但它的
机器本地前缀只取到 store:

```bash
STORE="$(cd "$MCPP_HOME/registry/data/xpkgs" && pwd)"     # 215:113
… if [[ "$paths" == *"$STORE"* ]] ; then FAIL …           # 215:124
```

而 farm 在 **`$MCPP_HOME/registry/subos/default/lib`** —— **不在 `data/xpkgs` 之下**。

> **⇒ A1 落地后,一条泄漏的 farm 路径会让 215 保持绿色。**

两处补齐,与 A6 同一个 PR:

1. **前缀扩到整个 mcpp home**(至少加 `registry/subos`)。判据应当是"这台机器的私有状态",
   而 store 只是它的一个子集 —— 今天这条断言的**覆盖面窄于它自己的标题**。
2. **补 `--mode system` 的用例**。215 只跑默认档(`vendored`);`system` 档从未被任何用例
   检查过路径泄漏,而它恰恰是唯一不重写 rpath 的那一档。

**vendor 驱动无论哪一档都不进 bundle** —— 它与运行中的内核模块版本锁定,且专有栈禁止再分发。
这是 `host_requirements` 模块开篇就写明的事,本轮不动。

---

## 3. 缺陷 B:缺声明必须降级,不得使构建失效(xlings#543)

### 3.1 B1 — `!info.present` ⇒ 降级

`resolve_runtime_binding` 返回一个**总是成功**的 binding,携带 `present` 与 `note`:

| 情况 | 今天 | 改为 |
|---|---|---|
| 无 `.xlings.json` / 无 `subos_info` 块 | **hard error**(全平台) | `present=false` + `note`,**构建继续**;rule A/B 报 `Inconclusive` 并说明是因为**没有声明** |
| `runtime` 字段为空 | hard error | 同上 |
| 用户 `[xlings] subos = "x"` 而 `x` 不存在 | hard error | **保留 hard error** |

最后一行是这条规则的边界,值得单独说清:

> **矛盾要报错,缺席要降级。**
> "你点名要 subos `x`,而 `x` 不在" 是矛盾 —— 用户的输入无法被满足。
> "subos 存在但没有自我描述" 是缺席 —— 有一部分事实不可知,**其余全部照常可用**。
> 今天这两者被同一条 `return unexpected` 处理。

这与 `subos_info.cppm` 自己的 *"A NOTE ON SILENCE"* 一节完全一致:那一节要求每一次降级都填 `note`
并由调用方打印 —— 它从设计上就假定了**降级是存在的**。今天没有降级路径,只有失败路径。

**Windows 上的具体后果**:binding 退化为 `platform="windows"` + `present=false`,
没有 loader、没有 libc、没有 farm;`validate_runtime_artifact` 走它第一句已有的
*"ELF/glibc rules are not applicable"*;`mcpp build` / `mcpp test` 正常完成。

### 3.2 B2 — schema 检查改成上限,与读取器对齐

```cpp
- if (info.schema != kSupportedSchema)  → hard error
+ 高于 kSupportedSchema  → 读懂的字段照用,note 说明忽略了什么(读取器已经这么做了)
+ 低于/等于               → 照用
+ 结构性无法使用         → present=false + note(走 B1 的降级)
```

**判据一句话**:**发布数据不得使读它的程序失效。** 这是 index-floor 事故的原话,
`subos_info.cppm:45-49` 也已经把它写进注释 —— 只是消费者没照做。

### 3.3 B3 — 文案按调用方分层

那句 *"a GL application will not find its drivers"* 是**图形场景**的解释,不是 binding 层的事实。
binding 层只说事实(*"subos '<x>' 没有 `subos_info` 块;运行期环境声明不可用"*),
GL 那一句留给真正与图形相关的调用点。

**判据**:一条诊断不该提到调用方**可能根本不存在**的概念。在 Windows 上讲 GL 驱动,
使读者去找一个不存在的问题 —— 与 xlings#537 里 `subos info` 低报 EGL 是同一种伤害。

### 3.4 B4 — `The system cannot find the path specified.`

报告里这一行**在 2026.8.8.4 上同样出现**(只是当时不致命)⇒ **它是独立的、更早的缺陷,不是本次回归**。
该文案是 `cmd.exe` 的,说明有一处**经 shell 调用了一个在 Windows 上不存在的路径**。

**本轮只做到"定位并单开 issue",不盲改**:没有 Windows 现场,任何修法都是猜。
需要报告者补一条 `mcpp build --verbose`(或设 `MCPP_LOG=debug`)输出以定位是哪次调用。
**B1 落地后这条会从"致命+噪声"降为"纯噪声"**,不再阻塞任何人。

---

## 4. 可观测性:让"为什么我的 GL 程序能跑"可回答

三处,都是既有载体的补齐,不新增机制:

1. **`resolution.json`** —— 运行期搜索目录逐条记 provenance:
   `payload`(不可变载荷)/ `subos_farm`(可变符号链接农场)/ `package`(描述符声明)。
   **顺序即语义**,所以记录必须保序。
2. **`mcpp why runtime`** —— 打印 `NEEDED → 从哪个目录解析到`,并标出该目录的 provenance。
   §1.1 里那个 `pass` 之所以骗过人,正是因为它没说"从哪解析到的"。
3. **`mcpp doctor`** —— binding 处于 `present=false` 时,把 `note` 作为一条 **info**(不是 warn)呈现,
   并说明后果范围(rule A/B 不可评估;运行期环境声明不可用),而不是让它只在构建输出里一闪而过。

---

## 5. 测试策略:怎么保证它不空转

这个仓库反复付学费的形状是**测试与被测对象共享同一个错误假设**。逐条设防:

### 5.1 必须存在的四条断言

| # | 断言 | 为什么这一条不能省 |
|---|---|---|
| **T1** | 产物 `DT_RPATH` 的**最后一项**是 binding 的 farm 目录 | 直接钉死 §2.1 的排序不变式。**判据是生成物**,不是源码 |
| **T2** | `libc.so.6` 解析到**载荷目录**,不是 farm | 排序若被改反,T1 可能仍过而 T2 必红,并说明原因 |
| **T3** | 一个 `NEEDED` 只存在于 farm 的产物 **能真正运行**(rc=0) | **唯一能戳破 §1.3 假绿的断言**。宿主也有 `libGL.so.1`,所以只有"真的跑"能区分两个加载器 |
| **T4** | 一个 `NEEDED` 谁都提供不了的产物,构建**变红**并指名是哪个 so | 防止 A3 只改了状态枚举而没接到失败门上 |
| **T5** | `pack --mode system` 的产物里**不含**任何 `$MCPP_HOME` 下的路径 | 见 §2.6:现有的 215 前缀只取到 `data/xpkgs`,farm 从它下面漏过去 |
| **T6** | Windows job 上,一个 `subos_info` 缺失的 fixture 能 `mcpp build` 成功 | 缺陷 B 的直接判据,且**不需要任何图形能力** |

### 5.2 ⚠️ 三个已知会让测试空转的坑

1. **不要断言 `readelf` 里"有 farm 这条路径"就收工。** 本机宿主同时有 `libGL.so.1`,
   路径正确但顺序错误、或私有 loader 打不开,`readelf` 都看不出来。**T3 必须真的 exec。**
2. **不要用 `# requires: gcc` 之类在 Windows/macOS 不授予的能力**,否则整条 case 静默跳过 ——
   #272 / #412 已经各踩一次。缺陷 B 的用例**必须在 Windows job 里真的跑到**,
   而且它**不需要**任何图形能力:一个空 `subos_info` 的 fixture 加一次 `mcpp build` 就够。
3. **不要依赖 CI 的 subos 里装了 GL。** T3 需要"一个只存在于 farm 的库",
   **不必是 libGL** —— 从 binding 的 farm 与载荷目录做一次差集,取任意一个即可。
   差集为空时用例应当 **skip 并打印原因**,不得静默通过。

### 5.3 对照组

沿用 `tests/e2e/214_executable_carries_dt_rpath.sh` 已经证明有效的手法:**用共享库当对照**。共享库不带 `--disable-new-dtags`,
它的标签就是链接器默认值 —— 默认值哪天变了,是**库那条**先红并说明原因,
而不是可执行那条为了别的理由静默地继续通过。farm 这一轮同理:
farm 对可执行与共享库都要发,但**排序断言只在可执行上做**,库那条负责钉住默认行为。

---

## 6. 不做什么

- **不新增 `[build] libraries` / capability 语法。** 用户今天用 `ldflags = ["-lGL"]` 能表达,
  本轮的目标是让这条已经能写的东西**跑得起来**。语法层是独立议题,值得单独一份设计
  (它要和 mcpp-index 描述符的 `linkIntent` 对齐,范围比本轮大)。
- **不读 `$XLINGS_SUBOS_LIB`。** §1.4 已实测它在本机指向另一个 home。
- **不碰 `LD_LIBRARY_PATH` 或任何全局搜索路径。** 沿用上一轮的结论,§1.4 只是又加了一条证据。
- **不在 mcpp 里出现任何库名。** 不出现 `libGL`、不出现 `LIBGL_DRIVERS_PATH`、不判断"这是不是图形程序"。
  farm 是一个目录,mcpp 只知道它的**可变性**和**顺序**。图形只是第一个被它治好的病人。
- **不改标签契约。** `loader_contract` 已经是对的(§1.2 实测 `unit_ldflags` 生效),本轮一行不动。
- **不替 xlings 修 #537。** 那是 `subos info` 面板的判定语义(它以消费者标签为条件而没表达这个条件),
  归 xlings。mcpp 侧的对应事实已由 rule E 记录在 `resolution.json` 里。
- **不盲改 B4 的 Windows 噪声。** 没有现场就没有判据。

---

## 7. 实施顺序与风险

| 步 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **1** | **B1 + B2 + B3**(binding 降级 / schema 上限 / 文案分层) | 无 | **低**。纯粹放松约束,且**立刻解掉 Windows 用户的阻塞**。应当先发 |
| **2** | **A3**(闭包分档 + `UnresolvableNeeded`) | 无 | 中。**必须先于 A1** —— 否则 A1 落地后没有任何断言能证明它对,§1.1 的 `pass` 会继续 `pass` |
| **3** | **A1 + A4**(farm 入 `runtimeSearchDirs`、末位、护栏) | 步 2 | 中。**改动 `resolution.json` 与产物 RPATH ⇒ 指纹与既有校验缓存需要一并考虑** |
| **4** | **A6**(pack `system` 档剥离 + 升 host requirement + 扩 `215` 的前缀与档位) | 步 3 | 低 |
| **5** | **A5**(声明 `XLINGS_SUBOS_LD_PATHS=0`) | 键名待 xlings#540 敲定 | 低。今天是无操作 |
| **6** | 观测(§4)+ 测试(§5) | 步 3 | 低 |

**顺序上唯一不可交换的是 2 在 3 之前。** 理由在 §1.3:先加路径再补判据,等于在一个不会响的报警器上面加功能 —— 而这正是这一片区域已经付过两次学费的形状。

**步 1 可以独立成一个 PR 先发**:它与图形无关,解的是一个正在阻塞 Windows 用户的回归。

### 跨仓状态

| 事项 | 归属 | 本轮是否阻塞 |
|---|---|---|
| E2b 的声明式退出键名 | xlings#540 | **否**(A5 先声明,后生效) |
| `subos_info` 增 `library_dirs` 声明(E2a) | xlings#540 | **否**。A1 从 binding 推导,已足够;E2a 落地后把推导换成读声明,是**同一处**的替换,不是第二处推导 |
| `subos info` EGL 低报 | xlings#537 | 否,归 xlings |
| Windows `subos_info` 块是否该写 | xlings | 否 —— B1 之后写不写都不再影响 mcpp 能否构建 |
