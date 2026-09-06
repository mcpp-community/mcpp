# 构建系统的四处通用缺口

2026-09-07。本文只处理**通用构建基础设施**:其他构建系统都有对应物、与异构无关、
与本仓库的领域无关的那些能力。异构方向特有的缺口(RDC)不在本文,理由见 §0.2。

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

### 0.2 RDC 为什么不在本文

relocatable device code 的存在理由是"设备编译器把 `__device__` 函数的跨 TU 调用
推迟到一次设备链接"。这句话无法脱离设备概念陈述,因此它属于 docs/20 的领域,
不属于本文。

它复用的原语在引擎里已经存在:`mcpp::action` 的 `object` 归宿就是"一个外部步骤
产出的对象加入普通链接",SYCL lane 的 `sycl_device_link.o` 正是这个形状。所以
RDC 是一个 rule 包的工作量,不是本文四项中的任何一项。

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

### 2.3 声明 `exports` 隐含编译期默认 hidden

仅有 version script 会收窄动态符号表,但对象里的符号仍是默认可见性,链接期优化拿
不到收益,而且 Mach-O 与 PE 的渲染需要编译期配合。因此:**声明 `exports` 时,引擎
同时把编译期默认置为隐藏**(ELF/Mach-O 的 `-fvisibility=hidden`)。

这是一处行为变化,必须写进文档:一个此前依赖默认可见性做跨 DSO 内部调用的项目,
在声明 `exports` 之后会链接失败。这正是作者声明 `exports` 时所要求的语义,失败点
也在链接期而非运行期,因此是可接受的。

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

### 3.3 传播性:私有

`link-flag` **只作用于本包的链接,不到达消费者**,与 `include-dir` 同规。理由相同:
一个依赖发出的任意标志落到消费者的链接行上,正是 `include-dir` 的私有性所要避免的
耦合。

`link-script` 是既有的例外,而它的例外理由在文档里写得很清楚 —— 板级内存布局是
消费者无法自行写出的东西。任意标志不具备这条性质,因此不继承这个例外。

依赖确实需要改变消费者链接方式的情形,已有 `[runtime]` 的 link intent 承担,且那条
路径是中立的、可按方言渲染的。

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

**开放问题**:`mcpp pack` 目前决定哪些文件进入产物。构建程序贡献的 artifact 条目
与 pack 的选择规则如何合并,需要在实现前读 `src/pack` 确定,本文不预设答案。

## 5. 缺口四:探测库(是包,不是引擎)

### 5.1 现状

配置头生成今天就能写(§1.4)。缺的不是能力,是**公共实现**:每个移植过来的 C 项目
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

## 6. 准入自检(docs/05 附录 A)

| 项 | 是否重复了别处已给出的答案 |
|---|---|
| `exports` | 否。没有任何 section 回答"这个产物发布哪些符号" |
| `link-flag` | 否。`ldflags` 是声明式的,本项是构建程序算出来的 |
| `role = "manifest"` | 否,且**刻意复用** `[runtime].artifacts` 而非新开 section |
| 探测库 | 不是键 |

四项均为封闭语法、开放词表:`exports` 的内容由作者定,引擎只负责渲染;
`link-flag` 的内容引擎不解释;role 的白名单加一项而语义由读者定义。

## 7. 判据

每条都要求两侧可测 —— 拿掉实现会红,而不是"没测成"与"通过"同读数。

| # | 判据 |
|---|---|
| C1 | 声明 `exports` 的共享库,`nm -D --defined-only` 只列出声明的符号;不声明时列出全部。两侧都断言,否则"少了几个"与"根本没链上"读数相同 |
| C2 | 同一份 `exports` 在 ELF 与 PE 上各渲染一次,两边导出集合**相同**。跨平台是这条设计的全部理由,单平台绿零信息量 |
| C3 | 声明 `exports` 后编译期默认为 hidden:一个依赖默认可见性做跨 DSO 内部调用的夹具**链接失败**,且失败点在链接期 |
| C4 | 构建程序发出的 `link-flag` 出现在链接命令行上,顺序在 `ldflags` 之后;**且不出现在消费者的链接行上**(私有性的反向断言) |
| C5 | `role = "manifest"` 的文件在打包后位于声明的相对路径上,内容里的路径为包内相对路径。判据读**打包后的产物**,不读构建目录 |
| C6 | 探测库在一台**没有宿主编译器**的机器上仍能完成探测。这是 §5.2 唯一能证伪的判据 |

C6 值得单独说明:它是这批里唯一无法在开发机上验证的判据 —— 开发机总有
`/usr/bin/cc`,一个错误读宿主的实现在那里永远绿。它必须跑在 hermetic 容器里,
本仓库已有 `hermetic e2e (no host toolchain, container)` 这个 job。

## 8. 分期

| 期 | 内容 | 依据 |
|---|---|---|
| 一 | `link-flag` | 最小:一条指令 + 一处透传。且它是 §2.4 的逃生口,应先于 `exports` 落地 |
| 二 | `exports` + 隐含 hidden | 打开"可发布稳定 ABI 的 `.so`"这一档,同时惠及运行时与驱动 |
| 三 | `role = "manifest"` + `mcpp:artifact` | 只有驱动这一档需要;且受 §4.2 的发布顺序约束,越早落地引擎越好 |
| 四 | `mcpplibs:probe` | 与引擎正交,任何时候可做;但 C6 要求它一开始就跑在 hermetic job 里 |

前三期合计的引擎改动量小于 RDC 一项,且互不阻塞。
