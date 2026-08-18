# Windows 动态库分发、cl.exe 消费,与 `.ixx` 的默认支持

> 2026-08-18 · 针对 2026.8.18.1 发布后遗留的 P1 / P2 / `.ixx` 三问
> 状态:**分析与设计,未实施**

三个问题看起来独立,实际共用一条主线:**mcpp 在这三处都把「工具链没有替我们
做的事」当成了「做不了的事」**,而工业界对每一件都有已经跑了十年以上的做法。

| # | 问题 | 一句话结论 |
|---|---|---|
| **Q1** | `kind = "shared"` 在 `*-windows-msvc` 上被拒 | 拒绝的理由(符号导出)成立,但**结论不成立** —— CMake 十年前就用自动生成 `.def` 解决了,且不依赖 dumpbin |
| **Q2** | 打包库不能被原生 `cl.exe` 消费 | 是 mcpp 自己的 manifest 用了 GNU 拼写;方言中立通道已经存在,只是不在条件通道里 |
| **Q3** | `.ixx` 默认是否支持 | **不支持,而且失败方式很糟**:实测编译出对象、然后从链接里消失,报 `undefined reference` |

---

## 1. Q3 先说,因为它是实测出来的,而且最脏

### 1.1 实测

```toml
[build]
sources = ["src/*.ixx", "src/*.cpp"]      # 没有 module_extensions
```

```
$ mcpp run
   Compiling ixxprobe v0.1.0 (.)
error: build failed
failed: bin/ixxprobe
ld: obj/main.o: in function `main':
    undefined reference to `mk::answer@mathkit()'
```

加上 `module_extensions = [".ixx"]` 之后同一份代码 `ixx-ok=42`,通过。

### 1.2 根因:两个答题者对「这是不是模块接口」给了不同答案

看未声明时生成的 `build.ninja`:

```ninja
build obj/mathkit.ixx.ddi : cxx_scan .../src/mathkit.ixx
build obj/mathkit.ixx.o | gcm.cache/mathkit.gcm : cxx_object .../src/mathkit.ixx | ...
  bmi_out = gcm.cache/mathkit.gcm          ← BMI 产出了

build bin/ixxprobe : cxx_link obj/main.o   ← obj/mathkit.ixx.o 不在里面
```

- **扫描器**读到 `export module mathkit;`,记下 `provides = mathkit`,于是规划器给这条边挂了 `bmi_out`;
- **分类器** `classify()` 对不在表里的扩展名返回 `SourceKind::Other`(`source_kind.cppm:322`);
- 而链接集合只看 kind:`links_unconditionally(Other) == false`,`is_implementation_source(Other) == false`;
- 于是**对象被构建出来,然后没有任何人链接它**。

这是本仓库反复出现的那个形状:**同一个决定有两处推导,而下游只读其中一处**。

### 1.3 `.ixx` 在三个编译器上本来就能用

mcpp **从不依赖驱动的扩展名启发式**:模块接口的语言由
`bmi_traits(tc).moduleInterfaceLangFlag` 显式给出 —— MSVC `/interface /TP`、
clang `-x c++-module`、gcc `-x c++`(`model.cppm:309/325/346`)。所以
`.ixx` 一旦进了表,三个编译器都直接可用;这与「clang 的驱动不认识 `.ixx`」
并不矛盾,因为 mcpp 根本没让驱动去猜。

> ⚠️ 我此前的笔记记成「Clang 不认 `.ixx`」并把它当成产品限制。那说的是**驱动默认**,
> 而 mcpp 覆盖了它 —— 判据要落在 mcpp 实际发出的命令行上,不是编译器的默认行为。

### 1.4 因此:**不是内置 `.ixx`,而是让「已声明」真的自动适配、「未声明」不再静默**

⚠️ **这一节在 review 中被更正过一次。** 第一版结论是「把 `.ixx` 放进
`builtin_extension_table()`」。那是错的方向:**扩展名集合是配置,不是 mcpp 要
逐个追加的内置清单** —— 今天补 `.ixx`,明天补 `.ccm`、`.cxxm`,而每一次追加都是
mcpp 替工程做了一个工程自己能说清楚的决定。

真正要成立的是两件事:

