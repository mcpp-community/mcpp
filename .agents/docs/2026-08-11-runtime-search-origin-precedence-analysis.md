# `$ORIGIN` 被 SubOS farm 遮蔽 —— helloegui 运行期 undefined symbol 分析与修复方案

日期:2026-08-11
起因:`mcpp run`(helloegui,依赖 imgui 0.0.6)链接成功、运行即死
涉及版本:2026.8.11.2(PR #413 首次把 SubOS 库视图写进 DT_RPATH)
状态:**分析完成,方案已定,未实施**
本批 PR 范围:**A + B + C2 + C1**(决策记录见 §6.1)

---

## 0. 一句话

PR #413 把 SubOS farm(`<mcpp home>/registry/subos/default/lib`,一个 300 条目的
平铺符号链接视图)加进了产物的 DT_RPATH,但它落在 **`$ORIGIN` 之前**;于是产物
运行期加载的 `libX11.so.6` 不是链接时的那一个,而是 farm 里 xim 上游的另一份构建。
两份 libX11 不可互换 —— mcpp 自建的那份因为 `-static-libstdc++` 把整个 libstdc++
烙了进去并**对外导出**,链接期正是它满足了可执行文件的 `std::runtime_error::what()`。

模块自己写着「FARM LAST」这条不变量(`src/build/plan.cppm:660`),而
`$ORIGIN` 由另一个生产者在另一条通道上发出,两者从未在同一个排序里相遇。

---

## 1. 现象

```
$ mcpp run
    Finished dev [unoptimized + debuginfo] in 3.26s
     Running `target/x86_64-linux-gnu/eb46e2850893f013/bin/helloegui`

.../bin/helloegui: symbol lookup error: .../bin/helloegui:
    undefined symbol: _ZNKSt13runtime_error4whatEv
```

同时刷出 13 条 runtime closure 警告(`rule B inconclusive … has no loader path /
has no library directory`)。**这两件事无关**,见 §5。

复现:稳定,100%。第二次 `mcpp build`(指纹变为 `4ab95c7547486246`)警告消失,
**崩溃依旧** —— 崩溃与首次运行无关。

---

## 2. 根因链(每一环都有实测)

### 环 1 — 产物的 DT_RPATH 里 `$ORIGIN` 排在 farm 之后

```
$ readelf -d bin/helloegui | grep RPATH
 RPATH: [ …/xim-x-glibc/2.44/lib64
        : …/xim-x-gcc/16.1.0/lib64
        : …/compat-x-glx-runtime/…/glx_runtime/lib
        : /home/speak/.mcpp/registry/subos/default/lib     ← farm
        : $ORIGIN ]                                         ← 载荷目录,最后
```

**两个生产者,一条 ninja 命令行,没有共同的排序:**

| 条目 | 生产者 | 落入 |
|---|---|---|
| glibc / gcc / glx_runtime | `flags.cppm` runtime_dirs | 全局 `$ldflags` |
| **farm** | `flags.cppm:508-526` → `f.ld`(`:996-999`) | 全局 `$ldflags`(末尾) |
| **`$ORIGIN`** | `plan.cppm:450-467` `shared_library_link_flags` | per-unit `$unit_ldflags` |

链接规则(`ninja_backend.cppm:867-872`):

```
command = $cxx $in -o $out $ldflags $unit_ldflags
```

`$ldflags` 在前 ⇒ farm 在前 ⇒ **`$ORIGIN` 永远最后**。

`flags.cppm:509` 的注释写的是「appended after everything else so it is LAST in
the artifact's DT_RPATH」—— 它只在 `$ldflags` 内部为真,而 `unit_ldflags` 还在后面。

### 环 2 — 同一个 SONAME 在两个目录里都存在

「同名」指的是 **SONAME,不是包名**。本工程里同时存在两个物理文件:

