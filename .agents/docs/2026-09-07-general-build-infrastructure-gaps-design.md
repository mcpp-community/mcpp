# 通用构建基础设施:缺口、归属与验证

2026-09-07。本文处理**通用构建基础设施**:其他构建系统都有对应物、其存在理由不引用
任何领域概念的那些能力。异构方向特有的东西不在缺口清单里,但 §7 给出归属规则,
§10 给出用一个陌生厂商证伪整条架构主张的实验。

本文经过一次修订(§14 变更记录)。修订推翻了初稿的两处判断,均已就地更正。

## 0. 范围

### 0.1 判据:什么算"通用"

一项能力进入本文,当且仅当 CMake / Meson / Autotools / Cargo 中至少两个有对应物,
且它的存在理由不引用任何领域概念(设备、加速器、内核)。

| 项 | 对应物 | 结论 |
|---|---|---|
| 导出面 | CMake `CXX_VISIBILITY_PRESET`、`WINDOWS_EXPORT_ALL_SYMBOLS`;Meson `vs_module_defs` | 通用 |
| 通用链接标志 | Cargo `cargo:rustc-link-arg` | 通用 |
| 包内布局 | `install(FILES ...)` / Meson `install_data` | 通用 |
| 探测库 | `check_include_file` / `check_function_exists` / `check_type_size` | 通用 |
| 配置头 | `configure_file` | 通用,但**不是引擎改动**(§5) |
| 开放词表的"空"取值 | CMake 无对应物;Meson/Cargo 亦无 —— 但它们的谓词词表是封闭的,**因而不需要**;开放词表是本生态自选的性质,这条规则随之而来 | 通用(§6.1) |
| 生成输入的依赖粒度 | CMake `add_custom_command(OUTPUT ...)` 的逐文件依赖;Ninja 的 `order_only` | 通用(§6.2) |
| 多语言(Fortran) | 四者皆有 | 通用(§6.3) |

### 0.2 RDC 为什么不在本文(初稿判断已更正)

relocatable device code 的存在理由是"设备编译器把 `__device__` 函数的跨 TU 调用
推迟到一次设备链接"。这句话无法脱离设备概念陈述,因此按 §0.1 的准入线它不是通用
基础设施。

**更要紧的是:它也不是引擎缺口。** 初稿把 RDC 列为"真缺"并暗示需要引擎支持,按 §7
的归属规则复核后不成立。它需要的原语今天全部存在:规则包用 `action` 以 `-rdc=true`
编每个设备 TU(各自以 `object` 归宿进入链接),再用一个 `action` 跑设备链接步骤,
其产物同样以 `object` 归宿加入普通链接;设备运行时库走 `link_lib`。**没有新的边种类,
没有新的产物性质,引擎零改动。**

结论:RDC 属于 `rules-cuda` 的工作量,与本文各项**并行**,不构成依赖。

## 1. 现状,实测

四条,均在 2026-09-07 于 `main` 上核实:

1. 引擎认识 25 条 `mcpp:` 指令。链接相关只有三条:`link-lib`、`link-search`、
   `link-script`(即 `-T`)。**不存在通用的链接标志出口。**
2. 共享库的导出面在两个平台上都是"全导出":ELF 走默认 visibility;PE 由
   `src/build/coff_exports.cppm` 自动生成 `.def`,语义对齐 CMake 的
   `WINDOWS_EXPORT_ALL_SYMBOLS`。**不存在收窄导出面的声明。**
3. `[runtime].artifacts` 已经是"包内相对路径 + role + provenance"的声明,role 是
   封闭白名单。**包内布局不需要新 section。**
4. 配置头生成的四块拼图都在:`toolchain_dir()` / `sysroot_dir()` 给出正确的编译器
   与 sysroot,build.mcpp 是真正的程序因而能写文件,`include-dir` 让本包 TU 看见,
   `rerun-if-changed` 保证增量。**这一项不是引擎缺口。**

## 2. 缺口一:导出面

### 2.1 问题

一个 `.so` / `.dylib` / `.dll` 目前只有一种导出策略:全部导出。这在两类项目上不成立。

**ICD / 插件。** Vulkan loader 只按名字取 `vk_icdGetInstanceProcAddr` 与
`vk_icdNegotiateLoaderICDInterfaceVersion`。一个把内部符号一并导出的 ICD,会与
loader 以及同进程内的另一个 ICD 撞名。

**一个镜像里两个 C++ 运行时。** 实测:SYCL 示例构建时 mcpp 自己的重复符号检查报告

    warning: sycl-saxpy: 68 symbols in this image are also provided by a library
    it loads.  _Unwind_DeleteException()  _Unwind_GetGR()  ...

`libsycl.so` 对着 libstdc++ 编译,mcpp 产物链 libc++,双方都导出 unwinder 符号。
收窄导出面是这类问题的标准解法。

绕过办法今天存在:`cflag` / `cxxflag` 塞 `-fvisibility=hidden`。它能用,但它是标志
不是声明,而且 PE 上没有对应物 —— 那里只有"全导出"或作者自己手写 `.def`。

### 2.2 设计:一个中立声明,三种平台渲染

```toml
[targets.mydriver]
kind    = "shared"
soname  = "libmydriver.so.1"
exports = "abi/mydriver.exports"     # 或内联:exports = ["vk_icd*"]
```

文件内容是符号模式,一行一条,`#` 起注释:

```
vk_icdGetInstanceProcAddr
vk_icdNegotiateLoaderICDInterfaceVersion
```

引擎按平台渲染同一份声明:

| 平台 | 渲染为 |
|---|---|
| ELF | version script,经 `-Wl,--version-script=` |
| Mach-O | `-exported_symbols_list` |
| PE | `.def`,**取代**自动生成的全导出版本 |

**这与 `[runtime]` 的既有先例同构。** `[runtime]` 存在的理由正是"一句中立的话,由
引擎按方言渲染",而不是让作者写三份平台专用文件。导出面是同一形状的第二个实例,
因此它不是一个新概念,是一条既有原则的应用。

### 2.3 声明 `exports` 不隐含编译期 hidden(实现时更正)

