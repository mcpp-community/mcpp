# 一个谓词族,六处缺陷:目标侧的分层判据

2026-08-25 · 缺陷分析 + 修复方案(六条中两条已修,四条待定)

本文不是缺陷清单。六处缺陷共享**同一个形状**,而那个形状源于一处架构疏漏;
逐条修完仍会有第七条,除非把疏漏本身补上。

每一条都给出实测依据。凡未实测的推断,明确标注。

---

## 0. 一句话

> **mcpp 用「图有没有供给系统」这一个跨两层的谓词,回答了六个不同的问题,
> 而其中五个只取决于两层里的某一层,第六个两层都不取决于。**

```cpp
bool system_from_graph() const {
    return kernelAbi.fromGraph() || cAbi.fromGraph();   // ← 这个 OR
}
```

它成立的那一天是对的:在作者面前的那个排布里,两层总是一起来自图。
第二种排布出现时——**后端跑在平台之上**,内核接口来自图而 C 库仍是载荷的——
两层分开了,而六处判据全部答错。

---

## 1. 现状

### 1.1 已发布

| | 状态 |
|---|---|
| `2026.8.25.1` | 已发布,索引 artifact 已核验(`xim-index-1956eb7` 内含该版本) |
| main CI | 8/8 绿 |
| PR #506(`2026.8.25.2`) | 27 项全绿,**待合入** |
| PR #505(镜像校验补重试) | 待合入 |

### 1.2 七个下游 pin PR

