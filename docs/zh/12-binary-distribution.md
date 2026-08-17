# 12 - 分发预编译库

[English](../12-binary-distribution.md) | **简体中文**

> 把一个库以**接口 + 预编译二进制**的形式分发,而不是发源码。
> 这是闭源场景、离线场景,以及「构建农场已经编过一遍了」的场景。
>
> 姊妹篇:[02 - 打包应用](02-pack-and-release.md) 讲的是打包**程序**;
> [10 - 发布一个库](10-publishing-a-library.md) 讲的是源码通路。

## 一段话讲完

`mcpp pack <target>` 构建一个库目标,产出一个**普通的 mcpp 包** ——
一份正常的 `mcpp.toml`、消费者要编译的接口、以及它随后链接的二进制。
消费者用它和用任何依赖一样。**没有新的 manifest 段、没有新的归档格式、
没有新的解析路径。**

```bash
mcpp pack mathkit                              # 静态库包
mcpp pack mathkit-shared                       # 动态库包(今天仅 Linux/ELF)
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu   # 一个包,两条腿
```

## 打什么由谁决定

只由 `[targets.<name>].kind` 决定:

| `kind` | `mcpp pack <name>` 产出 | `--mode` |
|---|---|---|
| `bin` | 应用 bundle(见 [02](02-pack-and-release.md)) | 四档 |
| `lib` | **静态库包** | — |
| `shared` | **动态库包** | — |

**没有 `--lib`,也没有 `--artifact static|shared`。** `kind` 本来就是 mcpp
记录「一个产物是什么」的地方;再加一个开关就是同一件事的第二个说法,
而两个说法可以互相矛盾。要同时发布两种形态,就声明两个目标 ——
这本来也是 `mcpp build` 同时产出两者所必需的:

```toml
[targets.mathkit]
kind = "lib"

[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
```

不带名字直接 `mcpp pack`,mcpp 会挑唯一可打包的目标,或者告诉你有哪些候选。

## 两种接口模式

一个包可以同时带两种,消费者用其中一种或两种都用。

```
mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23/
├── mcpp.toml
├── include/          ← 文本接口:#include,永不编译
├── interface/        ← 模块接口:消费者编译它
└── lib/<triple>/     ← 产物
```

| | `include/` | `interface/` |
|---|---|---|
| 是谁的输入 | 预处理器 | **编译器** |
| 消费者要编译吗 | 否 | **是**,编出 BMI |
| 约束什么 | libc ABI | 编译器、C++ 标准库、C++ 档位 |
| 能裁剪吗 | **不能**,见下 | **不能**,它是算出来的 |

`lib/` 按**三元组**分目录,不按 OS 分:MinGW 与 MSVC 同为 Windows,
一个产 `libfoo.a` 一个产 `foo.lib`。

### 为什么两者都不许裁剪

同一个包的**源码**分发会把 `include_dirs` 里的每一个头都放到消费者的 include
路径上。二进制包若只发一部分,**同一个库就会因为分发形式不同而有不同的公开面**。
而且「哪些头是公开的」布局已经回答了:`include/` 公开,`src/` 不公开。
一个私有头放在 `include/` 下是工程布局的错误,不是打包选项。

## 哪些 `.cppm` 会被发布

**lib root 的模块闭包** —— 按约定是 `src/<包名尾段>.cppm`,或 `[lib].path`。
该单元 purview 里 import 到的东西,传递地,都发布;其余都不发。

```
src/mathkit.cppm    export module mathkit;  export import :api;   → 发布
src/api.cppm        export module mathkit:api;                    → 发布
src/secret.cppm     module mathkit:secret;          ← 实现分区
src/impl.cpp        module mathkit;                               → 不发布
```

`mcpp pack` 会打印两张清单:

```
   Interface mathkit.cppm, api.cppm
    Withheld capi.c, impl.cpp, secret.cppm
```

**闭源分发要看第二张。**

> **`.m.o` 不是判据。** 实现分区照样产出 BMI 和对象。按「会不会产出 BMI」
> 来挑发布集,就会把 `secret.cppm` 发出去。