初稿写的是"声明 `exports` 时引擎同时把编译期默认置为隐藏"。**实现时更正为不隐含。**

三种格式上的收窄都是**链接期**属性:version script 限制的是动态符号表,
`-exported_symbols_list` 与 `.def` 同理。因此隐含一个编译期效果会让一个键有两个效果,
而第二个效果还改变**本库各翻译单元之间**如何看见彼此 —— 那是一个有独立理由的独立决定。

`-fvisibility=hidden` 仍可经 `[build] cxxflags` 使用以取得代码生成收益。相应地,初稿的
判据 C3(声明后夹具应链接失败)**作废**,因为它断言的正是这个被取消的耦合。

<details>
<summary>初稿原文(已作废)</summary>

仅有 version script 会收窄动态符号表,但对象里的符号仍是默认可见性,链接期优化拿
不到收益,而且 Mach-O 与 PE 的渲染需要编译期配合。因此:**声明 `exports` 时,引擎
同时把编译期默认置为隐藏**(ELF/Mach-O 的 `-fvisibility=hidden`)。

这是一处行为变化,必须写进文档:一个此前依赖默认可见性做跨 DSO 内部调用的项目,
在声明 `exports` 之后会链接失败。这正是作者声明 `exports` 时所要求的语义,失败点
也在链接期而非运行期,因此是可接受的。

</details>

### 2.4 不做什么

- **不做符号版本的完整语法。** `foo@@LIB_1.0` 与 `foo@LIB_0.9` 并存是 ELF 独有的
  能力,无法中立表达。需要它的项目把 map 文件签入仓库,经 `[build] ldflags` 使用;
  需要**生成** map 的项目走 §3 的出口。
- **不做 per-symbol 的属性宏。** `__declspec(dllexport)` 那一套是源码的事。

## 3. 缺口二:通用链接标志

### 3.1 问题

`link-lib`、`link-search`、`link-script` 之外没有出口,因此**构建程序算出来的**链接
标志无法送达。三个具体场景:

| 标志 | 谁需要 |
|---|---|
| `-Wl,--version-script=<生成的 map>` | 导出面随 feature 组合变化的库(§2.4) |
| `-Wl,--wrap=malloc` | 接管 C 库符号的运行时:内存池、tracing、sanitizer |
| `-Wl,--exclude-libs,ALL` | 静态吞入的第三方库不得再导出,否则其符号成为本包 ABI 的一部分 |

第三条与 §2 是同一问题的两半:一半管自己的符号,一半管吞进来的符号。

### 3.2 设计

```
mcpp:link-flag=<flag>          mcpp::link_flag(s)
```

按发出顺序追加,位置在 manifest 的 `[build] ldflags` 之后。

### 3.3 传播性:到达消费者(实现时更正)

初稿判它私有,与 `include-dir` 同规。**实现时更正:这个类比是假的,而且代码就是证据。**

`linkUsage.ldflags` 是 `buildConfig.ldflags` 的一份拷贝,`propagateLinkFlags` 把依赖的
每一条 ldflag 推到消费者 —— **引擎今天没有"私有链接标志"这个策略可表达**。

更要紧的是类比本身错在哪:`include-dir` 私有,是因为编译接口有一个声明式的公开对应物
(`[build] include_dirs`),构建期程序若能加宽它就是绕过 manifest。链接标志没有这个
分裂 —— `[build] ldflags` 本来就传播。让"算出来"的形态与它自己的声明式孪生行为不同,
才是不一致,而不是防护。

**后果写明而不藏起来**:依赖发出的 `--version-script` 也会落到消费者链接行上,而这通常
不是它的本意。这个隐患**不是新的** —— 依赖在 `[build] ldflags` 里写同一条标志一直如此
—— 所以这条指令加宽的是**谁能算出这个值**,不是**这个值能到达哪里**。

相应地,C4 的反向断言("不出现在消费者的链接行上")作废。

## 4. 缺口三:包内布局

### 4.1 问题不是 `install()`

mcpp 世界里没有系统前缀:消费者解析包,不扫路径。`[resources]` 是把资产**嵌入产物**
(图标、版本元数据),不是安装。因此 `install(FILES ... DESTINATION /usr/share)` 这个
形状在这里是错的。

真实需求窄得多,且**只有被第三方按路径扫描的文件才有**:

| 机制 | 谁扫 |
|---|---|
| `/usr/share/vulkan/icd.d/*.json`,或 `VK_DRIVER_FILES` 指向的文件 | Vulkan loader |
| `OCL_ICD_FILENAMES`(追加语义)/ `OCL_ICD_VENDORS`(替换语义) | OpenCL ICD loader |
| 任意 dlopen 插件目录 | 宿主程序 |

关键点:**那个 JSON 不是给 mcpp 消费者读的,是给 loader 读的。** 它必须是包内某个
确定相对路径上的真实文件。

### 4.2 设计:扩 role,不开新 section

`[runtime].artifacts` 已经是"包内相对路径 + role + provenance"的声明。按 docs/05
附录 A 第二条(一个键若重复了别处已给出的答案则不予准入),这里**不得**新开 section。

新增一个 role:

```toml
[runtime]
artifacts = [
  { role = "library",  path = "lib/libmydriver.so.1", provenance = "built" },
  { role = "manifest", path = "share/vulkan/icd.d/mydriver.json", provenance = "built" },
]
```

`role = "manifest"` 的含义:**一个被本包之外的加载器按路径读取的数据文件**。它与
`library` 的区别不是格式,是读者 —— 这是 role 白名单里唯一缺的那一类。

**已知约束**:打包之后 `runtime.artifacts` 是封闭白名单,而已发布的描述符会跳过它
不认识的键。因此新增 role **必须**先落地引擎、发布,再由包使用;顺序反了会让老
引擎静默丢掉这条 artifact。这与 SPEC-004 §4.3 的规则同源。

### 4.3 生成文件与路径回填

ICD JSON 的内容里要写 `.so` 的位置,而那是构建期才知道的。两条约束:

1. JSON 里写的**必须**是相对于包根的路径,不得是构建目录的绝对路径。否则包一经
   移动或分发即失效 —— 这是本仓库已经付过学费的形态(载荷内嵌绝对路径)。