| | mcpp compat 包 | xim 上游包 |
|---|---|---|
| 包身份 | `compat.x11 v1.8.13`(源码包) | `xim:libX11@1.8.10` |
| 谁产出 | **mcpp 自己从源码编译**(`obj/compat_x11/src/*.o`) | xlings 装的预编译产物 |
| 落在哪 | `target/…/bin/libX11.so`(= `$ORIGIN`) | `…/xpkgs/xim-x-libX11/1.8.10/lib/`,经 farm 符号链接暴露为 `<subos>/lib/libX11.so.6` |
| **SONAME** | **`libX11.so.6`** | **`libX11.so.6`** |

上游版本都不同(1.8.13 vs 1.8.10),SONAME 却相同 —— 而**加载器只按 SONAME 找**。
farm 是 `xlings install` 每次都会重写的平铺视图,凡是 mcpp 有 compat 源码包、
xim 又有同一个库的预编译包,就会撞上。这不是边缘情况,是常态。

**两条通道,两套顺序,从未被约束成一致:**

| | 构建期通道 | 运行期通道 |
|---|---|---|
| xim 包 | `--sysroot=<subos>` ⇒ `<subos>/lib` 成为链接器默认目录 | farm 进 DT_RPATH(#413 新加) |
| mcpp compat 包 | `-Lbin` | `$ORIGIN` |

- `-L` 序:`-L<glibc>` `-L<gcc>` **`-Lbin`** … sysroot 默认目录在最后
  ⇒ `-lX11` 选中 **`bin/libX11.so`**
- RPATH 序:`glibc : gcc : glx : <subos>/lib : $ORIGIN`
  ⇒ 运行期选中 **farm 里的 xim 那份**

即:**mcpp 拿 A 链接,却让产物去加载 B。**

链接期用了哪一份不必推理,**产物自己证明了**:exe 里
`_ZNKSt13runtime_error4whatEv` 是动态未定义且没有 `NEEDED libstdc++.so.6`,
而只有 `bin/libX11.so` 导出该符号(见环 3)。若链接期用的是 farm 那份,该符号
要么链接期报错,要么从 `libstdc++.a` 拉入 —— 两种都产生不出现在这个产物。

`LD_DEBUG=libs` 实测(前 4 个 rpath 目录逐个 miss,farm 命中):

```
find library=libX11.so.6 [0]; searching
  trying file=…/xim-x-glibc/2.44/lib64/libX11.so.6            ✗
  trying file=…/xim-x-gcc/16.1.0/lib64/libX11.so.6            ✗
  trying file=…/glx_runtime/lib/libX11.so.6                   ✗
  trying file=…/registry/subos/default/lib/libX11.so.6        ✓  ← 命中 farm
```

`$ORIGIN` 根本没被走到。

### 环 3 — 两份 libX11 不可互换

| | `bin/libX11.so`(mcpp 自建) | `xim-x-libX11/1.8.10`(farm) |
|---|---|---|
| 导出 std 符号数 | **2931**(777 T + 2777 W) | **0** |
| `_ZNKSt13runtime_error4whatEv` | 导出 | 无 |
| 符号版本 | 无(`GLIBCXX_*` verdef 不存在) | — |

mcpp 自建的那份为什么会导出整个标准库:

1. `flags.cppm:86-90` 把 `SharedLibrary` 映射为 `dist::Role::Distributable`
   (`distribution.cppm:47-51`:"Binary / SharedLibrary — leaves this machine")
2. `distribution.cppm:330-338`:ELF + libstdc++ + SelfContained ⇒ `-static-libstdc++`
3. **每个链接单元都带 `obj/std.o`**(`import std` 的模块对象)。实测
   `bin/libXau.so` 的输入是 8 个 `.o`(纯 C)**加一个 `obj/std.o`**,
   于是 `libstdc++.a` 被整个拖进来 —— libXau 从应有的 ~20KB 变成 **9.5MB**

### 环 4 — 可执行文件的 `-static-libstdc++` 因此落空

链接行(`build.ninja` 第 1957 行的 unit_ldflags):

```
… -Lbin -Wl,-rpath,'$ORIGIN' -lX11 … -lXext -static-libstdc++ -Wl,--disable-new-dtags
```

`-lX11` 在驱动追加的 `-lstdc++`(此处即 `libstdc++.a`)**之前**。ld 处理到
`-lX11` 时该符号已被这个**共享库**满足 ⇒ 归档成员从不被拉入 ⇒ 引用留在动态未定义:

```
$ nm -D --undefined-only bin/helloegui | grep runtime_error
 U _ZNKSt13runtime_error4whatEv
$ readelf -d bin/helloegui | grep NEEDED
 … libX11.so.6 … libm.so.6 libgcc_s.so.1 libc.so.6      ← 没有 libstdc++.so.6
```

即:**helloegui 的 C++ 运行时事实上来自 `libX11.so`**。它声称的
self-contained 是假的,而这份「假」在环 1 把 libX11 换成上游那份的瞬间变成硬崩溃。

### 因果验证(A/B,不是推理)

| 实验 | 结果 |
|---|---|
| 原样运行 | `symbol lookup error` |
| `LD_PRELOAD=bin/libX11.so.6` | **正常启动,GUI 窗口出现**(挂起到被 kill) |
| 副本 patchelf,载荷目录提到 RPATH 首位 | **正常运行 8s 被 timeout 杀掉(exit 143)** |

第三条是决定性的:**只调整 RPATH 顺序,不改任何代码,崩溃消失。**

---

## 3. 为什么测试没拦住 —— 一个被实测结果反向驯化的断言

PR #413 新增了 `tests/e2e/219_runtime_search_farm_is_last.sh`,标题就叫
"farm is last"。它绿着,而真实产物里 farm 不是最后一位。

`tests/e2e/219…sh:151-171`:

```bash
# The farm must be the last ABSOLUTE entry — not literally the last entry.
#
# `$ORIGIN`-relative entries are a different kind: they address the artifact's
# own directory, not this machine, so they travel with it and their position
# says nothing about which machine-local directory wins. …
# (Measured on a real GLFW app, whose DT_RPATH ends `… : <subos>/lib : $ORIGIN`.)
RPATH_LAST_ABS="$(… [x for x in DT_RPATH.split(':') if x.startswith('/')] … [-1])"
```

作者**实测到了本文分析的这个真实顺序**,判定它是可接受的,于是把断言从
「字面最后」放宽到「最后一个绝对路径条目」。

那条理由恰好反了:决定胜负的**正是**「同一个 SONAME 在 `$ORIGIN` 和 farm 里都有」,
而这在 mcpp 生态里是常态而非例外(见环 2)。

**准确地说,这条断言不是「把坏顺序钉成预期」,而是对它结构性失明** —— 因为
`$ORIGIN` 不以 `/` 开头,被那行 python 过滤掉了:

| | 绝对路径条目 | 最后一项 | 判定 |
|---|---|---|---|
| 未修 `… : <subos>/lib : $ORIGIN` | `[glibc, gcc, glx, <subos>/lib]` | farm | ✓ 通过 |
| 修好 `… : $ORIGIN : <subos>/lib` | `[glibc, gcc, glx, <subos>/lib]` | farm | ✓ 通过 |

两者**完全一样**。所以它在坏产物和好产物上给出同一个结论,而它的名字叫
"farm is last" —— 它在一个 farm 并非最后的二进制上报告「farm 最后」。
这比「会在修复后变红」更糟:修复根本不会惊动它。

> 教训(与既有记录一致):断言被实测结果驯化时,要先证明「实测到的形态是对的」,
> 而不是把它当成基准。这条不变量的正确判据是**行为**(同名库解析到谁),
> 不是**形状**(哪一项排在末尾)—— 形状断言在这里天生不够,这正是 §4-B 必须补
> 一条行为不变量的原因。

模型层面同样有缺口:`runtime_search.cppm` 的 `rank()`(`:82-92`)只有
Payload / Package / SubosFarm / HostDefault 四档,**`$ORIGIN` 不在闭包里**。
`plan.cppm:660` 声称「`search::ordered` is what enforces it」,但它管不到
一个由别的通道发出的条目 —— 这个排序模型对最关键的目录是装饰性的。

---

## 4. 修复方案

### A(必须,修崩溃)— farm 移到整条链接行的尾部

**改动**

1. `src/build/flags.cppm`:`CompileFlags` 新增 `std::string ldFarmTail;`
   `farm_ld` 不再拼进 `f.ld`(`:996-999`),改为 `f.ldFarmTail = farm_ld;`
2. `src/build/ninja_backend.cppm:1400-1418`:组装 `unit_ldflags` 时,
   在 `flags.ldStdlibFor(role)` **之后**、`lu.loaderTagFlag` **之前**追加
   `flags.ldFarmTail`;`StaticLibrary` 跳过(与 `ldStdlibFor` 对 Intermediate
   返回空一致)。
   - `loaderTagFlag` 必须保持字面最后 —— 加载器标签由 ld 看到的最后一个
     `--enable/--disable-new-dtags` 决定(该处注释已写明)

**结果**

```
RPATH: glibc/lib64 : gcc/lib64 : glx_runtime/lib : $ORIGIN : <subos>/lib
```

载荷目录仍在最前(libc/libstdc++ 来自钉住的载荷),`$ORIGIN` 次之(产物链接时
看见的正是这些同级库),farm 真正兜底。

**否决的替代**

- *把 `$ORIGIN` 提到全局 ldflags*:`-Lbin` 与 `-Wl,-rpath,'$ORIGIN'` 是成对的
  per-unit 事实(只有消费共享库的单元才需要),提成全局会给每个产物无条件加一条
- *调换 ninja 规则里 `$ldflags` / `$unit_ldflags` 的次序*:会同时移动所有 `-L`
  搜索序与 `-specs`,影响面远超本问题

**风险**:低。`verify_hermetic_link`(`hermetic.cppm:103`)检查的是 `flags.ld`
解析出的库路径是否落在允许根内;farm 路径位于 `tc.sysroot`(= `<subos>`)之下,
本就在允许集合里,把它从被检字符串中移走不改变判定。

### A+(单独 PR,紧随本批)— 让闭包模型真正拥有这个顺序

`runtime_search.cppm` 增加 `Origin::Artifact`,`rank()` 置于 `Package` 与
`SubosFarm` 之间;`runtime_search_closure`(`plan.cppm:667`)把产物输出目录
(记为 `$ORIGIN`)纳入闭包。收益:

- `resolution.json` 记录的闭包与 DT_RPATH 变得**逐项可比**(今天记录里没有
  `$ORIGIN`,e2e 219 的「记录 vs 产物」比对因此天生有个缺口)
- 「FARM LAST」不再靠两个生产者各自自觉

代价:`is_machine_local()` 需为 Artifact 定义语义(`$ORIGIN` 随产物走 ⇒ 非
machine-local)。

**不涉及 `pack`(已核)**:`is_machine_local` 全仓只有一个生产消费方
(`prepare.cppm:6417` → 写进 `resolution.json` → `doctor.cppm:703` 打
`[machine-local]` 标签);`src/pack/pack.cppm` 不 import `mcpp.platform.runtime_search`,
它自己用 patchelf 把所有 RPATH 重写成 `$ORIGIN/../lib`(`pack.cppm:745-779`)。

A+ 有两个档位,建议只做 (i):

- **(i) 轻**:产物输出目录以 `Origin::Artifact` 进入闭包**记录**。收益是记录与
  DT_RPATH 变得逐项可比,测试可以硬比对。改动 = enum + 3 处 switch +
  `plan.cppm` 加一条 + doctor 显示。
- **(ii) 重**:让闭包成为**唯一**的 rpath 生产者,即把 `$ORIGIN` 的发出也从
  `shared_library_link_flags` 挪进来。这才真正消灭「两个生产者」,但 `$ORIGIN`
  本质是 per-unit 的(只有消费共享库的单元才需要),挪进全局闭包意味着要给闭包
  引入 per-unit 概念 —— 改动量与风险都明显更大。记 issue,不急。

### B(必须)— 把测试改回真不变量

1. `tests/e2e/219_runtime_search_farm_is_last.sh:151-171`:断言 farm 是 DT_RPATH
   的**字面最后一项**,删除「last ABSOLUTE entry」的放宽与那段理由
2. **新增行为不变量**(比形状断言更硬):构造一个同时存在于 `$ORIGIN` 与 farm 的
   SONAME,断言 `LD_DEBUG=libs` 解析到 `$ORIGIN` 那一份。这是本次缺陷的直接判据
3. 单测 `tests/unit/test_ninja_backend.cpp`:给带 `-Wl,-rpath,'$$ORIGIN'` 的
   链接单元设置 `plan.runtimeSearch` 含一条 `Origin::SubosFarm`,断言在
   `ldflags + " " + unit_ldflags` 的合成串里 `$ORIGIN` 的位置 **早于** farm。
   (仅 Linux 分支产出 farm_ld,其它宿主 `GTEST_SKIP`)

先写测试、确认变红,再改代码。

### C2(已批准,与 A 同一个 PR)— 共享库的 C++ 运行时契约改为 toolchain-coupled

**今天**:mcpp 每建一个 `.so` 都按「要离开这台机器的成品」处理 ⇒
`-static-libstdc++` ⇒ 把整个 libstdc++ 塞进这个 `.so` 并对外导出。

**C2**:`.so` 不再自带 std,而是 `NEEDED libstdc++.so.6`,运行期从 gcc 载荷目录
解析 —— 那个目录本来就是 DT_RPATH 第 2 项,已经在了。

| | 今天(self-contained) | C2(toolchain-coupled) |
|---|---|---|
| 一个进程里几份 libstdc++ | exe 一份 + 每个 `.so` 一份 | 一份(见「残留」) |
| `bin/libXau.so` | 9.5 MB | ~20 KB(叠加 C3 后) |
| `.so` 单独拷走能不能跑 | 能(自带) | 不能,需带上 `libstdc++.so.6` |
| 跨 `.so` 边界抛 std 异常 | 有风险(两份 typeinfo) | 正常 |

**改动点**

1. `distribution.cppm:47-51` `Role` 拆出 `SharedLibrary` —— 今天 `Binary` 与
   `SharedLibrary` 共用 `Distributable`,注释就写着 "Binary / SharedLibrary —
   leaves this machine",这一行正是本缺陷的策略源头
2. `distribution.cppm:123` `default_contract`:新角色 → `Contract::ToolchainCoupled`;
   `to_string(Role)`(`:97`)补一项
3. `flags.cppm:83-92` `role_of`:`LinkUnit::SharedLibrary` 不再落到 `Distributable`
4. `flags.cppm:52,54`:`std::array<…, 3>` → `4`
5. manifest:`cxx_runtime` 已经是 role-aware 的表形式
   (`{ default = …, tests = … }`,`toml.cppm:933-949`),补一个 `shared` 键;
   `types.cppm:421-426` 与 target 段的 `:621-622` 同步
6. **机制表无需改动**:`distribution.cppm:330-338` 已把 ELF + libstdc++ +
   ToolchainCoupled 处理成「不发任何标志」,驱动默认链 `libstdc++.so`,而 gcc
   载荷目录本来就在 `-L`/`-rpath` 里

**预期效果(可直接测)**

- `nm -D --defined-only bin/libX11.so | grep -cE '_ZNSt|_ZNKSt|_ZSt'` 从 2931 → 0
- exe 链接期 `-lX11` 不再满足 `runtime_error::what()` ⇒ 从 `libstdc++.a` 拉入
  ⇒ exe 的 `-static-libstdc++` **恢复为真**

**⚠️ C2 会掩盖 A 的症状 —— 同 PR 时这是首要风险**

C2 之后,即使 RPATH 顺序仍然是坏的,helloegui 也**不会再崩** —— 符号已经在 exe
内部。但产物加载的**仍然是 farm 里的 libX11 1.8.10,而不是链接时的 1.8.13**:
一次响亮的崩溃被换成一个静默的版本错配。

**所以 A 的回归测试绝不能依赖崩溃。** §4-B-2 那条行为不变量(同名 SONAME 必须
解析到 `$ORIGIN`)不是锦上添花,它是 A 在 C2 之后唯一还能变红的判据。
写测试的顺序必须是:先在未打 A 也未打 C2 的二进制上确认它红,再分别验证。

### C1(与 C2 同批)— 逃生舱的护栏

C2 之后,用户仍可显式写 `cxx_runtime = { shared = "self-contained" }` 让 `.so`
静态链 libstdc++。此时必须同时发 `-Wl,--exclude-libs,libstdc++.a`
(视情况含 `libsupc++.a` / `libgcc.a`),否则符号泛滥原样复现。

C1 不再是止血手段,而是让「非默认选项」不至于重新打开这个洞。

### C3(单独 PR,不阻塞本批)— 别把 `obj/std.o` 塞进不需要它的单元

**接缝已定位**:`ninja_backend.cppm:1362-1381` 对 `Binary` / `TestBinary` /
`SharedLibrary` **无条件**追加 `obj/std.o`,完全不看该单元是否真的 `import std`。
实测 `bin/libXau.so` 的输入 = 8 个纯 C `.o` + 一个 `obj/std.o`。

C2 之后 `.so` 不再内嵌 libstdc++,但**仍然会因为 std.o 而 `NEEDED
libstdc++.so.6`** —— 纯 C 的 compat 包(libXau / libXdmcp / libX11)凭空多一条
依赖。C3 把它摘掉,顺带让 libXau 回到 ~20KB、libX11 回到上游量级。

需要先确认的:`import std` 的传递性(依赖的 BMI 传递 import std 的历史坑见
`dep-bmi-cache-cross-version-poisoning`),以及静态库单元的处理。

### 残留(记 issue,不在本批)— 两份 libstdc++

C2 之后,exe(SelfContained,静态)+ 真正用 C++ 的 `.so`(ToolchainCoupled,
动态)= 一个进程两份 std,跨 `.so` 边界抛 std 异常会失败(typeinfo 不同)。
今天不会发生(compat 包都是纯 C,C3 还会把 std.o 摘掉),但这是 C2 引入的新形态。

顺带一个观察,值得写进那个 issue:本工程产物的 PT_INTERP 是
`<store>/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2` —— **它本来就离不开载荷**,
所以 exe 上的 `-static-libstdc++` 在这个配置下几乎买不到东西。
「exe 是否也该 ToolchainCoupled」才是那个 issue 的真正问题。

### D(低优先)— 首次运行 rule B 全线 inconclusive

实测:

| | `binding.loader` | `binding.library_dirs` | 警告 |
|---|---|---|---|
| 首次运行(同时安装工具链) | `""` | `[]` | 13 条 |
| 第二次 `mcpp build` | `…/xim-x-glibc/2.44/lib/ld-linux-x86-64.so.2` | `[…/xim-x-glibc/2.44/lib]` | 无 |

`runtime_binding.cppm:337-380` 从 `<subos>/lib64`、`<subos>/lib` 里找
`libc.so.6` 与唯一的 `ld-linux-*` 来填这两个字段;磁盘上二者都在(`lib/libc.so.6`
符号链接、唯一一个 `ld-linux-x86-64.so.2`),说明**首次运行时求值早于 farm 落盘**。
精确接缝(binding 解析点 vs 载荷/farm 写入点)还需要一次探针确认,不要照着推理改。

影响:新用户的第一次构建看到 13 条自己无法处理的警告,而 rule B 恰在最该生效的
那一次运行里失效。

**必须说清楚:即使 rule B 完全正常,它也抓不到本文的崩溃。** 它只比对
libc / PT_INTERP 的同一性(`elf_runtime.cppm:761-801`),不管其它 SONAME 解析到谁。
把 D 修好不能替代 A。

---

## 5. 两件事无关

首次运行的 13 条警告(D)与崩溃(A)在时间上同时出现,容易被读成一件事。
第二次构建警告消失、崩溃照旧,已经把它们分开。

---

## 6. 实施顺序

**本批 PR = A + B + C2 + C1。** A+(i)、C3、D、以及「两份 libstdc++」各自独立。

1. **先写测试,并在未打任何补丁的二进制上确认全红**
   - B-3 单测(`test_ninja_backend.cpp`):`$ORIGIN` 必须早于 farm
   - B-1 e2e 219:断言 farm 是 DT_RPATH 的**字面**最后一项
   - B-2 e2e **行为**不变量:同名 SONAME 必须解析到 `$ORIGIN`
     —— ⚠️ 这一条**不得依赖崩溃**,否则 C2 一落地它就假绿(见 §4-C2)
2. **A**:farm 移到 per-unit 尾部 → B-1 / B-3 转绿,B-2 转绿
3. **C2 + C1**:角色拆分 + 契约改判 + `--exclude-libs` 护栏
   - 判据:`nm -D bin/libX11.so | grep -cE '_ZNSt|_ZNKSt|_ZSt'` = 0
   - 判据:exe 不再有 `U _ZNKSt13runtime_error4whatEv`
4. **合并验证(缺一不可)**
   - helloegui 全链复现 ⇒ GUI 启动
   - 产物的 `libX11.so.6` 解析到 `$ORIGIN`(`LD_DEBUG=libs` 实证,不看形状)
   - **把 A 单独 revert 掉,B-2 必须重新变红** —— 证明 C2 没有把 A 的判据吃掉
5. 全量单测 + e2e(注意既有本机噪声:共享 gcc specs 污染、`pipefail`+`grep -q`
   的 SIGPIPE flake —— 本机红需逐条与已发布二进制比对,不可直接当回归)
6. 生态验证:C2 改的是 `.so` 的运行期契约,发版前必须在真实 mcpp-index
   workspace 上跑一遍(compat 包全是 `.so` 的重灾区)
7. 独立开:**A+(i)**、**C3**、**D**、**两份 libstdc++**

## 6.1 决策记录(2026-08-11)

| 项 | 决定 |
|---|---|
| C2(共享库 → toolchain-coupled) | **采纳**,与 A 同一个 PR |
| C1(`--exclude-libs` 护栏) | 随 C2 一起 |
| A+ | 只做 (i) 轻档,**单独 PR**;(ii) 记 issue |
| C3(`std.o` 无条件追加) | 单独 PR,不阻塞 |
| D(首次运行 rule B inconclusive) | 单独 issue,需先探针定位接缝 |

---

## 7. 附:关键实测命令

```bash
# 顺序
readelf -d bin/helloegui | grep RPATH
# 谁被加载
LD_DEBUG=libs <私有 loader> bin/helloegui 2>&1 | grep -A6 'find library=libX11'
# 两份库的差别
nm -D --defined-only bin/libX11.so | grep -cE '_ZNSt|_ZNKSt|_ZSt'          # 2931
nm -D --defined-only <store>/xim-x-libX11/1.8.10/lib/libX11.so.6 | grep -cE '_ZNSt|_ZNKSt|_ZSt'  # 0
# 决定性 A/B
patchelf --force-rpath --set-rpath "<bin 绝对路径>:<原有其余项>" ./helloegui-copy && timeout 8 ./helloegui-copy
```

> 注:本机 `readelf` / `ldd` 被 xlings shim 劫持(`readelf` 那条还指向一个已消失的
> scratchpad 路径),诊断一律走 `/usr/bin/` 绝对路径。
