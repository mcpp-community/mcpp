# 33 —— 编写运行时适配包

**读者：**要把**宿主**上已经装好的某个库，接到一个 mcpp 在 Linux 上构建出的产物
手里的人 —— 图形驱动、Vulkan ICD、某个专有运行时。

**本章回答的那一个问题：**产物为什么看不见一个明明装在机器上的库，以及一个包
要声明什么才能修好它。

**不在这里：**打包一个由 mcpp 安装的库，那是
[32 —— 编写一个载荷](32-authoring-a-payload.md)；只要那个库**可以**被再分发，
那一章才是正确答案。运行时契约本身的字段是
[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md) 的 `[runtime]` 一节。

在此之前：[32 —— 编写一个载荷](32-authoring-a-payload.md)。
在此之后：[34 —— 编写板级支持包](34-authoring-a-bsp.md)。

## 这一机制要解决的那次失败

库已经装好，加载器也找到了它的 manifest，`dlopen` 依然失败：

```
DRIVER: Found the following files: /usr/share/vulkan/icd.d/lvp_icd.json …
ERROR:  libvulkan_lvp.so: cannot open shared object file
```

库就摆在 `/usr/lib/x86_64-linux-gnu` 里。够不到它的是**进程**本身：mcpp 构建出的
二进制运行在 mcpp 自己的 glibc 之下，带着自己的一条搜索路径，

```
interp: …/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
rpath : …/xim-x-glibc/2.39/lib64:…/xim-x-gcc/…/lib64:$ORIGIN
```

于是这个进程内部发起的、按裸 soname 的 `dlopen`，根本不会去搜宿主的库路径。没有
任何东西坏掉；产物只是没往那里看 —— 而这一点，正是 mcpp 构建得以可复现的那条
性质本身。

## 适配包的定义

**一座符号链接农场，加上让它可被够到的那点元数据。** 不内嵌任何字节，不再分发
任何字节，包里没有一个上游字节。`runtime.library_dirs` 把包自己拥有的一个符号
链接目录放上产物的运行期搜索路径，链条就此解析得通。

工程把适配包当一个普通依赖声明即可，不必再做别的事。

## 驱动本身不能做成一个包的原因

专有驱动的用户态与**内核模块处于 ABI 锁步**，而它的许可证又禁止再分发。这两条
都不是靠打包上多花力气能解决的问题，所以这类驱动被建模为一种**宿主能力**
—— 机器要么有，要么没有 —— 适配包是产物够到它的方式。

**开源**驱动是另一种情形，答案也不同：它本身就是一个载荷（CPU 上是
`xim:mesa-lavapipe`，AMD 硬件上是 `xim:mesa`），用着它的机器完全不需要这座
农场。自 2026.09.05 起，当某个已发布载荷提供了同名库、且它的符号集合覆盖宿主
那一份时，适配包会优先取载荷。农场因此实际记录下来的，是专有用户态与尚未打包
完的部分。

## 适配包最容易出的三个错

**模式列表必须覆盖传递依赖。** 整条链都要经同一个目录解析开。Mesa 的软件光栅器
会拉进 LLVM，NVIDIA 驱动会拉进它自己那一族。只列出 ICD 这一个，结果是同一句
`cannot open shared object file` 在下一层重新出现。

**`libstdc++` 属于这份列表，这不是疏漏。** mcpp **静态**链接 libstdc++ —— 它不
出现在构建产物的 `NEEDED` 里 —— 因此一个被 `dlopen` 出来的 C++ 驱动，除非宿主
那一份在这里被提供，否则没有任何东西供它解析。

**不得把任何一项当作必需。** 一台完全没有这类驱动的机器是一种合法配置，这个
生态里每一台 CI runner 都是这样的机器。此时农场为空，程序如实报告它实际找到了
什么。一个在宿主库缺失时报错的适配包，会把一种受支持的配置变成一次构建失败。

## mcpp 在这个面上做的检查

适配包发布的库是经 `dlopen` 找到的，没有任何链接边指向它们，因此运行期闭包检查
—— 它沿产物的 `DT_NEEDED` 走 —— 按构造到不了这些库。mcpp 在链接之后单独走一遍
这个面，把结果作为警告报出：

```
warning: 1 of 13 libraries a dependency published for dlopen cannot be loaded
on this artifact's search path:
    libur_adapter_cuda.so.0 needs libnvidia-ml.so.1
```

三种读数被区分开来，只有一种会被报告：

| 该库所需的 SONAME | 含义 | 是否报告 |
|---|---|---|
| 在产物的搜索路径上 | 无话可说 | 否 |
| 存在于农场里，但链接悬空 | 这台机器没有这个驱动 | 否 |
| 到处都找不到 | 适配包没有携带它 | 是 |

中间那一行正是这项检查只做警告、不做错误的原因：悬空链接是「宿主驱动尚未安装」
这一状态的既定形状，一条在那里也失败的检查，会把受支持的配置变成构建失败。

完整结果 —— 连同两个分母 —— 发布在 `resolution.json` 的 `runtime.dlopen_surface`
里：

```json
{ "members": 13, "walked": 13,
  "findings": [ { "library": "libur_adapter_cuda.so.0", "dir": "...",
                  "soname": "libnvidia-ml.so.1", "kind": "missing" } ] }
```

即使没有任何发现，`members` 与 `walked` 也照常发布。一座构建失败的农场枚举不出
任何成员，否则「没有发现」与「什么都没检查」这两件事读起来会一模一样。

不适用这项检查的构建，发布同一条记录，只带 `reason`，不带读数：

```json
{ "members": 0, "walked": 0, "findings": [],
  "reason": "this build produces no program; the surface is reached from a process and belongs to whatever runs" }
```

四种理由分别是：运行时绑定不是 hermetic 的；设置了 `allow_host_libs`；本次构建
不产出程序；本次构建没有产出链接后的产物。目标不是 Linux 时不发布任何记录 ——
这条记录本身是 ELF 形状的，对一个本次构建根本不会产出的格式给出「空答案」，
只会制造另一种混淆。

读这条记录的测试应当把 `reason` 当作「没有测到」处理，而不是「测过、结果干净」
—— 「不适用」与「从未跑过」正是这条记录存在的意义所要分开的那两件事。

## 当前边界

- **按构造只适用于 Linux。** macOS 的 dyld 与 Windows 的 PE 加载器没有对应的这一
  层，因此面向它们的工程不声明适配包。
- 适配包无法让一台机器上原本不存在的驱动工作起来。它只移除一个障碍 —— 可达性
  —— 其余的按「缺席」如实报告。
- 农场的内容在适配包被安装的那一刻就确定了。之后才装上的驱动，要等到适配包被
  重新安装才会被纳入。