2. 因此生成它的是 build.mcpp,而声明它的是 `[runtime].artifacts`。二者的接缝需要
   一条指令让构建程序贡献一个 artifact 条目:

```
mcpp:artifact=<role>=<relpath>          mcpp::artifact(role, relpath)
```

**开放问题已查清**(2026-09-07 读 `src/pack/manifest_emit.cppm`):

`[[runtime.artifacts]]` 的发出**完全由 packer 自己决定**,它从 `doc.legs`(构建出来的
库)加一条 `interface` 条目生成,**不携带作者在源 manifest 里写的 `[runtime].artifacts`**。
`role` 在 manifest 解析侧是自由字符串,没有白名单;白名单效应来自读者:`prebuilt.cppm`
认 `static-library` / `shared-library` / `interface`,`prepare.cppm` 认前两个。

因此本项的工作量比初估大,且落在 packer 而非 manifest 解析:

1. `mcpp:artifact=<role>=<relpath>` 指令(与 §3 的 `link-flag` 同形,一行表项);
2. packer 要把该文件**拷进产物**并把条目**写进描述符** —— 这是新行为,今天的 packer
   只发它自己产出的东西;
3. 该文件的内容通常由构建程序生成(ICD JSON 里要写 `.so` 的位置),所以第 2 步接收的
   是构建目录里的一个路径,而落点是包内相对路径。

结论:**这一项不是"加一个 role",是给 packer 增加一条"携带被声明的文件"的通路。**
分期不变(三期),但依据从"小改动"改为"边界清楚、工作量中等,且不阻塞其他各项"。

## 5. 缺口四:探测库(是包,不是引擎)

### 5.1 现状

配置头生成今天就能写(§1 第 4 条)。缺的不是能力,是**公共实现**:每个移植过来的 C 项目
都要自己写一遍"这个头在不在""这个函数能不能链上""这个类型多宽"。不做的后果是
CMake 模块生态碎片化的重演 —— 每个项目一份略有差异的 `check_function_exists`。

### 5.2 一条硬约束:探测不得读宿主

这是本文唯一一条会被写错的设计。autotools 的探测按构造读宿主,而本生态的不变量是
相反的:**探测必须用生态解析出的编译器与 sysroot 进行**。

因此探测库的每个入口都经 `toolchain_dir()` / `toolchain_sysroot()` /
`toolchain_binutils_dir()` 组装命令行,任何一条走 `/usr/bin/cc` 的实现都是错的。
这与 rule 包驱动第二编译器时的规则是同一条(v2 设计 §1 第 1、2 条推论)。

### 5.3 形状

一个包 `mcpplibs:probe`,供 build.mcpp 导入:

```cpp
import mcpp;
import mcpp.probe;

int main() {
    mcpp::probe::Ctx cx;                       // 从 toolchain_* 组装,不读宿主
    bool mman = cx.has_header("sys/mman.h");
    bool slcpy = cx.links("strlcpy", "#include <string.h>");
    int  lw   = cx.sizeof_type("long");
    mcpp::probe::configure_file(cx, "config.h.in", out / "config.h");
    mcpp::include_dir(out);
}
```

探测结果必须**按工具链指纹缓存**,否则每次构建重探。缓存键取
`toolchain_fingerprint` 已有的值,不新造。

## 6. 新增的三处缺口(修订加入)

### 6.1 开放词表不能靠枚举取反

**问题。** CPU 回退今天只能这样写:

```toml
[target.'cfg(not(any(accelerator = "cuda", accelerator = "vulkan")))'.build]
sources = ["src/cpu/*.cpp"]
```

`accelerator` 的取值是**开放的** —— docs/20 明确说"第五个后端是一个包,不是引擎改动"。
因此这条谓词的含义会随生态增长**静默改变**:新增一个后端之后,每个工程的回退谓词都
必须被编辑。漏一个的后果是 CPU 实现与设备实现同时进入编译集,或者该编时没编。

这不是措辞问题,是一条**一般规则**:

> 一个开放词表的键,`not(any(<枚举>))` 永远不等价于"该词表为空"。因此每个开放词表的
> 层键**必须**提供一个不依赖枚举的"空"取值。

**设计。** 取值 `none`:

```toml
[target.'cfg(accelerator = "none")'.build]
sources = ["src/cpu/*.cpp"]
```

`accelerator = "none"` 为真当且仅当本次构建的加速器集合为空。`not(accelerator = "none")`
自然表达"有任意设备后端"。

**为什么是 `none` 而不是 `cpu`。** 三条:

1. **本仓库已有这个拼法。** `os = "none"` 就是裸机(docs/05 §2.7.2)。同一份 manifest
   里,同一个词,同一个意思。
2. `cpu` 引入歧义。`accel = "cuda"` 时 `cfg(accelerator = "cpu")` 是真是假?为真则 CPU
   源码永远参与编译,破坏互斥语义;为假则必须写 `accel = "cuda, cpu"`,改动既有 manifest。
3. `accel` 这条轴回答的是"哪个设备编译器、哪个架构"。CPU 不需要设备编译器,它不在
   这条轴上。把它塞进去会让这条轴同时承载两种问题。

**"CPU 后端与设备后端并存"是另一个需求,今天已经可写。** 那种形态(如 ggml 的 CPU
backend 与 CUDA backend 同时编入一个产物)不需要本项:CPU 源码放无条件的
`[build] sources`,设备源码放 `cfg(accelerator = "x")`。`not(...)` 只在**互斥接缝**
(要么这个实现,要么那个)时才需要,而互斥接缝正是本项要修的场景。

**实现规模。** cfg 求值器里 `accelerator` 走的是集合成员判定,`none` 需要一条特判:
集合为空时为真。不是免费的,但是一条条件。

**推广。** 同一条规则适用于每个开放词表的层键。`compiler`、`c-abi`、`compiler-runtime`
是否也需要 `none`,应在实现本项时一并裁定,而不是逐个再议 —— 否则这条规则会以每次
一个键的方式被重新发现。

### 6.2 生成输入的依赖粒度

**问题。** `mcpp:generated=` / `source` 归宿的语义是(docs/07 原文)"**本包每一条编译边
都等它**"。一个生成的头因此构成**包级栅栏**。

