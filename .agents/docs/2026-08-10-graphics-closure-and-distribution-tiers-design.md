# 图形栈打通:一个标签、一条没人依赖的边、一个被钉住的 pin

> 日期:2026-08-10
> 基线:mcpp `main` `3f237ed`(版本 `2026.8.10.1`)、xlings `2026.8.10.4`、
> mcpp-index `main` `b4e28f2`(xpkg 0.0.57)、xim-pkgindex `main` `df01c872`
> 本机:x86_64-linux-gnu / gcc 16.1.0 / NVIDIA + X11
> 前置阅读(结论,不必重读过程):`2026-08-10-graphics-stack-usability-design.md`
> (三层故障的分层)、xlings `2026-08-10-graphics-stack-design.md`(标签契约与 E1–E5)
>
> 本文所有"已验"结论都在本机跑过,命令与输出在正文里。凡未实测的一律标 **未验**。

---

## 0. 结论先行

上一份文档把图形栈的不可用分成三层(装 / 建 / 跑)。这一轮把每一层追到**代码或数据的
那一行**,结果是:**三层里有两层的既有结论是错的**,而按错的结论去修,修完还是坏的。

| 层 | 上一份的结论 | 核实后 | 证据 |
|---|---|---|---|
| **L1 装** | xim-pkgindex 数据缺陷:四段版本不可范围比较 | ❌ **已被 xlings `2026.8.9.2` 修好**;真因是 mcpp-index CI 的 `MCPP_VERSION` 钉在 `2026.8.8.2`(内带 xlings `2026.8.8.1`) | §1.2 |
| **L2 建** | #405:缓存命中丢 std 次序 | ✅ 方向对,但 **issue 里写的根因是错的**,按它改修不好 | §1.1 |
| **L3 跑** | provider 有名无物 | ✅ 成立,但**上游还有一层**:mcpp 链出来的可执行文件全是 DT_RUNPATH,拿不到 GPU 与 provider 声不声明无关 | §1.3 |

一句话:

> **图形栈拿不到 GPU 的直接原因,是 mcpp 自己链接时用了 DT_RUNPATH。**
> 这一条与索引、与 provider 声明、与 xlings 全都无关 —— 它在 mcpp 的链接命令行里。

---

## 1. 事实核验:三条被推翻的既有结论

### 1.1 #405 的 issue 根因是错的(我自己写的那条)

**实测复现**(本机,mcpp `2026.8.8.2`,缓存已有 `mcpplibs.cmdline@0.0.2`):

```console
$ mcpp new b && cd b && mcpp add mcpplibs.cmdline@0.0.2
$ cat > src/main.cpp <<'EOF'
import mcpplibs.cmdline;          # 刻意不 import std
int main() { mcpplibs::cmdline::App app("b"); return 0; }
EOF
$ mcpp build
      Cached mcpplibs.cmdline v0.0.2 (3 units)
error: build failed
std: error: failed to read compiled module: No such file or directory
std: note: compiled module file is 'gcm.cache/std.gcm'
mcpplibs.cmdline: error: failed to read compiled module: Bad import dependency
```

生成的 `build.ninja` 三行,把根因定死:

```ninja
65: build gcm.cache/std.gcm : stage_file <cache>/std/50c5b8df12f1bf98/gcm.cache/std.gcm
83: build _mcpp_staged_cache : phony <3 个 dep .o> <3 个 dep .gcm>
90: build obj/main.o : cxx_object …/src/main.cpp | obj/main.cpp.ddi.dd || _mcpp_staged_cache
93: build bin/b : cxx_link … obj/main.o obj/std.o
```

- **第 65 行:边存在。** issue 评论断言"`needsStdModule=false` ⇒ `plan.stdBmiPath` 为空
  ⇒ 不发 std 的 `stage_file` 边"—— **不成立**。
  `graph_or_targets_import_std`(`src/build/prepare.cppm:651`)看的是 `scan.graph`,
  而 `scan_packages(packages)` 的 `packages` **包含依赖包根**
  (`prepare.cppm:3784` / `:4183` 各 `push_back` 一次),依赖单元本来就在图里,
  谓词恒为真。`servedFromCache` 要到 `:6060` 才置位,在谓词之后。
- **第 83 行:聚合里没有它。** `_mcpp_staged_cache`(`ninja_backend.cppm:1053–1083`)
  只收 `plan.compileUnits` 里 `servedFromCache` 的 `.o` 与 BMI;
  std 的两条 `stage_file` 边在 `:956–983` 更早发出,**不在那个循环里**。
- **第 90 行:没有任何东西依赖 `gcm.cache/std.gcm`。** 它作为 implicit input 只出现在
  `cu.imports` 含 `"std"` 的编译边上(`:1215`/`:1222`)。消费方自己不 import std ⇒
  零引用 ⇒ **ninja 永不执行第 65 行**。
- **第 93 行:`obj/std.o` 反而是可达的** —— 它是链接边的输入(`:1295`)。
  所以坏的只有 BMI 那一半,而且要到编译 `main.o` 时才炸。

> **真因:一条正确的边,没有任何人依赖它。**
> issue 里的修法(把 `imports_std` 写进 `entry.json`,hit 时 OR 进 `needsStdModule`)
> 作用于一个**本来就已经为真**的谓词 —— **改完仍然坏**。
>
> 这是同一个 issue 上第二次根因写错。第一次是"跨版本缓存投毒",被自己的测量否掉;
> 这一次是"谓词漏判",被 `build.ninja` 否掉。**issue 里的根因和代码一样需要证据**,
> 而这一条的证据只要 `grep std.gcm build.ninja`,三秒。

### 1.2 L1 不是数据缺陷,是 pin 落后

**实测**(本机 xlings `2026.8.10.2`):

```console
$ xlings info "xim:libglvnd@>=1.7.0.1" --agent
  available: 1.7.0.1, 1.7.0
  selected version: 1.7.0.1          ← 解析成功
```

