# `crossTarget` 非空被当成「系统来自图」

2026-08-26 · 核心问题 + 修复方案 + CI 验收体系(待 review,尚未实施)

前置:[`2026-08-26-target-matrix-six-tables.md`](2026-08-26-target-matrix-six-tables.md)

---

## 0. 一句话

> **写出 `--target` 这个动作,被当成了「这个构建的系统来自依赖图」。**

而这两件事互不蕴含:一个项目可以显式命名它的宿主目标而完全不用图;也可以不写
`--target` 而让图供给一切。

⭐ **这与 2026.8.25.1 / .2 修的七条是同一个形状** —— 一个谓词回答了比自己更窄的问
题。区别只在这次的谓词不是「跨两层的 OR」,而是**「命令行上有没有这个字符串」**。

---

## 1. 判据

同一台机器、同一个编译器、**同一个目标**,只差写不写 `--target`:

```
$ mcpp build                            ✅ ELF 64-bit LSB pie executable, x86-64
$ mcpp build --target x86_64-linux-gnu  ❌ hermetic link check failed
```

不带 `--target` 时宿主目标**就是** `x86_64-linux-gnu`。两条链接线:

| | 无 `--target` | 带 `--target x86_64-linux-gnu` |
|---|---|---|
| | `-stdlib=libc++` | — |
| | `--rtlib=compiler-rt` | — |
| | `--unwindlib=libunwind` | — |
| | `lm.link_flags()`(`-L…/xim-x-glibc/…`、loader) | — |
| | — | `--target=x86_64-unknown-linux-gnu` |

成功那条走的是 **clang 自带的完整运行时栈**(libc++ + compiler-rt + libunwind),
它不需要 glibc 的启动对象。失败那条把这些**全部丢掉**,clang 于是回退到默认
(libstdc++ + libgcc + 宿主 crt),启动对象落到 `/lib/x86_64-linux-gnu/` 外面,
hermetic 检查拒绝。

## 2. 根因

`flags.cppm:616`:

```cpp
if (!crossTarget.empty()) {
    // ⭐⭐ THE TARGET SIDE COMES FROM THE GRAPH, SO THE HOST'S MODEL
    // CONTRIBUTES NOTHING …
    link_toolchain_flags += " -fuse-ld=lld";
    link_toolchain_flags_c = link_toolchain_flags;
} else {
    if (lm.mode == CLibMode::Sysroot) link_toolchain_flags += lm.link_flags(…);
    link_toolchain_flags_c = … + kLinkDriverFlagsC;
    link_toolchain_flags  += kLinkDriverFlags;
}
```

⭐ **注释说的是「目标侧来自图」,而条件问的是「`crossTarget` 非空」。** `crossTarget`
只是 `--target=<llvm 三元组>` 这个字符串,它在**任何**显式目标下都非空 —— 包括显式
写出宿主目标、也包括一个完全没有依赖的项目。

那段注释里的实测(`ld64.lld: unknown argument '--as-needed'`)是真的,而且当时的修复
是对的:**当系统真的来自图**时,宿主的 C 库模型与载荷的 C++ 运行时都不该出现在链接
线上。错的是把「来自图」写成了「`crossTarget` 非空」。

### 这解释了矩阵里的两格

| 格 | 现象 | 现在可知的原因 |
|---|---|---|
| **A** llvm × `x86_64-linux-gnu` | hermetic 拒绝 | 同一分支,丢掉 `lm.link_flags()` 与三个运行时 flag |
| **B** llvm × `x86_64-windows-gnu` | `ld.lld: unknown file type` | **同一分支**,同样丢掉;诊断落在别处 |

⚠️ **A 与 B 不是「同源但不同因」,是同一处代码的两个实例。** 上一版文档把它们写成
两条,不准确。

⚠️ **仍未查清:** hermetic 检查对 A 报得很准而对 B 什么都没说。**未实测。**

---

## 3. 修复方案

### 3.1 条件换成它注释里说的那件事

`TargetSide` 已经有这个答案,不需要新概念:

```cpp
// 现在:问命令行上有没有这个字符串
if (!crossTarget.empty()) { …丢掉宿主模型… }

// 应为:问系统是不是真的来自图
if (!plan.targetSide.cAbi.prebuilt()) { …丢掉宿主模型… }
```

`cAbi.prebuilt()` = `Payload || Xpkg`,它正是「C 库来自这台机器已有的东西」。
⭐ 这与 2026.8.25.1 修 ①③④ 时用的是**同一个谓词** —— 那三处也是把「图供给」写成
了别的东西。

⚠️ **判据必须两向:**
- 系统来自图时,宿主模型仍然不得出现(否则 `ld64.lld: unknown argument` 回归)
- 系统来自载荷时,**无论有没有写 `--target`,链接线都必须相同**

⭐ 第二条是**天然的判据**:同一台机器上 `mcpp build` 与
`mcpp build --target <宿主目标>` 生成的 `ldflags` 应当逐字节相等。这不需要新的断言
机制,它是一个恒等式。

### 3.2 `crossTarget` 本身要留