| 仓库 | 状态 | 成因 |
|---|---|---|
| openkal | 12/12 绿 | — |
| openkal-linux | 2/2 绿 | — |
| openkal-macos | 2/2 绿 | — |
| openkal-opensbi | 1 红 | **缺陷 ⑤**,已修(#506) |
| openkal-windows | 1 红 | **缺陷 ⑥**(issue #507) |
| openkal-uefi | 1 红 | **缺陷 ⑥**,同上 |
| openkal-musl | 1 红 | **缺陷 ⑦**,本文 §2.7,尚未修 |

---

## 2. 六处缺陷,一个形状

编号沿用发现顺序。①–④ 已随 `2026.8.25.1` 修复,⑤ 已修待合,⑥⑦ 待定。

### 2.1 ① 链接线丢掉载荷 C 库的路径

```
error: hermetic link check failed
         crt1.o (bare name — the linker cannot resolve it)
```

判据用 `system_from_graph()` 决定「要不要整条替换链接线」。内核接口来自图
⇒ 替换 ⇒ 载荷 C 库的 `-B` 一起没了。**决定它的是 C 库那一层**。

修:`!cAbi.prebuilt()`。

### 2.2 ② 缓存键跨着一处不兼容命中

`compile_flags(spec)` 长出第二个参数 `targetCxxRuntime`,而缓存键仍按一个参数算。
实测 `openkal@0.7.0` 出现 6 个槽位对应 5 个尺寸各异的 BMI。

⚠️ 这类缺陷不在加参数那天失败,在下一次缓存命中时失败。

### 2.3 ③ 契约表拿掉了载荷的 C++ 运行时

```
undefined reference to __cxa_allocate_exception
```

同样的 OR。理由 `check_layering` 早已反向陈述:载荷的 C++ 运行时是**对着载荷的
C 库**配置的,当且仅当那份 C 库在用时它才可用。

### 2.4 ④ `linkage = "dynamic"` 的「无效」诊断在说谎

```
warning: `linkage = "dynamic"` has no effect … The artifact is static.
$ readelf -d → NEEDED libm.so.6, libgcc_s.so.1, libc.so.6
```

警告自己给的理由(「那些包被当作对象编进本次构建」)是 **C 库单独一层**的性质。
载荷的 libc 有共享对象,`dynamic` 就被兑现了。

### 2.5 ⑤ 声明一层,让裸机目标丢掉唯一能产出它的编译器

**三行清单即可复现**:

```toml
provides = ["mcpp:kernel-abi=openkal"]
```
```
$ mcpp build --target riscv64-none-elf
  Resolved gcc@16.1.0 → riscv64-none-elf → …/bin/g++
  g++: error: unrecognized argument in option '-mabi=lp64d'
  g++: error: unrecognized command-line option '--target=riscv64-none-elf'
```

`prepare.cppm:4837` 的 `graphSuppliesSystem`(同一个 OR)取消目标行的编译器 pin。

**对宿主行这是对的**——那一行说的是「哪个载荷供给这个目标的 C 库」,图供给了就
用不上它。**对裸机行不是**——目标表自己写着「pin 是 llvm,因为 clang/lld 按构造
就是交叉编译器」。宿主 g++ 发不出 riscv64,图里有什么都不改变这件事。

⭐ **这一条揭示了比前四条更深的东西:pin 有两种,而代码里只有一种。**
一种是约定(哪个载荷供给 C 库),一种是能力陈述(哪个编译器能发出这个目标)。

修:PR #506,区分记在**读取那一行的同一处**,不在决定点重新推导。

### 2.6 ⑥ 服务不了的目标被宿主静默服务了

```
Target x86_64-windows-gnu → x86_64-unknown-linux-gnu
…
src/stream.cpp:68:9: error: 'GetFileType' was not declared in this scope
```

一行里两个操作系统。CI 只装了原生 gcc,mcpp 既不装 mingw 载荷也不拒绝,
直接用宿主 gcc 服务了 Windows 目标。openkal-uefi 是同一条,症状在链接器:

```
…/xim-x-binutils/2.42/bin/ld: unrecognized option '--subsystem'
```

**对照**:装了载荷的机器上同一条命令完全正确
(`Resolved gcc@16.1.0 → x86_64-windows-gnu → …/x86_64-w64-mingw32-g++`)。

⭐⭐ **真因在「首次运行」那条路上,而它把 `--target` 丢了。** 交叉验证把它逼了
出来(CI 2026-08-25):

```
First run  no toolchain configured — installing gcc@16.1.0 (glibc, native ABI)
 Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
                                   ↑ 路径里没有目标
```

同一条命令在已有工具链的机器上是:

```
 Resolved  gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…/x86_64-w64-mingw32-g++
```

这条分支回答的是「这台机器没有工具链,给它一个」,而答案是一份**宿主**载荷;
`overrides.target_triple` 在这条路上**从未被读取**。于是一台从未构建过任何东西的
机器上,`mcpp build --target x86_64-windows-gnu` 装了原生 gcc 并用它编译 Windows
源码——而载荷解析那条路上 `autoInstall=true` **本来就在**,只是没被走到。

修法不是加一个条件,而是让首次运行**汇入**那条已经会处理目标的路径:装完默认之后,
若请求了目标,就拿这个默认重新解析一次。为此 `resolve_target_toolchain` 由 `auto`
改为 `std::function`(它要回调自身),深度为一——第二遍走的是首次运行刚刚让其成立的
`tcSpec.has_value()` 分支。

⑥ 因此有两半,而两半都需要:**守卫**让错配变成一句拒绝而不是一百行后的 Win32 报错,
**首次运行汇入**让本来就能服务的目标不再走到那句拒绝。

issue #507。

### 2.7 ⑦ macOS 上「图供给系统」不等于「平台什么都不需要」

```
Target arm64-apple-darwin23.6.0 → arm64-apple-macos14.0
       kernel-abi   openkal   (openkal-macos@0.3.4, graph)
       c-abi        musl      (openkal-musl@0.3.5, graph)
ld64.lld: error: library not found for -lSystem
ld64.lld: error: undefined symbol: clock_gettime_nsec_np
ld64.lld: error: undefined symbol: pthread_create_from_mach_thread
```

**判据**:`macos, llvm` 这一格在 main(mcpp `2026.8.19.4`)**通过**,在
`2026.8.25.1` 上失败。是同一跨度里的回归。

⚠️ **不是 `2026.8.25.1` 引入的。** 这个排布下,旧谓词 `system_from_graph()` 与新
谓词 `!cAbi.prebuilt()` **取值相同**(c-abi 来自图),所以 §2.1 的修改没有改变
这一格的行为。它来自 #486 引入替换本身。

⭐⭐ **架构层面的真因:在 Darwin 上,内核接口就是 libSystem。**

openkal-macos 的清单写着 `ldflags = ["-nostdlib", "-lSystem", …]` ——
它**包裹** libSystem 而不是替换它。而 `flags.cppm:1458` 的替换把整条 `f.ld`
换成 `crossTarget + 少数几个 flag`,SDK 的库搜索路径随之消失,于是 `-lSystem`
找不到。

这不是 macOS 的特例,是模型的缺口:**「图供给了这一层」与「平台的那一层不再被
链接」是两件事**,而代码把它们当成了一件。Linux/musl 上二者恰好重合(musl 是
自足的),Darwin 上不重合。

---

## 3. 架构疏漏:缺的是「谁被替换」这一维

五层模型回答了**每一层来自哪里**(`Origin::{Payload,Xpkg,Graph,None}`),
这是对的,六处缺陷都不是因为这个模型错。

缺的是第二个问题:

> 一层来自图,**平台的那一层是否因此不再参与链接**?

| 排布 | c-abi 来源 | 平台的 C 库还参与吗 | 现状判定 |
|---|---|---|---|
| 传统栈 | Payload | 是 | ✅ 正确 |
| openkal 全图栈(Linux/musl) | Graph | 否 | ✅ 正确 |
| 后端跑在平台上 | Payload | 是 | ①③④ 曾判错,已修 |
| 裸机 | None | 无此物 | ✅ |
| **openkal on Darwin** | **Graph** | **是**(libSystem 是内核接口本身) | ❌ **⑦,未修** |

最后一行是模型里没有的格子。它不是边角情形——它是「一个实现包裹平台接口」的
一般形态,而 openkal 的设计前提正是「后端可以跑在平台之上」(①③④ 修的就是
这个前提在 Linux 上的那一半)。

---

## 4. 修复方案

### 4.1 立即(不改模型)

| # | 动作 | 风险 |
|---|---|---|
| A1 | 合入 #506(⑤)与 #505(镜像重试) | 低,均已全绿 |
| A2 | ⑦ 的止血:替换链接线时**保留 sysroot / SDK 的库搜索路径**,只替换启动对象与目标选择 | 中,需 Darwin 实测 |
| A3 | ⑥ 的止血:请求的目标无载荷时**惰性安装或拒绝**,不得回落宿主三元组 | 中,涉及安装路径 |

⚠️ A2 的判据必须落在 **Darwin 真机/真 runner** 上——本机是 Linux,`-lSystem`
这一格在这里无法复现。

### 4.2 结构(补上缺的那一维)

给 `TargetSide` 增加**一个**问题的答案,而不是给每处判据加一个条件:

```cpp
// 平台自身的 C 库是否仍参与链接。
//
// 「这一层来自图」不蕴含「平台的这一层不再被链接」。musl 是自足的,
// 所以在 Linux 上二者重合;libSystem 既是 Darwin 的 C 库也是它的内核接口,
// 一个包裹它的实现仍然要链接它。
bool platform_c_library_still_links() const;
```

来源:目标 OS + c-abi 的实现是否声明自己包裹平台(**新增清单键**,
例如 `wraps = ["platform-c-library"]`,由 openkal-macos 声明)。

⭐ **不要用 OS 判断。** `if (os == "macos")` 会在下一个同形平台(illumos、
某些 BSD)上再错一次,而且把生态的性质写进了引擎——正是 `subos_info.cppm`
的模块注释点名反对的分层倒置。**由包声明,引擎读取。**

### 4.3 防止第七条

六处缺陷全部通过了当时的测试。共同点:**判据施加在正确的对象上,但那个对象
回答的是另一个问题**。

- ⭐ 新增谓词时,写下它回答的**那一个**问题,并列出**每个 `Origin` 值**下的答案。
  五条 targetside 单元测试就是按这个写的(一个 `Origin` 一条),它们在 ①③ 上有效。
- ⭐ 跨层的 OR/AND **必须**在注释里说明为什么两层都参与。§2.5 里保留的两处
  `system_from_graph()` 各有一句;新增的没有就不许合。
- ⚠️ 单元测试打在模型上抓不到「选错谓词」——模型是对的。抓得到的是 e2e,
  前提是那些 e2e **真的在 CI 跑过**(见 §5)。

---

## 5. 测试与 CI:两个已确认的空洞

### 5.1 写了没跑

`285`–`289` 声明 `# requires: llvm`,而两个 linux e2e shard 报的能力行是

```
Detected capabilities: elf unix-shell fresh-sandbox gcc patchelf pack …
```

**没有 `llvm`**,shard 从不装,`run_all.sh` 在 skip 时退 0。五条专门衡量这个生态
的测试一次都没执行,而套件一直绿。

已修:`openkal-cross.yml` 新增 `ecosystem-e2e` job——装 gcc + llvm,直跑六条,
再**逐条断言 PASS 行出现**。它第一次跑就抓到 §5.2。

⚠️ 断言最终 OK 行不够:脚本内部的「运行」步骤会各自降级成 SKIP 而 OK 行照印
(288 实测)。运行阶段那一行要单独断言。

### 5.2 判据的「否」与「没测成」同读数

一次会话里我自己新写的六条 e2e,**四条**犯了它:

| 判据 | 「否」的真因 |
|---|---|
| `objdump -d aarch64.o \| grep -c 'cas\|swp'` | 宿主 GNU binutils 只编了 x86_64 ⇒ **文件头、零指令、零报错** |
| `readelf -l \| grep -q INTERP`(断言**缺席**) | 读不了的文件输出 0 行 ⇒ 与静态镜像同读数 |
| `readelf -d \| grep -c NEEDED` | 「没有动态段」与「readelf 什么都没说」同为 0 |
| `case $first in */subos/*/bin)` | **CI 自己的 PATH 本就以它开头** ⇒ 分不清谁放的 |

规则:

- ⭐ **判据带分母**:`7 LSE instructions out of 148906`,不是 `7 LSE instructions`。
- ⭐ **先确立读到了东西,再问里面有没有。** 零条 = 工具读不了 ⇒ SKIP 或硬失败,
  **不是**关于被测性质的答案。
- ⭐ **断言「没变」要前后两值并排比**,不能比对模式。
- ⭐ **工具取自产生该产物的那条工具链**,不用 `command -v`。

### 5.3 交叉验证通道:三轮红,全部是判据自身的缺陷

新加的通道让七个生态仓库从 mcpp 的 PR 分支现场构建再跑自己的测试。它从 7 红收敛
到 0 红,而**中间每一轮红都不是生态代码的问题**,是我写的那段 CI 脚本:

| 现象 | 真因 |
|---|---|
| `package 'mcpp@2026.8.25.2' not found in the synced index` | 引导安装先于构建步骤跑,而我把 pin 钉到了未发布的版本 |
| `xlings: version '2026.8.17.1' not found` | 克隆出来的 `.xlings.json` 工作区 pin 抢走了「谁来构建 mcpp」的决定权 |
| 版本号对、代码旧 ⇒ ⑤ 通过而 ⑥ 不通过 | `find … \| head -1` 挑到缓存 `target/` 里上一次推送留下的二进制 |
| 修上一条时把 GNU 的 `-printf` 写进跑 macOS 的仓库 | 那七处是**新克隆**,本就不需要按时间排序 |
| `Finished release in 173.44s` 之后报 "mcpp did not build" | Windows 上产物叫 `mcpp.exe`,而 `find -name mcpp` 找不到 |

⭐ 最后两条与本文 §2 的六条**同型**:**构建成功了,是判据看错了地方**。

⚠️ **通道本身要有判据。** 「七个全绿」不等于「它们用了 PR 的 mcpp」——那一步可能
整个没跑。收尾核对的是每个 run 的日志里出现
`under review: mcpp 2026.8.25.2 (from <branch>)`,七个全中才算数。

### 5.4 覆盖矩阵的实际空洞

| 排布 | mcpp e2e | openkal 侧 CI |
|---|---|---|
| 传统栈 | 大量 | — |
| 内核接口来自图 + 载荷 C 库 | 285 ✅ | openkal-linux ✅ |
| 三层全来自图(Linux) | 286 ✅ | openkal-musl(linux)✅ |
| 交叉到 aarch64 | 287 ✅ | — |
| 无 OS 无 C 库 | 288 ✅ | openkal-opensbi ⚠️(缺陷⑤) |
| 一宿主扫四目标 | 289 ✅ | — |
| **openkal on Darwin** | **无** | openkal-musl(macos)❌ |
| **Windows 目标无载荷的机器** | **无** | openkal-windows / uefi ❌ |

⭐ 最后两行就是 ⑥⑦ 能存活到今天的原因。**修 ⑥⑦ 的同时必须补上这两行**,
否则下一次同样看不见。

### 5.5 有三格,本机永远验不了

| 修复 | 判据在哪 | 为什么本机不行 |
|---|---|---|
| ⑤ 裸机 pin | e2e 292 | —— 本机可验 |
| ⑥a 同 OS 不变量 | e2e 293 | —— 本机可验 |
| **⑥b 首次运行汇入目标解析** | `ci-fresh-install` / `bare Windows` / 生态 CI | **开发机永远不是首次运行** |
| **⑦ macOS 平台锚点** | openkal-musl 的 `macos, llvm` | 本机是 Linux,`-lSystem` 这一格不存在 |
| #504 列表状态 | e2e 294 | —— 本机可验 |

⚠️ **这不是「测试写得不够」,是这两格的前提条件本机不成立。** 结论有两条:

1. 这类修复**必须**由跨仓库 CI 把关,而那条通道在它们被写出来之前并不存在——
   这正是 ⑥⑦ 能活到 2026.8.25.2 的机制,不是巧合。
2. ⚠️ **本机全绿不能当作可以合入的证据。** ⑥b 的第一版在本机通过了全量单元测试、
   五个目标零误伤、三条新 e2e,而它是**无限递归**:生态仓库上 `Resolved` 打四遍后
   `exit 139`(SIGSEGV,爆栈),mcpp 自己的 `bare Windows` 上 `First run` 反复打印
   后 exit 1。写下「深度为一」的注释时,我推理的是「第二趟走不到这里」——而那一行
   在分支之外,每一趟都求值,标志没有任何人复位。

   ⭐ **一个递归,若其终止依赖于「递归调用不会改变的状态」,那它就不是深度为一的
   递归——无论注释怎么写。** 闸要结构上不可能循环,标志在**调用之前**置位。

---

## 6. 发布链条:两处已证实的脆弱点

### 6.1 「发布就绪」的判据用错了对象

我按记忆里的判据(「索引 main 的 latest 指向它」)执行,读到 `2026.8.25.1`,
随即重钉七个下游 PR。七个全红:

```
[error] package 'mcpp@2026.8.25.1' not found in the synced index
        (xim@artifact:8df3b47, …), synced 0 seconds
```

**默认客户端解析的是 artifact 快照,不是 git main。** 两者之间隔着
`publish-artifact.yml`。

⭐ 正确判据:把 artifact 取下来读它。

```sh
curl -fsSL -o p.json .../xim-index-latest.json     # source_commit 要等于索引 main 的 sha
curl -fsSL -o a.tar.gz .../xim-index-<sha>.tar.gz
sha256sum a.tar.gz                                  # 与 p.json 的 artifact.sha256 比对
tar xzf a.tar.gz && grep '\["latest"\]' */pkgs/m/mcpp.lua
```

### 6.2 一个没有重试的 GET 判掉整条发布

`2026.8.25.1` 的发布红了两次。两次都报 16 个资产「already mirrored, skipping」,
然后因其中**一个**的 502 判失败:

```
[mirror] FAIL: missing/unverified: https://gitcode.com/.../linux-x86_64.tar.gz
  502ERR  https://gitcode.com/.../linux-x86_64.tar.gz
```

手工抓下来:**5,772,395 字节,sha256 与发布的校验和逐位相同**。文件从未缺过。

已修(#505):两处校验 GET 补 `--retry-all-errors`;并修掉 `502ERR` 拼接
(`|| echo ERR` 是追加不是替换,导致状态码无法 grep)。

---

## 7. 建议的执行顺序

1. **合入 #506 + #505**,发 `2026.8.25.2`。⑤ 随之解决,openkal-opensbi 转绿。
2. **⑥(issue #507)**:目标无载荷时惰性安装或拒绝。openkal-windows / uefi 两个
   PR 依赖它。同时补 e2e:「请求一个本机无载荷的目标」两向断言。
3. **⑦**:先在 Darwin runner 上取得失败现场的完整链接命令行(`-v`),确认丢的
   确实是 SDK 的 `-L`;再按 §4.2 由包声明、引擎读取。补 e2e 到 macOS 矩阵。
4. **#504**(`toolchain list` 漏掉可构建的目标)——它是 ⑥ 的报告侧,同一处混淆,
   建议与 ⑥ 一起做,共用新谓词。
5. 回填:`2026.8.19.4 → 2026.8.24.6` 这一跨度还有没有第八条?**建议做一次
   有针对性的差分**——把 #486 触碰的每个判据列出来,逐个问「它回答的是哪一个
   问题、取决于哪一层」。六条里有五条出自那一次改动。