134 个 shader 无所谓 —— 它们是叶子,没有别的 TU 等它们。但一个**被少数 TU 包含的生成
头**会让全包的编译边排在它后面。在驱动、编译器这一档的规模上,这是"并行构建"与
"分阶段构建"的差别。

**设计方向。** 需要"某条编译边依赖某个具体生成文件",而不是"全包等全部生成物"。
两个候选形状:

- 让 `action` 的输出可被具体源码 glob 引用(声明式的边)
- 让生成物携带一个标签,源码侧按标签声明依赖

**本文不选型。** 这一项与 ninja 图的构造方式耦合较深,选型前需要读 `src/build/plan`
与 `ninja_backend`,确认哪种形状不会与既有的 dyndep/BMI 调度冲突。本文的职责是**指出
它是通用缺口并给出触发条件**:当一个包同时具备(a)生成的头文件,且(b)编译边数量
达到千级时,包级栅栏成为主要瓶颈。

### 6.3 多语言:Fortran

**问题。** mcpp 覆盖 C / C++ / 汇编。异构与 HPC 栈里 Fortran 不是边缘 —— 参考 LAPACK、
大量求解器,以及 Fortran + OpenMP target 这个在科学计算代码里非常常见的组合。没有
Fortran,数值栈的一大块进不来。

**归属。** 按 §7 三测试:不点名厂商(测试 1 不触发),不改变产物性质(测试 2 不触发),
但**新增一种编译边的种类**(测试 3 触发)⇒ **引擎侧**。

**规模诚实说。** 这一项比本文其余各项都大:它要求工具链模型承认第三种编译器、模块/
接口文件(`.mod`)有自己的依赖图(与 BMI 类似但不同)、以及 Fortran/C 的名字修饰与
调用约定。**本文把它记为已识别的缺口,不给设计** —— 给它一个半成品设计比不给更坏。

## 7. 归属:引擎侧还是插件侧

不列清单,给三条测试。任一为是即归该侧;测试按顺序应用。

| # | 测试 | 归属 |
|---|---|---|
| 1 | 它是否点名某个厂商或工具? | **插件**。仓库已在强制这条:`tests/unit/test_core_vendor_probes.cpp` 以文件数为分母,断言 `src/` 去注释后不含厂商工具名 |
| 2 | 它是否改变**产物是什么**(符号面、包内布局、身份)? | **引擎**。packer、索引、消费者三方必须就此达成一致,而这种一致无法住在插件里 |
| 3 | 它是否**新增一种边或节点**? | **引擎**。插件声明边,不发明边的种类 |

三条测试判的是**引擎侧还是插件侧**,不判**通用还是领域**。后者由 §0.1 的准入线判。
两把尺子会交叉,`kind = "device"` 就落在交叉格里:它是引擎侧(测试 2),但它谈的是
`accel`,按 §0.1 的准入线属**领域**。因此它**不在本文的缺口清单、判据与分期里**,
归 docs/20;此处列出只是为了让归属表完整。

应用到已识别的各项:

| 项 | 归属 | 触发的测试 |
|---|---|---|
| `exports` | 引擎 | 2 |
| `link-flag` | 引擎 | 3(边的属性) |
| `role = "manifest"` | 引擎 | 2 |
| `accelerator = "none"` | 引擎 | 3(谓词词表) |
| 生成输入粒度 | 引擎 | 3 |
| Fortran | 引擎 | 3 |
| `kind = "device"` | 引擎,但**属领域侧**(见下) | 2 —— 它让 `mcpp pack` 能**测量**出 `accel` 而不是抄声明 |
| 探测库 | 插件/包 | 1 |
| **RDC** | **插件** | 均不触发(见 §0.2) |
| `.omp` 岛、`.stdpar` 岛 | 插件 | 1 |

## 8. 编程模型的三分法,与岛化的边界

docs/20 目前是二分:SYCL 分得开,OpenMP offload 与 stdpar 分不开。**这个二分不成立,
应改为三分。**

| 模型 | 可岛化? | 依据 |
|---|---|---|
| SYCL | **天然** | kernel 是 `submit` 里的闭包,本来就隔离 |
| OpenMP offload | **重构后可以** | 把 `target` 区域提成函数放进自己的 TU,该 TU 用 offload 标志编;**调用方不需要任何 offload 标志** |
| stdpar | **重构后可以** | 见下 |

### 8.1 stdpar 可以岛化(初稿判断已更正)

初稿称 stdpar 与 mcpp 的收益"互斥"。这个说法过头了,应更正。

`-stdpar` 拆开是两件事,**两件都可表达**:

| 层面 | 是什么 | 表达为 |
|---|---|---|
| 编译侧 | 含并行算法调用点的 TU 由该编译器生成 kernel | 岛,与 `.sycl` 同形 |
| 链接侧 | 整个进程的分配器换成托管内存 | **链接行属性**,`[runtime]` 的 link intent |

关键在于分配器替换是**链接期**的事。一个普通 TU 里分配的内存,只要最终链接进了托管
分配器,就是设备可见的;不需要每个 TU 都由该编译器编译。

而且 mcpp 对"依赖强加给消费者的全镜像属性"已有先例:`cxx_runtime`。C++ 运行时的选择
正是这个形状 —— 一个包的选择决定整个镜像并沿依赖传播。托管分配器是同一类东西。

**真正丢掉的是卖点,不是能力。** stdpar 的价值主张是"源码一行不改",而岛化要求一次
重构。这是产品张力,不是架构矛盾。规范应当这样陈述,而不是宣称做不到。

### 8.2 唯一真正的对立,以及它会自己过期

只有一处:**整目标编译 + C++20 模块**。没有 offloading 编译器接受具名模块,所以一个
整目标 target 就是一个没有模块的 target。

这是**编译器能力**造成的,不是设计取舍,并且是**时限性的** —— 等 offloading 编译器
支持模块,这条对立自行消失。一条会过期的约束与一条设计取舍应分开记录,处理方式不同。

### 8.3 OpenMP 岛的已知边界