`--target=` 仍然必须下发 —— 它是「这是给哪台机器的」。要改的只是**它不再兼任「系统
来自图」的信号**。

---

## 4. CI 验收:让矩阵由测量维持

### 4.1 现状为什么看不见这些

| 已有 | 覆盖什么 | 看不见什么 |
|---|---|---|
| `openkal-cross.yml` 3 宿主 × 3 目标 | openkal 体系 | **载荷体系一格都没有** |
| `ci-linux-e2e` 等 | 宿主构建(不写 `--target`) | **写 `--target` 的那条路** |
| `ecosystem-e2e`(本次新增) | openkal 六条 e2e | 同上 |

⭐ **A 与 B 能存活,是因为现有 CI 里没有任何一格是「载荷体系 × 显式 `--target`」。**
openkal 那三宿主矩阵全部走图,恰好是不受影响的那一半。

### 4.2 方案:一条脚本,三个宿主,输出同一张表

新增 `tests/matrix/scan.sh`,在 `windows` / `linux` / `macos` 三个 runner 上各跑一
次,对**每一个** `kKnownTargets` 行 × 每一个可用编译器 × 两种系统来源:

```
mcpp build --target <t>        → 取构建报告的五层 + build.ninja 的 ldflags
```

产出一行 TSV:

```
host    target    compiler  compiler-triple  sysroot  c-abi  c++-abi  openkal  status
linux   x86_64-linux-gnu  gcc   x86_64-unknown-linux-gnu  payload  gnu  libstdc++  -  ok
linux   x86_64-linux-musl llvm  x86_64-unknown-linux-musl -        musl -          -  unsupported
```

**`status` 只有三种取值**,而三者的区别是本方案的核心:

| status | 含义 | 判据 |
|---|---|---|
| `ok` | 构建成功且五层与期望表一致 | 产出存在 + 报告逐字段相等 |
| `unsupported` | 期望表就说不支持 | **跳过,不是失败** |
| `mismatch` | 与期望表不符 | **失败** |

⚠️ **「跳过」必须是期望表说的,不是运行时发现的。** 一格因为「今天这台机器没装某个
载荷」而跳过,与「这个组合本就不支持」是两回事 —— 前者会让矩阵在缺件的机器上悄悄
变绿,正是本次会话里反复出现的那种假绿。

### 4.3 期望表放在仓库里,与代码一起改

`tests/matrix/expected.tsv` —— 六张表的机器可读形式。CI 比对实测与它:

- 实测 `ok` 而期望 `unsupported` ⇒ **失败**,提示「支持面扩大了,更新期望表」
- 实测 `mismatch` ⇒ 失败,打印两侧差异
- 实测 `unsupported` 而期望 `ok` ⇒ **失败**,这就是 A/B 这类回归

⭐ **这条最重要:期望表是仓库里的文件,改了行为就必须同时改它。** 那样「支持矩阵」
不再是一份会悄悄过期的文档 —— 它是一次测量与一份声明的比对。

### 4.4 恒等式判据(不需要期望表就能查的一类)

有几条关系与具体取值无关,任何宿主上都成立:

| 恒等式 | 抓的是什么 |
|---|---|
| `mcpp build` 的 ldflags == `mcpp build --target <宿主目标>` 的 ldflags | **A/B 的根因** |
| 报告里 `c-abi` 为 `(payload)` ⇒ ldflags 必须含该载荷的 `-L` | 「声明了却没接上」 |
| 报告里 `c-abi` 为 `(graph)` ⇒ ldflags 不得含宿主 C 库路径 | 图模式的反向 |
| 请求目标的 OS == 解析三元组的 OS | 2026.8.25.2 已有守卫,此处是回归网 |

⭐ **这四条不依赖期望表,也不依赖机器上装了什么** —— 它们是结构约束,任何一格只要
跑起来了就该满足。建议先做这一层,再做 §4.3 的全表比对。

### 4.5 接进哪里

- 新 workflow `ci-target-matrix.yml`,`ubuntu-24.04` / `windows-2022` / `macos-14`
  三个 job
- 每个 job 跑 `scan.sh` → 与 `expected.tsv` 比对 → 上传实测 TSV 作为 artifact
- ⚠️ **必须断言扫描真的跑了**:`scan.sh` 输出行数 ≥ 期望表中该宿主的行数,否则
  「一格没跑」与「全部通过」在退出码上没有区别

---

## 5. 建议顺序

1. **§4.4 的四条恒等式** —— 不需要期望表,先落地。第一条恒等式**当场就会红**,
   那就是 A/B 的判据。
2. **修 §3.1 的条件** —— 换成 `cAbi.prebuilt()`,两向验证。
3. **§4.2/4.3 的全表扫描 + 期望表**,三宿主接进 CI。
4. 之后 issue #510(sysroot 声明不安装)与 D(裸机 pin 让位)可独立进行;它们会被
   §4.3 的期望表自动覆盖。

⚠️ **顺序不能反。** 先有判据再改代码 —— 否则「修好了」这句话没有任何东西支撑它,
而这正是本次会话反复付出代价的地方。

