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
| `kind = "lib"`(静态) | ✅ 所有 target —— **仅在 Linux 上验证过**(见下) |
| `kind = "shared"` on Linux/ELF | ✅ —— 包里同时带链接名与 SONAME |
| `kind = "shared"` on PE / Mach-O | ❌ 拒绝 —— 导入库与 install-name 尚未建模 |
| `kind = "shared"` on `*-musl` | ❌ musl target 是静态链接的 |
| 发布预编译 BMI | ❌ 未尝试;BMI 与编译器构建逐位绑定 |
| 把依赖打包进去 | ❌ 改为声明依赖(见上) |

### 验证到哪一步

`mcpp pack <库目标>` 的端到端验证**只在 Linux 上做过**。命令本身 ——
挑哪个目标、未知名字怎么拒、workspace 根上怎么打 —— 三平台都覆盖了。

缺口在 `pack` 内部那次库构建,而且不是理论上的:放开到 Windows
(MSVC-ABI clang)那条 CI 腿上会以一句 `error: build failed` 失败;
macOS 上闭包测试则因为 `ar` 解析到一个报「未安装」的 xlings shim 而无法检查归档。
两者都未解决。在这一行改口之前,**请把库打包当成 Linux 上的能力**。