如果被发布的接口**确实** import 了一个实现分区,消费者没有那份源码就编不出来 ——
于是 `mcpp pack` 停下来:

```
error: the published interface imports mathkit:secret , which no unit in this
       build provides.
```

要么重构让接口够不到它,要么把它改成 `export module` 分区并接受源码被发布。

## 兼容性 tag

每个产物都记录它是为哪套工具链编的:

```
x86_64-linux-gnu-gcc16-libstdcxx16-c++23      # C++ 模块接口
x86_64-linux-gnu                              # 纯 extern "C" 接口
```

`<arch>-<os>-<env>`,接口是 C++ 时再加 `<compiler><major>`、
`<stdlib><major>`、`c++<档位>`。

**短 tag 是一句真实的声明,不是漏写。** 一个接口全是 `extern "C"` 的库
只约束 libc ABI、不约束 C++ ABI,所以它发三段就停 —— 于是能链进任何编译器。
未指定的维度就是不关心,C 库的 tag 数因此是「每个三元组一个」而不是
「每个三元组 × 每个编译器一个」。**不需要任何开关:形状本身就是声明。**

`c++` 档位按**下限**比对而不是相等:消费者档位更高可以,更低不行。

## 消费者的构建会检查什么

两件事,而且都是不检查就会静默出错的:

**接口与二进制仍然配对。**

```
error: acme.mathkit@0.1.0: 'interface' does not match what was packaged.
  recorded fnv1a:25b2cf2a79d71c40
  found    fnv1a:fe404d5be85118ff
```

这条闸门存在是因为另一种结果被实测过:把随包接口里一个结构体的两个 `int`
成员互换 —— Itanium ABI 不 mangle 字段顺序 —— 消费者**编译过、链接过、
运行过、打印出交换后的错数据**,任何工具都没有一句诊断。
digest 挡不住发布者一开始就发错配对(那只有原子产出能防),
但它能挡住配对在事后被拆开。

**二进制是为这套工具链编的。**

```
error: acme.mathkit@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags :
                   x86_64-linux-gnu-gcc15-libstdcxx15-c++23
  closest is x86_64-linux-gnu-gcc15-libstdcxx15-c++23, and it differs on:
    compiler  needs gcc15, this build has gcc16
    stdlib    needs libstdcxx15, this build has libstdcxx16
```

诊断里**列出它有哪些 tag** 是刻意的:一句「找不到」会让人去找一个
就在自己硬盘上的包。

## 怎么消费

三种写法,同一条代码路径:

```toml
# 一个目录(你直接拷给同事的那种)
mathkit = { path = "vendor/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23" }

# 私有 git 仓库
mathkit = { git = "ssh://git@internal/mathkit-dist.git", tag = "v0.1.0" }

# 索引条目 —— 形状与源码包一字不差
mathkit = "0.1.0"
```

消费者的 manifest 里没有任何一处写着「这个是预编译的」。

### 在包目录里直接 build 会被拒绝

```
error: … is a distribution package produced by `mcpp pack`, not a source tree.
```

它的 `interface/` 里是声明,定义在旁边的归档里。在那儿构建会把声明编出来、
产出一个几乎空的库、然后报告成功。

## 一个包,多个 target

`--target` 可重复。生成的 manifest 每条腿一个条件块,消费者的构建各选各的:

```toml
[target.'cfg(all(arch = "x86_64", os = "linux", env = "gnu"))'.build]
ldflags = ["-Llib/x86_64-linux-gnu", "-lmathkit"]

[target.'cfg(all(arch = "x86_64", os = "linux", env = "musl"))'.build]
ldflags = ["-Llib/x86_64-linux-musl", "-lmathkit"]
```

因为选择发生在**消费者的构建期**(那时解析后的 target 已知),
胖包的交叉编译天然正确,**不需要索引侧或安装侧做任何支持**。