1. **已声明的扩展名必须在三个工具链上都直接可用 —— 这一条今天已经成立。**
   模块接口的语言由 `BmiTraits::moduleInterfaceLangFlag` 显式给出
   (`/interface /TP` / `-x c++-module` / `-x c++`),mcpp 从不让驱动去猜扩展名。
   所以 `module_extensions = [".ixx"]` 一写,gcc / clang / MSVC 全部可用
   ——**实测 `ixx-ok=42`**。
2. **未声明的扩展名必须当场拒绝,而不是编译出一个没人链接的对象。**
   这才是缺陷本体,而且它与 `.ixx` 无关:任何 `classify()` 归为 `Other` 的扩展名
   都会掉进同一个洞。

拒绝写在**扫描器**里(分类发生的那一处),消息点名文件、扩展名与该写的键:

```
error: scanner errors:
  …/src/mathkit.ixx: 'mathkit.ixx' is listed in [build] sources, and mcpp has
  no role for the extension '.ixx'.
  Its object would be compiled and then linked by nothing, so this is refused
  rather than built. If it is a module interface, declare the extension:
      [build]
      module_extensions = [".ixx"]
  Otherwise remove it from `sources` — headers belong in `include_dirs`, and
  Windows resource scripts in `[resources]`.
```

**为什么不猜。** 让 mcpp 看到未知扩展名里有 `export module` 就自动当模块接口,
是给「这是不是模块接口」加**第三个**答题者 —— 而本节的缺陷正是前两个答题者
(扫描器的 `provides` 与分类器的 `kind`)不一致造成的。

---

## 2. Q1:MSVC 的 `kind = "shared"`

### 2.1 拒绝的理由成立

MSVC 在没有 `__declspec(dllexport)`、也没有 `.def` 时,DLL 不导出任何东西 ——
导入库为空,消费者拿到一片 unresolved externals,而那些符号就在对象里。
这一点在链接器层面也被证实:**lld-link 的 MSVC 形态不做 auto-export**,
而 MinGW 形态做;LLVM 侧的说法是 MSVC 路径要控制导出数量,以免撞上
**PE 的 65535 导出上限**([lld-link auto-export 讨论][lld])。

所以 `*-windows-gnu` 能用而 `*-windows-msvc` 不能,差别确实只在「链接器替不替你导出」。

### 2.2 但「做不了」是错的 —— 工业界有两条成熟路径

| 路径 | 代表 | 机制 | 正确性 | 需要改源码 |
|---|---|---|---|---|
| **A. 导出宏** | MSVC 官方指导、Qt、Boost、CMake `generate_export_header` | 作者在声明上标 `__declspec(dllexport/dllimport)` | **完全正确**,含数据符号与 vtable | 是 |
| **B. 自动 `.def`** | **CMake `WINDOWS_EXPORT_ALL_SYMBOLS`**(2015 年,CMake 3.4) | 扫描 `.obj` 的 COFF 符号表,生成 `.def` 交给链接器 | 函数正确;**数据符号与 vtable 有明确限制** | 否 |
| C. MinGW auto-export | GNU ld / lld-mingw | 无显式导出时导出全部 | 同 B,且不适用于 MSVC ABI | 否 |
| D. 干脆用静态库 | 相当一部分项目在 modules 时代的选择 | —— | —— | 否 |

**B 是关键发现,因为它推翻了「需要 dumpbin」这个隐含前提。** CMake 的实现
`bindexplib` **自己解析 COFF**,不调用任何外部工具([bindexplib.cxx][bx]):
读 image header 的 machine 字段(I386/AMD64/ARM/ARMNT/ARM64),遍历符号表,按下列
规则筛选:

- 只取 `IMAGE_SYM_CLASS_EXTERNAL` 且 `SectionNumber > 0`、Type 为 `0x20` 或 `0x0` 的符号;
- **DATA 判定**:Type == 0 且所在节带 `IMAGE_SCN_MEM_WRITE` ⇒ 写成 `name DATA`;
- 跳过 `??_G` / `??_E`(析构变体)、含 `.` 的托管符号、`__t2m` / `$$F` / `$$J`、
  ARM64EC 的 `$ientry_thunk` 等 thunk 变体;
- i386 与 `__cdecl` 情形去掉前导下划线。

CMake 官方文档同时**明确写出它的边界**([WINDOWS_EXPORT_ALL_SYMBOLS][cm]):