当 `declare target` 的全局数据跨 TU 时,岛会漏:设备镜像要求那个全局也在设备侧发出,
而它定义在宿主 TU 里。`.omp` 岛成立的条件是"offload 区域对设备全局自包含",这条限制
必须写进规则包的文档,不能留给用户在链接错误里发现。

## 9. 术语:"island" 这个词

概念是真的:**存在一组 TU,由另一个编译器二进制处理,其产物加入普通链接。** 这是一条
构建系统的轴,业界没有为它命名 —— CMake 直接把 CUDA 当一门"语言"绕过去了。

但这个词有两个问题:

1. **它与业界既有的轴交叉。** 业界的二分是 single-source / separate-source。按那条轴
   **CUDA 是单源**(一个 `.cu` 里既有 `__global__` 又有主机代码,由 nvcc 内部拆分),
   而 docs/20 把 CUDA 称作岛。熟悉 CUDA 的读者会在这里卡住,因为两条轴用了同一批例子
   给出相反的归类。
2. **`island` 在链接器词汇里已被占用。** ARM / Mach-O 的 *branch island*(veneer)是
   长跳转桩。mcpp 是一个谈链接的构建系统,这个碰撞是实际的。

**建议:保留词,补一句对齐**,写进 docs/20:

> 本文的"岛"是**构建系统**的轴:哪些 TU 交给另一个编译器二进制。它与编程模型的
> single-source / separate-source 轴正交。CUDA 在后者是单源,在前者是岛。

不建议改词:"岛 / 接缝"这对比喻自洽,且已进入多份文档;改名的代价大于这句对齐。

## 10. 验证:用一个陌生厂商证伪

本文各项都是"补齐"。而整条架构最强的主张是另一句:**引擎里没有厂商知识,加一个厂商
等于加一个规则包。** 它有测试在守(§7 测试 1),但**从未被一个引擎没见过的厂商检验
过** —— 现有四条 lane 全在 NVIDIA / Khronos 谱系里。

### 10.1 靶子

昇腾 CANN 栈。三条理由:

1. 完全不同的 ISA、编译器与运行时 API,与既有四条 lane 无谱系重叠。
2. Ascend C 是岛形态 —— docs/20 已把它列为岛的例子,但那是**推断**,这里可变成实测。
3. 垂直完整,从框架适配到驱动。

### 10.2 不做全栈,做一个跨层切片

全栈不可行,五条理由:驱动是内核态(按不变量本就排除);毕昇编译器是 LLVM 量级;
Python 层(`pyasc`、`pypto`、框架适配)不在覆盖内;规模是数人年;**没有真机或模拟器
则退化为编译验证** —— 正是本仓库给现有 lane 打的差评,代价放大百倍。

还有一条更微妙的:CANN 是普通 C++ 而非模块化 C++,所以这个实验测的是 mcpp 作为通用
构建系统的能力,**不测它的差异化能力**。这不是反对理由,但必须清楚测的是哪一半。

切片:

| 做 | 不做 |
|---|---|
| `rules-ascendc` 规则包 | 从源码构建毕昇编译器 |
| 毕昇**作为载荷**(厂商 URL,与 dpcpp 同待遇) | `ge` 图引擎 |
| 运行时 host API 作为载荷 | Python 层 |
| 一个算子库的**少量算子**原生构建 | `driver` |
| 算子注册元数据(若为 JSON,需核实)→ `role = "manifest"` 的真实用例 | `ops-nn` 全量 |
| 一个最小消费者,在真机或模拟器上跑出正确结果 | |

这条切片贯穿应用 → 库(导出设备代码)→ 运行时 → 设备编译器 → 设备执行,并恰好压在
`exports`、`role = "manifest"`、探测库、以及 RDC 的昇腾对应物上。**四项够用则通过;
不够则暴露第五项 —— 那正是想要的产出。**

### 10.3 调研结果(2026-09-07 实测)

三处不确定已调研,**全部证实,无阻碍项**(§10.3.3 的初次否定结论已被推翻)。仓库全部可匿名 clone
(`gitcode.com/cann/*`,分支 `8.5.0`),许可为 CANN Open Software License Agreement 2.0。

#### 10.3.1 无硬件执行:存在,且是厂商自己的测试路径(证实)

Ascend C 有**三种**运行模式,经 `-DCMAKE_ASC_RUN_MODE=` 选择:

| 模式 | 需要硬件 | 走不走岛 |
|---|---|---|
| `npu`(默认) | 是 | 是 |
| `sim`(NPU 仿真) | 否 | **是**(见下) |
| `cpu`(CPU 调试) | 否 | **否** |

仿真器**随工具链发布**,按 SoC 分库:`${ASCEND_DIR}/*/simulator/<SoC>/lib`,
`Findpvmodel.cmake` 里的目标是 `pvmodel_ascend910` / `pvmodel_ascend310p` /
`pvmodel_ascend610` 与 `pem_davinci_ascend910B1` / `pem_davinci_ascend310B` /
`pem_davinci_ascend610Lite`。**asc-devkit 自己的单元测试就链接它们**
(`tests/unit/basic_api/ut/CMakeLists.txt`),所以这是厂商既有的无卡测试路径,
不是我们发明的用法。

**关键区分,而且它决定判据能不能用:**

`cpu` 模式链接 `tikicpulib::${SOC_VERSION}` 并使用 `compiler/tikcpp/` 的头文件
(实测于 `cmake/asc/legacy_modules/function.cmake`)。也就是说**同一份 kernel 源码由
宿主编译器编译**,构建图里根本没有岛。用 `cpu` 模式跑绿,证明的是 kernel 数值对,
**不是岛的机制对** —— 它没走那条路径。这正是本仓库反复付学费的形态:判据施加在
错误的对象上。

`sim` 模式保留岛。**这一条已由源码证实,不再是推断**(`asc-devkit/tools/ascc/cmake/`
与 `cmake/asc/legacy_modules/`):