> 这些块是 `cfg(...)`,绝不是裸的 `[target.'<三元组>']` 键。
> 在 mcpp 2026.8.17.2 之前,裸三元组在没有显式 `--target` 时是失效的 ——
> 用它的包会在 CI 里正常、在开发者机器上静默丢掉 flag。
> mcpp 生成的是在**所有**客户端上含义一致的那种写法。

## 依赖

静态归档**不携带**它依赖的代码,所以包会把依赖记下来,由消费者解析:

```toml
[dependencies]
"compat.zlib" = "1.3.2"
```

`path` 与 `git` 依赖会被丢弃:它们指向发布者的磁盘,原样发出去等于
给消费者一个在他们那里含义完全不同的地址。如果你的库依赖这类东西,
要么把它也发布出去,要么在打包前 vendor 掉。

## 老版本 mcpp 拿到这种包会怎样

**能构建。** 生成的 manifest 里每一个键都是既有的,所以老客户端读得懂、
链得上。它做不到的是执行上面那两道闸门 —— 它无从知道
`provenance = "mcpp-pack …"` 有什么含义。

这是**降级**而不是变砖,方向是对的。但它意味着**闸门只保护新客户端**,
如果你面向的是混合版本的用户群,这一条应当写进发布说明。

## 当前边界

| | 状态 |
|---|---|
| `kind = "lib"`(静态) | ✅ 所有 target,三平台都测了 |
| `kind = "shared"` on Linux/ELF | ✅ —— 包里同时带链接名与 SONAME |
| `kind = "shared"` on PE / MinGW(`*-windows-gnu`) | ✅ —— 包里同时带 `.dll` **和它的导入库** |
| `kind = "shared"` on Mach-O(`*-macos`) | ✅ —— install name 是 `@rpath/<file>`,`.dylib` 可重定位 |
| `kind = "shared"` on PE / MSVC(`*-windows-msvc`) | ❌ 拒绝 —— 见下 |
| `kind = "shared"` on `*-musl` | ❌ musl target 是静态链接的 |
| 发布预编译 BMI | ❌ 未尝试;BMI 与编译器构建逐位绑定 |
| 把依赖打包进去 | ❌ 改为声明依赖(见上) |
| 用**原生 `cl.exe`** 消费这种包 | ❌ 见下 |

### MSVC 为什么拒绝 `kind = "shared"`

**不是链接器的问题** —— `link /DLL /IMPLIB:` 本来就能用。是**符号导出**:
MSVC 在没有 `__declspec(dllexport)`、也没有 `.def` 列出符号时,DLL **什么都不导出**,
于是导入库是空的,每个消费者都会拿到一堆 unresolved externals,而那些符号
明明就在对象文件里 —— 报错点离病因很远。mcpp 选择拒绝,而不是产出这种诊断:

```
target 'mathkit': kind = "shared" is not supported for the MSVC ABI (x86_64-windows-msvc).
  ...
  Use kind = "lib" for this target, or build it for *-windows-gnu (MinGW),
  where the linker auto-exports.
```

MinGW 的链接器会自动导出,这就是 `*-windows-gnu` 支持而 `*-windows-msvc` 不支持的
全部原因。要补齐它需要生成 `.def`(对对象做一次符号扫描)—— 那是一个构建图节点,
不是一个 flag。

### 包里的链接 flag 是 GNU 拼写

生成的 manifest 用这种方式选腿:

```toml
[target.'cfg(all(arch = "x86_64", os = "windows", env = "msvc"))'.build]
ldflags = ["-Llib/x86_64-windows-msvc", "-lmathkit"]
```

mcpp 用到的每个 driver 都吃这一套 —— 包括 Windows 上默认的、面向 MSVC ABI 的
clang。**原生 `cl.exe` 不吃**:它不认 `-L`。所以固定了
`[toolchain] windows = "msvc@system"` 的消费者目前链不上打包库。