> Global **data** symbols must be explicitly marked with `__declspec(dllimport)`
> in order to link to data in the `.dll`.
>
> In cases that the compiler generates references to the virtual function table,
> such as in a delegating constructor of a class with virtual functions, the
> whole class must be marked with `__declspec(dllimport)`.

**这两条限制不是可以隐瞒的实现细节 —— 它们决定了 B 是默认值而不是终点。**

### 2.3 C++20 modules 与 DLL 的官方姿势,恰好就是 mcpp 的模型

微软在两个 Q&A 里给出的答案一致([Q&A 1665106][ms1]、[Q&A 1695780][ms2]):

1. 模块接口里的实体**必须**带 `__declspec(dllexport)`:
   ```cpp
   export __declspec(dllexport) void myFunction();
   export class __declspec(dllexport) Test { public: Test(); };
   ```
2. 消费者要么拿到 `.ifc`(用 `/reference` 指过去),**要么把 `.ixx` 加进自己的工程,
   由自己编译出 `.ifc`**。

第二条正是 **mcpp 的分发模型**:发接口**源码**,消费者自己编。所以 mcpp
不需要发 `.ifc` —— 而 `.ifc` 与编译器构建逐位绑定,发它等同于发 BMI,
mcpp 已明确不做(docs/12「当前边界」)。

> 换句话说:**mcpp 在 modules × DLL 这件事上的模型是微软自己推荐的那一条,
> 缺的只有导出。**

---

## 3. Q2:为什么原生 `cl.exe` 消费不了打包库

### 3.1 现状

生成的 manifest 用 GNU 拼写选每条腿:

```toml
[target.'cfg(all(arch = "x86_64", os = "windows", env = "msvc"))'.build]
ldflags = ["-Llib/x86_64-windows-msvc", "-lmathkit"]
```

clang(Windows 默认工具链)吃这一套;**原生 `cl.exe` 不认 `-L`**。

### 3.2 两条看起来能走、实测走不通的路

**(a) 直接写文件路径。** 每个 driver 都接受裸路径作为链接输入,看起来是方言中立解。
实测失败:

```
ld: cannot find lib/x86_64-windows-gnu/libmathkit.a
```

ninja 执行链接命令时 cwd 是**输出目录**,而只有 include 家族前缀(`-I`、`-L` …)
会被 `normalize_include_flags` 相对包根绝对化;无前缀 token 没有挂靠点。
而 manifest 里写绝对路径就不再可重定位。

**(b) 把链接意图挪到方言中立通道。** `[runtime] link_library_dirs` + `libraries`
本来就按 flavor 渲染(`flags.cppm` 的 `LinkIntentFlavor::PeMsvc` → `/LIBPATH:` +
`<n>.lib`)。问题是它**只在顶层读**,而包需要**按腿**给 —— 两条腿的库同名时,
顶层单一搜索路径会让链接器挑到错的那条。

而把它做成条件段 `[target.'cfg(…)'.runtime]` 有一个实测过的陷阱:
**旧版 mcpp 读到它不报错,而是静默忽略**。于是把 flag 从 `ldflags` 挪过去,
会让所有旧客户端**一个链接 flag 都拿不到**。

### 3.3 因此:两种拼写都带,新客户端优先中立形式

这是唯一同时满足「老客户端仍可用」与「cl.exe 可用」的形状:

```toml
# 老客户端读这个(GNU 拼写,clang/gcc 可用)——保持现状,不动
[target.'cfg(...)'.build]
ldflags = ["-Llib/<triple>", "-lmathkit"]

# 新客户端读这个,并在读到时忽略上面那条
[target.'cfg(...)'.runtime]
link_library_dirs = ["lib/<triple>"]
libraries         = ["mathkit"]
```

**判据:重复不是问题,冲突才是。** 新客户端若两条都应用,`cl` 仍会因为 `-L` 失败;
所以规则必须是「条件 `runtime` 存在 ⇒ 同一条腿的 `ldflags` 不再应用」,
而不是简单相加。这条规则只对**分发包**(`provenance` 以 `mcpp-pack` 开头)生效,
普通工程的 `ldflags` 语义不变。

---

## 4. 方案:分四层,零新增 manifest 段

沿用库分发的既有判据 —— **新增段 0 个**;第 3 层复用的是已经存在的 `[runtime]` 键,
只是让它可以出现在条件通道里。

### L1 — 未知扩展名不再静默(独立、最小、立即可做)