| 证据 | 内容 |
|---|---|
| `CMakeDetermineASCCompiler.cmake:47` | `find_program(CMAKE_ASC_COMPILER NAMES "bisheng" PATHS ".../ccec_compiler/bin/")` |
| `CMakeASCInformation.cmake:49` | `CMAKE_ASC_COMPILE_OBJECT = "<CMAKE_ASC_COMPILER> … -c -x asc <SOURCE>"` |
| `host_config.cmake:69` | `CCEC_LINKER = <toolkit>/ccec_compiler/bin/ld.lld` —— 设备侧链接 |
| **全仓 `RUN_MODE` 判断** | **每一处都是 `STREQUAL "cpu"`,不存在 `sim` 分支** |

最后一行是决定性的:构建期只区分 `cpu` 与**非** `cpu`,因此 **`sim` 与 `npu` 走完全
相同的编译路径**,bisheng 被调用,岛成立。两者的差别在**运行期**(加载哪套运行时 /
仿真库),不在构建图。

分支点上唯一的额外动作是:非 `cpu` 分支多跑一个 `update_host_stub.py` —— 生成宿主侧
的启动桩,而那正是"存在一个真实设备二进制需要被拉起"的标志。

**为什么这个疑问值得问。** 反过来的设计是存在的,而且有正当理由:指令级仿真比跑宿主
代码慢几个数量级;宿主编译器能给 gdb / ASAN / printf。CANN 的选择是把这两种诉求拆开
—— `cpu` 模式**就是**那个设计,所以 `sim` 若不执行真实设备指令便与 `cpu` 重复,没有
存在的理由。三种模式而非两种,本身就是答案。

#### 10.3.2 算子注册元数据:JSON,且按 SoC 分(证实)

实测 `ops-math`(72 MB,`math/` 下 1451 个 `.cpp`、927 个 `.h`):

```
math/<op>/op_kernel/          设备侧
math/<op>/op_host/            宿主侧
math/<op>/op_host/config/<soc>/<op>_binary.json
math/<op>/op_host/config/<soc>/<op>_simplified_key.ini
```

`math/` 一棵树里 166 个 JSON、143 个 INI。JSON 的内容是算子签名到**设备二进制文件名**
的映射:

```json
{ "op_type": "Abs",
  "op_list": [ { "bin_filename": "Abs_1c4543fdfe...",
                 "inputs": [ { "dtype": "bfloat16", "format": "ND", ... } ] } ] }
```

**这是 `role = "manifest"` 的教科书用例**:一个数据文件,由本包之外的运行时按路径读取,
以文件名指向设备产物。

而且 SoC 目录有六个 —— `ascend310p`、`ascend910`、`ascend910_93`、`ascend910_95`、
`ascend910b`、`kirinx90`。**注册文件是按目标条件化的**,因此这个切片同时压在
`role = "manifest"` 与 SPEC-004 的目标轴上。

另外两项确认:**源码布局本身已经是岛** —— 每个算子的 `op_kernel/` 与 `op_host/` 是分开
的目录;设备架构标志是 `CMAKE_ASC_ARCHITECTURES=dav-2201`,与 `sm_89` 同类。

#### 10.3.3 工具包的获取:官方镜像,可匿名拉取(实测,已推翻初次结论)

**初次调研把这一条判成"未能证实、构成阻碍"。复查后推翻。**

首先纠正一个框架错误:初次把"毕昇"与"模拟器"当成两个获取问题。**它们在同一个包里** ——

| 组件 | 路径 |
|---|---|
| 设备编译器 | `${ASCEND_DIR}/compiler/ccec_compiler/bin/bisheng` |
| 模拟器 | `${ASCEND_DIR}/*/simulator/<SoC>/lib` |

两者都在 CANN 工具包内部,所以这是**一个**获取问题,不是两个。

**官方获取方式是 Docker 镜像**,而不是 `.run` 安装包(实测于 `ops-math/QUICKSTART.md`):

```
swr.cn-south-1.myhuaweicloud.com/ascendhub/cann:8.5.0-910b-ubuntu22.04-py3.10-ops
```

**它可以匿名拉取。** 实测过程与结论:

| 步骤 | 结果 |
|---|---|
| `GET /v2/` | `401` |
| 匿名 token:`GET /swr/auth/v2/registry/auth?service=dockyard&scope=repository:ascendhub/cann:pull` | **签发**(2107 字节 JWT) |
| 持 token 取 manifest | **200**,manifest list,含 `arm64` 与 `amd64` |

**那个 401 差点让我下错结论。** 裸的 401 与"需要凭据"读数相同,但它同样是匿名 token
握手的第一步(Docker Hub 就是这样)。只看第一步会把"可匿名获取"误判成"不可得" ——
这正是本仓库记过的形态:判据的"否"与"没测成"同读数。**判据必须走完握手。**

**合规性:正好落在不变量允许的那一档。** 本生态的规则是"专有厂商用户态要么在它已经
在的地方链接,要么从**厂商自己发布的 URL** 取 —— 绝不拷进 xlings-res 的发布物"。
从华为自己的 registry 拉官方镜像**就是**这一档,不需要再分发权。

结论:**这一条不再是阻碍。** 剩下的是工程问题(镜像里取哪些目录、怎么做成载荷),
不是许可问题。

#### 10.3.3b AscendNPU IR 已开放,存在第二个切入点

毕昇开放了 **AscendNPU IR**(昇腾自有的 MLIR dialect),并给出 Triton 的完整路径:
Triton IR → Linalg IR → AscendNPU IR → 算子二进制,配套 `triton-ascend`。

这不改变"毕昇二进制在工具包里"这个事实,但它意味着**规则包有两个可选的切入高度**:

| 切入点 | 输入 | 代价 |
|---|---|---|
| Ascend C(§10.2 的切片) | `op_kernel/*.cpp` | 直接对应现有算子库 |
| Triton / AscendNPU IR | Triton kernel | 与 CUDA 侧的 Triton 生态同构,但离现有 CANN 算子库更远 |

**本文不选型**,只记录第二条存在。对"验证 mcpp 完备性"这个目的而言 Ascend C 更直接,
因为它才是 `ops-math` 里那 1451 个 `.cpp` 的实际形态。

#### 10.3.4 一个附带发现:CMake 把 Ascend C 当作一门"语言"

`cmake/asc/ASCConfig.cmake` 注释写着 `plugin support ASC language`,并从
`$ENV{ASCEND_HOME_PATH}/compiler/tikcpp/ascendc_kernel_cmake/ASC_CMake/FindASC.cmake`
引入。开源仓库里只有 `include()`,**真正的设备编译机制在工具包内部**。

