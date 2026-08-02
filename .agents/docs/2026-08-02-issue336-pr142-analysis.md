# 深度分析:issue #336(macOS 静态 libc++)与 mcpp-index PR #142(boost-ext.ut)

> 日期:2026-08-02 · 分析基准:mcpp `main` @ 76152d9(v2026.8.2.2)、mcpp-index PR #142 @ `c8cbfa14`
> 验证手段:本地读码 + 本地 libc++ 20.1.7 归档反汇编 + GitHub CI/诊断作业日志
> **未做**:macOS 上的端到端复现(本机无 macOS,所有 macOS 运行期结论均标注证据来源)

---

## 0. TL;DR

- **issue #336 的两条主张都成立**,且问题二比 issue 标题暗示的更严重:它不是"测试二进制"的问题,**默认配置下任何 macOS 产物(含 `mcpp build` 出的可执行文件)只要有全局对象在静态初始化期碰 `std::cout` 就必崩**,自 0.0.50(#116)起就存在;0.0.86(#202)只是把测试二进制也拖进同一形态,并顺手废掉了唯一的逃生门。
- **问题一(`static_stdlib=false` 对测试失效)是纯粹的疏漏**,`flags.cppm:659-682` 与 `:636-658` 是同一段逻辑的两份推导,只有后者带 `f.staticStdlib` 门。这是本仓的老病灶("同一决策两处推导")。
- **issue 对问题二的根因链正确,但对"为什么包侧修不了"的归因错了**:它写的是"静态库符号的 ODR 实体分裂",而 PR #142 自己的诊断日志(`nm` 只有一个 `DoIOSInitC1Ev` 定义、shim 确实绑到它)与本地反汇编都指向**纯粹的次序问题**——shim 排在第 3 个初始化器,而崩溃点排在它前面。
- **本地反汇编推翻了两条被写进 issue/PR 的"事实"**:
  1. `std::__1::ios_base::Init::Init()` 在 libc++ 20 **不是空的**(fork PR 的 status note 说"empty");它是带 `__cxa_guard` 的函数内静态,内部调用 `DoIOSInit::DoIOSInit()`,而 `DoIOSInit::DoIOSInit()` **就是真正 placement-new 出 `cin/cout/cerr` 的那个函数**(重定位表里直接引用 `_ZNSt3__1L6__coutE` / `_ZNSt3__14coutE` / `basic_ostream` 的 vtable)。
  2. 但 `ios_base::Init` 在 libc++ 头文件里**只有前向声明**(`ios:70: class Init;`,全树无定义)。**标准为 SIOF 提供的官方解药在 libc++ 上对用户不可用**——这才是"包侧修不了"的真原因,而且它比 issue 写的理由更硬:不是修不好,是**标准接口根本没暴露**。
- 由此得到一条 **issue 没列的第 4 种修法**,它能修默认路径而不是只修逃生门:**在 macOS 静态 libc++ 时,由 mcpp 在链接行 `$in` 的最前面插一个自带的 stream-init 对象**。成本极小,收益是"默认就对"。
- **PR #142 当前不可合并**,不只是因为被 #336 卡住:它带着 155 行临时诊断 workflow、PR 正文与代码已经不一致(正文描述的是两次迭代之前的形态)、设计文档里写着一个**已被证伪的根因**,而描述符文件顶部的权威注释同样把一个**不成立的根因**(module `std::cout` ODR 分裂)写成了事实,并为此付出了实实在在的代价(丢掉 `export import std;` + 24 行 GMF + 一个 Clang 上不存在的 `-Wno-template-body`)——而该"修复"根本没修好(CI 仍 139)。

---

## 1. 事实核验表