四段版本的范围比较由 xlings **`2026.8.9.2`**(`f203b6b`,"generalize the version grammar
— N segments")修好,原因写在它自己的 commit 里:`fontconfig 2.15.0.1` 撞过同一个洞。

而 mcpp-index 的 CI:

```yaml
# .github/workflows/validate.yml:152
MCPP_VERSION: "2026.8.8.2"
```

日志实证(run `31379222823`,`workspace (linux default 0/4)`):

```
MCPP_VENDORED_XLINGS: …/mcpp-2026.8.8.2-linux-x86_64/registry/bin/xlings
error: xlings install_packages failed … 'compat.glx-runtime@2026.08.08'
  xlings reported: E_INVALID_INPUT: package 'xim:libglvnd@>=1.7.0.1' not found
```

`2026.8.8.2` 内带 xlings `2026.8.8.1` —— **正好落在修复之前**。

> **判据:`xim-pkgindex` 一个字都不用改。** 上一份文档提的"数据面止血:改成裸名"
> 是对一个已经修好的缺陷的补丁,做了反而把 mesa/libglvnd 的版本约束一起降级。
> 需要改的只有 `MCPP_VERSION`,而它要等 mcpp 发一个 pin 了新 xlings 的版本。

诊断面那条仍然成立且仍然欠着:`E_INVALID_INPUT: … not found` 在约束不可解析时也这么说,
**这一轮它又把我送去查一个不缺的包**。归属 xlings,不在本设计范围,单独提。

### 1.3 mcpp 链出来的可执行文件全是 DT_RUNPATH

**实测**(本机默认工具链):

```console
$ gcc a.c -o a.out -Wl,-rpath,/tmp/xyz            && readelf -d a.out | grep PATH
 0x…1d (RUNPATH)   Library runpath: [/tmp/xyz]     ← 默认
$ gcc a.c -o b.out -Wl,--disable-new-dtags,-rpath,/tmp/xyz && readelf -d b.out | grep PATH
 0x…0f (RPATH)     Library rpath:   [/tmp/xyz]
```

全仓搜索:`--disable-new-dtags` **零处**;唯一相关的一处是
`src/toolchain/post_install.cppm:321`,它显式写入 **`--enable-new-dtags`**。

xlings 那份设计的实测表(同一路径内容、只改标签类型):

| 可执行文件标签 | egl | gles2 | egl-surfaceless | glx |
|---|---|---|---|---|
| **DT_RPATH** | **NVIDIA** | **NVIDIA** | **NVIDIA** | **NVIDIA** |
| DT_RUNPATH | llvmpipe | llvmpipe | llvmpipe | NVIDIA |

机制:DT_RUNPATH **只对携带它的那个对象自己的查找生效**;DT_RPATH 对进程内**任意深度的
`dlopen`** 生效。GL 程序到驱动要经三到四层 `dlopen`(glvnd → vendor → EGL 外部平台模块 →
它的依赖),而这些 `dlopen` **都不是应用发起的**,是 `libGLX.so.0` / `libEGL.so.1` 发起的。
所以应用二进制的 `dlopen` 引用数是 **0**,"这个程序需不需要传递标签"在二进制上看不出来。

`src/build/plan.cppm:913` 的注释写着:

> Putting the directory in the artifact's RUNPATH instead is: DT_RUNPATH reaches the
> object that carries it **and the dlopen() it performs**, and nothing else.

**这句话对 glibc 那个场景是对的**(私有 libc 由产物自己 `dlopen`),
**但它被当成通则用了** —— 图形链上 `dlopen` 的发起者是**别的对象**,
"and nothing else" 正是失败本身。

顺带,E1c 已经点名过 mcpp:"我原以为 mcpp 那种刻意链宿主的产物会因非 form-X 自动豁免 ——
实测不成立:mcpp 构建出的 xlings 二进制,INTERP 指向 mcpp 自己 store 里的 glibc,
**是 form-X**。" 也就是说 mcpp 的产物**在 rule E 的管辖范围内**,只是 mcpp 没有履约。

### 1.3.1 归属:E2 要拆成两半看,不是"归 mcpp"也不是"归 xlings"

xlings 的 E2b 是**一个 ld 包装器**,一次追加三样东西:

```sh
exec <real-ld> "$@" -rpath "$XLINGS_SUBOS_LIB" -rpath-link "$XLINGS_SUBOS_LIB" --disable-new-dtags
```

**标签**和**路径**是两件不同的事,mcpp 对它们的答案相反。

**实测一:mcpp 确实链过 xlings 的 `ld`** —— 所以包装器会作用到 mcpp。

```console
$ <xim-x-gcc>/bin/g++ -print-prog-name=ld
ld                         ← 载荷里没有自带 ld,从 PATH 解析 ⇒ 就是 xim binutils 那个
```

**实测二:`<subos>/lib` 是一个带 libc 的目录** —— 199 个条目里包含:

```
ld-linux-x86-64.so.2   libc.so.6   libm.so.6   libpthread.so.0   crt1.o crti.o crtn.o
```

于是:

| 半边 | mcpp 该怎么办 | 理由 |
|---|---|---|
| **`--disable-new-dtags`(标签)** | **mcpp 自己发** | ① mcpp 有大量**不经过那个 ld** 的链路:交叉 musl / mingw、`-fuse-ld=lld`、`gcc@system` / `msvc@system`、host 工具子构建;② mcpp 要能在自己的 e2e 里断言它;③ 与包装器**同向**,后出现者胜出,不冲突 |
| **`-rpath <subos>/lib`(路径)** | **mcpp 必须不继承** | 那是**第二个带 libc 的目录**。mcpp 的 Rule A/B 闭包校验(#396/#400)要求解释器 + 直接/传递 libc 与**同一个** RuntimeBinding 一致 —— 一条未经审查的 libc 路径要么触发它自己的守卫,要么在运行期静默选错 libc。pack 档更是必须把它剥掉 |

**实测三(意外收获,独立佐证 §3.1)**:这台机器上共享 gcc 载荷的搜索路径里,sysroot 指向的是
**另一个工程的 subos**:

```
libraries: … /home/speak/workspace/github/openxlings/xim-pkgindex-fromsource/.xlings/subos/default/lib/ …
```

—— 与本工程毫无关系。这正是 `mcpp-clean-link.specs` 存在的原因("that file has been patched
by every home that ever installed against this shared payload"),也再一次说明
**共享载荷是共享可变状态**,不能承载契约。

> **结论:E2 仍然需要 xlings 做,mcpp 做自己那一份不是替代。**
> 两者覆盖的是**不同人群**,而 mcpp 覆盖不到的那部分恰恰是 xlings 的定位承诺:
>
> - 用户手敲 `gcc -lGL`(**E2 自己的验收判据就是"用户零 flag"**)
> - 从源码构建的 xim recipe(它们不经 mcpp)
> - subos 里的 cmake / meson / xmake / cargo / go 工程
>
> mcpp 只能保证"mcpp 链出来的产物"。**"用户态 Linux 发行版"的那一半只有 xlings 能给。**

**因此本设计对 xlings 提两条接口要求**(不在 mcpp 侧实现,但必须在 E2b 落地**之前**谈妥):

1. **E2b 必须给出一条声明式的退出**,让 mcpp 能只要标签、不要路径。
   E1c 已经立过规矩:**退出必须是声明出来的,不能靠推断**,也不能靠"某个工具重写 spec
   去对抗另一个工具的默认值"。若 E2b 无退出即落地,mcpp 的 pack 产物会被烙进一条
   store 绝对路径,且 mcpp 自己的 libc 闭包守卫会开始对自己的产物报警。
2. **E2a(`XLINGS_SUBOS_LIB` 契约声明)对 mcpp 有独立价值** —— 有它,mcpp 读声明;
   没有它,mcpp 只能硬编码 `<subos>/lib`,那又是一处"同一决策两处推导"。

### 1.4 附带发现(不在本设计范围,单独记)

本机 `xim-x-binutils` 的 `as` / `readelf` shim 指向一个**已删除的旧会话 scratchpad home**:

```
[error] xlings: executable 'as' not found
[error]   path: /tmp/claude-1000/…/3eae0253-…/accept-run1/home/.mcpp/registry/data/xpkgs/xim-x-binutils/2.42/bin
```

与记忆里 #293(e2e 写穿符号链接损坏真实工具链)同族。**是本机环境损坏,不是本轮要修的缺陷**,
但它会让"用 mcpp 工具链验标签"这类验证静默失败,所以本文所有 ELF 验证一律用**原生解析**
或系统 `readelf`,不依赖 xim binutils。

---

## 2. 架构:一条脊柱,三个档位

上一份文档把 dev / pack / static 三档并列。核实之后可以更紧:

> **三个档位是同一个"运行期能力"模型的三次求值,不是三套机制。**

```
              ┌──────────────────────────────────────────┐
              │  [runtime] 能力模型(已存在,未被使用)     │
              │  RuntimeRequirement → RuntimeArtifact    │
              └───────────────┬──────────────────────────┘
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
   ┌─────────┐          ┌──────────┐          ┌──────────┐
   │  dev    │          │  pack    │          │  static  │
   │ 吃 store│          │ 自包含    │          │  musl    │
   ├─────────┤          ├──────────┤          ├──────────┤
   │需求必须落│          │不可打包者 │          │能力需求  │
   │到物,且  │          │进宿主清单 │          │= plan 期 │
   │身份一致  │          │产物零 store│         │  硬拒    │
   └─────────┘          └──────────┘          └──────────┘
        └─────────────────────┴─────────────────────┘
                              │
                     ┌────────▼────────┐
                     │ 标签契约(横切)  │
                     │ 可执行 DT_RPATH  │
                     │ 库   DT_RUNPATH  │
                     └─────────────────┘
```

模型**全都已经在代码里**,只是没人填也没人验:

| 类型 | 位置 | 现状 |
|---|---|---|
| `RuntimeRequirement{kind,value,phase,requester,required}` | `src/manifest/types.cppm:504` | 已定义 |
| `RuntimeArtifact{role,provider,path,provenance,abi,digest,hostFingerprint}` | `src/manifest/types.cppm:512` | 已定义 |
| `subos_info.contract.artifacts` 读取 | `src/xlings/subos_info.cppm:236` | 已实现 |
| `mcpp why runtime` 打印 | `src/doctor.cppm:585` | 已实现,本机输出 `(none declared)` |

本机 `~/.xlings/subos/default/.xlings.json` 的 `subos_info` 块:

```json
{"created_by": "xlings 2026.8.5.1", "envs": {}, "runtime": "glibc@2.39", "schema_version": 1}
```

—— 没有 `contract`。**所以 dev 档的校验必须是三值的**:`OK` / `MISMATCH` / `NOT_DECLARED`,
把"验过了是对的"和"根本没得验"分开。二值会让 `NOT_DECLARED` 被读成 `pass`,
这正是这一轮反复踩的那个形状。

### 2.1 mcpp 自己算,`.wiring` 只做增强

xlings `2026.8.10.4` 在 **libglvnd 载荷的 vendor 目录**下写了一份纯文本记录
(`src/core/subos/graphics.cppm`,`kRecordName = ".wiring"`),逐 vendor 一行:

```
dispatch=<libglvnd payload>
<vendor> state=ok|native|broken|unverified reason=<…>
```

**第一版设计把它当作机制,那是错的。** 拆开看,mcpp 要的两件事来源不同:

| mcpp 要判什么 | 数据从哪来 | 依赖 `.wiring` 吗 |
|---|---|---|
| **物在不在、身份对不对**(声明 0.1.2 却解析进 0.1.1) | **纯路径事实**,mcpp 自己走一遍就有 | ❌ **不依赖** |
| **这个 vendor 到底能不能打开** | xlings 在接线时拿着 `patchelf` 逐入口点探过 | ✅ 只在这里 |

mcpp 自己那一遍**用已有的 ELF 解析器就能走完**,和 `read_graphics_wiring` 前三步一样:

```
libGLX.so.0(dispatch,mcpp 本来就知道它)
  → 解析 DT_RPATH                        → vendor 目录
  → 枚举 vendor SONAME、解符号链接         → 真实载荷路径
  → 与声明的 provider 身份比对             → OK / MISMATCH
```

**而这正是 §3.E 唯一的判据**(本机现状:声明 `0.1.2`、解析进 `0.1.1`)。它是纯路径事实,
不需要探测、不需要懂 GL、**不需要任何一方的记录**。

`.wiring` 只在"想额外报告 vendor 能不能打开"时才读,而那恰好是条件语义有缺陷的那一半:

| 消费者标签 | `libEGL_nvidia` | 真实渲染 |
|---|---|---|
| DT_RUNPATH | 打不开 | llvmpipe |
| DT_RPATH | LOADED | NVIDIA |

`state=broken` **以消费者标签为条件,而格式没有表达这个条件**(xlings#537,低报)。

> **所以 mcpp 的读法是:主判据自算,`.wiring` 降级为可选增强,且永不单独定生死。**
> - 缺失 / 读不动 / 不认识的 `state` ⇒ 一律按 `unverified` 读,**绝不读成 pass**
>   (未知状态读成通过,正是 xlings 自己在 `parse_wiring_record` 里写下的防线)。
> - 显示 `state` 时**标注 mcpp 是按自己产物的标签重新求值的**:
>   `broken` + `reason=runpath-not-transitive` 对 **DT_RPATH** 的消费方不适用。
> - 未知 key 一律忽略 ⇒ xlings 按 #537 补上条件字段之后,mcpp **不需要跟着改**。
>
> **收益:E 不再等 #537,也不再和 xlings 的记录格式耦合。**
> 第一版把别人一份"条件未表达"的记录放在承重位上 —— 那等于把自己的正确性
> 挂在别人的格式缺陷上。**能自己算的事实,不要去读别人的结论。**

---

### 2.2 构建期与运行期必须分开,而且 mcpp 不自适应硬件

两个问题分开答。

**分开吗 —— 必须。** 而且判据不是"看起来更整洁",是:

> **mcpp 的构建期结论,必须在"构建机 ≠ 运行机"时仍然正确。**

dev 档上两台机器是同一台,分不分看不出来;pack / static / xpkg 上它是决定性的。
把这条当硬约束,就自动排除掉一整类错误做法:

| 时期 | mcpp 该产出什么 | 明确不产出 |
|---|---|---|
| **构建期** | **声明事实**:物在不在、身份对不对、标签对不对、什么必须来自宿主 | 任何关于**这台机器的 GPU** 的结论 |
| **运行期** | **什么都不产出** —— 报 `NOT_EXERCISED` 并指向 `xlings doctor` / `gl-doctor` | 不启动被测程序去"试试看" |

违反它的典型形态就是 autoconf `AC_RUN_IFELSE` 三十年的教训:**拿构建机跑一下,
去决定目标机上的事**。今天的图形产物 RUNPATH 里那三条构建机 store 绝对路径,
是同一个错误的另一种拼写 —— 产物只在构建它的那台机器上成立。

**自适应硬件吗 —— 不。而且这不是保守,是这套架构的本意。**

libglvnd 的派发、EGL vendor JSON、Vulkan ICD JSON —— **这一整套的存在理由就是
"适配发生在加载期、由加载器、在目标机器上完成"**。在构建工具里再实现一遍,
既是重复,又必然更差:构建工具只能看见构建机。

所以 mcpp 的职责是**不要挡住它**,共三条,全在本设计里:

| mcpp 要做的 | 对应 |
|---|---|
| 标签对 —— 让深层 `dlopen` 能穿透 | §D |
| 路径通 —— dispatch 找得到 vendor,且身份一致 | §E |
| 不打包不该打包的 —— 驱动留给宿主,自带 libc 的档直接拒 | §F / §G |

> **一句话:DT_RPATH 那一条修的不是"适配",是"不阻断适配"。**
> 这也解释了为什么它一条就能把 egl / gles2 / egl-surfaceless 三格从 llvmpipe 翻成 NVIDIA ——
> 加载器一直有能力找对,是标签把它挡在门外。

**唯一合法的"看硬件"**:**测试**的能力轴(有没有 display / 有没有驱动),
用来决定一条 e2e 是 `PASS` / `FAIL` / **`NOT_EXERCISED`**(§I4)。
**适配测试的运行条件,和适配产物的内容,是两件事** —— 前者必须做,后者绝不做。

---

## 3. 变更清单

九项。每项给:改哪、为什么是那里、判据(**先证伪再采信**)。

### A. xlings pin `2026.8.9.2` → `2026.8.10.4`

- **改**:`src/xlings.cppm:47` `kXlingsVersion`(唯一真源,16 个 pin 点由
  `.github/tools/check_version_pins.sh` 机器校验)。
- **为什么是 `.4` 不是 `.2`**:`2026.8.10.4` 是 rule E + `--force-rpath` 落地的那一版
  (`cadcf77`)。装机时会校验标签契约,与 §D 同向。
- **可获得性已验**:`xim-pkgindex` `origin/main` 的 `pkgs/x/xlings.lua`
  `["latest"] = { ref = "2026.8.10.4" }`。(本机索引缓存陈旧只到 `.2`,是缓存不是索引。)
- **判据**:bump 后在**干净 home** 里 `xlings info "xim:libglvnd@>=1.7.0.1"` 必须解析到
  `1.7.0.1`;**证伪**:回退 pin 到 `2026.8.8.1`,必须重现 `not found`。

> ⚠️ 跟着 pin 走的**不是** `.xlings.json` 的 bootstrap pin —— 那是自举起点,
> 按发布流程在**发布之后**单独收尾。见 `.agents/skills/mcpp-release/SKILL.md`。

### B. #405 —— 把 std BMI 放进 staged 聚合

- **改**:`src/build/ninja_backend.cppm:1053–1083`。`staged` 非空时,把
  `std_bmi_dst`(以及 `has_std_compat` 时的 `compat_bmi_dst`)一并 `push_back` 进
  `staged`,再发 `_mcpp_staged_cache` phony。
- **为什么是那里**:那个聚合**就是为"stage 边丢掉了编译边携带的次序"而发明的**
  (`:1035–1052` 的 ORDERING 注释写得很清楚,它当时解决的是模块分区)。
  std BMI 是同一类问题的另一个实例,只是它的 stage 边发得更早、不在那个循环里。
  **在别处修等于在同一个决策上再推导一次。**
- **不改**:cache key、`entry.json` schema、`kCacheEpoch`。**现有缓存全部继续有效。**
- **为什么不按 issue 的修法**:见 §1.1,那个谓词已经是真的。
  → **issue #405 的根因段必须重写**,否则下一个人按它改,改完仍然坏。
- **判据(e2e `212_cached_dep_std_is_ordered.sh`)**:
  清空某个 `import std` 的包的 pkg 缓存 → 建项目 A(miss,必须 OK)→
  建项目 B(hit,必须也 OK),两个消费方**都刻意不写 `import std`**。
  再加一条**图断言**:`_mcpp_staged_cache : phony` 那一行必须包含 `std.gcm`。
  - ⚠️ **不得先删产物**。删了会让 ninja 以"图过期"的样子失败,快路径回退到完整
    prepare,**未修的二进制也会绿**(#407 的复现里已经踩过一次)。
  - ⚠️ 行为断言单独不够:一台恰好已有 `std.gcm` 的机器上它会假绿,所以要有图断言。

### C. #407 —— build.ninja 自己说明它是哪张图

- **改**:
  1. `emit_ninja_string` 在文件头写一行 `# mcpp:graph=<normal|test>`;
     形态由 plan 携带(`includeDevDeps || !extraTargets.empty()` ⇒ `test`)。
  2. `try_fast_build` / `try_fast_run`(`src/build/execute.cppm`)读这一行,
     **只有 `normal` 才允许走快路径**;缺失(旧文件)或不匹配 ⇒ 当作 miss,回退完整 prepare。
- **为什么不是"`mcpp test` 也调 `forget_build_cache_entry`"**:那是**写入侧修补** ——
  每一个未来的图重写者都必须记得调它。#387 已经为 `--configure-only` 调了一次,
  `mcpp test` 是第二次,下一个模式是第三次。**读取侧不变式只需要成立一次**:
  快路径校验它即将重放的那张图。
- **顺带删掉**:`configure.cppm:101` 的 `forget_build_cache_entry` 调用与
  `execute.cppm:288` 的函数本身 —— 由 header 检查取代,`211_configure_only_cdb.sh`
  的可观察行为不变(它断言的是"下一次 `mcpp build` 必须重新链 target")。
  **保留两套 = 同一决策两处推导**,正是要消掉的东西。
- **`tests/` 不需要进 `sources_newer_than`**:两张图分开之后,普通构建的图里根本没有
  测试文件,改坏一个测试不可能让 `mcpp build` 失败。**不加第二个机制。**
- **判据(e2e `213_build_after_test_is_not_the_test_graph.sh`)**:
  `mcpp build` → `mcpp test` → `mcpp build`,第三步之后 `bin/<target>` 必须存在且被重链;
  把 `tests/*.cpp` 改成非法 C++ 之后 `mcpp build` 必须**仍然成功**。
  **不删任何产物。** **证伪**:去掉 header 检查,这条必须变红。

### D. DT_RPATH 契约 —— xlings E2 的 mcpp 那一半

- **规则**(与 xlings 逐字一致):
  - **ELF 可执行文件**(`LinkUnit::Binary` / `TestBinary`)→ **DT_RPATH**
  - **共享库**(`LinkUnit::SharedLibrary`)→ **保持 DT_RUNPATH**
  - Mach-O / PE / musl 全静态 → 不适用
- **为什么库不能一起翻**:xlings `#593` 已实测 —— 在**库**(interposer)上强制 RPATH
  **有害**,传递性会把搜索路径推进那个库往下的每一次查找,`eglInitialize` 直接失败。
  这是"翻一半"而不是"翻全部"的**实测理由**,不是保守。
- **怎么改**:按 link unit 的 kind 追加 `-Wl,--disable-new-dtags` —— 与
  `shared_library_link_flags`(`src/build/plan.cppm:391`)同层,**不是**
  `flags.cppm:797` 那个 `supports_rpath` 块(那里是整条链接规则共用的,分不出 kind)。
  ⚠️ **必须排在任何 `--enable-new-dtags` 之后**(后出现者胜出)。
  `post_install.cppm:321` 把 `--enable-new-dtags` 写进 clang 的 `.cfg`,
  配置文件参数在命令行参数之前,所以命令行会赢 —— **但这一条必须被验证,不能被推理**:
  gcc 与 clang 两条路径都要有断言。
  (xlings 的第一版包装器就是把参数放在前面,标签仍是 RUNPATH,而当时只测了 GLX ——
  GLX 本来就通,所以没暴露。)
- **验证 —— mcpp 侧的 rule E**:链接后解析产物 dynamic section,断言标签符合上表。
  - 读法:**原生解析**,`src/platform/elf_runtime.cppm` 已经在读
    `DT_RPATH`/`DT_RUNPATH`(`:395`/`:400`),**不 shell out**(本机 binutils shim 已损坏,§1.4)。
  - 必须**读完整个 dynamic section**:两个标签同时存在时加载器忽略 DT_RPATH,
    而 DT_RPATH-first 是常见布局 —— 只读第一个命中会读反。
  - 节奏:**warn-first**,与 closure_check rule A/D 的既定推进方式一致。
- **判据(e2e `214_executable_carries_dt_rpath.sh`)**:
  gcc 与 clang 各建一个 bin + 一个 shared lib;bin 必须 `(RPATH)`,lib 必须 `(RUNPATH)`。
  **证伪**:去掉那个 flag,bin 那条必须变红 —— 而且**测试自己要先断言默认值是 RUNPATH**,
  否则将来链接器默认一变它就静默空转(xlings 的钻机踩过这个)。

### E. dev 档 —— 需求必须落到物,且物的身份一致

- **改**:
  1. **原生解析求解**(§2.1):dispatch 的 DT_RPATH → vendor 目录 → 解符号链接 →
     真实载荷路径,与声明的 provider 身份比对。**不读任何一方的记录。**
  2. `mcpp why runtime` 的 `artifacts:` 段:把 `(none declared)` 分成
     **`(not declared by the environment)`** 与 **`(declared, N artifacts)`** 两种,
     并对每个 artifact 校验上面那条身份一致性。
  3. 结论**三值**:`OK` / `MISMATCH` / `NOT_DECLARED`。
  4. `.wiring` 存在时**附加**显示每个 vendor 的 `state`,标注按本产物标签重新求值;
     缺失 / 未知 state ⇒ `unverified`,未知 key ⇒ 忽略。**它永不单独定生死。**
- **为什么这条不需要懂 GL**:本机现状是 `xlings install graphics` 声明 rebind 到
  `nvidia-gl-host-link@0.1.2`,而 `subos/default/lib/libGLX_nvidia.so.0` 仍解析进
  **0.1.1** 的载荷。"声明 0.1.2,解析到 0.1.1" 是**纯路径事实**。
- **一致性而非新机制**:mcpp **已经对 glibc 严格执行这条** —— `glibc@2.44` 只解析那一个
  载荷,陈旧即错误,绝不从多个已装版本里挑"看起来能用的第一个"(#400)。
  套到 runtime artifact 上是把同一条规则用第二次。
- **判据**:本机当前状态必须报 `MISMATCH` 并指出 vendor 绑定陈旧;
  **不得报 `pass`,也不得报 `NOT_DECLARED`**(`.wiring` 是有的)。
  在一个没有图形栈的 home 上必须报 `NOT_DECLARED`。

### F. pack 档 —— 四个模式已经有了,问题是没有一个对图形应用成立

`mcpp pack` **已经是四档**(`src/pack/pack.cppm:30`),不缺模式:

| `--mode` | 内部名 | libc / 普通依赖 | 目标机器要有什么 |
|---|---|---|---|
| `system` | `None` | 全部来自宿主 | 一个足够新的发行版 |
| `vendored`(默认) | `BundleProject` | 第三方 `.so` 随产物,libc 来自宿主 | 一个足够新的 libc |
| `self-contained` | `BundleAll` | **连 libc / ld.so 一起自带** | 几乎什么都不要 |
| `static` | `Static` | musl 全静态,无 PT_INTERP | 什么都不要 |

**把"图形应用"这一列填上,四个格子里有两个是结构性矛盾:**

| `--mode` | 普通应用 | **图形应用** |
|---|---|---|
| `system` | ✅ | ✅ 驱动来自宿主,本来就该这样 |
| `vendored` | ✅ | ✅ **这是图形应用的正确默认档** |
| `self-contained` | ✅ | ❌ **结构性矛盾**(见下) |
| `static` | ✅ | ❌ **结构性矛盾**(§G) |

> **`self-contained` + 图形 = #392 的镜像。**
> `BundleAll` 让 PT_INTERP 指向**自带的 ld.so**、进程里跑**自带的 libc**。
> 而宿主的 vendor(`libGLX_nvidia.so.0` 等)是**必须来自宿主**的 —— 它与内核模块锁步、
> 且 EULA 禁止再分发。当 `libGLX.so.0` `dlopen` 它时,那个宿主 `.so` 的 DT_NEEDED
> (`libc.so.6` / `libm` / `libdl`)会解析到**自带的那份**。
> 私有 libc 与宿主二进制相遇即死 —— #401/#392 已实测过这一族崩溃,方向相反、机制相同。
>
> 所以 `self-contained` 对图形应用的实际含义是:**"自包含,除了决定你能不能看见东西的那一部分"**。
> 今天它链得过去,然后静默走软件渲染或直接死。**这一档必须在 pack 期被诊断,不能静默产出。**

在这个基础上,现状**已经有一半**:`src/pack/pack.cppm:653` 对**主二进制**设了
`$ORIGIN/../lib`。缺四件:

| # | 缺什么 | 后果 | 改哪 |
|---|---|---|---|
| **F1** | `patchelf --set-rpath` **默认写 DT_RUNPATH** | 打出来的 GL 程序拿不到 GPU —— 与 §D 同一个缺陷,在分发侧再犯一次 | `pack.cppm:350` 加 `--force-rpath`(仅可执行文件) |
| **F2** | **只有主二进制被重写**;bundle 进来的 `.so` 保留构建机 RPATH | 实测图形产物 RUNPATH 里三条 `<xlings-store>/…` 绝对路径,换机器即死 | 对 `toBundle` 里每个 ELF 同样重写为 `$ORIGIN` |
| **F3** | 闭包按 **DT_NEEDED** 求 | vendor / EGL vendor JSON / Vulkan ICD 全是 `dlopen` 来的,**`ldd` 永远看不见** | 见下 |
| **F4** | 没有宿主要求清单 | 用户拿到一个"缺了什么、缺在哪"说不出来的包 | 产出 `HOST-REQUIREMENTS`(见下) |

**F3/F4 的设计要点 —— 驱动不能打包,也不该打包。**
NVIDIA 用户态与内核模块锁步且 EULA 禁止再分发;宿主 Mesa 同理绑宿主内核/DRM。
所以 pack 的正确行为**不是**把它们抓进来,而是:

```
bundle:  dispatch(libglvnd)+ DT_NEEDED 闭包 - 宿主耦合者
declare: 宿主必须提供的能力清单  →  <prefix>/HOST-REQUIREMENTS
```

清单每行 = 一条 `RuntimeRequirement`,加上"加载器靠什么找到它"。四个入口点是**独立的加载链根**,
发现方式各不相同,所以清单必须逐入口点写,不能合并成一句"需要 OpenGL":

```
# HOST-REQUIREMENTS — 这些必须由宿主提供,不能打包(内核锁步 / 许可)
capability=opengl.glx.driver   discovery=rpath-of-dispatch   soname=libGLX_<vendor>.so.0
capability=opengl.egl.driver   discovery=json-dir            env=__EGL_VENDOR_LIBRARY_DIRS
capability=opengl.gles.driver  discovery=glvnd-dispatch      soname=libGLESv2.so.2
capability=vulkan.icd          discovery=json-dir            env=VK_ICD_FILENAMES
```

`discovery` 那一列不是装饰:EGL 的 JSON 里 `library_path` 是**绝对路径**,
所以"把目录搬过去"对 EGL 无效而对 GLX 有效 —— 用户拿到清单才知道该怎么补。

- **判据(e2e `215_pack_has_no_build_machine_paths.sh`)**:
  `mcpp pack` 之后,遍历 bundle 里**每一个** ELF,原生解析 RPATH/RUNPATH,
  **不得出现任何以 xlings store 前缀开头的路径**;可执行文件必须 `(RPATH)`;
  bundle 的 `.so` 必须 `(RUNPATH)`;`HOST-REQUIREMENTS` 必须存在且非空(当有能力需求时)。
  **证伪**:去掉 F2 的重写,这条必须变红。
- **未验**:packed 图形程序在一台**没有 xlings** 的机器上真的能跑 —— 需要第二台机器,
  归 §6 风险。判据只能验到"产物里没有构建机路径",这一条**必须在文档里说出来**,
  不能让"e2e 绿了"被读成"分发验证过了"。

### G. 自带 libc 的档 —— 遇到能力型需求必须拒绝

原先这一条只写 `static`。核完 §F 之后规则要放大一格,因为 `self-contained` 与 `static`
坏在**同一件事**上:

> **一旦产物自带 libc,它就不能再消费一条"必须由宿主在运行期满足"的需求。**

- **规则**:当产物自带 libc(`--mode static` 或 `--mode self-contained`,以及
  `--target *-musl` 静态链接)且解析后的 plan 里存在 `phase == "run"` 的能力型
  `RuntimeRequirement` 时,**在 plan / pack 期失败并说明理由**。
- **为什么用这个判据而不是"驱动名单"**:名单要维护、会漏、且"是不是驱动"是个含糊问题。
  "**这条需求要由宿主在运行期满足**"是**已声明的数据**,不是猜测 —— 也不需要 mcpp 认识 GL。
- **为什么两个模式一条规则**:`static` 是"没有 libc",`self-contained` 是"自己的 libc",
  对**宿主提供的 `.so`** 而言后果相同 —— 它带着自己对宿主 libc 的要求进来,而进程里没有那份。
- **今天的行为**:两档都链得过去,运行时崩或静默降级 —— 最坏的一种失败。
- **降级出路**:错误信息里给出可行档
  (`--mode vendored` 保留第三方 `.so` 自带、libc 与驱动都来自宿主)。
  **拒绝必须给出下一步,否则用户只会去关掉这个检查。**
- **判据(e2e `216_selfcontained_refuses_host_capability.sh`)**:
  `--mode static` 与 `--mode self-contained` 各拉一个带 `[runtime] capabilities` 的依赖,
  两条都必须红,且错误里要说**是哪一条能力、为什么自带 libc 与它不相容、改用哪一档**。
  **证伪**:换成 `--mode vendored`,两条都必须绿。

### J. xlings 包格式:要,但它不是第五个 pack 模式

**问题**:普通应用有四档就够了(§F);**图形应用连 `self-contained` 都不能真正自包含** ——
驱动那一格永远只能来自宿主。tarball 能做到的极限是**描述**这个要求(F4 的 `HOST-REQUIREMENTS`),
做不到的是**满足**它:目标机器上 libglvnd、vendor 桥、EGL vendor JSON 目录该怎么摆,
tarball 说不了也管不了。

**能满足它的只有一种东西:一个能声明依赖、由目标机器解析的包。** 那就是 xpkg。

**而这几乎不是新工作 —— 三样东西都已经在**:

| 已有 | 位置 |
|---|---|
| xpkg 产出路径(`mcpp publish` / `mcpp emit xpkg`) | `src/publish/pipeline.cppm` |
| xpkg **已能表达应用**(`kind = bin`) | `src/manifest/xpkg.cppm:1438` |
| xpkg **已能表达 `[runtime].requirements` / `.artifacts`** | `src/manifest/xpkg.cppm:1818` / `:1892` |

> **F4 的产出就是 xpkg 描述符的输入。** `HOST-REQUIREMENTS` 里的每一行
> (`capability=` / `discovery=` / `soname=`)本来就是一条 `RuntimeRequirement`。
> 做完 F4,xpkg-app 只剩把同一份数据投影到描述符的 `[runtime].requirements`,
> 外加一条 `deps = { "xim:graphics" }` 之类的声明 —— **由目标机器上的 xlings 去装配那条链**。

**为什么不做成 `mcpp pack --mode xpkg`**:`pack` 的四档全都产出**自足的 tarball**
(拿到就能解压运行,程度不同而已)。xpkg 不是"更自包含",它是**把解析推迟到目标机器**,
前提是那台机器**装了 xlings**。把它塞进 `pack` 会让 `--mode` 这一维同时表达两件事,
而用户看到的是同一个词。**它属于 `publish` —— 那一维本来就是"交给索引去解析"。**

| 分发目标 | 命令 | 目标机器要有 | 图形应用可用? |
|---|---|---|---|
| 通用 host | `mcpp pack --mode system` / `vendored` | 足够新的发行版 / libc | ✅ |
| 自包含 | `mcpp pack --mode self-contained` / `static` | 几乎什么都不要 | ❌ §F/§G 硬拒 |
| **xlings 生态** | **`mcpp publish`(应用 role)** | **装了 xlings** | ✅ **唯一能自己装配驱动链的** |

**本轮范围**:做 F3/F4(数据),**并把 xpkg-app 的 role 打通到能产出描述符**;
不做索引侧的应用分发策略(命名空间、`xlings install <app>` 的 shim 归属)——
那牵到"裸名 shim 归属"这条已经弄坏过用户环境的旧账,单独设计。

- **判据**:同一个图形工程,`mcpp publish` 产出的描述符里
  `[runtime].requirements` 必须**逐条等于** `mcpp pack` 产出的 `HOST-REQUIREMENTS`。
  **两处不能各推导一次** —— 它们是同一份数据的两种投影。
  **证伪**:改掉其中一处,一致性检查必须变红。

### H. #392 收口

`2026.8.10.1`(PR #400)已落地三条:私有 glibc 按精确身份解析、Rule A/B 链接期闭包校验、
`mcpp run` 不再把私有 glibc 泄漏给子进程。issue 报告的"fixup 按字典序扫描"因此消失。

剩下的物理事实(私有 glibc 与宿主 loader 相遇即死)归 openxlings/xlings#525。

- **本轮动作**:发布后在报告者的原始场景上复核,由**报告者**确认后再关。
  **不替他宣布修好。**

### I. mcpp-index 侧

| # | 动作 | 说明 |
|---|---|---|
| **I1** | `MCPP_VERSION` → 新版本 | **这一条单独就会让 8 个图形成员由 FAIL 转 ok**,以及让 `graphics install: no side effects` 那个 job 转绿(它红在同一句) |
| **I2** | `min_mcpp` / `latest_mcpp` 按 `validate.yml:62–87` 已写明的 lock-step 规则同步 | ⚠️ 索引是数据、mcpp 是程序:**发布数据不得让已发布的程序失效**(记忆:index floor 把旧客户端变砖) |
| **I3** | cron 全量跑红了要**被看见** | 现在 cron 已跑全量但**红了不拦任何东西**;至少要自动开 issue 或发通知。否则全量跑和不跑没有区别 |
| **I4** | 一条真的创建 GL context 的 e2e,**三值** | `PASS` / `FAIL` / `NOT_EXERCISED`(无 display 或无驱动)。没有三值,它只能在"跳过"和"失败"之间二选一,而**跳过会被读成没问题** |

- **判据(I1)**:bump 后的全量 `validate` 里,`gui-stack` / `imgui-window` /
  `eui-neo` ×5 八个成员全部 ok。**证伪**:把 `MCPP_VERSION` 退回 `2026.8.8.2`,必须重现 8 红。
- **⚠️ 顺序**:I1 必须等 mcpp 发布**并进入索引**之后。判据是**索引 main 的 latest 指向它**,
  不是"release 页面有了"。

---

### 版本号

一档全发 ⇒ 一个版本承载 A–I。按 `YYYY.M.D.N`(月日不补零、`.0` 保留给正式版):
当日落地则 **`2026.8.10.2`**,跨日则顺延为当日的 `.1`。
`.xlings.json` 的 bootstrap pin **不跟着这次改** —— 它在发布并进入索引之后单独收尾。

---

## 4. 顺序与依赖

```
A(pin)───┬────────────────────────────────────→ 发布 ──→ I1 ──→ I2/I3/I4
B(#405)──┤                                        ↑
C(#407)──┤                                        │
D(标签)──┤                                        │
E(dev)───┤                                        │
G(拒绝)──┤                                        │
F(pack)──┴──→ J(xpkg-app,投影 F4 的同一份数据)───┘

H(#392 复核)  发布之后,由报告者确认
```

- **A / B / C / D / E / F / G 七项两两独立**,可并行实现,一次发布。
- **E 不再依赖 D**(第一版依赖):主判据改成 mcpp 原生解析路径身份(§2.1),
  只有可选的 `.wiring` 标注用到本产物的标签,而那一段不承重。
- **J 依赖 F4**:xpkg 描述符的 `[runtime].requirements` 与 `HOST-REQUIREMENTS`
  是同一份数据的两种投影,**必须一处推导**。
- **I1 依赖发布**;I2–I4 依赖 I1。
- **B 与 C 都碰 `build.ninja` 的生成/消费**,但改的是不同段落,无冲突;
  各自的 e2e 互相不遮蔽(B 断言 phony 内容,C 断言 header 与图形态)。
- **D 与 F1 是同一条规则的两次施工**(链接期 / patchelf 期),
  实现时共用一个"这是可执行文件吗"的谓词,**不要各写一个**。

---

## 5. 判据总表(每条都必须先证伪)

| 项 | 判据 | 证伪方式 |
|---|---|---|
| A | 干净 home 下 `xim:libglvnd@>=1.7.0.1` 解析到 `1.7.0.1` | pin 退回 `2026.8.8.1` → `not found` |
| B | `212`:A(miss)OK 且 B(hit)OK;`_mcpp_staged_cache` 行含 `std.gcm` | 撤掉 push_back → 变红。**测试不得先删产物** |
| C | `213`:`build→test→build` 后 target 被重链;坏测试文件不影响 `mcpp build` | 去掉 header 检查 → 变红 |
| D | `214`:gcc/clang 两路,bin `(RPATH)`、lib `(RUNPATH)` | 去掉 flag → 变红;**测试先断言默认是 RUNPATH** |
| E | 本机报 `MISMATCH` 并指出 vendor 绑定陈旧;无图形栈的 home 报 `NOT_DECLARED`;**删掉 `.wiring` 后主判据不变** | 伪造一致的绑定 → 转 `OK` |
| F | `215`:bundle 内**每个** ELF 的 RPATH/RUNPATH 无 store 前缀;清单非空 | 去掉 F2 → 变红 |
| G | `216`:`static` 与 `self-contained` 两档 + 能力需求 → 都红且给出 `vendored` 出路 | 换 `--mode vendored` → 两条都绿 |
| J | `mcpp publish` 的 `[runtime].requirements` 逐条等于 `HOST-REQUIREMENTS` | 改掉其中一处 → 一致性检查变红 |
| I1 | 全量 validate 里 8 个图形成员 ok | `MCPP_VERSION` 退回 → 8 红 |
| I4 | 有 display 的机器 `PASS`;无 display `NOT_EXERCISED`(**不是** skip) | 拔掉 display → 必须是 `NOT_EXERCISED` 而不是绿 |

---

## 6. 风险

| 风险 | 实测/评估 | 处置 |
|---|---|---|
| **D 的爆炸半径**:DT_RPATH 优先级高于 `LD_LIBRARY_PATH`,用户不能再覆盖 | mcpp #401 **已经**把 `LD_LIBRARY_PATH` 注入删掉了,subos 里也根本不设它 ⇒ **今天不破坏任何现存行为**;代价在未来的覆盖能力 | 接受。真需要时按 xlings E1c 的形状加**显式声明的退出**,不靠推断 |
| **D 只在一台机器上验过**(NVIDIA/X11/x86_64) | xlings 那份设计的 E5 同样欠着 | rule E **warn-first**,不转硬门禁;跨硬件覆盖单列 |
| **F 的"能跑"没验** | e2e 只能验"产物里没有构建机路径" | 文档里明说;真正的验收需要第二台无 xlings 的机器 |
| **A 的 pin 跨了 5 个 xlings 版本**(`.9.2`→`.10.4`) | 其中 `.10.4` 会在装机时跑 rule E,可能对**旧 elfpatch 打过标签的已装载荷**发 warn | 那正是 rule E 该说话的场景;warn 不拦安装 |
| **C 删掉 `forget_build_cache_entry`** | `211` 已覆盖可观察行为 | 先加 header 检查并让 `211` 绿,再删函数;两步不要合成一步 |
| **xlings E2b 若无退出即落地** | 已实测 `<subos>/lib` 带 `libc.so.6` + `ld-linux`;mcpp 确实链过那个 `ld` | §1.3.1 的两条接口要求必须在 E2b 之前谈妥。**这是本设计唯一一条跨仓阻塞项** |

---

## 7. 明确不做

- **不在 mcpp 里探测 GPU / Mesa / NVIDIA / WSL / ICD / driver。** mcpp 算路径事实,不做发现。
- **不让产物自适应硬件**(§2.2)。适配是加载器在目标机器上的事;
  构建工具只看得见构建机,再实现一遍必然更差。mcpp 的职责是**不阻断**适配。
  唯一合法的"看硬件"是**测试的运行条件**(有无 display),不是产物的内容。
- **不把 `.wiring` 放在承重位上**(§2.1)。能自己算的事实,不去读别人的结论 ——
  尤其当那份结论的条件语义还没被表达出来时。
- **不在 `mcpp pack` 里加第五个 `--mode`**(§J)。xpkg 不是"更自包含",
  是"把解析推迟到目标机器",它属于 `publish`。
- **不在构建过程中执行 provider 提供的探针。** dev 档的收益已被"物 + 身份"覆盖;
  pack/static 档探针问的是**另一台机器**(autoconf `AC_RUN_IFELSE` 三十年的教训);
  且在构建期执行第三方可执行文件是一大块不必要的安全面。
- **不改 `xim-pkgindex` 的 libglvnd 约束。** §1.2:那个洞已经修好了。
- **不把 `<subos>/lib` 放进任何环境变量或全局搜索路径。** #401 已实测:
  那个目录同时装着 vendor 库**和私有 glibc**,任何"按目录可达"的修法都会让宿主二进制
  立刻死于 `__pointer_chk_guard`。**vendor 只能按对象可达(RPATH)。**
- **不把 `tests/` 加进 `sources_newer_than`。** §C:两张图分开之后不需要第二个机制。
- **不动 `compat.glx-runtime` 的 dispatch RPATH** —— 它已经是对的。
- **不替 #392 的报告者宣布修好。**

---

## 附:这一轮的三条方法论

1. **`grep` 一次生成物,胜过读三遍源码。**
   #405 的根因错了两次,而 `grep std.gcm build.ninja` 三秒就定死了它。
   **生成物是判据,源码是假设。**

2. **"我这一片是绿的"从来不等于"它能用"** —— 这一轮的第五、第六个实例:
   - index `main` 绿 ≠ 图形包装得上(`workspace` job 是 `skipped`)
   - **8 个成员全红了两周,而没有任何一方的 CI 认为自己红了**
   - 缓存第一次通过 ≠ 第二次通过(#405,miss 与 hit 是两条路径)

3. **测量工具与被测对象共享同一个错误假设时,它们会一致地错。**
   对账工具没抓到标签缺陷,因为它自己的探针也是默认 dtags 编的 ——
   它复现的正是记录所描述的那个失败。
   本文所有 ELF 判据因此都要求:**测试先断言自己的形态**
   (`214` 先断言默认是 RUNPATH,否则将来链接器默认一变它就静默空转)。