| 改动 | 位置 |
|---|---|
| `builtin_extension_table()` **保持 `{".cppm"}`** —— 扩展名集合是配置 | `source_kind.cppm` |
| `sources` 命中但 `classify()` 为 `Other` ⇒ **硬错误**,点名文件、扩展名与 `module_extensions` | `scanner.cppm::scan_file`,分类发生的那一处 |

- **实测两侧**:未声明 `.ixx` ⇒ 上面那条消息;声明后 ⇒ `ixx-ok=42`。
- ⚠️ 这是**行为变化**:`sources` 里混进 `.md` / `.txt` 的工程会开始报错。
  需要一轮全量 e2e 确认没有工程靠「未知扩展名被静默忽略」活着。
- docs/10 的「用了 `module_extensions` 的包要声明版本下限」**不变** ——
  它本来就是关于「旧客户端不认识这条键」的,与本层无关。

### L2 — MSVC 自动 `.def`(把拒绝变成默认可用)

新增一个构建图节点,不是一个 flag:

```
obj/*.obj  ──▶  [def-gen]  ──▶  bin/<name>.def  ──▶  link /DEF:<name>.def /DLL
```

- **实现:mcpp 自己解析 COFF**,规则照 §2.2 列的 bindexplib 语义。
  判据:**不得依赖 `dumpbin`** —— 它只在 VS 开发者环境里,而 mcpp 的 Windows 默认
  工具链是 clang,`mcpp build` 不进 VS 环境。`llvm-nm` 是可选的第二实现,不是首选:
  多一个外部依赖就多一处「这台机器上没有」。
- 导出数量超过 **65535** 时必须**报错并说明**,不是截断。
- 生成的 `.def` 是构建产物,进 `target/`,**不进分发包** —— 包里带的是 DLL 与导入库。

### L3 — 导出宏(正确性上限,与 L2 并存)

L2 的两条限制(数据符号、vtable)**无法由工具消除**,只能由标注消除。所以:

- mcpp 提供生成导出头的能力(等价 CMake 的 `generate_export_header`),
  或约定一个 `MCPP_EXPORT` 宏;
- 与 modules 的写法就是微软示例的那个:`export __declspec(dllexport) void f();`
- **默认走 L2,作者需要导出数据或跨 DLL 的多态类型时走 L3。**
  docs 必须把这条边界写在「当前边界」里,而不是让人踩到。

### L4 — cl.exe 消费

- `ConditionalConfig` 增加承载 `[runtime]` 链接意图的能力(`link_library_dirs` /
  `libraries`);
- `manifest_emit` 对每条腿**同时**写 `ldflags` 与条件 `runtime`;
- 消费侧:分发包的某条腿若有条件 `runtime`,**忽略同腿的 `ldflags`**。

---

## 5. 依赖与顺序

```
L1(.ixx + 未知扩展名)   ── 独立,可先合
L2(自动 .def)          ── 依赖:Windows CI 有 msvc job(已有)
   └─ L3(导出宏)       ── 依赖 L2 的边界文档
L4(cl.exe 消费)        ── 独立于 L2/L3,但只有 L2 落地后才有 msvc 动态库可消费
```

## 6. 验证矩阵(每一格都要能指出「失败时报什么」)

| 断言 | 载体 | 平台 |
|---|---|---|
| `.ixx` 无需声明即可用 | e2e | 三平台 |
| `sources` 里的未知扩展名被拒并点名 | e2e | 三平台 |
| MSVC DLL 导出非空、消费者链接通过 | e2e `# requires: msvc` | Windows |
| 导出的数据符号在消费端仍需 `dllimport`(**限制本身可复现**) | e2e | Windows |
| 导出数 > 65535 时报错而非截断 | 单测(合成符号表) | 三平台 |
| `cl.exe` 消费打包库 | e2e `# requires: msvc` | Windows |
| 老客户端仍能消费同一个包 | e2e(静态 + 真实) | 三平台 |

## 7. 明确不做

- **不发 `.ifc` / BMI。** 与编译器构建逐位绑定;微软给的第二条路(发 `.ixx` 让消费者自编)
  正是 mcpp 已经在做的。
- **不自动给源码插 `dllexport`。** 那是改用户代码。
- **不在 MinGW 上生成 `.def`。** 链接器已经自动导出,再生成一份只会引入第二个真相。

---

## 参考