两个推论:

1. 移植意味着**读工具包自带的 cmake 来学会它发什么标志**,与 `rules-sycl` 驱动 dpcpp
   的做法同形,不是新问题。
2. 这是 §9 那条轴的第三方佐证:CMake 用 `enable_language(ASC)` 把它放进**引擎**,
   mcpp 用规则包把它放进**包**。**同一条轴,不同的归属** —— 而归属正是 §7 要裁决的事。

### 10.4 可行性闸(两次修正后)

| 目的 | 需要 | 结论 |
|---|---|---|
| **架构验证**(本实验) | 无 | **可开工**。裁决判据 `git diff src/` 为空完全在构建期 |
| **设备执行判据** | `sim` 模式 + 工具包 | **可得**。工具包镜像匿名可拉(§10.3.3),`sim` 无需硬件(§10.3.1) |
| 真机验证 | 昇腾硬件 | 可选,不阻塞以上任何一项 |

**没有阻碍项。** 初稿列的三条前置里,两条证实、一条被推翻;剩下的都是工程量。

**没有待确认的技术问题。** 初稿留的最后一条(`sim` 是否仍调用 `bisheng`)已由源码
证实为"是"(§10.3.1)。

### 10.5 裁决判据

阶段一(`rules-ascendc` + 一个 hello kernel)两条:

1. **设备执行。** 在 `sim` 模式下跑出正确结果,**断言设备名/执行路径而非结果数值**
   (数值在 `cpu` 模式与静默回退时同样正确)。**不得用 `cpu` 模式充当这条** ——
   理由见 §10.3.1。`sim` 调用 `bisheng` 已证实,因此这条落在正确的对象上。
2. **引擎无厂商知识。** 见下,这条初稿写错了。

#### 10.5.1 第二条判据的更正:`git diff src/` 为空分不开两种失败

初稿把它写成"`git diff src/` 为空"。**这个判据的"否"有两个成因,而它分不开:**

| 若 `src/` 有改动 | 含义 | 是否推翻主张 |
|---|---|---|
| 改动里出现昇腾专有标识 | 引擎吸收了厂商知识 | **推翻** |
| 改动是 `exports` / `link-flag` 这类**通用**能力 | 引擎缺一项通用基础设施 | **不推翻** —— 那正是 §2–§6 在补的东西 |

两者读数相同,所以这条判据在通用缺口落地**之前**跑,必然把第二种误报成第一种。

**更正后的两级判据:**

| 级 | 判据 | 何时可跑 |
|---|---|---|
| **主** | 移植完成后,`tests/unit/test_core_vendor_probes.cpp` **仍然绿** —— 它以文件数为分母,断言 `src/` 去注释后不含厂商工具名 | **任何时候**。它按性质判定,不按有没有改动判定 |
| 严 | `git diff src/` 为空 | 仅在 §13 的一、二、三期落地**之后**才有意义 |

主判据才是这条架构主张的直接检验,而且它不需要前置。严判据是附加的更强陈述。

主判据为否 —— 即为了让昇腾跑起来,不得不把厂商标识写进 `src/` —— 那就是"这条架构
主张在第一个陌生厂商面前没有成立"。这个结论比移植成功更有价值,也更该早点知道。

### 10.6 范围警告

交付物是**验证物,不是承诺长期维护的 fork**。算子库切片钉在 `8.5.0`,不承诺跟随。

## 11. 准入自检(docs/05 附录 A)

| 项 | 是否重复了别处已给出的答案 |
|---|---|
| `exports` | 否。没有任何 section 回答"这个产物发布哪些符号" |
| `link-flag` | 否。`ldflags` 是声明式的,本项是构建程序算出来的 |
| `role = "manifest"` | 否,且**刻意复用** `[runtime].artifacts` 而非新开 section |
| `accelerator = "none"` | 否,且**刻意复用** `os = "none"` 的既有拼法,不新造词 |
| 生成输入粒度 | 否。现有语义是包级,本项是同一概念的细化,不是第二个概念 |
| Fortran | 否 |
| 探测库 | 不是键 |

四项均为封闭语法、开放词表:`exports` 的内容由作者定,引擎只负责渲染;
`link-flag` 的内容引擎不解释;role 的白名单加一项而语义由读者定义。

## 12. 判据

每条都要求两侧可测 —— 拿掉实现会红,而不是"没测成"与"通过"同读数。

| # | 判据 |
|---|---|
| C1 | 声明 `exports` 的共享库,`nm -D --defined-only` 只列出声明的符号;不声明时列出全部。两侧都断言,否则"少了几个"与"根本没链上"读数相同 |
| C2 | 同一份 `exports` 在 ELF 与 PE 上各渲染一次,两边导出集合**相同**。跨平台是这条设计的全部理由,单平台绿零信息量 |
| ~~C3~~ | **作废**(§2.3):它断言的隐含 hidden 在实现时被取消,一个键一个效果 |
| C4 | 构建程序**算出来的** `link-flag` 到达链接器。判据是**链接器的行为**而非命令行文本:e2e 620 让程序算出 `-Wl,--defsym=…=42`,产物打印那个符号的地址。grep build.ninja 会对"写下了但没交给链接器"同样成立 |
| C5 | `role = "manifest"` 的文件在打包后位于声明的相对路径上,内容里的路径为包内相对路径。判据读**打包后的产物**,不读构建目录 |
| C6 | 探测库在一台**没有宿主编译器**的机器上仍能完成探测。这是 §5.2 唯一能证伪的判据 |
| C7 | 声明 `cfg(accelerator = "none")` 的回退源码,在 `accel` 为空时编译、在任意后端被命名时不编译;**且新增一个后端后该谓词的行为不变** —— 这条才是本项的理由,单后端下绿零信息量 |
| C8 | 一个包同时具备生成头与千级编译边时,生成头**不**阻塞与它无关的编译边。对照组是今天的包级栅栏 |
| **C9** | **验证项(§10)。** 昇腾切片移植完成后:(a) `test_core_vendor_probes.cpp` 仍绿 —— 主判据,按性质判定,任何时候可跑;(b) `sim` 模式下跑出正确结果且**断言执行路径而非数值**;(c) 四项通用缺口够用,否则暴露的第五项本身就是产出。**不得用 `cpu` 模式充当 (b)** —— 它不走岛(§10.3.1) |