**改成直接写文件路径也不行**(`lib/<triple>/mathkit.lib` 才是每个 driver 都吃的
拼写):ninja 执行链接命令时 cwd 是**输出目录**,而只有 include 家族前缀
(`-I`、`-L` …)会被 `normalize_include_flags` 相对包根绝对化 —— 没有前缀的 token
就会到错误的地方去找:`ld: cannot find lib/x86_64-windows-gnu/libmathkit.a`。
而 manifest 里写绝对路径就不再可重定位了。要补齐它,需要让条件通道能承载
`link_library_dirs` / `libraries` —— mcpp 已经能按方言渲染它们,只是只在顶层读。

### 实现分区

`mcpp pack` 把实现分区(`module M:part;` 无 `export`)当私有:**源码留下,
对象随归档发出去**。

如果被发布的接口 **import 了**一个实现分区,消费者没有那份源码就编不出 BMI,
所以它**会**被发布 —— 而 `mcpp pack` 会说出来:

```
warning: secret.cppm is an implementation partition, and the published interface
         reaches it — so its SOURCE is being published.
```

> 在 mcpp 2026.8.17.2 之前,扫描器把 `module M:part;` 记成**「requires `M:part`、
> provides 空」** —— 一个文件 requires 自己的名字,于是图里**没有**从「import 分区
> 的单元」到「定义分区的单元」的边,构建顺序无约束:GCC 与 macOS clang 靠各自的
> 依赖扫描兜住了,**Windows clang 以 `failed to read compiled module` 失败**。
> 如果你一直在 Windows 上回避实现分区,原因就是这个。

### mcpp 判不出类别的分区

`[scan_overrides."<glob>"]` 说的是文件提供哪些模块,**没有地方能说**那条声明是否
带 `export`;P1689 扫描器也可能省略 `is-interface`。两种情况下源码都会被发布 ——
消费者没有它就编不出 BMI —— 而 `mcpp pack` 会告诉你你处在哪一种:

```
warning: secret.cppm provides a module PARTITION and mcpp cannot tell which kind:
         the unit is declared in `[scan_overrides]`, which has nowhere to say
         whether the declaration carries `export`, …
```

> 在 2026.8.17.2 之前,这种情况以「它是接口」到达 —— 那个**不产生任何警告**的答案 ——
> 于是这样声明的实现分区被一声不响地发布了。**发布得太少**会让消费者编译失败并点名
> 模块;**发布得太多**会把私有源码发出去,而什么都不会失败。未知必须出声。

## 这些说法验证到哪一步、在哪台机器上

e2e 套件按宿主能力给每条测试开门,所以「套件是绿的」和「这条跑了」是两句不同的话。
实际跑在哪里:

| 说法 | linux | macOS | windows |
|---|---|---|---|
| 布局、两种接口模式、闭包、两道闸门、workspace 根、指名 target、`sources = []`、裸三元组谓词 | ✅ | ✅ | ✅ |
| 胖包,两条腿**同一个**产物名(`gnu` + `musl`) | ✅ | *不可能* | — |
| 胖包,两条腿**两个**产物名(`msvc` + `mingw`) | — | *不可能* | ✅ |
| 跨 OS 边界的胖包(一条 PE 腿) | ✅ | — | — |
| `lib.exe /REMOVE:` 真的删掉了 | — | — | ✅ |
| PE 共享库:产出、打包、链接、运行 | ✅(wine) | — | — |
| Mach-O 共享库离开构建树仍可加载 | — | ✅ | — |
| MSVC 以「导出」为理由拒绝 `kind = "shared"` | — | — | ✅ |
| 已发布的 mcpp 消费本版产出的包 | 仅本机 | 仅本机 | 仅本机 |

*不可能* 不是缺口:macOS 宿主只能服务一个 target(`host_can_serve`,
`registry.cppm`),那里根本产不出两条腿的包。

最后一行如实记录一个真的洞:每个 CI job 都从一份已发布的 mcpp 自举,但那个入口是
xvm 的 **shim**,在 e2e 套件改过的环境里它回答「未安装」。所以老客户端检查的
**静态那半**(生成的 manifest 不含任何旧 mcpp 读不了的段)到处都跑,而**真实那半** ——
用上一版发布的 mcpp 去构建这个包 —— 是**手工跑的,不是 CI 跑的**。