| # | issue/PR 的主张 | 核验结果 | 证据 |
|---|---|---|---|
| 1 | `ldStdlibDefault` 尊重 `staticStdlib` | ✅ 属实 | `src/build/flags.cppm:636` `if (f.staticStdlib && !macosDeploymentTarget.empty() && !llvmRootForStdlib.empty())` |
| 2 | `ldStdlibTest` 无条件注入静态 `-load_hidden` | ✅ 属实 | `src/build/flags.cppm:673` `if (!llvmRootForStdlib.empty())` —— 无 `staticStdlib` 项 |
| 3 | 按 `LinkUnit::TestBinary` 二选一 | ✅ 属实 | `src/build/ninja_backend.cppm:1222-1223` |
| 4 | 该行为自 #202 起 | ✅ 属实 | `git log -S ldStdlibTest`:`3730d23 fix(build): macOS test binaries link the toolchain's own libc++ (A1) + unpin llvm (#202)` |
| 5 | 文档仍写着可以 opt-out | ✅ 属实 | `docs/05-mcpp-toml.md:197`、`docs/zh/05-mcpp-toml.md:181` |
| 6 | 链接行是 `$cxx $in -o $out $ldflags $unit_ldflags`(静态库在目标文件之后) | ✅ 属实 | `src/build/ninja_backend.cppm:781` |
| 7 | `static_stdlib` 不能按平台条件化 | ✅ 属实,且是**显式设计** | `src/manifest/toml.cppm:1052` `kKnownConditionalBuildKeys` 只有 8 个输入类键;`:1066` 的警告文案明确把 `static_stdlib` 与 `target`/`linkage` 一起归为"selection knobs …cannot be conditioned";`ConditionalConfig` 只携带 `BuildInputs`(`types.cppm:347-362`,注释说明这是**刻意**的防漂移设计) |
| 8 | libc++ 的 `<iostream>` 没有 `ios_base::Init` 守卫 | ✅ 属实 | 本地 libc++ 20.1.7 头树:`iostream` 里零 `Init`;`ios:70` 只有 `class Init;` 前向声明,**全树无定义** |
| 9 | 真正构造流的是带优先级的 `_GLOBAL__I_000100` | ✅ 属实 | 本地 `llvm-ar x libc++.a iostream.cpp.o` + `objdump -dr`:`_GLOBAL__I_000100` → `__start_std_streams` → `ios_base::Init::Init()` → `DoIOSInit::DoIOSInit()`,后者的重定位表直接写 `__cout`/`cout`/`basic_ostream` vtable |
| 10 | PR #142 的 macOS CI 因此挂 | ✅ 属实 | run 30749985831 job 91502184327:`Running bin/ut` → `ut ... FAIL (exit 139)`;linux/windows 两条腿绿 |
| 11 | "libc++ 20 的 Init ctor 是空的"(fork PR #1 status) | ❌ **不成立** | 反汇编见 #9;`ios_base::Init::Init()` = guard acquire → `DoIOSInit` ctor → `__cxa_atexit` → guard release |
| 12 | "包内 9 种修复全失败,因静态库符号 ODR 实体分裂"(issue) | ⚠️ **结论对、归因错** | 诊断日志 `=== nm: who defines _ZNSt3__19DoIOSInitC1Ev ===` **只有一个定义**,S5 的 `ut_doios` 反汇编里 `bl 0x10003bf64 ; std::__1::DoIOSInit::DoIOSInit()` 绑的就是它 —— 没有分裂。真原因是 shim 的初始化器排在 `__init_offsets` **第 3 位**,崩溃点在它之前 |
| 13 | 描述符注释:macOS 崩溃因 `export import std;` 造成 module 与库两份 `std::cout`(dev 1) | ❌ **不成立** | (a) issue 的最小复现是 `#include <iostream>` + 全局对象,**零模块**;(b) dev 1 已在 `3eff2899` 合入,head `c8cbfa14` 的 macOS CI **仍然 139** |
| 14 | 设计文档 §2.3:因模块 BMI 未带模板成员定义 → dispatch 落空槽 | ❌ **不成立** | 同上;且崩溃地址 `0xffffffffffffffe8` = `this + (-0x18)`,是 vptr 为零的读取,不是未实例化 dispatch |
| 15 | issue 复现段:"加 `static_stdlib = false` 依然崩" | ⚠️ **与代码不符,需在 macOS 复核** | `mcpp build && mcpp run` 产出的是 `LinkUnit::Binary`,走 `ldStdlibDefault`;`staticStdlib=false` 时它就是 `-lc++`(动态)→ 按 issue 自己的实验 E 应当**不崩**。问题一只影响 `mcpp test`。这条复现步骤大概率是把 `mcpp test` 的观察写到了 `mcpp run` 上 |

---

## 2. 问题一:opt-out 对测试二进制失效

### 2.1 机制

`flags.cppm` 在 macOS 分支里对同一件事(该链静态 libc++ 归档还是动态 `-lc++`)推导了**两次**:

```
:617-618   f.ldStdlibDefault = " -lc++";  f.ldStdlibTest = " -lc++";
:636       if (f.staticStdlib && !macosDeploymentTarget.empty() && !llvmRootForStdlib.empty())
:654-656       f.ldStdlibDefault = " -nostdlib++ -Wl,-load_hidden,libc++.a -Wl,-load_hidden,libc++abi.a";
:673       if (!llvmRootForStdlib.empty())            // ← 少了 staticStdlib 与 deploymentTarget 两个条件
:678-680       f.ldStdlibTest    = " -nostdlib++ -Wl,-load_hidden,libc++.a -Wl,-load_hidden,libc++abi.a";
```

这正是本仓反复出现的那类债:**同一决策在两处独立推导,新语义只加在其中一处**。#202 的意图(测试别再用系统 `-lc++` 配工具链头文件,那会炸 `__hash_memory`)是合理的,但它把"默认形态"和"唯一形态"混为一谈。

### 2.2 影响面

- `mcpp test` 在 macOS 上**没有任何**方式回到动态 libc++;
- `docs/05-mcpp-toml.md` 与中文版同步说谎,已经说了约一年(#124 的措辞至今未改);
- 对 PR #142 这类包,后果不是"少个选项",而是**没有任何 workaround 能让 CI 转绿**。

### 2.3 修法(issue 建议 1)代价与风险

把 `:673` 的条件对齐 `:636` 即可,~1 行。但必须同时讲清楚 **opt-out 的真实代价**,否则等于把 #202 修掉的坑重新挖开给用户踩:

- `staticStdlib=false` ⇒ 测试链系统 `/usr/lib/libc++.1.dylib`,却仍用工具链的 libc++ **头文件**编译。issue 的实验 E 在 llvm@20.1.7 上通过,但 #202 的原始故障是 **libc++ 22 把 string hashing 移出内联**导致 `undefined __hash_memory` —— 也就是说**这条路在 llvm@22 上大概率仍然坏**。
- 因此建议:合入 1 的同时,在 `staticStdlib=false` 且 macOS 且工具链 libc++ 主版本 > 系统时**发一条 schema/build 警告**,而不是静默切换。

---

## 3. 问题二:静态 libc++ 下的初始化次序(真正的缺陷)

### 3.1 完整机制链(本地反汇编逐环证实)

```
用户/依赖 TU 的全局对象 ctor  ──┐
                                ├─ 都进主可执行文件的 __init_offsets,顺序 = 链接顺序
libc++.a::iostream.cpp.o     ──┘   (归档成员按需拉取 ⇒ 永远排在目标文件之后)

iostream.cpp.o 里:
  _GLOBAL__I_000100                 ← 带优先级标记,但 Mach-O 没有 ELF 的 .init_array.<prio> 机制
    └→ __start_std_streams : ios_base::Init
         └→ ios_base::Init::Init()  ← 非空!guard acquire → ... → guard release
              └→ DoIOSInit::DoIOSInit()
                   └→ placement-new __cin/cin/__cout/cout/__cerr/cerr  ← 真正构造流的地方
```

- ELF 上不会出问题:链接器按 `.init_array.NNNNN` 段名排序,priority 101 天然排到用户全局(默认 65535)之前。
- Mach-O 上 `init_priority` **只在单个 TU 内有意义**(clang 在 TU 内部排 `__mod_init_func`),跨 TU 完全是链接顺序。
- 动态 libc++ 之所以永远正确:dyld 保证被依赖的 image 先于依赖它的 image 初始化,`libc++.1.dylib` 的初始化器整体跑在主可执行文件之前。**"静态 vs 动态"才是分界线,`-load_hidden` 与此无关**——issue 的这个判断是对的。

结论:**这是 Darwin 上"静态链接 libc++ + 全局对象用流"的通用缺陷,不是 mcpp 引入的**;但 mcpp **把它设成了默认**,并且没有诊断、没有文档、在测试路径上还没有开关。

### 3.2 为什么包侧修不了(比 issue 给的理由更硬)

issue 归因为"静态库符号的 ODR 实体分裂"。这条被 PR #142 自己的诊断日志证伪(见核验表 #12)。真实约束是两条:

1. **标准的解药在 libc++ 上不可用**。`[ios.base]` 规定 `std::ios_base::Init` 是可用的完整类型,`<iostream>` 的效果等同于声明一个 `ios_base::Init` 静态对象——libstdc++/MSVC STL 正是这么做的,所以它们不受影响。libc++ **只在 `ios:70` 前向声明 `class Init;`,全头树无定义**,用户写 `std::ios_base::Init x;` 编译不过。也就是说:**在 libc++ 上,用户没有任何符合标准的手段解决 SIOF**。
2. **剩下的都是 TU 内次序问题**。S5 实验(自行声明 `DoIOSInit` 并构造)符号绑定完全正确,唯一的毛病是它的初始化器排在 `__init_offsets` 第 3 位,而崩溃的初始化器在它之前。这**不是**"包侧修不了"的证据,而是"shim 放错位置"的证据。

> 由 #2 直接得到一条**尚未被试过的包侧修法**:把 shim 放到 `#include "ut.hpp"` **之前**(同一 TU 内声明序即初始化序)。若 `cfg` 的初始化器来自 ut.hpp,shim 前置后应当排到它之前。**这条值得在 PR #142 上先试一次**——它可能让 PR 在 mcpp 修复落地前就能自解。

### 3.3 修复选项评估

| 选项 | 是什么 | 修默认路径? | 成本 | 风险 |
|---|---|---|---|---|
| **1. `ldStdlibTest` 跟随 `staticStdlib`**(issue 建议 1) | 对齐 `:673` 的门 | ❌ 只恢复逃生门 | ~1 行 + e2e | 低;但把 #202 的 header/dylib 版本裂开重新暴露给显式 opt-out 的用户(llvm@22 上可能仍坏) |
| **2. `static_stdlib` 可条件化**(issue 建议 2) | 加进 `kKnownConditionalBuildKeys` | ❌ | 中 | **与既有设计正面冲突**:`ConditionalConfig` 刻意只携带 `BuildInputs`,注释明写"key 不在其中就无法静默解析"。要做就得开第二条通道(`conditionalSelectors`),不能简单加 key。**但 toml.cppm:1066 把 `static_stdlib` 与 `target`/`linkage` 并列的理由不成立**:前者仅在 `flags.cppm:512` 被读,严格晚于 `merge_conditional_build_inputs`,不构成循环;后两者才真的参与谓词求值本身 |
| **3. 修 lld 的初始化器排序** | 上游 LLVM | ✅ | 极高 | **基本不可行**:Mach-O 格式层面就没有 ELF 的按优先级分段机制,不是 lld 的疏忽 |
| **4.(本报告新增)mcpp 注入 stream-init 对象** | macOS + 静态 libc++ 时,生成一个极小 TU 排在 `$in` **最前**,构造流 | ✅ **修默认** | 小 | 依赖 libc++ 内部符号(见下) |

**选项 4 的具体形态**(避免伪造 `namespace std` 里的类型,用 asm label 直接绑符号):

```cpp
// target/.mcpp/macos_stream_init.cpp —— 仅 macOS + 静态 libc++ 时生成,排在链接行首位
extern "C" void mcpp_libcxx_ios_init(void*) asm("__ZNSt3__18ios_base4InitC1Ev");
namespace { struct Force { Force() { char storage[1]; mcpp_libcxx_ios_init(storage); } } force; }
```

要点:
- 调 `ios_base::Init::Init()` **而不是** `DoIOSInit::DoIOSInit()`,因为前者带 `__cxa_guard`,后来 `_GLOBAL__I_000100` 再跑一次时是 no-op;直接调后者会对已构造的流二次 placement-new。
- 该对象排在 `$in` 首位 ⇒ `__init_offsets[0]`,先于任何用户/依赖初始化器。
- 它引用 libc++ 的符号 ⇒ 把 `iostream.cpp.o` 从 `-load_hidden` 归档拉进来,可见性仍是 hidden,不破坏 #117 的 split-brain 修复。
- 唯一代价:耦合 `_ZNSt3__18ios_base4InitC1Ev` 这个 mangled 名。它是 libc++ ABI 的**导出稳定符号**(`_LIBCPP_EXPORTED_FROM_ABI`),多年未变;且可以在生成时用 `llvm-nm` 校验符号存在,不存在就退回现状 + 发诊断。

**推荐组合**:**1 + 4 + 文档**。1 恢复承诺过的语义(低成本、马上让 PR #142 有出路);4 让默认路径真正正确(这才是缺陷本身);文档补一段 macOS 静态 libc++ 的 SIOF 说明。选项 2 独立评估——它有独立价值(跨平台工程确实需要按平台关静态 stdlib),但**不该作为本 bug 的修法**,那是拿工程配置去绕引擎缺陷。

### 3.4 必须补的回归测试

`mcpp` 现有 e2e 没有任何一条覆盖"静态初始化期用流"。建议加一条 macOS-only e2e:

```cpp
#include <iostream>
struct G { G() { std::cout.rdbuf(); } } g;
int main() { std::cout << "ok\n"; }
```
断言:(a) 默认 `mcpp build && mcpp run` 输出 `ok`(选项 4 落地后);(b) `mcpp test` 同源用例通过;(c) `static_stdlib=false` 时 `otool -L` 含 `libc++.1.dylib`(选项 1 落地后)。

---

## 4. PR #142(boost-ext.ut)独立评审

包本身的形态判断是对的:`boost-ext` 独立命名空间(非 `compat`、非 `boost`)、`name = "ut"` 单原子段、Form B + `generated_files` wrapper、pin 在 v2.3.1 release tag、无 feature。**但当前 head 不可合并**,按严重度排:

### 4.1 阻塞级

1. **macOS CI 红**(exit 139)。根因在 mcpp,不在包。除非 §3.2 末尾的"shim 前置"包侧试验成功,否则应**等 #336 修复后再合**。
2. **`.github/workflows/diag-ut-macos.yml`(155 行)是临时诊断产物,必须删**。19 个 commit 里 16 个是诊断 churn,合并前需要 squash 成一条。

### 4.2 正确性 / 一致性级

3. **描述符顶部的权威注释写了一个已被证伪的根因**。它把 macOS 崩溃归因为 `export import std;` 造成 module 与库两份 `std::cout` 的 ODR 分裂,并称 dev 1 是"the macOS fix"。事实是:dev 1 已合入,macOS 仍 139;而 issue 的最小复现根本不含模块。这段注释会长期误导后来者,**必须重写**。
4. **dev 1 的代价是白付的**。为了这个不成立的根因,描述符丢掉了上游干净的 `export import std;`,换来 24 行 GMF `#include` 列表 + `-Wno-template-body`。既然它没修好任何东西,应当**回退 dev 1、恢复 `export import std;`**——除非能证明 dev 1 独立地修了别的什么(目前证据里没有)。
5. **`cxxflags = { "-Wno-template-body" }` 在 Clang 上是未知警告选项**。诊断日志里明写:`warning: unknown warning option '-Wno-template-body'; did you mean '-Wno-empty-body'?`。它在两条 Clang 平台上给每次编译加一条噪音警告,并且在任何开 `-Werror` 的消费端会直接失败。若 dev 1 回退后不再需要它,一并删;若仍需要,应当按编译器族条件化。
6. **设计文档 §2.3 记录的根因(模块 BMI 未带模板成员定义 → dispatch 落空槽)同样已被证伪**,§6 表格里还有一处版本号笔误(`"2.2.3"` 应为 `"2.3.1"`)。设计文档是本仓的知识资产,留着错误根因比不留更糟。
7. **PR 正文与代码严重脱节**:正文描述的是"verbatim 上游 cppm + 两处 shim、不删任何一行",而实际文件已经删了 `export import std;`、加了 GMF、加了 `-Wno-template-body`、加了 dev 3 的 8 行显式实例化;正文的验证表也只覆盖 Windows 两条工具链,没有 macOS。**合并前必须重写正文**。

### 4.3 次要

8. 4 个新文件缺行尾换行(`\ No newline at end of file`)。
9. dev 3(从上游 `master` 取的显式实例化块)与 dev 1 的关系在注释里自相矛盾:一处说 dev 3 修 macOS SIGSEGV(设计文档 §2.4 表格),另一处说"dev 3 does NOT address the macOS ODR-split, dev 1 does"(描述符注释)。实际两者都没修 macOS。留 dev 3 本身无害(上游自己的 forward-port),但理由要写对。
10. CN 镜像缺失是已知且 lint 允许的,不阻塞。

---

## 5. 建议行动

**mcpp 侧(issue #336)**

1. 修 `flags.cppm:673` 的门,对齐 `:636`(issue 建议 1),并对 macOS + `staticStdlib=false` + 工具链 libc++ 版本高于系统的组合加一条构建警告。
2. 落地选项 4(注入 stream-init 对象),修默认路径。
3. 补 §3.4 的 macOS e2e 三条断言。
4. 文档:`docs/05-mcpp-toml.md` + 中文版补一段 macOS 静态 libc++ 与静态初始化次序的说明。
5. 选项 2(`static_stdlib` 条件化)另开 issue 评估——顺带修正 `toml.cppm:1066` 那条警告文案里对 `static_stdlib` 的错误归类。

**mcpp-index 侧(PR #142)**

1. 立刻试一次"shim 前置到 `#include \"ut.hpp\"` 之前"——若成功,PR 可自解,不必等 mcpp。
2. 无论上条结果如何:删 diag workflow、squash、回退 dev 1(含 `-Wno-template-body`)、重写描述符顶部注释与设计文档 §2.3/§2.4、重写 PR 正文、补行尾换行。
3. 在 PR 正文里明确链接 #336,说明 macOS 腿的红是引擎缺陷而非包缺陷。

---

## 6. 从 mcpp 视角的评估:需要修吗 / 应该修吗

### 6.1 先把"谁的缺陷"讲清楚

缺陷本身**不是 mcpp 造的**:Darwin 上静态链接 libc++ 会丢跨 TU 初始化次序,是 Mach-O 格式 + libc++ 头文件不带守卫这两件事叠出来的。Apple 与 LLVM 的主流姿态一直是"别静态链 libc++"。

但**默认值是 mcpp 选的**。#116 把 macOS 默认设成静态 libc++ 有正当理由(系统 libc++ 会把可运行下限钉死在构建机的 OS 版本,`std::print` 的支撑符号在 macOS 14 上不存在,是真实的用户可见 bug)。选了一个与平台主流相反的默认,就要自己扛住这个默认引入的失败模式——**mcpp 拥有的不是缺陷,是默认值的后果**。这一条决定了"应该修"。

### 6.2 稳定性评估

**缺陷侧的失败质量是最坏的一档**:

- 确定性(不是竞态),但**零诊断**:`exit 139`、无任何输出、`mcpp test` 只报 `FAIL (exit 139)`;
- **非局部**:崩溃点在依赖的头文件里(`ut.hpp:1620`),没有任何线索指向链接参数;
- **静默**:构建全绿,只在运行期炸;
- 触发条件"全局对象的构造函数里用流"并不罕见(日志单例、自注册型测试框架、静态 `std::ostream&` 包装),PR #142 一次就撞上了。

**修复侧的爆炸半径高度不对称**,这决定了推进节奏:

| 修法 | 影响面 | 最坏情况 | 可回退性 |
|---|---|---|---|
| 建议 1(补 `staticStdlib` 门) | **只在用户显式写 `static_stdlib=false` 时改变行为**,默认路径字节不变 | 显式 opt-out 的用户在 llvm@22 上撞回 #202 的 `__hash_memory` | 改回 1 行 |
| 选项 4(注入 init shim) | **每一次 macOS 链接**;指纹变化触发一次全量重建 | libc++ 未来改掉 `_ZNSt3__18ios_base4InitC1Ev` ⇒ **链接期报错(响的,不是静默的)** | 生成时 `llvm-nm` 探针,查不到就退回现状 + 发诊断 |
| 建议 2(条件化 `static_stdlib`) | 新增 manifest 语义,**永久 API 面** | 语义面一旦发布不可撤 | 不可回退 |

一个容易被误判成"hack"的点值得说清:**选项 4 不是绕过,是把静态链接恢复成动态链接本来就提供的保证**。动态 libc++ 之所以永远正确,正是因为 dyld 让库的初始化器整体跑在主可执行文件之前;shim 做的就是同一件事。`ios_base::Init::Init()` 带 `__cxa_guard`,提前调用只是把构造点前移,不改变任何既有正确程序的语义。

**回归测试是空洞**:全仓 e2e 没有任何一条覆盖"静态初始化期使用标准库运行期状态"。这个空洞独立于本 bug 存在——它是"能不能锁住"的问题,不是"这次修没修对"的问题。

### 6.3 跨平台评估

精确的暴露面**只有一格**(libstdc++ 一侧本地验证):

| 平台 / stdlib | `<iostream>` 是否自带守卫 | 静态链接时 | 结论 |
|---|---|---|---|
| Linux/MinGW × libstdc++ | ✅ `iostream:82: static ios_base::Init __ioinit;`(gcc 16.1.0 本地实测) | 每个 TU 自带守卫 | 免疫 |
| Linux × libc++ | ❌ 无守卫 | ELF 按 `.init_array.00101` 段名排序 | 免疫 |
| Windows × MSVC STL | (未本地验证)由 CRT 初始化段保证 | — | CI 实测绿 |
| **macOS × libc++** | ❌ 无守卫 | **Mach-O 无按优先级分段,`init_priority` 只在单 TU 内有效** | **必坏** |

两条推论:

1. **修复应当键在 "Mach-O × 静态 libc++" 这个交集上**,不是键在 "macOS"。若将来 mcpp 支持 iOS 或别的 Mach-O 目标,是同一格;若 Linux 走 clang+libc++ 静态,不是。
2. **这是一个方向相反的可移植性悖论**:`static_stdlib=true` 是为了让**产物**跨 macOS 版本可移植而引入的,副作用却是让**源码**跨 OS 不可移植——同一份合法 C++ 在三个平台正常、在第四个平台启动即崩。这直接打在 mcpp 的产品命题("同一份 manifest、同一份源码、四个平台")上,比"有多少人会踩"更重要。

**与既有决策的一致性**也指向同一个方向。mcpp 处理平台差异的既定政策是**吸进引擎、让 manifest 保持统一**,已有四个先例:PE 无 RPATH → 引擎把依赖 DLL 拷到 exe 旁;MinGW 独立 exe 惯例 → 引擎发 `-static`;musl 的 PT_INTERP → 引擎让 host helper 静态;#195 hermetic link → 引擎补 `-B`。**选项 4 是这条政策的直接延续;建议 2 是对它的背离**——它把引擎缺陷翻译成"每个工程自己在 manifest 里写一句话绕开"。

### 6.4 通用架构评估

**(a) "同一决策两处推导"再次命中,而且第三个消费者已经在那儿了。**
`ldStdlibDefault` / `ldStdlibTest` 是"这个链接单元该拿静态还是动态 libc++"的两份独立推导,新语义只加在其中一份 ⇒ 契约静默收紧。架构级修法不是补一个门,而是**收敛成一个 `(LinkUnit::Kind, staticStdlib, toolchain) → stdlib 链接策略` 的函数**。
顺带一个**需要单独核实的问题**:`ninja_backend.cppm:1222` 是二分派,凡非 `TestBinary` 都吃 `ldStdlibDefault`——**`SharedLib` 也在内**。也就是说 macOS 上产出的 `.dylib` 会把一份 hidden libc++ 静态嵌进去。hidden 保证了不与宿主的那份串绑(这正是 #117 想要的),但一旦有 std 类型跨 dylib 边界传递,两份 libc++ 的 ABI 就分家了。这不是本 issue 的范围,但它恰好是"两处推导"漏掉第三种情形的活样本。

**(b) "选择旋钮 vs 构建输入"的判据画错了一格。**
`ConditionalConfig` 只携带 `BuildInputs`、超出即拒的设计是好的(防漂移,`types.cppm:347-362` 的注释写得很清楚)。但 `toml.cppm:1066` 给出的分类理由是"selection knobs …resolved before the predicate is evaluated",而 `static_stdlib` **只在 `flags.cppm:512` 被读,严格晚于 `merge_conditional_build_inputs`**,不构成循环。真正的判据应该是 **"它是否参与解析出 target triple 本身"**:`target` 和 `linkage` 参与(循环),`static_stdlib` 不参与。这条误分类**独立于本 bug 有价值**,值得单独修——但**不该拿它当本 bug 的修法**。

**(c) 缺少链接单元语义的一等模型。**
Default/Test 的分叉背后是一条真实语义:*测试二进制跑在构建机上,分发产物必须自包含*。现在它被编码成两个字符串字段,而不是一条策略。若显式建模(如 `StdlibLinkPolicy { SelfContained, HostCoupled }`,再由 LinkUnit 种类 + 目标格式推导),"测试可以host-coupled"就成了可读的决策,第三种情形也就无处可漏。

**(d) 零诊断是独立的产品缺口。**
2026.8.1.1 刚做完"`mcpp test` 可观测/有界"。同一条线上,macOS 下子进程以 SIGSEGV 且**零输出**退出时,mcpp 完全有条件发一条提示("macOS 静态 libc++:全局对象在静态初始化期使用标准库流会崩,见 …;可用 `[build] static_stdlib = false` 规避")。成本极低,且即使选项 4 不落地也能把这类事故从"不可行动"变成"三分钟定位"。

### 6.5 结论与优先级

| 档 | 事项 | 依据 | 成本 |
|---|---|---|---|
| **必修** | `flags.cppm:673` 补 `staticStdlib` 门 + 两处推导收敛成一处 | 文档化契约被静默违反,属缺陷而非设计问题;默认路径零影响 | ~1 行 + 收敛重构 + e2e |
| **必修** | 文档补 macOS 静态 libc++ 的静态初始化次序说明 | 现在文档在说谎 | 一段话 |
| **应修** | 运行期诊断(macOS + 静态 libc++ + 子进程 SIGSEGV 且零输出) | 把不可行动的 139 变成可行动 | 小 |
| **应修** | 选项 4:注入 init shim,修默认路径 | 产品命题层面的跨平台一致性;与既有"吸进引擎"政策一致 | 中,必须带 `nm` 探针 + 静默回退 + macOS CI 证据 |
| **另开** | `static_stdlib` 条件化 + 修正 `toml.cppm:1066` 的误分类 | 独立有价值,但不是本 bug 的修法 | 中 |
| **不做** | 翻掉 macOS 静态 libc++ 默认 | 会直接退回 #116(下限钉死在构建机 OS) | — |
| **不做** | 等 lld / Mach-O 修排序 | 格式层面就没有该机制 | — |
| **平行** | 向 LLVM 反馈 `ios_base::Init` 在头文件里无定义 | 看上去不符合 [ios.base];是根上的修,但慢,且不能替代上面任何一条 | 小 |

一句话:**必修的那条是缺陷不是设计题,今天就能进下一个补丁版;真正值得投的是选项 4,因为它修的是"同一份源码四平台一致"这条产品命题,而不是某个包的 CI。建议 2 有它自己的价值,但拿它当修法等于把引擎缺陷外包给每一个用户的 manifest。**

---

## 7. 延伸:mcpp 需要支持"动态"吗 / 构建与分发的核心

### 7.1 一个布尔值,四种真实语义

`static_stdlib` 是个构建词汇("怎么链"),但代码里它在四种配置上展开成四种**分发结果**:

| 配置 | `static_stdlib = true` 实际做了什么 | 产物的运行期 C++ 依赖 |
|---|---|---|
| Linux × libstdc++ | `-static-libstdc++` | 无(自包含) |
| Windows MinGW × libstdc++ | **`-static` 整条链**(连 libwinpthread) | 无(自包含) |
| macOS × libc++ | `-nostdlib++ -Wl,-load_hidden,libc++.a` | 无(自包含,但引入本 bug) |
| **Linux × libc++(clang 工具链)** | **什么都不做**(`flags.cppm:520` 要求 `stdlib_id == "libstdc++"`,`needs_explicit_libcxx` 只在 macOS 为真) | **工具链的 `libc++.so.1`** —— 既不自包含也不宿主耦合,是**工具链耦合** |

第四行不是推测:llvm 22.1.8 的 slim 包漏带 `libatomic.so.1` 时,`import std` 产物运行期报 `cannot open` —— 那次事故本身就证明了这条配置下的产物动态依赖工具链目录。

**同一个 `true`,三种不同的分发结果,其中一种是静默空转。** 这不是疏忽堆出来的,是因为这个字段名描述的是**手段**,而它承载的是**意图**。

### 7.2 需要支持动态吗:需要,但"支持动态"是错的提法

先说需要的那一面——有些场景动态不是次优而是**唯一正确**:

- **发行版打包**:Debian/Fedora/Homebrew/AUR 的打包政策普遍要求链系统运行时,静态链会被拒。mcpp 刚做完 Homebrew 分发,这条离得很近。
- **插件 / `dlopen` 模型**:宿主与插件必须共享同一份 stdlib,否则跨边界的 std 类型、`type_info` 比较、异常传播全部分家。
- **与系统上预编译的 `.so`/`.dylib` 互操作**:对方用系统 stdlib 编,你就得是同一份。
- **安全更新的责任归属**:静态链意味着 stdlib 的每个 CVE 都要重新构建并重新分发全部产物。
- **测试二进制**:它**根本不分发**。#124 的原始立场("per-unit stdlib, cargo-test stance")在语义上是对的;#202 把它翻过来是为了绕开一个具体故障,不是因为静态更对。

诚实的另一面:动态路径不是"什么都不做就对"。#202 的 `__hash_memory` 说明它有一条硬约束——**头文件必须与运行期库同源**。所以动态不是"少做一件事",是"换一套需要各自维护的约束"。

因此正确的提法不是"要不要支持动态",而是:**把这个字段从"链接手段的布尔值"升级成"分发契约的枚举"**。三档就够,而且三档的机制 mcpp **今天全都已经有了**,只是没有名字:

| 档 | 含义 | 现有机制 |
|---|---|---|
| `self-contained` | 产物不依赖构建机之外的任何 C++ 运行时 | 今天的默认(三平台三套 flag) |
| `host-coupled` | 链运行主机上的系统运行时 | 今天的 opt-out;**也是测试二进制本来该有的形态** |
| `toolchain-coupled` | 链 mcpp 装的那份工具链的动态运行时 + rpath | `linkRuntimeDirs` + `-Wl,-rpath` 已经在跑;Linux×libc++ 事实上就在这一档 |

有了名字之后,三件今天纠缠不清的事各自归位:测试默认 `host-coupled` 是**可读的策略**而不是一个 `if`;`SharedLib` 那个漏掉的分支有地方安放;`[target.'cfg(macos)']` 要不要条件化,变成"分发契约能不能按目标平台不同"这个**答案显然是能**的问题。

### 7.3 从通用构建 / 分发视角:三个核心点

**(1) 构建和分发是两个目标函数,而 mcpp 用同一层词汇表达它们。**

- 构建关心 *可重现、可缓存、正确*:用哪个编译器、哪些 flag、什么顺序。
- 分发关心 *产物在哪些机器上能跑*:运行期依赖集是什么、下限在哪、谁负责更新。

`static_stdlib` 是穿着构建外衣的分发意图,§7.1 的四行表就是这件事的直接后果。同样的错位还解释了另外两个已知症状:它被误分类进"selection knob 不可条件化"(因为大家不确定它是构建输入还是选择旋钮——**它两者都不是**),以及它和 `LinkUnit` 的种类纠缠不清(测试不分发,所以它对测试根本不该有同一个含义)。

顺着这条线看,mcpp 事实上已经做对了很多**分发层面**的事,只是散着、各自推导:macOS deployment target(下限)、Windows DLL 部署到 exe 旁、MinGW `-static`、musl 全静态、macOS 静态 libc++。**这五处是同一件事的五处推导**——"同一决策多处推导"这条反复出现的债,根在这里,不是巧合。

**(2) 分发契约必须可验证,而不只是被声明。**

`docs/05-mcpp-toml.md` 声称"默认产物在任何 macOS ≥ 14 上开箱即用",但仓库里没有任何一条断言验证过产物的运行期依赖集。这类断言是 O(1) 成本的:

- macOS:`otool -L` 的输出集合 ⊆ 允许集
- Linux:`readelf -d` 的 `NEEDED` 集合
- Windows:导入表(这个手段 mcpp 在 winlibs 那次已经用过)

但要说清它的边界:**这类静态断言只能锁住"契约有没有被悄悄改掉",锁不住本 bug**——#336 是契约兑现了(依赖集确实只剩 libSystem)而语义坏了。所以分发验证需要**两类**断言配对:依赖集断言(契约的形)+ 最小冒烟运行(契约的实)。§3.4 那条 e2e 属于后者,而前者今天完全是空的。

**(3) C++ 的"自包含"账单是分期付的,而且还没付完。**

Rust/Go 的静态自包含之所以顺,是因为它们的运行时没有跨 TU 的全局构造次序、没有跨 DSO 的类型身份问题。C++ 的自包含会逐个撞上:符号可见性与 ODR、静态初始化次序、跨 DSO 的 `type_info` 与异常、`operator new` 的唯一性。

**mcpp 把 Rust 的默认搬到 C++ 上,就得逐笔把这些 C++ 特有的坑吸收进引擎。**这份账单已经付了两笔:#117 是 split-brain(可见性/ODR),#336 是初始化次序。这个模型有预测力——**下一笔大概率在 `SharedLib` 那一格**:macOS 的 `.dylib` 今天会嵌一份 hidden libc++(§6.4a),那么 exe 与 dylib 各嵌一份 hidden `libc++abi` 时,跨边界抛异常的 `type_info` 匹配与 `__cxa_*` 状态是否还成立,是一个具体的、可以现在就写测试去证伪的问题。

这不是"所以别做自包含"——自包含的收益(macOS 下限、MinGW 独立 exe、musl 全静态)都是真的。这是说:**选了这条路就要承认它是一条需要持续投入的路,并且把投入前置成契约与测试,而不是等 issue 来收账。**

### 7.4 落到最小可行动作

1. **先做名字,再做实现**:把 `static_stdlib` 的三种展开收敛成一个 `DistributionContract` 枚举(旧字段保留为别名,零破坏)。这一步不改任何 flag,只是把五处推导指向同一个来源。
2. **让 `LinkUnit` 从契约推导链接形态**,而不是二分派;测试默认 `host-coupled`,`SharedLib` 显式定档。这一步顺手修掉 #336 的问题一。
3. **补依赖集断言**(三平台各一条),把分发契约变成机器可验的东西。
4. `Linux × libc++` 下 `static_stdlib` 空转要么实现、要么发警告——**静默空转是最坏的一种**。

---

## 9. 实施记录(2026.8.3.1 / PR #337)

落地形态与 §8 的设计一致,但有两处**在实现中被证据修正**的地方,记在这里比记在 PR 里更耐久。

**契约收敛成两个 API 决定**

- `src/build/distribution.cppm` = 角色 / 契约 / 机制表(总函数)。`flags.cppm` 只调用它,`ninja_backend.cppm` 只问 `role_of(kind)`。原来的五处推导归零。
- C++ 运行时 flag 从全局 `ldflags` 移到**每个链接单元**的 `unit_ldflags`。必须如此:两个角色在同一次构建里可以持不同契约,全局通道表达不了。移动的都是驱动级 flag(`-static-libstdc++` / `-static-libgcc` / MinGW `-static`),相对库的位置无意义。

**修正 1:诊断的判据不是"降级",是"违约"。**
初版让"契约兑现不了就必报"。结果是 MSVC 运行时**每一次 Windows 构建**都会打印一条 `self-contained 未实现` —— 而 mcpp 从来没有为 MSVC 承诺过自包含(它压根不发 `/MT`)。这不是违约,是平台边界,且用户无从行动。改成:**只有显式写下的契约兑现不了才报**(`static_stdlib = false` 也算显式 —— 没人会把开关设成默认值来求非默认行为);而 mcpp 确实做了承诺的格子(默认档下缺 `libc++.a`)照报不误。

**修正 2:shim 不能有能力弄坏链接。**
初版靠 `__attribute__((weak))` 兜底"符号不存在就退化成空操作"。首轮 macOS CI 全红,教了两件 Mach-O 的事:

1. **`__asm__` label 是逐字使用的,clang 不会替它补 Mach-O 的全局 `_` 前缀。** C++ 符号 `_ZNSt3__18ios_base4InitC1Ev` 在这里必须写成 `__ZNSt3__18ios_base4InitC1Ev`。写错不是静默失效,是 `ld64.lld: error: undefined symbol: ZNSt3__18ios_base4InitC1Ev`,每一条 macOS 链接都挂。
2. **声明上的 `weak` 不是 Mach-O 的 weak-undefined 形态**(那是 `weak_import`),所以它根本没有兜住那个坏引用。

两条都修了,但真正的安全网挪到了**上游**:后端只在归档确实定义该符号时才生成 shim TU —— 扫归档第一个成员里的 ranlib 符号索引,不起子进程。**一个在引用之前的检查,不可能像引用本身那样弄坏链接**;符号拼写变了就是"少一个次序修复 + 一条诊断",不是构建失败。

**本次没做,理由已在 §6.4 写清**

- INV-2 跨链接边的契约传播硬错误:单次构建内所有单元共用一个工程级契约,在构建内近乎空转;真正的风险是 macOS `.dylib` 嵌一份 hidden libc++ 而宿主嵌另一份(`ninja_backend.cppm:1222` 的二分派让 `SharedLibrary` 走分发档)。留作下一项,不做半个。
- ELF 上 `host-coupled` 去掉工具链 rpath:属打包轴,文档已明确写出这条边界。
- MSVC 的 `/MT`:表里点名承认缺口。

**顺带落地的能力**:Linux + clang/libc++ 的自包含从"静默空转"变成真的(显式链 `libc++.a` / `libc++abi.a` / `libunwind.a`,实测 `NEEDED` 只剩 libc/libm/loader)。

---

## 附:本地可复现的取证命令

```bash
A=~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu/libc++.a
llvm-nm --defined-only "$A" | grep -E 'DoIOSInit|start_std_streams|GLOBAL__I|ios_base4Init'
llvm-ar x "$A" iostream.cpp.o
llvm-objdump -dr --disassemble-symbols=_GLOBAL__I_000100          iostream.cpp.o
llvm-objdump -dr --disassemble-symbols=_ZNSt3__18ios_base4InitC2Ev iostream.cpp.o
llvm-objdump -dr --disassemble-symbols=_ZNSt3__19DoIOSInitC2Ev     iostream.cpp.o   # ← 引用 __cout/cout/vtable

# ios_base::Init 在头文件里只有前向声明,全树无定义:
grep -rn 'class Init' ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/include/c++/v1/ios
grep -rn 'Init::'     ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/include/c++/v1/ | grep -v __cxx03   # 空
```