**Fortran(§6.3)没有判据,因为本文没有给它设计。** 这是有意的:一个没有设计的条目配上
一条判据,会让它看起来比实际成熟。它在 §13 里也不占期次。

C6 值得单独说明:它是这批里唯一无法在开发机上验证的判据 —— 开发机总有
`/usr/bin/cc`,一个错误读宿主的实现在那里永远绿。它必须跑在 hermetic 容器里,
本仓库已有 `hermetic e2e (no host toolchain, container)` 这个 job。

## 13. 分期

| 期 | 内容 | 依据 |
|---|---|---|
| 一(**已实现** 2026.9.6.5) | `accelerator = "none"` | 最小,且它修的是一条**会随生态增长而静默失效**的写法 —— 越晚落地,要改的既有 manifest 越多 |
| 一(**已实现** 2026.9.6.5) | `link-flag` | 同样最小:一条指令 + 一处透传。且它是 §2.4 的逃生口,应先于 `exports` 落地 |
| 二(**已实现** 2026.9.6.5,不含隐含 hidden) | `exports` | 打开"可发布稳定 ABI 的 `.so`"这一档,同时惠及运行时与驱动 |
| 三 | `role = "manifest"` + `mcpp:artifact` | 只有驱动这一档需要;且受 §4.2 的发布顺序约束,越早落地引擎越好 |
| 四 | `mcpplibs:probe` | 与引擎正交,任何时候可做;但 C6 要求它一开始就跑在 hermetic job 里 |
| 五 | 生成输入粒度 | 触发条件明确(§6.2),未达到该规模前不做 |
| — | Fortran | 已识别,本文不给设计 |
| 并行 | RDC(`rules-cuda`)、`.omp` 岛 | 插件侧,不依赖以上任何一项 |
| **验证** | **昇腾切片(§10)** | 见下 |

一、二、三期合计的引擎改动量小于 RDC 一项,且互不阻塞。RDC 归插件侧之后,**关键路径
上不再有大件**。

### 13.1 验证项的位置

**主判据(C9a)不依赖任何一期,随时可跑。** 它按性质判定 —— 移植后 `src/` 里有没有
出现厂商标识 —— 而不是按有没有改动判定。

**严判据(`git diff src/` 为空)必须在一、二、三期之后。** 在那之前跑,一次"缺通用
能力"会被误读成"引擎吸收了厂商知识"(§10.5.1)。

因此推荐的顺序是:**一、二、三期落地 → 昇腾切片**。但如果想更早拿到信息,只跑主判据
也是有效的,而且它可能提前暴露第五项通用缺口 —— 那种情况下,缺口清单本身就被验证
补全了一次,这比等到三期做完再发现要便宜。

**这一项不是引擎工作量,是判断整套设计对不对的实验。** 它的产出是一份带判据的报告
加一个规则包(§10.6),不是一个要长期维护的 fork。

## 14. 变更记录

| 版本 | 变更 |
|---|---|
| 初稿 | 四处通用缺口:`exports`、`link-flag`、包内布局、探测库。配置头与包内布局在核实 main 后各自缩小 |
| 修订一 | 两处判断被推翻并就地更正:**RDC 归插件侧,引擎零改动**(§0.2);**stdpar 可岛化,初稿的"互斥"说法过头**(§8.1)。新增三处缺口(§6)、归属规则(§7)、编程模型三分法(§8)、术语对齐建议(§9)、以及用陌生厂商证伪的实验(§10) |
| 修订二(第三处结论已被修订三推翻) | §10 的三处不确定实地调研(§10.3),两处证实一处未证实。第三处更正:**"有硬件或模拟器"不是开工硬闸**,架构验证完全在构建期(§10.4);真实前置只有毕昇的合规取得。新增两条实测结论:`cpu` 模式**不走岛**因而不能充当设备判据(§10.3.1),以及 CMake 把 Ascend C 当作一门语言 —— 同一条轴、不同归属的第三方佐证(§10.3.4) |
| 修订三 | §10.3.3 的结论被**推翻**:工具包官方镜像 `swr.cn-south-1.myhuaweicloud.com/ascendhub/cann` **可匿名拉取**(走完 token 握手实测),且从厂商自有 registry 取正落在不变量允许的一档。同时纠正一个框架错误:毕昇与模拟器**在同一个包里**,是一个获取问题不是两个。**至此该实验没有阻碍项。** 另记 AscendNPU IR 已开放,规则包存在第二个切入高度(§10.3.3b) |
| 修订六 | 一、二期实现落地(mcpp 2026.9.6.5),实现过程中三处判断被更正:**`link-flag` 到达消费者**(§3.3,`linkUsage.ldflags` 是 `buildConfig.ldflags` 的拷贝,私有形态引擎无从表达);**`exports` 不隐含 hidden**(§2.3,一个键一个效果);**C3 作废、C4 改写**。另查清 §4.3 的开放问题:packer 不携带作者声明的 artifacts,所以三期是给 packer 增加一条通路而非加一个 role |
| 修订五 | 综合复核。两处补齐:验证项此前**既无判据也无期次**,现补为 C9 与 §13.1;`kind = "device"` 此前是孤儿条目,现按"两把尺子"说明它落在引擎侧但属领域,归 docs/20。一处更正:C9 的严判据 `git diff src/` 为空**分不开两种失败**(吸收了厂商知识 vs 缺一项通用能力),改为两级判据,主判据用既有的 vendor-probe 测试(§10.5.1) |
| 修订四 | 最后一条待确认项闭合:`sim` **确实调用 bisheng**,由 `asc-devkit` 内 vendored 的 `ASC_CMake` 证实 —— ASC 是一门 CMake 语言,其编译器就是 `bisheng`,且**全仓 `RUN_MODE` 判断只区分 `cpu` 与非 `cpu`,不存在 `sim` 分支**。至此 §10 无待确认项 |