---

## 6. 落地回填(2026-08-26,实测)

⚠️ 这一节记的是**实际做出来的东西与 §3–§5 的差**。计划本身没有错,但实施过程里
量到了三件计划没有预见的事,每一件都改变了方案。

### 6.1 通道是三条,不是两条

`§3.1` 说要改一处条件。实际有三处在问同一个错问题:
`link_toolchain_flags`、`payload_ld`、`atomic_ld`。而 `payload_ld` 那处的注释
**预告了两条**:

> the C-runtime group reaches the link line through TWO channels, and a reader
> who fixed one saw the identical error and could reasonably conclude the fix
> had not worked.

⭐ 找出第三条的,是 §4.4 的恒等式:差异 7 项 → 5 项 → 3 项 → 0,每修一处它就
指向下一处。没有它,修完两处会看到「还是红」,而这条注释会让人以为已经找全。

第四条在别处:mingw 分支 `isMingwTc && cAbi.prebuilt()` 返回的链接线里**没有
`--target=`**。它的注释同样写明了前提(`x86_64-w64-mingw32-g++` 不需要 `--target`
因为它没有别的),而条件没有检查这个前提——clang 有别的。实测
`ld.lld: error: obj/main.o: unknown file type`,对象是一个完全合法的
`Intel amd64 COFF`。

### 6.2 「能力 pin」不止裸机一行

`§3.3` 把能力 pin 等同于 `is_freestanding()`。实测 `x86_64-windows-musl` 也是:
没有任何 gcc 载荷能发出 PE + musl(mingw 载荷发的是 PE + MinGW CRT,那是隔壁
`-gnu` 那一行)。声明 gcc 会解析到宿主的 **Linux** musl 载荷,报「没有 C++ 前端」。

真因是 `to_xim_package` 里 `t.is_musl()` 被当成「这是 linux-musl」用,而那段
注释自己写的就是 linux-musl。`x86_64-windows-musl` 是后加的行,径直走了进来。

⇒ 谓词收敛到 `triple::pin_is_capability()`,三处决定共用它。

### 6.3 ⭐⭐ 判据不该靠匹配句子——机器接口

`§4` 的整套验收建立在解析 mcpp 打给人看的输出上。**在同一次会话里,这个方案
自己踩了它要防的坑**:我把一句拒绝从 `cannot emit it` 改成
`cannot be emitted by`,e2e 297 的断言当场变成空转——它仍然「通过」,只是不再
匹配任何东西。

⇒ 两条命令进入 `--format json`:

```
mcpp toolchain list --format json                → mcpp.toolchain.list
mcpp why toolchain --target T --toolchain C ...  → mcpp.why.toolchain
```

后者只解析不构建,给出五层、驱动器、三元组、C 库模型,以及 `status` 与
`reason`。`reason` 是一个记号,由新模块 `mcpp.build.refusal` 在每一处拒绝的
`return` 之前记下。

⚠️ **消息仍然是承诺**。297/298 照旧断言拒绝点名了目标、规则与出路——换掉的只是
**分类**。一个答案集有限的问题,不该用子串搜索来问。

⚠️ **但查询不能取代构建**。`llvm × x86_64-windows-gnu` 解析得完全正常,失败在
链接期的封闭性检查上;只查不建会把它报成绿。scan.sh 两样都做:分类取自
`reason`,结论取自 `mcpp build` 的退出码。于是整个矩阵**没有一处字符串匹配**。

⚠️ 而这套机器本身又造了一次同型缺陷:一处没有记号的拒绝分支让矩阵写下
`unsupported / none`——「拒绝了」与「没有理由」共用一个词。现在无记号的拒绝报
`other`。

### 6.4 探针本身会说谎

第一版 scan.sh 对每一格都写 `#include <cstdio>`,四个裸机目标全报
`fatal error: 'cstdio' file not found` 并被记成缺陷。那不是 mcpp 的失败,是探针
问错了问题。⭐ **一个自己就编不过的探针,产出的整列都是关于探针的。**

同型的第二次:graph 模式下仍写裸机形状,而 openkal-musl 供给的是一个**有宿主的**
C 库,`_start` 去找 `main`,得到 `undefined symbol: main`。

### 6.5 实测结果(x86_64 Linux,2026-08-26)

40 格(payload 24 + graph 16),**0 个 mismatch**。理由分布:

| status | reason | 格数 |
|---|---|---|
| ok | none | 14 |
| unsupported | tier-planned | 12 |
| unsupported | capability-pin | 6 |
| unsupported | layer-requirement | 4 |
| unsupported | convention-unreplaced | 3 |
| unsupported | host-cannot-serve | 1 |

⚠️ **`expected.tsv` 里没有 `mismatch`,这是刻意的。** 写下 `mismatch` 就是把一个
缺陷声明成期望,矩阵会在它上面变绿。一格测出 `mismatch`,要么修 mcpp,要么让
mcpp 在决定处给出一句带 `reason` 的拒绝——没有第三条路。

