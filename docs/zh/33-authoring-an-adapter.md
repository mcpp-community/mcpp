# 33 —— 编写运行时适配包

**读者:**要让**宿主**提供的某个库能被 mcpp 产物在 Linux 上够到的人 —— 图形驱动、
Vulkan ICD、某个专有运行时。

**本章回答的那一个问题:**为什么产物看不见一个明明装在机器上的库,以及一个包要
声明什么才能修好它。

**不在这里:**打包一个由 mcpp 安装的库,那是
[32 —— 编写一个载荷](32-authoring-a-payload.md) —— 只要那个库**可以**被再分发,它
就是正确答案;以及运行时契约的字段,那是
[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md) 的 `[runtime]`。

在此之前:[32 —— 编写一个载荷](32-authoring-a-payload.md)。在此之后:
[34 —— 编写板级支持包](34-authoring-a-bsp.md)。

## 它为之存在的那次失败

库装好了。加载器也找到了它的 manifest。`dlopen` 仍然失败:

```
DRIVER: Found the following files: /usr/share/vulkan/icd.d/lvp_icd.json …
ERROR:  libvulkan_lvp.so: cannot open shared object file
```

库就在 `/usr/lib/x86_64-linux-gnu`。够不到它们的是**进程**:mcpp 构建出的二进制跑在
mcpp 自己的 glibc 之下,带着它自己的搜索路径,

```
interp: …/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
rpath : …/xim-x-glibc/2.39/lib64:…/xim-x-gcc/…/lib64:$ORIGIN
```

于是从那个进程内部发起的、按裸 soname 的 `dlopen`,**根本不会去搜索宿主的库路径**。
没有任何东西坏掉;产物只是不往那里看 —— 而这正是 mcpp 构建可复现的那条性质本身。

## 适配包的定义

**一个符号链接农场,加上让它可被够到的那点元数据。** 不内嵌任何东西、不再分发任何
东西,包里没有上游的字节。`runtime.library_dirs` 把一个包自有的符号链接目录放上产物
的运行期搜索路径,于是整条链解析得开。

工程把适配包当作一个普通依赖声明,除此之外什么都不做。

## 驱动本身不能成为一个包的原因

专有驱动的用户态与**内核模块处于 ABI 锁步**,而它的许可证禁止再分发。这两条都不是
靠努力能解决的打包问题,所以这类驱动被建模为**宿主能力** —— 机器要么有、要么没有
—— 而适配包是产物够到它的方式。

**开源**驱动是另一种情形,取另一个答案:它就是一个载荷(CPU 上是
`xim:mesa-lavapipe`,AMD 硬件上是 `xim:mesa`),而用着它的机器完全不需要这个农场。
自 2026.09.05 起,当某个已发布载荷提供了同名库、且它的符号集合覆盖宿主那份时,适配包
优先取载荷 —— 于是农场实际记录下来的,是专有用户态与打包欠账。

## 适配包最容易做错的三件事

**模式列表必须覆盖传递依赖。** 整条链都要经由同一个目录解析。Mesa 的软件光栅器会
拉进 LLVM;NVIDIA 驱动会拉进它自己那一族。只列 ICD 一个,结果是同一句
`cannot open shared object file` 往下挪一层。

**`libstdc++` 应当在列表里,这不是疏忽。** mcpp **静态**链接 libstdc++ —— 它不出现在
构建产物的 `NEEDED` 里 —— 所以一个被 `dlopen` 的 C++ 驱动,除非宿主那份在这里被提供,
否则没有任何东西可供它解析。

**不得有任何一项是必需的。** 一台完全没有这类驱动的机器是**合法配置**,而这个生态的
每一台 CI runner 都是这样的机器。此时农场为空,程序报告它实际找到了什么。一个在宿主库
缺失时报错的适配包,会把一种受支持的配置变成构建失败。

## mcpp 对这个面的检查与报告

适配包发布的库是被 `dlopen` 找到的,没有任何链接边指向它们,因此运行期闭包检查
—— 它从产物出发沿 `DT_NEEDED` 走 —— 按构造到不了这些库。mcpp 在链接之后单独走一遍
这个面,并把结果作为警告报出:

```
warning: 1 of 13 libraries a dependency published for dlopen cannot be loaded
on this artifact's search path:
    libur_adapter_cuda.so.0 needs libnvidia-ml.so.1
```

三种读数被区分开,只有一种被报告:

| 该库需要的 SONAME | 含义 | 是否报告 |
|---|---|---|
| 在产物的搜索路径上 | 无话可说 | 否 |
| 在农场里存在,但链接悬空 | 这台机器没有这个驱动 | 否 |
| 到处都不存在 | 适配包没有携带它 | 是 |

中间那一行正是这条检查是警告而不是错误的原因:悬空链接是「宿主驱动尚未安装」的
既定形状,一条在那里失败的检查会把受支持的配置变成构建失败。

完整结果(含两个分母)发布在 `resolution.json` 的 `runtime.dlopen_surface`:

```json
{ "members": 13, "walked": 13,
  "findings": [ { "library": "libur_adapter_cuda.so.0", "dir": "...",
                  "soname": "libnvidia-ml.so.1", "kind": "missing" } ] }
```

即使没有任何发现,`members` 与 `walked` 也会被发布。一个构建失败的农场枚举出零个成员,
否则「没有发现」与「什么都没检查」就读起来一模一样。

这条检查不适用的构建,发布同一条记录,但只带 `reason`、不带读数:

```json
{ "members": 0, "walked": 0, "findings": [],
  "reason": "this build produces no program; the surface is reached from a process and belongs to whatever runs" }
```

四种理由是:运行时绑定非 hermetic;设置了 `allow_host_libs`;本次构建不产出程序;本次构建
没有链接产物。目标不是 Linux 时不发布任何记录 —— 这条记录是 ELF 形状的,对一个本次构建
根本不产出的格式给出「空答案」本身就是另一种混淆。

读这条记录的测试应当把 `reason` 当作「没有测到」而不是「测了,结果干净」—— 「没适用」与
「没检查过」正是这条记录存在的意义所在。

## 当前边界

- **按构造只适用于 Linux。** macOS 的 dyld 与 Windows 的 PE 加载器没有对应的这一层,
  因此面向它们的工程不声明适配包。
- 适配包无法让一台机器上不存在的驱动工作起来。它只移除一个障碍 —— 可达性 —— 其余的
  按「缺席」如实报告。
- 农场的内容在适配包被安装时决定。之后才装上的驱动,要到适配包被重新安装时才会被
  纳入。