- [WINDOWS_EXPORT_ALL_SYMBOLS — CMake 文档][cm](限制原文)
- [Kitware/CMake `Source/bindexplib.cxx`][bx](COFF 筛选规则)
- [LLVM:lld-link 的 MSVC 形态不做 auto-export][lld]
- [Microsoft Q&A:C++20 modules 在共享库中的用法][ms1]
- [Microsoft Q&A:不发 `.ifc` 可行吗][ms2]

[cm]: https://cmake.org/cmake/help/latest/prop_tgt/WINDOWS_EXPORT_ALL_SYMBOLS.html
[bx]: https://github.com/Kitware/CMake/blob/master/Source/bindexplib.cxx
[lld]: https://github.com/llvm/llvm-project/pull/71087
[ms1]: https://learn.microsoft.com/en-gb/answers/questions/1665106/how-to-use-c-20-modules-in-shared-libraries
[ms2]: https://learn.microsoft.com/en-us/answers/questions/1695780/c-20-modules-in-shared-libraries


---

## 8. 实施记录(2026-08-18,2026.8.18.2)

四层全部落地,单 PR。与设计的偏差各自有实测依据。

### 8.1 与设计不同的地方

| 设计说 | 实际做的 | 为什么 |
|---|---|---|
| L1「把 `.ixx` 放进 builtin」 | **不内置**,只做「未声明即拒绝」 | 扩展名集合是配置,不是 mcpp 逐个追加的清单(review 时更正) |
| L1 只涉及扫描器 | 还修了 **lib root 约定**与 **pack 的 manifest 输出** | `mcpp pack` 必须跟着 `module_extensions` 走,否则用户要配两次 |
| L3「提供导出头/宏」 | **检测 `.drectve`,标注优先** | 为「我标注过了」加一个键,就是给对象已经说过的事再加一个说法 |

### 8.2 实测抓到的、设计里没有的缺陷

**`.ixx` 库打出来的包是静默错的。** lib root 约定把 `.cppm` 写死,于是闭包从一个
不存在的文件开始:

```
$ mcpp pack mathkit
     Interface (headers only)      ← 模块接口整个没了
      Withheld (nothing)
  Packed …-x86_64-linux-gnu        ← C 表面的 tag
```

**两半都错,而第二半比第一半更糟**:空的发布集合正是打包器判定「C 表面」的依据,
所以丢掉接口的同时,这个包也不再约束 C++ ABI,兼容性闸门停止检查编译器与标准库。
一个「说得比实际少」的包,正是整个分发设计要防的那种失败。

**给 `mcpp.manifest.types` 加一条模块边会让 GCC 16.1 ICE。** 探测型的 lib-root
解析需要扩展名表(`mcpp.source_kind`),而 `types` 是几乎所有东西都依赖的低层模块。
加上那条 import 之后,GCC 在编译**与改动无关的 `src/main.cpp`** 时 ICE,
清掉 gcm.cache 也不行 —— 与本仓库此前遇到的模块毒化形状一致。
**解法不是加边,而是把函数移到边已经存在的地方**(`mcpp.manifest.toml` 本就 import
了 `mcpp.source_kind`)。

### 8.3 验证到哪一步

| 断言 | 载体 | 平台 |
|---|---|---|
| 已声明的扩展名无需额外帮助;未声明的当场拒绝 | e2e 260 | 三平台 |
| pack 跟随扩展名;包自带声明;`.cppm` 包 manifest 不变 | e2e 261 | 三平台 |
| COFF 筛选规则(外部/已定义/DATA/跳过表/aux/i386 下划线/拒绝) | 单测 ×16(合成) | 三平台 |
| 真实 mingw 对象可读;真实标注对象被识别 | 单测 ×3(committed fixture) | 三平台 |
| MSVC 产出带非空导出的 DLL,消费者链通并运行 | e2e 258 | **Windows** |
| 标注过的库不被自动导出覆盖 | e2e 258 后半 | **Windows** |
| `.def` 是构建图节点,输入是链接同一批对象 | e2e 258 | **Windows** |
| MinGW 不生成 `.def`(链接器已自动导出) | 本机实测 `0 edges` | Linux |

**尚未验证**:`cl.exe` 消费打包库的端到端(需要 msvc job 里再加一条消费用例);
数据符号在消费端仍需 `dllimport` 这条限制目前只写在文档里,没有做成可复现的测试。
