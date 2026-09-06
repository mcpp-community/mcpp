# Changelog

> 本文件追踪 `mcpp-community/mcpp` 公开仓的版本演进。
> 格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [Unreleased]

### `[feature-xlings]` 声明的工具,构建程序找不到

`[feature-xlings.<f>]` 从诞生起就参与供给:在那里写下一个包,`<f>` 生效时它就会被下载
并安装。但构建程序的环境是由 `[xlings.workspace]` **单独**填充的,于是载荷已经躺在
store 里,`mcpp::xpkg_dir` 仍然返回 `""`。

这个缺陷的形状是「答案已解析,却没有接到决定上」:供给端读的是「声明 + 生效的 feature」,
查询端读的只是「声明」。它在 llama.cpp 的 Vulkan 后端上现形 —— 规则程序拿不到
`xim:shaderc`,而它唯一说得出口的话是「请声明 xim:shaderc」,指向一条作者早已写下的声明。
一条指错文件的诊断比没有诊断更坏。

修法是让查询端读同一个集合:`fillXpkgDirs` 现在把调用方**已经算好的** feature 闭包
里的 `[feature-xlings]` 条目并进来。「是否安装」仍然是唯一的过滤器,所以
`when = "dev"` 的条目对消费者依然回答 `""`。

判据是 e2e 614,它有对照:同一个工程构建两次,`--features gpu` 下拿到载荷路径,不带
feature 时拿到空串。只断言前者的用例,在一个「凡装过的包都作答」的 mcpp 上同样会通过。
用的包是 `xim:ninja` —— mcpp 自己 bootstrap 进沙箱的那个,所以判据不需要网络;版本从
store 里**读出来**而不是写死,否则 mcpp 升 ninja 会把这个 feature 的测试变红。

### `[feature-deps]` 的共享库没上链接行

同一族的第二个缺陷,现形于同一次构建。`[feature-deps]` 在解析期就并进了 root 的
dependencies,所以那个依赖**被解析、被下载、被编译** —— 每一行日志都说它在。但 plan
读 root 的边时读的是 `packages[0]`,而它是在那次并入**之前**拍下的快照,于是依赖的共享库
既没进链接行也没进 implicit inputs。

它安静是因为形态:库工程的 `mcpp build` 产出静态档案,而档案不解析符号,所以构建成功。
失败出现在链接可执行文件的人那里 —— 报的是那个依赖自己的入口点未定义。llama.cpp 的
Vulkan 后端就是这样撞上的:`libllama.a` 建好了,`vulkan_decode` 链接时 `vkGetInstanceProcAddr`
未定义。

修法沿用本文件里已有的写法:root 的真相在 `*m` 而不在 `packages[0]`(`checkVersionFloors`
早已为同一个理由这样分支)。

判据 e2e 615 的**形态**就是判据本身。同一个工程写成二进制时,有缺陷的引擎和修好的引擎
都通过 —— 实测过 —— 所以围绕 `mcpp run` 搭的夹具是一个不可能失败的测试。让缺陷现形的是
「库 root + 测试二进制去链接」,而这正是本生态里每一个库包的形态。

### 四个设备示例合并为 `examples/09-heterogeneous`

`09-cuda-kernel` / `10-vulkan-compute` / `11-sycl-kernel` / `12-hip-kernel` 是同一课的
四种编程模型:同一个 kernel、同一道接缝、同一条收窄的 glob、同一个答案,差别只在规则包
驱动哪个编译器。四个连号在课程表里说的是「四课」,而再加一个模型就会说成五课。它们现在
是 `examples/09-heterogeneous/{cuda,vulkan,sycl,hip}`,共享的那一课写在目录的 README 里。

同一次改动把落后于生态的钉子对齐了(它们改的是同几份 manifest):`cuda` 与 `vulkan`
从 `mcpp:plugins 0.1.1` 跟到 `0.2.0`(`sycl` 与 `hip` 早已是 0.2.0),`cuda` 与 `hip`
从 `compat:cuda-runtime` 改写为 `compat:cuda-driver` —— 那条目已冻结并改名,旧名仍可解析,
所以这是拼写修正而不是修复。

文档里的版本号**没有**跟着动。`*(2026.9.5.2+)*` 这类标记陈述的是某项能力何时落地,是
历史事实;把它跟到当前版本会让文档对自己的主题说谎。只有「当前应当写下什么」才跟随发布。

## [2026.9.6.1] - 2026-09-06

### `.sycl` 进设备扩展名表:判据是编译器,不是方言

`SourceKind::Device` 自陈是一个**图上的角色** —— 「不扫描、不产 BMI、由 mcpp 不驱动的
设备编译器编译」—— 而在此之前表里每一个扩展名都是普通 C++ 编译器会拒绝的方言。`.sycl`
不是:它的内容就是普通 C++。使它成为设备单元的是它交给谁 —— 一个带设备后端、且不接受
C++20 modules 的第二个编译器(icpx,或带 SYCL 前端的 clang)。

被否掉的替代是「让收窄的 glob 去承载 `.cpp`」。那会让同一个扩展名因为哪条 glob 先匹配
而指向两个不同的编译器,而设备构建赖以可读的接缝(设备代码只经 `extern "C"` 头进入程序)
正是因为文件名说明了一个单元在接缝的哪一侧。

这次加行**对既有构建是惰性的**,两个条件同时成立:设备扩展名不在 `default_source_globs`
里,所以没有任何 glob 变宽;而把这类扩展名写进 `sources` 在加行之前是硬错误,所以没有
任何文件悄悄换了角色。e2e 613 的第五段对**整张默认 glob 列表**做断言而不是对两个名字,
第三段则用同一份内容的 `.cpp` 拼法做对照 —— 只测 `.sycl` 的用例在一张「按内容分类」的
表上同样会通过。

配套的规则包在 `mcpp:plugins` 0.2.0:`mcpp.rules.sycl` 驱动 `xim:dpcpp` 载荷,
`mcpp.rules.hip` 在 NVIDIA 平台上把 HIP 当作 CUDA 运行时之上的一层头文件,
`mcpp.rules.spirv` 增加 `glslc` 路线(`xim:shaderc` 使它从一句声明变成一条路线)。
文档见 `docs/20-heterogeneous-builds.md` 的「lanes」一节。

### `mcpp clean --stale`:只清 target/ 里已无构建使用的指纹目录 (#565)

每次配置指纹变化都会在 `target/<三元组>/` 下新开一个目录,旧目录从不回收;`mcpp clean` 只有整删
一档,代价是全量重编,于是没人跑。`mcpp clean --stale` 以 `target/.build_cache` 记录的
(三元组, 指纹) 为当前,删掉记录过的三元组目录下其余兄弟目录并报告各自体积;未被记录但在
`--older-than`(默认 1d)之内写过的目录保留(`mcpp test` 的构建不写记录)。`--dry-run` 只列出。
没有构建记录时拒绝执行而不是猜;记录之外的目录(如 `mcpp pack` 的 `dist/`)不碰。
`fingerprint changed` 的警告末尾现在附带这条命令,让增长可见。

`--stale`、`--dry-run`、`--older-than` 三者任一都选中这一档:`mcpp clean --older-than 3d`
说的是一次有范围的清理,不该落进整删。负的时长被拒绝,`0` 表示不保留任何未记录目录。

### `mcpp search` 显示包的可用版本,`mcpp add` 的建议同样携带 (#487)

search 的命中行追加该包描述符 per-OS 版本表的并集:semver 降序、按键去重,默认显示最新 3 个
并以 `, ...` 标记截断,`--all-versions` 显示全部。描述符不可读或未发布任何版本的包保持原两列
输出——富化是尽力而为的展示,不是新的失败路径。

```
$ mcpp search imgui
  compat:imgui      Dear ImGui immediate-mode GUI library core sources       (1.92.8, 1.92.8-docking)
  mcpplibs:imgui    C++23 module package for Dear ImGui core and GLFW/OpenGL3 backends  (0.0.6, 0.0.5, 0.0.4)
```

`mcpp add` 未命中时的跨命名空间建议从裸 FQN 升级为带版本:`compat.eui-neo (0.5.6, 0.5.5, 0.5.3)`。
数据是白捡的——did-you-mean 扫描本就要打开每个候选 `.lua` 读身份(#278),版本只是同一段文本
的再一次遍历;排序复用 SemVer 解析(`version_req`),不可解析的键保留原文排在最后(#363 的
教训:任意的索引键无法从解析形态复原)。build 失败路径的同款提示同步升级,两条路径不说两套话。

排序与扫描各有单测钉住;e2e 162 断言 build 与 add 两侧的建议都带版本。#324 的遗留半边。

## [2026.9.5.4] — 2026-09-06

### 构建程序声明的文件输入,快路径此前不比较

`rerun_if_changed("data/table.csv")` 声明的是「这个文件的内容变了就重跑」。工程级快路径
在没有任何**源文件**比 `build.ninja` 新时跳过 `prepare_build`,而构建程序缓存正是在
`prepare_build` 里被读取的;快路径自己只问过 glob 输入的**路径集合**(#359),没问过声明
文件的内容。数据文件既不在 `src/` 下也没有 C++ 扩展名,mtime 扫描看不见它,于是改了数据、
`mcpp build` 打印 `Finished dev in 0.00s`、程序里编进去的还是上一次的字节。

快路径现在按缓存里记录的方式比较三类构建程序输入:glob 的路径集合、声明文件的内容哈希、
声明环境变量的值。e2e 612 用程序的**输出**做判据(陈旧的头文件与新的头文件让二进制打印
不同的字符串),并跑对照的另一半:输入没变时第二次构建仍然走快路径,否则「总是重建」也能
让第一条断言通过。发现它的是 `mcpp.tools.embed`——mcpp-plugins 0.1.1 里第一个非规则成员,
它把数据文件写成头文件,没有 action 可提交,所以完全落在这条路径上。

### 注释与文档里不再有装饰性符号

2026.9.5.3 的清理覆盖了 `docs/`、`README.md`、CHANGELOG、引擎源码、测试、示例与工作流
文件,没有覆盖 `bench/`、`tools/`、`scripts/`、`mcpp.toml` 与 `README.zh-CN.md`。程序
输出保留原样:用户在终端上读到的一行既不是文档也不是注释。`README.zh-CN.md` 同时补上英文
版已有的 Cortex-M 行,状态列改用与英文版相同的词。

## [2026.9.5.3] — 2026-09-05

### 官方构建插件集中为一个包:`mcpp:plugins`

规则包不再放在本仓库的 `examples/` 下。它们现在集中维护于
`mcpp-community/mcpp-plugins`,以一个包 `mcpp:plugins` 发布,消费者用 feature 选择成员,
在 `build.mcpp` 里以成员自己声明的模块名 import:

```toml
[dependencies.mcpp]
plugins = { version = "0.1.0", features = ["rules-spirv"], host-module = true }
```

命名收敛为两族:规则包 `mcpp.rules.<x>`,构建期工具 `mcpp.tools.<x>`;lib 根
`mcpp.plugins` 记录集合的版本。此前的 `mcpp.build.<x>` 撤回:那是引擎自己的模块族
(`mcpp.build.plan`、`mcpp.build.prepare`),插件不该与它同名。规则包规范曾以「模块名就是
裸包名,不能含点」为由撤回 `mcpp.rules.*`;I1(模块名由源码声明)落地后这个理由不再成立,
规范的 I8 与第 7 节按此修订。

**引擎为此扩了一处**:一个 `host-module = true` 的包,其解析后 `[build] sources` 里
—— 含 feature 加入的源文件 —— 的每一个模块接口单元都编成一个 host 模块,以各自声明的名字
注册,lib 根排在最前,feature 单元可以 import 它。只有清单里列出的源文件参与:未声明
`sources` 的包所推断出的 `src/**` 不被读取,所以此前发布的规则包暴露的仍是它当时暴露的那
一个模块。模块集合就是 feature 集合:未激活 feature 的单元不编译,import 它以未知模块失败。
e2e 610 分别断言这五条(含 `mcpp` 命名空间零告警、其他命名空间**每个单元一条**告警的对照);
`tests/unit/test_provisions` 覆盖接口单元的判定:实现单元、分区、全局模块片段与注释里引用的
声明都不算。

示例 09 与 10 像任何工程一样从索引消费 `mcpp:plugins`;`mcpplibs:rules-cuda@0.1.0`
在索引里保留并标注被取代。

### 设备源的判据是编译器,不是厂商

`SourceKind::Device` 的定义写着它陈述的是构建图里的角色 ——「由一个 mcpp 不驱动的
设备编译器编译」—— 并且明确说这样定义是为了让分类表**不按厂商长出一行**。而那张
扩展名表里恰好只有两行,都是 NVIDIA 的。

第二个设备 API 让这件事显形:一个写在带约束 glob 里的 shader 被拒绝为

    'scale.comp' is listed in [build] sources, and mcpp has no role for the
    extension '.comp'.

同一次运行里,规则包又被告知没有设备源并为此告警 —— 一个错误和一个警告在说同一个
文件的相反的话。

表因此扩到「由另一个编译器消费的语言」:CUDA 与 HIP,GLSL 的各个 stage 与无 stage 的
`.glsl`,HLSL,OpenCL C,Metal。共 18 个扩展名,完整清单见 `docs/20-heterogeneous-builds.md`。

这次扩表不能改变任何今天可用的构建,理由有两条且互相独立:设备扩展名**本来就不在**
默认 source glob 里,所以没有 glob 变宽;而这些扩展名今天在 `sources` 里是**硬错误**,
所以没有文件静默换了角色。两条都由 `tests/unit/test_source_kind` 断言,其中默认 glob
那条断言的是**整张列表**而不是曾经在场的两个名字。

### `mcpp.rules.spirv` 与 examples/10-vulkan-compute

新的规则把「GLSL 如何变成 SPIR-V」陈述一次,与 `mcpp.rules.cuda` 同形:引擎拥有图 ——
加速器轴、把 shader 路由到规则包而非 C++ 编译器的带约束 glob、动作边、指纹 ——
并且不认识 "vulkan" 或 "glslang" 这两个词;规则包拥有拼法。

它产出的是**头文件而不是目标文件**。SPIR-V 是程序交给 `vkCreateShaderModule` 的数据,
所以动作以 `role = "source"` 提交 —— 这是引擎**排在编译之前**的那一个角色,正是生成的
头文件需要的;`artifact` 角色的产物排在链接之前,那已经晚于包含它的那个编译单元。

示例 10 用同一份产物在三处得到同一个结果:宿主 ICD 上的 RTX 4080、只给 `VK_DRIVER_FILES`
的软件驱动载荷(无 GPU 参与)、以及 `mcpp build --no-accel` 下接缝后的 CPU 实现。

### 修正

- **变体切换不再被快路径回放。** 设备变体在指纹里,两次构建落在不同目录;而快路径在任何
  计划存在之前运行,回放的是**最后一次**构建的目录。实测:`mcpp build`、
  `mcpp build --no-accel`、`mcpp build` —— 第三次报 `Finished in 0.00s`,随后的 `mcpp run`
  执行的是 CPU 变体。build.ninja 的头行现在记录选择来自清单还是来自 `--accel`/`--no-accel`
  (`accel=default|override`),两条快路径只回放前者;缺该字段的旧图按未命中处理并被重写。
  e2e 611 与 `tests/unit/test_graph_shape` 断言。
- `MCPP_DEVICE_SOURCES` 以换行分隔。规则包若按 `;` 切分,对**恰好一个**设备源仍然正确,
  对两个则拼出一条不存在的路径;e2e 609 第一段以整张扩展名表作分母,当场把它抓了出来。
- glslang 若未链入 spirv-opt(`-Os not available; optimizer not linked`),规则包降级
  并告警一次,而不是让构建失败:可选的优化 pass 缺席不是错误,静默地不做优化才是。

### 文档

- 第 20 章由「加速器」更名为「异构硬件构建」(`docs/20-heterogeneous-builds.md`),副题指明
  GPU 与 AI 加速器目标以及宿主/设备混合编译;`accel` 键不变。
- 第 5 章与第 7 章(中英)补入 feature 选择的规则集合与 `mcpp.rules.*` / `mcpp.tools.*`
  命名;第 7 章中文版此前缺少命名一节,本次补齐。
- 文档、README、CHANGELOG 与代码注释里的装饰符号(告警、星标、勾叉等)全部移除;表格里
  只以符号承载的取值改写为词(`yes` / `no` / `partial` / `planned`)。

## [2026.9.5.2] — 2026-09-05

### 在编译任何东西之前比较机器的下界

有些机器事实**限定**了能为它构建什么,而忽略它们时,失败到得很晚。本轮的样本:
设备运行时不得新于它将运行其上的驱动;当它更新时,构建与链接都干净通过,程序在
第一次分配处失败,消息里既不提工具包也不提驱动。

两个数字在编译任何东西之前都是可知的。mcpp 不去问厂商的工具要它们 ——
`tests/unit/test_runtime_contract` 禁止 `src/` 里出现厂商探针,而且这条规则是对的:
一个学会跑一家厂商探针的引擎会学会跑四家。所以数字以**声明**抵达:

```toml
[[runtime.requirements]]
kind  = "version-floor"
value = "cuda.driver >= 12.0"
```

`mcpp.build.version_floor` 只做比较,**这个文件里不出现任何厂商名字**:
`cuda.driver` 是流经的数据。第二种后端不需要改动它。

### 探针通道:`mcpp::fact` / `mcpp::floor`(协议 v7)

构建程序陈述它**测得**的事实与它**需要**的下界,引擎比较并在不满足时给出两侧取值
(`version-floor-unmet`)。这是让 CUDA 探针得以整体离开 `src/` 的那条通道 ——
同样的读数现在由规则包产出,而它知道自己在跑哪个工具。

因此 **`mcpp self doctor` 的设备一节与 `mcpp.toolchain.devicehost` 一并删除**。
它读得对(载荷优先于宿主、`crt/host_config.h` 的宿主编译器上界、`nvcc --dryrun`
的不可达阶段),但它不属于引擎。新增 `tests/unit/test_core_vendor_probes`
在剥掉注释的源码上陈述这条性质,并自带分母:枚举到的文件太少即判失败。

### 逐 glob 的加速器约束

`[build] sources` 的条目可以带上它面向的加速器:

```toml
sources = [
  "src/*.cppm",
  { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" },
]
```

构建按它收窄;`--no-accel` 整条排除;不覆盖它的 `--accel` 被拒并点名两侧
(`accel-mismatch`);匹配为空的约束被拒并点名该 glob —— 空匹配是笔误或搬走了的
目录,而不是空操作。

### `--accel` / `--no-accel` 现在也挂在 `run` 与 `test` 上

此前只有 `build` 有,实测后果是一个工程的 CPU-only 变体**能构建却不能运行**。
两个动词接受同样的两个开关,给出任一个都绕开各自的快路径 —— 缓存的产物是按上一次
构建的轴产的。

### 第二个编译器需要的两个答案

`MCPP_TOOLCHAIN_SYSROOT` 与 `MCPP_TOOLCHAIN_BINUTILS_DIR` 陈述 mcpp 传给**它自己**
那个编译器的 `--sysroot` 与 `-B`。规则包驱动一个 mcpp 并未解析的编译器时,那个
编译器对环境一无所知:sub-OS 里 C 库不在 `/usr/include`,汇编器不在 `/usr/bin`,
于是它遇到的第一个 `#include` 就失败。`hipcc`、`-fsycl-host-compiler`、任何会编译
自己产物的生成器都有同一个缺口。

`gcc::binutils_prefix_dir` 把 `-B` 的守卫收敛到一处,此前有三份副本,其中一份的
注释写着它是另一份的镜像。

### Linux 上的动态构建程序 helper 改用 `DT_RPATH`

RUNPATH 只对 helper **自己**的 needed 生效,于是一个在运行期打开宿主库的构建程序
在下一跳失败:实测 `dlopen("<sentinel>/lib/libcuda.so.1")` 报
`libdl.so.2: cannot open shared object file`,而持有它的目录就在 helper 的 RUNPATH 里。
mcpp 链接的产物早就因为这个原因带 DT_RPATH。链接策略进 helper 的缓存身份,
旧 helper 会被重建而不是被重放。

### `--offline` 跳过首次使用的沙箱引导

`--offline` 承诺不碰网络,而 `load_or_init` 在空 home 里克隆索引、经 xlings 装
ninja 与 patchelf。实测:空 home 下 26 秒 / 126 MB → 0.3 秒。

### `examples/09-cuda-kernel` 走两条路线,并拒绝它不能配的对

主路线是 clang(`-x cuda`):工程自己的编译器编设备单元,没有第二个宿主编译器、
没有宿主编译器上界、没有 CUDA 的宿主头挡路。nvcc 是备用路线,并按名拒绝两对:
宿主编译器超出 `crt/host_config.h` 所述上界(实测 gcc 16 + nvcc 12.9 即便加了
`-allow-unsupported-compiler` 也死在 gcc 自己的 `<type_traits>` 里),以及工具包
旧于 C 库(12.9 的 `crt/math_functions.h` 为宿主重声明 C23 的
`cospi`/`sinpi`/`rsqrt` 不带 `noexcept`,而 glibc 2.41+ 带)。

同一个缝下还有一份 CPU 实现,由 `cfg(not(accelerator = "cuda"))` 选中,于是
`mcpp build --no-accel` 编它、`mcpp build` 编 `.cu`,两侧都不需要手写条件。

实测(RTX 4080,驱动 550.144.03 报 CUDA 12.4,LLVM 22.1.8):`mcpp run` 与
`mcpp run --no-accel` 都打印 `12 24 36 48`,来自不同的产物目录,后者不含
`cudaMalloc`。

## [2026.9.5.1] — 2026-09-05

### 加速器支持:设备编译单元、产物身份的加速器维、以及没人做的宿主编译器配对

一个为某个计算能力编译的库,被另一个计算能力的构建消费时,链接干净地完成,
程序在第一次 kernel 启动时失败,消息里既没有包名也没有任何一侧期望的架构。
C++ 构建生态里没有任何一个系统把「这个二进制是为哪个架构编的」记进它的身份 ——
这是整个品类的空白,不是 mcpp 特有的。

**`SourceKind::Device`。** `.cu` 与 `.hip` 是设备编译单元:从不被扫描 import,
从不产出 BMI —— 没有任何设备编译器接受 C++20 modules。`.cuh` / `.hiph` 是头文件,
改动其一仍使快路径失效。设备扩展名**刻意不进默认 source glob**,理由与内置模块
扩展名表停在 `.cppm` 的理由相同:放宽它会让一个 vendored 了设备源码、在别处构建
它的已发布包在下次升级后突然开始编译它,而这是作者无法修复的破坏。

**产物身份的加速器维。** 携带设备代码的产物把它记在兼容性标签旁边,`tag_check`
比较它。成员判定按两条硬件里真实存在的机制放宽:家族目标覆盖同 major、minor 不低
于它的范围;内嵌的可移植形式覆盖下界之上的一切。AMD 两者都没有,靠 archs 一侧的
generic target 取得同样的覆盖,所以空的下界不放宽任何东西。

字段与标签并列而不是标签的一段,因为架构列表是集合,而标签是用 `-` 拼接、
其 triple 本身含数量不定 `-` 的字符串。一个比较器,两个存储位置。

**`accelerator` 作为多值 cfg layer。** 一次构建可以同时启用多个后端。比较是处处
成员判定,而不是只在 `any(...)` 里 —— 让组合子改变操作数含义会使
`all(accelerator = "cuda", accelerator = "rocm")` 变成不可满足,而不是「两者都启用」。

**宿主编译器上界,读而不抄。** nvcc 拒绝比它在自己的 `crt/host_config.h` 里声明的
上界更新的宿主编译器,而 mcpp 的载荷常常更新。因为宿主编译器由 mcpp 提供,
它可以在任何编译发生之前作答。上界从工具包读出,所以一个 mcpp 从未见过的工具包
同样能作答;解析不了的头文件不产生上界,也就不产生断言。

**`[build] accel` / `--accel` / `--no-accel`,以及 `[package] accelerators`。**
前三者之间的关系与 `[toolchain]` 和 `--target` 相同。`--no-accel` 是显式请求
「不要加速器」,这是在一个同时发布了设备构建的包中选中 CPU-only 变体的方式。
`[package] accelerators` 与 `platforms` 同形,并刻意与产物的 `accel` 是不同字段:
声明由人手写,产物字段从构建测量。

真机验证(RTX 4080 / CUDA 12.0 / 驱动 550.144.03):`examples/09-cuda-kernel`
经规则包编出设备岛并运行,`mcpp run` 输出 `12 24 36 48`。

设计与调研:`.agents/docs/2026-09-05-accelerator-support-design.md`、
`.agents/docs/2026-09-04-ai-accelerator-toolchain-ecosystem-survey.md`。
新增手册章节 `docs/20-accelerators.md`(中英双份)。

## [2026.9.4.3] — 2026-09-04

### `mcpp run` 报告程序自己的退出码

在此之前所有非零退出码都被折成 `1`,为的是让 `2` 表示「起不来」以区别于「跑了但
失败」。区别值得保留,代价不值得:`main` 返回 `3` 的程序让 `mcpp run` 退 `1`,
qemu 报 `3` 的裸机镜像同样到达为 `1`。一条报不出退出码的命令没法写进脚本,而这
正是 `mcpp run` 的主要用途。

取值空间分三段,只有第一段属于程序:

| 区间 | 含义 |
|---|---|
| `0`–`124` | 程序自己的退出码,原样透传 |
| `125`–`127` | 尝试启动但被拒绝(`127` 找不到 / `126` 不可执行 / `125` 其他) |
| `2` | mcpp 在尝试启动之前就拒绝了(用法、配置、解析) |

中间那段是 `env`、`timeout`、`nice` 早已在用且被 shell 文档化的取值,所以 `126`
与 `127` 带着惯常含义到达。程序自己也可以退 `125`–`127`,mcpp 不靠数字区分 ——
启动失败一定向 stderr 写出原因,程序自己的退出码从不写。

`mcpp test` 不变,仍为 `0`/`1`:它聚合多个程序,没有单一退出码可透传。

**兼容性**:mcpp 自身的配置错误仍是 `2`,与其余所有命令一致 —— 变的只有「尝试
启动后被拒」这一种情况,而那时程序根本没运行。契约写在 `docs/11` §6。

### 没有构建进程能比启动它的 mcpp 活得更久

实测:每一次被 `timeout` 终止的 `mcpp run` 都留下一个空转占满一个核的 ninja,
其中一个的工作目录已经是 `(deleted)`、比它所属的整个沙箱活得还久。任何用
`timeout` 包住 mcpp 的 CI,每超时一次泄漏一个忙核。

子进程现在进入**自己的进程组**(Windows 上是 job object),mcpp 在收到
SIGINT/SIGTERM/SIGHUP 时对该组发 **SIGKILL**。用 SIGKILL 而不是 SIGTERM 是必要
的:ninja 把信号记进标志位,只在等待子进程处才检查;一个没有命令在跑的 ninja
永远到不了那个检查点,礼貌的信号被记录且永不执行 —— 那正是那些孤儿所处的状态。

守卫从单槽改为**多槽登记表**:一个跨构建的 `[hooks]` 命令与构建自己的 ninja 会
同时被守卫,单槽会让后注册者解除前者的守卫。

### `MCPP_NINJA_DEBUG`

设置后向 ninja 追加 `-d <topics>`(如 `explain`)。用于那个还没定因的 ninja 空转:
`ptrace_scope=1` 与 `perf_event_paranoid=4` 都取不到栈,而把 gdb 变成 ninja 的父
进程之后空转就不再发生 —— `-d explain` 是唯一能对这种进程取证的手段。

## [2026.9.4.2] — 2026-09-04

### Cortex-M 有 C 库了:`libdir` 填上,而它的键是**三元组**

`xim:picolibc-arm@1.8.12` 带七个多库(picolibc + compiler-rt builtins,同一个
LLVM 一起构建)。工程一行选入:

```toml
[target.thumbv7m-none-eabi]
sysroot = "xim:picolibc-arm@1.8.12"
```

**`libdir` 这一列在 ARM 上是三元组,不是 `<march>/<mabi>`,而差别是一次
无人报告的 ABI 替换。** riscv 的 `mabi` **就是**浮点 ABI(`lp64d` 与 `lp64` 是
两个值);ARM 的 `mabi` 是过程调用标准,两个变体都是 `aapcs`,浮点 ABI 在三元组的
`eabi`/`eabihf` 后缀里。于是 `armv7e-m/aapcs` 会给两份互不兼容的库命名同一个目录。

实测(构建 `xim:picolibc-arm` 时):按 `<march>/<mabi>`,七个档位塌成五个目录,
`armv7e-m/aapcs/libc.a` 带着 `Tag_ABI_HardFP_use` —— 硬浮点的构建,坐在软浮点
程序会去找它的位置上。**构建期什么都没报。**

填这一列**不**给这些行默认配 C 库:只有解析出 sysroot 之后才会读它,目标表仍然
不绑定任何一个。零 libc 仍是默认,这只是让选入这件事能成立。

判据是浮点 ABI 而不是「链接通过」:`tests/e2e/338` 构建**软浮点**那一行,断言产物
里没有硬浮点 ABI 标记,然后启动它并读退出码。并且断言产物**不为空** —— 实测:
缺了 crt0 的链接会成功并报 `Size fw  text 0 data 0`,一个格式良好、什么都没有的
ELF,任何只看 `Finished` 的检查都会放行。

runner 有了名字,工具有了档位,`--locked` 成为断言,`mcpp emit sbom`。

### 一条命令,加具名的例外

`mcpp run` 覆盖常见情形的**全部,真实硬件也一样**。在设备上「运行一个程序」意味着
写进去、复位、接上输出、读回退出状态 —— 这是**一条**命令(`probe-rs run`、
`qemu-system-* -kernel`),不是几条。板级包把它作为**默认** runner,于是开发者从
模拟器换到真板时,**敲的命令不变**。

```bash
mcpp run                     # 默认;模拟器与真板同一条
mcpp run --runner flash      # 具名的例外:只写不跑、看串口、起调试服务端、擦片
mcpp run --list-runners      # 这个工程提供了哪些
```

**引擎不认识任何 runner 名字。** `flash`、`serve`、`deploy`、`submit`、
`logcat` 对它一样陌生。**引擎里若有一份固定的名字表,就等于由引擎决定哪些领域可被
表达** —— 一个 web 包将无法自己加 `serve`。

**写程序名,不要写路径。** mcpp 先找本包 `[xlings] deps` 声明的载荷 `bin/`,
再找 `PATH`。用 `xpkg_dir` 拼绝对路径是多余的,而且引入了一个失败模式:声明不是
安装,查询返回空则没有配置任何 runner 而无话可说。

是否终止由 `mcpp::runner_longlived(name)` **声明**:
`openocd -c "program … exit"` 会终止而 `openocd -c "init"` 不会,拼写到最后一个
参数为止都一样,没有任何 argv 能表达这个区别。

`mcpp::run_exclusive()` 陈述「这个目标的运行不能重叠」—— 对一块板、一张 GPU、
一个串口、一个单席位 license 同样成立,`mcpp test` 据此串行化。

### `--locked`

锁一直是解析之后写、从不读回。现在它是断言:发生的解析必须等于记录的解析,不等则
**点名移动了的包与两个版本**。

**它绝不能遇上快路径。** 实测:加闸之前,一份被故意改坏的锁通过了
`mcpp build --locked` 并打印 `Finished` —— 旗标被接受、构建正确、**断言从未跑到**。

### `mcpp emit sbom`

CycloneDX 1.5,覆盖**已记录**的解析。读锁而不是重新解析 —— 一份描述了与所构建
者不同的图的文档比没有更糟。归在 `emit` 之下而不是新开一级命令:`emit` 已经是
「生成描述本工程的文档」。

### 工具有了档位,而且依赖声明的工具现在真的会被装

包依赖从一开始就有 `[dependencies]` / `[build-dependencies]` / `[dev-dependencies]`
这条轴,工具只有一张表。一个同时点名模拟器与调试探针的板级包,会把两个都装给每一位
消费者,包括只想把库编出来的那一位。

```toml
[xlings.workspace]
"xim:qemu-arm"  = "9.2.4-1"                             # 不写就是从前的行为
"xim:probe-rs"  = { version = "0.24.0", when = "run"   }

[feature-xlings.hardware]
"xim:probe-rs"  = "0.24.0"                              # 不要这个 feature 就永不下载
```

| `when` | 由谁安装 | 传播到消费者 |
|---|---|---|
| *(不写)* | 每个构建命令 | 是 |
| `build` | 每个构建命令 | 是 |
| `run` | `mcpp run`、`mcpp test` | 是 |
| `dev` | 只有声明它的那个包作为根时 | **否** |

**不写 `when` 保持今天的行为,所以没有迁移。**

**同时:`[xlings.workspace]` 的供给扩到全图。** 在此之前只有根工程的声明会被
安装,而查找(runner 按裸名找程序)已经跨全图 —— **在没有任何东西安装过的目录里
查找,是只可能失败的查找**。两者现在由同一个表达式定义。

档位带来的一个危险已被堵上:`mcpp build` 装得比 `mcpp run` 需要的少,而 run 的
快路径正是为跳过那一步存在的。构建缓存记下「这次构建留下了未安装的 run 档工具」,
`mcpp run` 的快路径据此拒绝该条目 —— 与它拒绝声明了 runner 的条目同理。

判据不测「装成了没有」,而测**mcpp 要装什么**:`MCPP_NO_AUTO_INSTALL=1` 下拒绝供给
并**点名它本来要装的集合**,于是 `build` 与 `run` 两条命令的差集就是被测的性质,
一次下载都不需要(`tests/e2e/335`)。

### ARMv7-A:第一个带内存管理单元的 32 位目标行

`armv7a-none-eabi` 与 `armv7a-none-eabihf` 两行,`verified`。表里其余每个 32 位行都是
M-profile:MPU 按基址与上限描述区域,没有页表项。A-profile 有真正的 MMU,于是它是第一个
能被问「**32 位**机器的页表项长什么样」的目标 —— 这正是 openarch 的地址空间抽象从未被
一台 32 位机器问过的问题。

实测 2026-09-04(`xim:qemu-arm@9.2.4-1`):两行都在 `-M virt -cpu cortex-a15` 上启动、
经半主机打印并报回退出状态。

**软浮点行同样需要 `-mfpu=none`,而这是在这个架构上重新实测的**,不是从 M-profile
推过来的:`armv7a-none-eabi` 在软浮点 ABI 下对一次 float 乘法仍发出 VFP 指令。

**半主机的退出调用与 M-profile 拼法不同。** AArch32 的 `SYS_EXIT`(`0x18`)把原因码
**直接**放在 `r1`;Cortex-M 传的 `{reason, code}` 块是 `SYS_EXIT_EXTENDED`(`0x20`)。
实测:把块传给 `0x18` 打印正确而**退出状态是错的**,只看输出的测试看不出来。

那条量化 `-mfpu=none` 的单元测试,谓词曾是 `starts_with("thumb")` —— 一个**拼法**
而不是它要陈述的性质。新行加进来时规则适用而测试**静默跳过**了它们,每条断言依然通过。
谓词已改为「32 位 ARM」。

### `mcpp run` 接受 `--features` 与 `--profile`

`build` 与 `test` 一直有这两条轴,`run` 没有 —— 于是 `run` **只能执行上一次 `build`
恰好留下的东西**:没有任何一种写法能跑一个 release 产物,或一个开了 feature 的产物。

而这正是整个设备面赖以成立的形状:板级包把「模拟器」与「真板」表达成 feature,
所以 `mcpp run --features hardware` 才是板子到手那天开发者敲的命令。**方案里唯一一个
自己跑不起来的场景就是它。**

### 顺带修掉一个既有缺陷:构建缓存不看 feature

缓存条目按 (target, profile, cache mode) 索引,而**输出目录按含 feature 的指纹索引**。
于是 `mcpp build --features loud` 写下的条目指向 loud 的目录,下一次**不带 feature 的**
`mcpp build` 命中它、0.00s 报成功,**把带 feature 的产物交给一个没要 feature 的请求**。

实测(修复前):同一个工程连续三次构建打印 `quiet`、`LOUD`、`LOUD`。

条目现在记录它的 feature 集合(归一化,`a,b` 与 `b a` 同一);两条快路径都比对它。

### 发现性

`mcpp why runners` 列出本工程提供的 runner,与其余解析结果并列;
`mcpp run --list-runners` 是同一份读取,单独报告。

新增 `docs/18-devices.md`、`docs/19-supported-versions.md`(中英双份)。
指令协议版本 6。

## [2026.9.4.1] — 2026-09-04

Cortex-M 落地为七个目标行,freestanding 链接开启死代码段消除。

裸机目标表从四行增至十一行。M-profile 是七行而不是一行:为 `thumbv7em` 构建的
目标文件使用 Cortex-M0 没有的指令,两种拼写产出互不兼容的目标文件,而表存在的
理由正是让 `--target <triple>` 单独足以产出正确的目标文件。

```toml
[build]
target = "thumbv7em-none-eabihf"
```

**浮点 ABI 不决定 FPU 是否被使用。** `eabi`/`eabihf` 由 clang 从 triple 读出,
它约束浮点值如何跨越函数边界,不约束函数内部发什么指令 —— 而 `thumbv7em` 架构
蕴含 FPv4-SP。实测:软浮点 ABI 下 clang 对一次 float 乘法仍发出 `vmul.f32`,在
没有 FPU 的 Cortex-M4 上于运行期触发异常,而编译与链接都是干净的。每个软浮点行
因此携带 `-mfpu=none`,包括架构本来就没有 FPU 的那几行 —— 一行陈述它保证的性质,
而不是从一个可以改变的默认值继承它。

freestanding 编译加 `-ffunction-sections -fdata-sections`、链接加 `--gc-sections`。
依赖的目标文件无条件进入链接(不像归档成员那样按未定义符号拉取),当 C 库改由
依赖图提供时,没有死代码段消除的镜像会装进整份 C 库,而 Cortex-M 器件只有几十 KB。

**链接脚本因此以新的方式承重**:中断向量表不被任何东西引用,`--gc-sections`
会回收它,板级脚本必须写 `KEEP(*(.vectors))`。

同时回填了 `docs/13` 中两条已被 2026.8.28.2 推翻的限制:当图中有包提供
`hosted-standard-library` 时,裸机目标上的异常、RTTI 与 `import std` 均可用。

## [2026.9.3.2] — 2026-09-03

`[xlings.workspace]` 的**推荐书写形态**定为命名空间在键上,官方包全部使用它;
mcpp 打印的建议行随之改成同一形态。

```toml
[xlings.workspace]
"xim:picolibc-riscv" = "1.8.12"
```

> **建议行是会被照抄的。** `[xlings] deps` 的替代提示此前给出的是
> `picolibc-riscv = "xim:1.8.12"` —— 命名空间在版本上。那种形态仍然接受,但它是
> **物化出来的文件**所用的词汇(那里的键是 xvm target,scope 限定版本),不是作者
> 书写的词汇(先点名一个包,再说用它的哪个版本)。一个教人写非推荐形态的提示,会
> 把约定推向它自己的反面。

### 变更

- `[xlings] deps` 的替代提示改为推荐形态,键带引号(TOML 裸键不能含冒号)。
- `docs/05` §2.13 明确写出推荐形态与它的理由,并说明另一种形态为何仍然接受;
  `docs/13` 与 `examples/07-project-subos` 的示例改成推荐形态。中英双份。

行为不变:两种形态解析结果完全相同,物化出来的 `.xlings.json` 也不变。

## [2026.9.3.1] — 2026-09-03

`[xlings]` 收敛成一张表。`[xlings.workspace]` 说出工程用哪个包、用哪个版本,mcpp
既供给它也把它物化成解析用的钉;`deps` 被取代;`envs` 移除;发布出去的描述符第一次
带上安装期的边。

设计与全部实测见
[`.agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md`](.agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md)。

> **一般形态比它自己的简写弱。** `docs/05` §2.13 写着 `[toolchain]` 是编译器那一项
> 的便捷写法、`[xlings.workspace]` 是一般形态;而 `[toolchain]` 会装
> (`resolve_xpkg_path(…, autoInstall=…)`),一般形态什么都不装。两个键说的是同一件
> 事,差别只在 mcpp 拿这句话去做什么 —— 而它们对应的那套机制,书写面只有一个。

### 变更

- **`[xlings.workspace]` 成为唯一的表。** 一条条目产出两个投影:一个安装地址
  (`[<ns>:]<target>[@<version>]`,供给用)与一份解析钉(`[<ns>:]<version>`,写进
  `.xlings.json` 的 `workspace` 对象)。命名空间写在键上或版本上都接受——
  `"xim:picolibc-riscv" = "1.8.12"` 与 `picolibc-riscv = "xim:1.8.12"` 是同一条,
  写在键上必须带引号(TOML 裸键不能含冒号);两半都写且不一致是错误。`""` 表示
  「存在即可,版本不限」,这是手写项目文件里已经在用的拼法。平台键取 xlings 自己的
  `linux` / `macosx` / `windows` / `default`,`macos` 作为别名保留。

- **`deps` 被取代,但仍然生效并被报告。** 报告里给出该写的那一行。**不拒绝**——拒绝
  会落到依赖的 manifest 上,而钉了那个包精确版本的工程改不了它。同一个包在两张表里
  给出两个版本则是硬错误:两者按顺序供给而后者赢得钉,接受它等于装一个、解析另一个。

- **`[xlings.envs]` 移除。** 它曾被物化进 `.xlings.json` 而没有任何东西读它:
  xlings 里两处 `envs` 分别属于某个程序的 shim 记录和某个 SubOS 的 provider 段,
  都不是这个形状;mcpp 自己给程序的运行环境来自 runtime binding。现在这个键是错误
  ——**一个什么都不做的键,在有东西声称它有效果时更坏**。

- **发布的描述符带上 `xpm.<platform>.deps`。** 此前 `mcpp emit xpkg` 一个字都不写,
  安装期的边只能手写进 `xpkg.lua` —— `riscv-virt-rt` 0.3.0 因此发出去时没有它自己
  目标行指名的 C 库。声明按**未解析**的形态保留在 `workspaceByPlatform` 里,因为
  描述符每个平台一块,而按本机解析已经丢掉了另外两个。

- **`[xlings]` 的三档继承写进文档。** 不写 `subos` 就是机器的环境加上工程自己的条目;
  写了 `subos` 就是隔离,机器那层不参与;工程内执行的 `xlings use` 压过两者。这条
  行为一直如此,而 mcpp 此前一字未提。

### 行为变化

- `[xlings.envs]` 从被忽略变成硬错误。索引里没有任何包用过它。
- `[xlings] deps` 仍然生效,但会打印一条指出替代写法的警告;`--strict` 下它是错误。
- 同一个包在 `deps` 与 `[xlings.workspace]` 里给出不同版本,现在被拒绝。

## [2026.9.2.1] — 2026-09-02

`[target.<triple>].runner` 对每一个目标生效,启动失败不再无声,`mcpp test` 把跑不起来
的测试报成 not run 并退 2,`[xlings]` 的值可以按宿主平台给出。

设计与实测见
[`.agents/docs/2026-09-02-runner-beyond-baremetal-design.md`](.agents/docs/2026-09-02-runner-beyond-baremetal-design.md)。

> **一个键被解析、被类型检查、被文档记录,而读它的那处代码在读之前就返回了。**
> `choose_runner` 在 `os == "none"` 之外一律返回空模板,于是宿主交叉目标
> (x86_64 上构建的 `aarch64-linux-musl`)的 runner 从未被查询过:`mcpp run` 裸执行
> 产物,内核以 `ENOEXEC` 拒绝,而 `run_exec` 把这次拒绝变成一个不打印任何东西的
> 127。同一层的有界启动器在自己的声明里写着「『起不来』与『跑了但失败』不能共用
> 一个退出码」,而它的两个调用方都靠**再 spawn 一次**来回落,把第一次的 errno 丢掉。

### 修复

- **`[target.<triple>].runner` 对每个目标生效。** freestanding 谓词只决定一件事:
  没有 runner 时是否在任何 spawn 之前就失败。宿主能不能执行一个外来 ISA 的产物不再
  被预测 —— 这台机器上 `binfmt_misc` 注册了 qemu-user 就能跑,报告 #544 的那台不能,
  而两者的三元组相同。mcpp 要么执行工程声明的 runner,要么尝试启动并报告内核的回答。

- **runner 的程序由 mcpp 定位,不由 `posix_spawnp` 定位。** 先在 `[xlings] deps`
  声明的每个载荷的 `bin/` 里找,再走 `PATH`。裸名在 `PATH` 上解析到的是 xvm 垫片,
  而垫片按**当前 SubOS** 回答而不是按包所在的位置回答(e2e 130 在 CI 里记录过这一条:
  同一个 job 里 `qemu-system-riscv64 --version` 成功,而 `mcpp run` 执行同一个裸名
  得到「未安装」)。「哪儿都找不到」在任何 spawn 之前判定,并且是错误而不是回落到
  裸执行:让产物在另一个解释器下带着另一组参数运行,正是这个键存在要防止的失败。

- **启动失败被定型并且只被报告一次。** `DeadlineRun` 与 `BoundedOutcome` 带上
  spawn 错误码;`run_exec`、`capture_exec` 与两个 deadline 包装各多一个末位
  `int* spawn_error`。调用方要了错误码就由调用方报告,没要就由启动器自己报告。
  没有第二次 spawn。

- **`mcpp test` 有了第四种状态。** 产物这台宿主装载不了的测试既没有通过也没有失败。
  它被报成 **not run**,原因在确立时打印一次,在汇总行里以与失败同等的分量再出现一次,
  退出码是 2 —— freestanding 的 no-runner 路径对同一种处境早就用这个码。1 保持
  「跑了并且失败」,0 保持「每个测试都跑了并且通过」。`--message-format json` 带上
  `"status":"not_run"` 与每条记录的 `reason`,汇总记录带上 `not_run` /
  `not_run_reason`;`--workspace` 另加 `tests_not_run` 与 `unrunnable_members`。

- **`--no-runner`,`mcpp run` 与 `mcpp test` 都接受。** 「这台宿主能直接执行该产物」
  是关于宿主的事实,而 manifest 没有宿主轴:`[target.<triple>]` 按目标索引,
  `[xlings] deps` 没有任何索引。为 x86_64 开发者写的 runner 在 aarch64 宿主上同样会
  被读到,而那里模拟器既无用也装不上。旗标由那台宿主上的操作者给出,因为只有那里
  知道这件事。

- **`[xlings]` 的值可以按宿主平台给出。** `deps` 的一项与 `[xlings.workspace]` 的一个
  值都可以写成 `{ linux = "...", macos = "...", windows = "...", default = "..." }` ——
  xlings 自己的 `.xlings.json` 对 `workspace` 接受的就是这个形式,`macosx` 作为它的
  拼法一并接受。在 manifest 加载时对本机解析,因此下游每一个读者看到的仍然是一张平表。
  未知的平台键是硬错误。在此之前 `[xlings.workspace]` 会**静默丢掉**一个表值,而
  `[xlings] deps` 根本没有条件化形式 —— 这让 `deps = ["qemu-user-aarch64"]`
  (索引里只为 x86_64 构建的包)在其余每一类宿主上都是硬构建错误。

- **`[target.<triple>]` 的未知键普查覆盖数组。** `runnerX = ["x"]` 会被报出来,
  而支持键的清单里补上了 `runner` —— 此前普查刻意跳过数组,代价是这张表读取的唯一
  一个数组键既不在清单里,拼错了也无人报告。

- **诊断名出的是规范拼法。** 六处消息此前打印驱动报出的三元组;其中三处打印的是
  一段供粘贴的 `[target.<triple>]`,而在 macOS 上那是 `arm64-apple-darwin24.6.0`,
  没有任何 `[target.…]` 查询会命中它。`RunnerChoice::tripleKey` 与查询本身在同一处
  求出,两者不可能各说各的。

### 行为变化

- 宿主三元组下已经声明的 `runner` 从此生效。在其程序缺失的宿主上,`mcpp run` /
  `mcpp test` 现在带消息失败,而不是静默裸跑产物;`--no-runner` 是出口。
- 此前对跑不起来的产物报 `FAIL (exit 127)` 的 `mcpp test` 现在报 `NOT RUN` 并退 2
  (仍然非零,CI 作业不会因此改变颜色)。
- `[xlings] deps` 与 `[xlings.workspace]` 接受表值;带未知平台键的表此前被丢弃,
  现在是错误。

## [2026.9.1.1] — 2026-09-01

#540 的七条审计,加上核验它们时挖出的四条没有人报过的。它们几乎全是同一族:
**mcpp 关于自己说了一句话,而 mcpp 不遵守它。**

完整核验、量化与设计见
[`.agents/docs/2026-08-31-issue540-seven-audit-findings.md`](.agents/docs/2026-08-31-issue540-seven-audit-findings.md)。

> **一条规则写在一处,却由它的一份手抄件来执行。** `kKnownBuildKeys`、
> `kKnownConditionalBuildKeys` 与 xpkg 的 `target_cfg` 列表,都是别处已有机器可读
> 形式(紧挨其上的 `doc->get_*()` 读取点、`BuildInputs` 的成员表)的转录,三份都
> 漂移了 —— 而 `kKnownBuildKeys` 漂移了**两次**,第二次就发生在描述第一次的注释上方
> 八行。代价不是少了一条警告,而是**一条假的警告**:`[build] has unsupported key
> 'std-module' (ignored)`,说的与实际发生的事情正相反。

### 修复

- **`[xlings] deps` 的供给从不检查自己是否成功。**(它的第一个真实受害者是本仓库自己的 e2e 88 —— 见 CHANGELOG 末尾) `xlings::call` 返回
  `expected<CallResult, string>`,只要子进程跑起来就处于**值**态 —— 能力自身的状态
  在 `CallResult` 里面,因为 xlings 讲完 NDJSON 协议后按设计退 0。调用点只测了
  `if (!r)`,于是 xlings 能报出的每一种失败都被读成了成功:

  ```
  $ mcpp build                      # deps = ["definitely-not-a-real-package"]
  Provisioning [xlings] deps (definitely-not-a-real-package)
      Finished dev [unoptimized + debuginfo] in 0.12s
  ```

  记号随后把这次假成功变成**永久**的 —— 下一次构建连 `Provisioning` 都不再打印。
  #531 自己的注释写着它修的缺陷是「声明看起来被接受了却什么都没做,这是一个配置键
  能有的最坏形态」;没人读结果,它的修法重现了那个形态。正确写法就在同一个文件里
  ——依赖安装路径写的是 `if (r && r->exitCode != 0 && …)`。

- **该路径不认 `MCPP_OFFLINE`,也不认 `MCPP_NO_AUTO_INSTALL`。** 它自称与
  `[toolchain]` 平权,而那条先例在任一开关下**硬错**并且报出触发的是哪一个。现在
  两个都认,拦的是安装**动作**而不是整块 —— 已供给好的工程仍然离线构建得出来。

- **记号记录的是全局效果,却存在项目里。** 安装落在 registry(刻意如此,原注释说明
  了理由),而 `<project>/.mcpp/.xlings-deps.stamp` 记着它。清掉或换掉 `MCPP_HOME`,
  项目仍然声称已装;`mcpp clean` 只删 `target/`,也清不掉。改按依赖列表的哈希存进
  registry,并且**只在成功时写**。

- **`[build] std-module` / `std-compat-module` / `std-module-flags` 被读取,却被报成
  unsupported。** 三个键在 #494 被移入 `[build]` 正是为了让它们可条件化,而
  `kKnownBuildKeys` 从未收录 —— 唯一一句关于它们的话说反了。

- **条件轴拒绝 `BuildInputs` 的两个成员。** `std-module-flags`(#494 就是为这条轴
  才把它挪上来的)与 `private_include_dirs` —— 后者更严重:xpkg 描述符的
  `target_cfg` 块,也就是**同一条轴的另一套语法**,是接受它的。两条列表的消息现在都
  由列表本身生成。

- **`[features]` 是唯一一个完全没有 schema 检查的结构化段落。** 把 `include_dirs`
  误写进 feature 里会零诊断地构建成功,而同样的错误写在 `[build]` 里会被报出来。

- **`mcpp build --help` / `mcpp test --help` 说默认档位是 release,而它是 dev。**
  六处说得对(解析器、它的注释、docs/05、一条 e2e、mcpp 自己的 mcpp.toml、
  `mcpp pack --help`),两处说错。`prepare.cppm` 里那条字段注释是没被报告的第三处。

- **`mcpp index update <name>` 承诺按索引筛选,而它只筛项目级索引。** 限制此前只写在
  一条注释里 —— 一个只有实现者看得到的地方,从外面看与「这功能坏了」无从区分。

### 新增

- **`[target.'cfg(<层> = "…")'.build]` —— 按已解析目标侧条件化(#494 / #540)。**
  docs/14 用一整节记载了这个能力,包括为什么它不能用 feature 选择代替;而
  `cfgpred::Ctx` 只由三元组构造,所以每一个这样的段落都被**静默**丢弃,包成功构建
  在错误的 C 库配置上。五个层名 `compiler` / `compiler-runtime` / `kernel-abi` /
  `c-abi` / `c++-abi` 现在是谓词的键,可与三元组键在 `all`/`any`/`not` 下组合。

  层谓词**不能**选择依赖 —— 层是从依赖图解析出来的 —— 这种段落会被报出并忽略,
  而不是被静默丢弃。

- **mcpp 不认识的 cfg 键会被报出来。** 求值器过去对未知键返回假,而那与「这一段本就
  不该匹配」读数完全相同。词汇表从求值器**导出**而不是被转录 —— 否则这条诊断自己就
  会成为本次发布正在修的那第五份手抄件。

- **[`docs/spec/exit-codes.md`](docs/spec/exit-codes.md)(SPEC-003)。** 2026-08-08
  的协议设计文档 §R4 把这份契约指派给了 `docs/spec/`,它一直没有写。`docs/11` 那张表
  落地的是 usage/internal 的一半;命令**跑了并且失败**时返回的 `1` 既不在表里也不在
  别处 —— 而按信封命令划定的那张表**给不出** `4`。

### 变更

- **`c-abi` 层报的是库名,不再是三元组的 env 段。** 二者在 `musl` 上重合,在 `gnu`
  上分叉:Linux 上它请求的是 glibc,Windows 上它命名的是工具链的 MinGW 形态,而后者
  的 C 运行时是 UCRT。docs/14 一直把实现列作 `glibc`/`musl`/`picolibc`,e2e 296 的
  文件头也把它期望的报告写作 `c-abi glibc (payload)`。在这个值只被打印的年代这只是
  措辞不一致;它现在是用户书写的谓词值。请求那一侧保留三元组的拼写(规范 §3.4),
  两者经 `c_abi_request_satisfied` 比较而非按相等。

- **`mcpp::target_libc()` 的文档改为它实际回答的问题** —— 供给 sysroot 的那个**载荷**
  包,而这个值是目标侧解析的一项**输入**。要按已解析的层分支,用层谓词。

## [2026.8.30.2] — 2026-08-30

六处缺陷,来自 #527 / #529 的分析,外加一处在实现 review 时挖出来、没有人报过的。
它们分属两族:**记录存在而做决定的代码不读它**,以及**运行期搜索路径把「声明的」
和「这台机器碰巧装了的」混在一起**。

完整分析、量化与设计见
[`.agents/docs/2026-08-30-issues-527-529-535-537-analysis-and-design.md`](.agents/docs/2026-08-30-issues-527-529-535-537-analysis-and-design.md)。

> **host 依赖的规则按轴分,而这个分叉是刻意的。** **工具链属于 mcpp 的契约**:
> `import std` 可用、闭包可计算、同一份构建在别的机器和 CI 上一致,都是关于
> 「一个 mcpp 解析得出、叫得出名字的编译器」的陈述,所以 `[toolchain] = "system"`
> 被**明确拒绝**(`msvc` 是唯一例外)。**而程序链接哪些库是程序自己的事**:
> 工程可以链 host 的库或自己的 `.so`,mcpp 说明代价并指出 mcpp-index 那条路,但不拒绝。

### 修复

- **`mcpp test` 每次调用都在重算一份没有变化的答案(#529)。** 两个 post-link ELF
  pass 都写了读回优化(stat 没变 ⇒ 复用上次判定),而 `prepare_build` 每次调用都用
  一个**全新的 json 对象**重写 `resolution.json` —— 那里面没有这两条记录。于是每次
  调用一开始,就把自己后端待会儿要找的备忘录删掉了。

  记录改存进 `.mcpp-runtime-verdicts.json`(它本来就活得过 `prepare_build`),
  `resolution.json` 继续发布一份副本 —— 这正是 `sync_resolution_verdict` 已有的
  形态。同时:
  - 剪枝的判据从「不在本次 plan 里」改为「产物已不在磁盘上」。`mcpp build` 与
    `mcpp test` 共用一个输出目录而 link unit 集合不同,前一个判据让两条命令互删
    对方的记录;
  - 记录的失效键纳入 SubOS farm 的 `.xlings.json` 时间戳与 `MCPP_ALLOW_HOST_LIBS`
    —— 让备忘录持久化,就产生了一条以前不存在的正确性义务。

  实测(10 个 link unit,每个 11.5MB,全热):

  | | 之前 | 之后 |
  |---|---|---|
  | `mcpp build -p <member>` | 0.70s | 0.32s |
  | `mcpp test -p <member>`(紧接 build) | 3.15s | **0.36s** |
  | `mcpp test -p <member>`(连续) | 1.95s | 0.40s |

- **`path` 依赖里新增一个源文件,fast path 看不见。** 陈旧性扫描只覆盖被构建的那个
  工程,于是 `mcpp build` 报 `Finished dev in 0.00s`,而那个模块从没编译过。内容改动
  之所以还能被抓到,靠的不是扫描,是 ninja 重链后的事后放弃。这是 #359 那条形态
  ("glob 输入变了而现存文件的 mtime 一个没动")在它当年没有覆盖到的目录里。
  workspace 成员之间就是 `path` 依赖,所以这不是边角情况。

- **`[toolchain] system` 现在被明确拒绝,而不再崩溃(#527 Bug 1)。**
  它此前配合 `build.mcpp` 会死在 `posix_spawnp('') failed (error 2)` —— 一条以崩溃形式
  出现的"拒绝"不是政策,是穿着政策外衣的 bug。

  **mcpp 只用它自己管理的工具链构建。** `PATH` 上的编译器无法被识别、无法被复现,于是
  `import std` 可用性、运行期闭包、"同一份构建在另一台机器上"全都不再是 mcpp 能承诺的
  东西。拒绝消息给出该写什么、去哪看可选项,并点明 `msvc` 是**唯一例外**
  (它点名的是一个族,mcpp 定位其安装),同时说明**host 库是另一条轴,不在拒绝之列**。

- **`standard = 26`(不带引号)被静默忽略。** 键被文档写成字符串,而 `get_string` 对
  裸整数返回空,于是工程按默认档位编译、零诊断。#527 自己的三处示例就是这么写的。
  两种拼法现在都接受。

### 新增

- **`[workspace.package]` 与 `[workspace.build]`(#527 Bug 2 / RFC 3)。** workspace 根
  的 `[build]` 此前完全没有传给成员;现在标量按「成员**声明过**就成员优先」继承,
  向量按 workspace 在前追加。

  「声明过」是**解析时记录的事实**,不是与默认值比较得出的推断 —— 成员在
  `standard = 26` 的 workspace 下刻意写 `standard = "c++23"` 必须保住,而那与默认值
  同为一串字节。这正是 cpp20 设计文档 §9-Q3 记下的前置条件。

  `allow_host_libs` 明确不可继承:它关掉的是某个具体产物的检查,workspace 根设一次
  就等于替所有后来加入的成员也关掉了。`[workspace.package]` / `[workspace.build]` 里
  不认识的键会被**拒绝**而不是忽略。

  没有 `[workspace.target.<triple>]`:根里普通的 `[target.<triple>]` 本来就按 triple
  被成员继承,为同一能力再加一种拼法只增加接口面。

  **继承作用到每一个成员,包括「作为兄弟成员的 `path` 依赖被编译」的那个**
  —— 也就是成员互相依赖这种最普通的形态。而 `path` 依赖里**不是成员**的那些
  (vendored 副本、example)不会获得这些标志:成员资格问的是 workspace 自己的
  `members` 列表,不是「这个路径在不在 workspace 目录下」。

  `[workspace.build] include_dirs` 里的相对路径按 **workspace 根**锚定(#224):
  它是在根 manifest 里写的,按各成员自己的目录解析会指向不存在的地方。

- **依赖声明了高于当前图的标准时会说出来。** C++ 模块图只有一个标准,依赖自己的
  `standard` 不生效 —— 这是对的;缺的是它一直不说。degraded 级别(`--strict` 提升),
  且**只对工程作者自己拥有的 manifest 生效**:索引里带 mcpp 段的描述符 782 个全都声明了
  `language`,其中 756/774 是 `import_std = false` 的 C 库带的样板值,信任「声明过」会
  让 c++20 的根工程对着整个索引报警。

- **方言标志没进 `import std` 预编译时,在编译前拒绝。** `[build] cxxflags` 里的
  `-fno-exceptions` / `-fno-rtti` 会到达每个 TU 却到不了 std BMI 预编译,于是每个
  importer 都在 mcpp 生成的文件里失败,而报错只讲机制不讲那个键。现在提前拒绝并指出
  `dialect_cxxflags`。读的是**生效后**的标志集合(`[build]` / `[profile.*]` /
  `[target.…]` 都算),并且在图中没有 `import std` 时不触发。

  这两个标志仍然**不自动提升**:依赖可以合法地不同意,消费者无权替它决定。

## [2026.8.29.1] — 2026-08-29

构建规则以普通包分发的机制自 2026.8.5.1 就能用,而**规范**一直没有:一个规则包
可以叫什么、可以依赖什么、它的依赖会流到哪里,三个问题都没有答案。索引里 133 个
描述符中只有 1 个规则包,所以这套抽象的每一条性质都只有一个样本撑着。

完整设计见
[`.agents/docs/2026-08-29-build-rule-package-spec.md`](.agents/docs/2026-08-29-build-rule-package-spec.md)。

> **身份由引擎执行,形状写进文档。** 分档的判据是「给不给得出判据」——
> 上一版规范里唯一一条纯约定(`mcpp.rules.*` 前缀),是唯一一条被无声推翻的。

### 新增

- **`[build-dependencies]` 现在真的有作用。** 这个段一直能被解析、能跨 workspace
  成员合并、能按 target 谓词条件化,而**没有任何做决定的代码读它** ——
  `types.cppm` 里那个字段自己标着 `// host-side tools (M5+ behavior)`。写下它
  得到的是一份能加载的清单、零诊断、零效果。

  段与边上的请求回答两个正交的问题:**段**回答「这个包本身进不进目标」,
  **边上的请求**(`tools` / `host-module`)回答「要它的哪一种构建期产物」。
  于是 protobuf 只写一次 —— 工程链 `libprotobuf`、构建期要 `protoc`:

  ```toml
  [dependencies]
  protobuf = { version = "35.1", tools = ["protoc"] }
  ```

- **规则可以依赖另一个规则。** 在自己的 `[build-dependencies]` 里声明,
  用 `host-module = true` 请求。内层规则先编译,同一条命令、同一套 flag,
  所以 BMI 的一致性仍是结构性事实。消费者**不能** import 它,除非那条边写了
  `reexport = true` —— 这条由 mcpp 执行而不是交给编译器,因为 GCC 会放行然后
  在别人的机器上失败。

  规则包此前是叶子,而那不是设计选择:两个既有的构建期通道(`tools` /
  `host-module`)都由**消费者**写在边上,规则没有替自己说话的地方。

- **`modules/`。** mcpp 自己的源码树按**子系统**分出九个独立包,各带 mcpp.toml、
  由 path 引用、依赖显式声明:`libs`(json+toml)、`log`、`versioning`、
  `source-kind`、`dyndep`、`platform`、`manifest`、`toolchain-model`、
  **`buildmcpp`**(build.mcpp 契约:协议、指令表、provision 模型、tool store)。

  单位是**子系统**不是文件——一文件一包只会把目录列表写成九份清单。
  `src/` 留构建工具的骨架:prepare / plan / execute、工具链族与探测、pm、pack、
  cli、xlings、runtime。

  按目录分组算强连通分量,21 个组里 17 个缠在一起,而每个组级环都由**可数的
  几条反向边**造成。切了三条,每一条都是「模块按位置命名而不是按职责命名」的同一
  个错误:

  * `src/platform/xlings/` 根本不是 platform,它是 xlings 集成 → `src/xlings/`;
  * `runtime_binding.cppm` 为读**一个**路径而 import 了整个 `mcpp.config`,
    把 OS 层压到了配置、xlings 与包管理器之上 → 改成由调用方传路径,文件移到
    `src/runtime/`;
  * `toolchain.fingerprint` import 的是 `toolchain.detect`,而它要的
    `Toolchain` 定义在 `toolchain.model` —— **那条 import 指向的是类型的消费者
    而不是提供者**,顺带把工具链探测拖了进来。这条是承重的:build.mcpp 侧每个
    模块都要 `fingerprint` 的 `hash_file`,只要它指着 `detect`,整个契约就传递地
    坐在包管理器之上,一行 import 就是「能不能成为独立模块」的全部距离。

  `build_program` / `hostprogram`(编译并运行构建程序的那一半)留在 `src/`:
  它们需要 `toolchain.registry` 与 `toolchain.stdmod`,而**编译并运行一个程序的
  东西按构造就是工具链消费者**,搬走只会把依赖挪个地方。

  **跨包边界的构建延迟:实测是免费的。** mcpp 的构建是关键路径受限的,加核无效,
  任何新的串行化都按 100% 墙钟付账。同机、空 `target/` 的冷构建:

  | | 墙钟 |
  |---|---|
  | 拆分前(单包 158 个 TU) | **51.07s** |
  | 拆分后(根 + 九个 `modules/` 包) | **49.52s** |

  两者在噪声范围内,不是「根的编译要等每个依赖的归档」那种边界会带来的串行化。

- **分层单元测试。** 每个子系统带自己的测试(`mcpp test -p <member>`,CI 逐个跑),
  根目录保留跨层的那套。两者的红有不同含义:子系统红=契约变了,根目录红=契约没变
  而消费者漂了。子系统测试还跑在根构建从不产生的配置里——**只有它自己和它声明的
  依赖**,所以一个悄悄依赖了未声明之物的子系统在根构建里能编过、在这里编不过。

  `mcpp test -p <member>` 对没有测试的成员**退 0**,于是「没有测试」与
  「测试通过」在 CI 输出里完全一样。`check_modules_wiring.sh` 因此把没有测试的成员
  逐个打印出来(不判失败——一个 vendored 解析器包确实没有自己的东西要陈述)。

- **`role = "check"` 的 stamp 由 mcpp 写。** 检查的判定是**退出码**,stamp 是构建图
  需要的记账——而此前要求**命令自己**创建它。分析器不这么工作:clang-tidy 成功时
  什么都不写。于是每个 check 都需要一个包装脚本,而 action 的 command 是 argv、
  不假设有 shell(这对 Windows 是对的),那个包装器**根本没法可移植地写出来**。
  ⇒ 生态里零消费者的那个 role,也没有获得消费者的途径,这不是巧合。

  引擎自己就是可移植的包装器(`mcpp __action-stamp <stamp>… -- <argv>…`,内部命令、
  不进用法):它在 mcpp 能跑的每个平台上都已经在磁盘上。已经自己写 stamp 的命令
  逐字节不受影响——已存在的文件不会被动。

  **显而易见的判据不区分**:实测 ninja **不会**因为声明的输出没生成而失败,
  它只是留着文件不存在、之后**每次构建都重跑**那条边。构建保持绿色,唯一的症状是
  活被反复重做。`tests/e2e/312` 因此断言 stamp 存在 + 无变更时不重跑,两条都先看它红。

### 修复

- **构建规则自己的 `[dependencies]` 会被链进消费者的二进制。** 排除谓词是按边的
  (「每条入边都是 host-module 边」),它对规则本身是对的、对规则**背后**的一切
  是错的。改成从目标的依赖根**正向可达**:两种情形一条规则,不需要特例,而且
  「同一个包既被工程直接依赖、又被某个规则的构建期依赖到达」这种情形自动正确 ——
  构建期路径不从工程要链接的东西里减去任何东西。

- **规则的模块名不再取自包名。** 取自规则源码声明的那个。此前 host 模块这条路径
  注册的是裸 `package.name`,而 `build_host_module` 按编译器族绑定这个名字的方式
  并不一致:MSVC 走 `/reference <name>=<ifc>`、Clang 走 `-fmodule-file=<name>=<pcm>`,
  GCC 则什么都不命名(BMI 隐式落在 `gcm.cache`,按**声明名**索引)。于是声明名与
  包名分叉的规则包在 GCC 上能构建、在另外两者上失败。包名因此不再承担任何 C++
  命名约束。

- **两个规则声明同一个模块名不再走到链接器那里。** BMI 与对象文件名都取自注册名,
  后者覆盖前者,存活的那个对象被送进链接两次,报出来的是
  `multiple definition of 'initializer for module X'`,一个文件名说两遍、
  一个包名说两遍,从不提有两个包参与。现在拒绝,并点名两个包与各自的 interface 路径。

- **`mcpp.` 模块名前缀**保留给由 mcpp 项目维护的规则,违反给警告(不是硬错误 ——
  引擎判定不了谁是官方,`path` 依赖与私有镜像都合法)。

## [2026.8.28.2] — 2026-08-28

依赖是静态并进来还是作为共享库放在旁边,以前由**包作者**定死,消费者没有任何
发言权;而两个包同时提供同一个库时,没有任何机制说这是错的。完整分析见
[`.agents/docs/2026-08-28-issue519-dependency-linkage-form.md`](.agents/docs/2026-08-28-issue519-dependency-linkage-form.md)。

> **一个库,一个提供者,一种形态。** 这条不变量在两个高度上执行:
> plan 期对 mcpp **决定**的东西,链接后对链接器**产出**的东西 ——
> 后者是唯一能看见 mcpp 从不知道其存在的那个库的高度。

### 新增

- **`[build] dependency_linkage`,以及依赖边上的 `linkage`。**(#519)

  ```toml
  [build]
  dependency_linkage = "shared"        # 全图默认;缺省即 "static"
  [dependencies]
  "compat.zlib" = { version = "1.3.2", linkage = "shared" }
  ```

  `static`(默认)与 mcpp 一直以来的行为**逐字节相同**,不写这个键的工程
  构建结果不变。也可按 `[profile.*]` 覆盖。

  形态是**每个链接映像解析一次**,不是每条依赖边一次 —— 同一个库在一个映像里
  出现两种形态,正是本条要抓的缺陷。边上的 `linkage` 只在**根工程**生效:
  依赖图深处的包无权决定最终程序的布局。

  **这不是 `[target.<triple>].linkage`**(那根是 C 库轴),而且两者**不独立**:
  整链静态的映像没有解释器,装不下任何共享对象。C 库静态链接的目标 ——
  **musl 的默认** —— 会拒绝 `shared` 并说明原因。

- **符号提供者检查。** 链接后核验映像里每个符号恰好有一个提供者。

  ELF 上可执行文件排在最前,被静态并进程序的库会在共享副本之上获胜:
  共享的那份永远不会被调用,那个库里的代码跑在一份它并非针对其链接的构建上。
  链接器与加载器都不报任何一句话。实测 `/usr/bin/git`(自己的 `error` 遮蔽
  glibc 的 `error(3)`)与 `/usr/bin/ls`(gnulib obstack 遮蔽 glibc 的)都是
  这个形状的野生实例。

  判据是**测量**不是声明,因此对 vendor 包里随附的库、`[system_deps]`
  引入的宿主库同样有效 —— 引擎不需要认识任何具体的库。

  **两段式,而第二段不可省。** mcpp 自己的 `kind = "shared"` 机制会
  **结构性地**产出「exe 导出、`.so` 绑过来」这个形状,而那是单份定义、
  完全良性的。只看第一段会在正确的构建上刷警告,而用户对此无事可做。
  真正的判据是「导出的东西**还有第二个提供者**」。

  默认警告,`--strict` 下升级为错误。判定写进
  `resolution.json` 的 `runtime.symbol_provision`,带计数与分母。

### 修复

- **依赖包的 `[targets.*] required_features` 从来没有生效过。**

  目标门控只有一处,判据是**根**的活跃 feature 集,依赖包的 `targets`
  一个都不过滤。一个描述符写下 `required_features`,得到的是它要求的**反面**:
  该目标对每一个消费者都构建,不管 feature 开没开。

  对 `kind = "shared"` 的目标这不是外观问题:包里只要存在任何一个 shared
  目标,它的**全部**对象就会从每个消费者的链接里被拿走。一个「可选」的目标
  因此悄悄改变了整个包对所有人的链接方式。

- **`-fPIC` 不在缓存键里。**

  它是全图的(图里任何一个 shared 链接单元存在,所有对象都带 PIC),而键取的是
  包**声明**的 flags,不是 flag 构造器算出来的。在形态由作者定死时可以幸存;
  一旦消费者能请求 shared,同一条目就会把非 PIC 对象喂给共享链接,而报错指向
  一个没人改过的文件。现在由 `make_plan` **决定一次**,编译标志与缓存键读同一位。

  `dependency_linkage` 同样进了工程指纹 —— 否则切换开关会复用上一次配置的
  构建目录(实测:两次构建落在同一个 `target/x86_64-linux-gnu/<fp>/`)。

- **`mcpp pack` 不收合成的共享库,包解开就起不来。**

  ```console
  $ ./app
  error while loading shared libraries: libcore.so
  ```

  闭包来自**运行产物**,而运行的是**暂存目录里的副本** —— 副本旁边的 `bin/`
  是空的,`$ORIGIN` 解析不到,库于是从不出现在闭包里,也就从不被打包。
  构建、打包、上传全程无话,失败发生在用户机器上。

  **不是本版引入的**:在 2026.8.26.1 上用作者声明的 `kind = "shared"`
  依赖同样复现。但 `dependency_linkage` 把它从「12 个自称 shared 的包」
  变成「任何一个包」都可达,所以在这里修。

- **在非 shared 目标上写 `soname` 会让整份 manifest 加载失败。**

  `soname` 是一个库被**找到**时用的名字,也是 mcpp 构建的那份与第三方携带的
  同一个库能解析到同一个文件的唯一途径。收窄为「非 **library** 目标才拒绝」,
  可执行文件仍然不允许声明。

  因此把 `soname` 写进索引描述符要等 `latest` 的 mcpp 下限跨过本版本 ——
  旧客户端读到的是加载失败,不是忽略。

### 文档

- `docs/05` §2.2 「共享库目标只支持 Linux/ELF」已过时:PE 与 Mach-O 早已支持
  (`tests/e2e/257`、`259`)。中英双份同步更正。

## [2026.8.27.2] — 2026-08-27

一个文件名把整个 Windows 构建打断了,而报错说的是别的事。完整分析见
[`.agents/docs/2026-08-27-issue516-windows-acp-glob-walk-fix.md`](.agents/docs/2026-08-27-issue516-windows-acp-glob-walk-fix.md)。

**这是 `#230` 的同一处漏网,不是新缺陷。** `#231` 加固了三个窄化站点,
漏掉了同一个 walk 循环里**早一行**执行的第四处。

### 修复

- **一个当前代码页拼不出的目录名,会让 `mcpp` 在 Windows 上以内部错误退出。**(#516)

  `src/modgraph/scanner.cppm` 的 `is_excluded_walk_dir()` 用
  `dir.filename().string()` 取目录名。MSVC 的 `path::string()` 走
  `WideCharToMultiByte(ACP)`,遇到该代码页拼不出的字符就抛 `std::system_error`:

  ```
  error: internal: unhandled exception: No mapping for the Unicode character
  exists in the target multi-byte code page.
  ```

  该函数是 walk 循环体的**第一行**,每个目录条目过一次 —— 所以它比 `#231`
  加固过的 `path_matches_glob` **更早**执行,加固那里对目录名从来无效。

  触发条件比看上去宽:`include_dirs = { "*" }` 这类以 `*` 开头的 glob,
  字面前缀为空,会从解压根**无界递归遍历整棵上游源码树**。
  在 mcpplibs/mcpp-index `891b2f7` 上量:130 个 recipe 里有 **103 个**至少含一条
  这样的 glob(口径:`.lua` 里出现以 `*` 开头的字符串字面量;抽查其分布为
  `*/include` / `*` / `*/src` / `*/mcpp.toml` 等,全部是 glob,无假阳性)。
  这个比例会随索引增长而变,写下的是当天那个 commit 的数。
  cpp-httplib 带了 `test/www/日本語Dir/`,于是 `httplib` / `httplib-tls` /
  `httplib-zstd` 三个测试在 Windows 上一起挂 —— 而 Linux/macOS 全绿,因为那两个
  平台上 `path::string()` 不做任何编码转换。

  **报错指向的方向是错的**:它看起来像下载器的解压/编码缺陷。实际上解压是**对的**
  —— `ERROR_NO_UNICODE_TRANSLATION` 的前提正是宽名里有 ACP 拼不出的字符;
  若真落成了 mojibake,反而不会抛。

- **窄化收敛成一条规则,而不是第四个 try/catch。**(#516)

  `mcpp::modgraph::try_narrow()` 是走查路径变成窄串的唯一入口。按用途分三档:
  与 ASCII 字面量比较 → **按 `path` 比,不窄化**;需要稳定身份(hash/digest)→
  `u8string()`;需要交给编译器/ninja/CDB → `try_narrow()` 并处理 `nullopt`。
  `.github/tools/check_narrow_conversions.sh` 是硬门(作用域被刻意收窄到
  `src/modgraph`、`src/scaffold` 两处 —— 第一版覆盖四个目录、22 个命中里 20 个是
  假阳性,那样的门一个月内就会被绕过)。

- **跳过的文件不再是静默的。** 一个无法命名的条目会按目录报告一次,走
  `mcpp.diag` 的 `degraded` 通道(它的批次不变式本就要求"因前提不满足而少做事
  必须给出 `impact`"):

  ```
  warning: '<dir>' contains names this system's active code page cannot represent
    impact: those files take no part in the build
  ```

- **`interface_set_digest` 不再依赖宿主代码页。**

  它用 `.string()` 折入文件名,而输入是对已发布包 interface 目录的**未经过滤**的
  `recursive_directory_iterator` 走查。除了会抛,它还让**在 Linux 上打包、在
  Windows 上校验**的同一棵树对非 ASCII 名字给出不同摘要 —— 表现为
  "does not match what was packaged",本文件能产生的最吓人的诊断。
  改用 `u8string()`(各平台同一串字节);纯 ASCII 名字字节不变,已发布包的摘要不变。

### 其他

- 内部依赖的 xlings pin 升至 `2026.8.27.4`。

## [2026.8.27.1] — 2026-08-27

目标侧被解析出来了,只发给了一个编译单元。完整分析见
[`.agents/docs/2026-08-27-openkal-native-path-three-issues.md`](.agents/docs/2026-08-27-openkal-native-path-three-issues.md)。

**与 `2026.8.25.x`/`2026.8.26.1` 是同一族的下一层。** 那两批修的是「谓词问错了」
与「答案没接到决定上」;这一批里,答案**接上了一个消费者,而它有五个**。

### 修复

- **编译侧的谓词,是 `2026.8.26.1` 在链接侧修掉的那条的孪生兄弟。**

  `hostflags.cppm` 问的是 `!crossTargetFlag.empty()` ——「命令行上有没有
  `--target=`」——而它的注释写的是「目标侧来自图」。同一台机器、同一个编译器、
  同一个目标,只差写不写 `--target`,编译线少了**六个 token**:

  ```
  --no-default-config  -nostdinc++
  -isystem <payload>/include/c++/v1
  -isystem <payload>/include/<triple>/c++/v1
  -isystem <glibc>/include
  -isystem <linux-headers>/include
  ```

  ⇒ 头文件来自一个库,目标文件链自另一个库。两侧现在读同一个
  `plan.targetSide.cAbi.prebuilt()`。`--no-default-config` 从这个条件里
  **拆了出来无条件发** —— 它不是载荷头文件集合的一部分,而 cfg 文件按
  `post_install.cppm` 自己的说法是「per-machine, per-install-path artifact」。

  `e2e 295` 写的就是这条恒等式,而它只比对 `^ldflags`,所以恒等式在**下一行**
  不成立而测试看不见。现在两条都比。

- **载荷目录名是 LLVM 词汇,而查找用的是 mcpp 词汇 —— 而且失配是静默的。**

  `include/<triple>/c++/v1` 与 `lib/<triple>` 由 LLVM 的构建写下,带的是
  `x86_64-unknown-linux-gnu`;`tc.targetTriple` 是 mcpp 的
  `x86_64-linux-gnu`。两者只在三元组是**探测来的**时候恰好相同。查找是
  `if (exists) push_back`,所以找不到就什么也不发生 —— 而那个目录里只有一个文件,
  `__config_site`,它的缺席产生的报错读起来像载荷坏了。两种拼法现在都试。

- **由图供给的目标侧,只到达了一个编译单元(mcpp#514 §A)。**

  提供 `mcpp:` 层的包发布的是**整个目标**编译时所依据的头文件集合,而它今天以
  `publicUsage` 的形态**沿依赖边**传播。于是根与 provider 自己的单元拿得到,而
  **兄弟依赖包**拿不到 —— `nlohmann.json` 不在 `openkal-llvm-runtime` 的下游,
  它在它旁边。结果是一次构建里两种口味的 BMI,任何同时导入两者的 TU 在第一个
  模板实例化处炸掉(`reference to 'space' is ambiguous`)。

  目标侧解析之后,`fromGraph()` 的层的 `publicUsage` 并入**每一个**包的
  `privateBuild`;`std` 模块的命令行也改读同一个集合,不再自己推一遍。

- **缓存键描述了编译器,没有描述它被指向的头文件集合(mcpp#514 §B)。**

  A 轴上的每一项都在描述**编译器**,没有一项描述它编译时所依据的**库** ——
  而两者是分开安装的。`driverIdentity` 按设计也覆盖不了它:
  `normalize_driver_output` **故意**抹掉路径,好让一个条目能被两个 home 共享。
  新增 `targetHeaderSet` 轴,取自已经解析好的 `linkmodel`,并**分两档相对化**:
  `<store>` 与 `<home>`。只做 `<store>` 一档不够 —— 最常见的那台机器走
  `CLibMode::Sysroot`,它唯一的编译 token 是 `--sysroot=<home>/registry/subos/default`,
  在 HOME 底下而不在 store 底下,于是每个条目都会带上这台机器的 home。分得开什么:
  两个载荷(路径里带版本号)⇒ 两个键;一个 home 下的两个 subos ⇒ 两个键;
  两个 home 下同名的 subos ⇒ 仍是一个键(由整工程指纹的第 11 项区分)。
  不 bump `kCacheEpoch` —— 旧条目是 miss 而不是不可用。

- **请求的版本和载荷目录的版本是两套词汇,而每个查找都按请求那套拼。**

  RuntimeBinding 带的是**声明的**版本(`glibc@2.44`),而 xlings 把载荷目录按这个
  请求**解析成**的版本命名(`2.44.2`)。索引在同一序列内挪动一次包,所有按声明版本
  拼目录名的查找就同时失效 —— 在每一台**新**机器上,在任何已存在的机器上都不出现。

  两个查找点各自拼过一遍,所以修好一个还剩另一个,而第二处的失败**根本不提版本**:
  glibc 的 include 目录只是没被加上,用户读到的是 libstdc++ 头文件里的
  `features.h: No such file`。⇒ 收敛到一个 `payload_dir_for_version`。

  **判据是「精化」,不是「按目录序挑一个」**:`2.44.2` 的版本**分量**以请求的分量
  开头。`2.4` 回答不了 `2.44`(逐分量比,不是逐字符)。两个载荷都精化同一个请求时
  返回**空** —— 「这个请求的解析结果」得是唯一一个才配叫答案,而按目录序挑正是这里
  每个调用者都拒绝做的猜测。反方向(拿更旧的载荷回答更新的请求)不接受。

- **home 发现有第四份拷贝,而且会伸到别的 home 里去。**

  `active_home_xpkgs()` 自己重推了一遍 home(漏掉自包含安装那一档);
  `find_sibling_package` 找不到时**无条件回落** `~/.xlings/data/xpkgs`。
  后者意味着一次密闭构建可以从**另一棵树**取载荷,而结果直接进每条编译命令的
  `-isystem`。前者改为 `mcpp::home::root()`,后者删除 —— 找不到会说话,找错了不会。

### 新增

- **`[build] private_include_dirs`** —— 指出 `include_dirs` 中在本包边界处停住的
  条目。`publicUsage` 此前整份接过 `privateBuild` 的目录,于是一个内嵌了带内部头
  覆盖层的库(musl 的 `src/include` 定义 `hidden`/`weak`/`weak_alias`)会把那些宏
  发给每一个消费者。它是 `include_dirs` 的**子集**而不是第二个列表:两类目录的
  相对顺序是承重的,而两个 TOML 数组表达不了一个顺序。

## [2026.8.26.2] — 2026-08-26

已经解析出的答案,没有被用来做决定。完整分析见
[`.agents/docs/2026-08-26-resolved-but-not-consulted.md`](.agents/docs/2026-08-26-resolved-but-not-consulted.md)。

**这不是 2026.8.25.x 那个「谓词回答了比自己更窄的问题」的家族。** 那一族是判据
问错了;这一族里谓词问对了、答案也算对了,只是那个答案**没有接到决定上**。两条都
是「多存了一个字段而没有多接一根线」,因此读判据时看不出来 —— 只在用户问「你既然
已经知道了,为什么还要我说一遍」时暴露。

### 修复

- **依赖声明的编译器被检查,但从未被采纳。**

  ```
  $ cat mcpp.toml
    [dependencies]
    openkal-llvm-runtime = "0.1.3"      # requires = ["mcpp:compiler=llvm"]

  $ mcpp build                          # 全局默认 gcc@16.1.0,而 llvm@22.1.8 已装
    error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
           Select that compiler …  mcpp toolchain default llvm
  ```

  这次拒绝没有换来任何信息:它要求的东西已经在本机,版本也已确定
  (`MCPP_TOOLCHAIN=llvm@22.1.8 mcpp build` 用时 1.02s)。付出的代价是让用户改一
  处**全局**状态去满足**一个**工程的一条依赖。

  真因是位置:`prepare.cppm` 有一个标题写着「the toolchain, resolved now that
  the graph exists」的接缝,它扫 `pkg.manifest.provides` 来决定编译器,**却不扫
  `requires_`** —— 后者在一千行之后才被收集,只用来否决这个决定。

  **修好之后不写任何东西,而这是位置带来的,不是额外加的开关。**
  `resolve_target_toolchain` 只有两个调用点,整个函数体(含首次运行的
  安装并持久化分支与全部三处 `write_default_toolchain`)都在图之后。把图的要求
  写进 `tcSpec` 的时机早于首次运行分支被求值,于是:

  | | 之前 | 现在 |
  |---|---|---|
  | 已有 gcc 默认的机器 | 拒绝,要求改全局默认 | 装/用 llvm,`config.toml` 不动 |
  | 什么都没装的机器 | 装 gcc → 持久化 gcc → 再拒绝 | 首次运行分支根本不进,直接装 llvm |

  状态行点名是哪个包要求的、顶掉了什么;`why toolchain --format json` 新增
  `compiler.chosenBy = {origin, requiredBy, replaced}`,让「为什么是 llvm」不必去
  解析那行提示 —— 那正是机器接口存在的理由所要消除的字符串匹配。

  拒绝只剩一种局面:工程自己在 `[toolchain]` 或 `[target.X]` 写下了相反的编译器。
  那种局面里全局默认与本次构建无关,因此原来那条 `mcpp toolchain default llvm`
  的建议**连问题都解决不了**,已改为指向那条陈述本身。两个包要求不同的族则是错误
  而不是一次挑选,并同时点名两个包。

- **tier 闸问的是补全后的身份,而不是请求。**

  ```
  $ mcpp build --target aarch64-linux
    error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)
  $ mcpp build --target aarch64-linux-musl
    Finished dev [unoptimized + debuginfo] in 0.99s
  ```

  `parse` 把缺失的 env 段按词法填成 `gnu`,那是为了让**身份**完整(输出目录、缓存
  键),`envExplicit` 就是为记住这个区别而存在的 —— 而 tier 闸不看它。被问的是
  「aarch64 的 Linux」,被回答的是「aarch64-linux-**gnu**」,报错还引用了一个用户
  从没打过的字符串。

  省略了 env 段的请求现在对着词表补全,规则 1(词法默认受支持就用它)排在最前,
  因此 `x86_64-linux` 一动不动,而这件事能自己退休。**`parse()` 未改**:身份
  必须保持词法、全量、与宿主无关。

- **`unknown target 'riscv64-linux'` 说的是假话。**

  `riscv64-linux-musl` 就在词表里(`planned`)。词法填充产生了一个**完全不存在**
  的行,于是一个已登记的目标族被报成未知。而且这条路径没有 `refusal::record`,
  `--format json` 把它报成 `reason: "other"`。现在它给出 planned 的诊断并点名
  `riscv64-linux-musl`;真正的拼写错误仍报 unknown,但带上了新的
  `unknown-target` 记号。

- **`why toolchain --format json` 的两个字段互相矛盾。**

  `cLibrary` 说 glibc/payload,`layers[].c-abi` 说 musl/graph。产物给出裁决 ——
  静态、无解释器、无 `DT_NEEDED`、11 个 openkal 符号 —— glibc 不在里面。两者各自
  准确,回答的却是不同的问题,而消费方无从判断该信哪个。新增
  `cLibrary.suppliesTarget`;是**增字段**而非改名或给 `mode` 加取值,因为
  docs/11 §6 承诺字段只增不删、含义永不改变。

- **而「不写任何东西」需要一个有名字的规则,不只是一个位置。**

  `write_default_toolchain` 有三个调用点。首次运行那个的条件是
  `!tcSpec.has_value()`,方案确实让它进不去;另外两个 —— Windows 首次运行改道、
  MSVC 不可用时的修复 —— 条件不是它,**都可达**。一台没装工具链的 Windows 机器
  构建一个要求 llvm 的工程,会把 llvm 写成这台机器的默认值。

  修法是给规则一个名字:`tc_origin_may_persist(TcOrigin)`,两处都调用,一条单
  测陈述它。这条缺陷是**读出来的**:它需要一台没有工具链的 Windows 机器,而
  `config.toml` 的 sha256 判据跑在已配好的环境里,两条分支一条都到不了。

### 示例

- **`examples/06-openkal-cross` 现在是四个目标,而且不再写 `[toolchain]`。**

  第四行 `--target aarch64-linux` 正是这次修好的那一条 —— 在此之前它补全成
  `aarch64-linux-gnu`(registered but not supported)并拒绝。同时删掉了那个
  `[toolchain] default = "llvm@22.1.8"` 段:`openkal-llvm-runtime` 自己声明
  `requires = ["mcpp:compiler=llvm"]`,mcpp 现在读它。实测在一台全局默认为 gcc 的
  机器上,四个目标全部解析出 `llvm@22.1.8`,而 `~/.mcpp/config.toml` 一字未改。

  顺带修掉一处失效的钉:该示例的依赖下界还写着 `0.1.1`,而那个版本的
  compiler-rt builtins 在 aarch64 上编不过。这个示例**不被任何 CI 构建**,所以它
  钉住的版本过期了也没有任何东西会说话。

### CI

- **目标矩阵的编译器轴跟着「装了什么」走,于是缓存能决定判据。**

  实测:同一个提交,PR 的 `scan (windows-x86_64)` 绿,合入 main 后同一个 job 红 ——
  那次 runner 恢复出来的缓存里多了一个 `gcc@16.1.0`,扫描产出 24 格而期望表为这台
  宿主声明的是 16 格,八格全部报成「表里没有这一格」。

  **期望表是一份声明**,编译器轴现在跟着它走(`MATRIX_COMPILERS`,由 workflow
  从 expected.tsv 的同一列算出,与「装哪些」那一步同源,所以两者不可能各说各话)。
  装着却不在声明里的版本写到 stderr —— 一次没跑的测量和一次通过的测量,在退出码上
  没有区别。

### 兼容性

**没有任何一次原本成功的构建换了行为。** 图声明编译器的情形里,原来的结局是
`check_requirements` 拒绝 —— 也就是说那些构建本来就不成功;`--target aarch64-linux`
与 `riscv64-linux` 原来是拒绝;`x86_64-linux` / `x86_64-windows` / `riscv64-none` /
`aarch64-macos` 的补全结果一字未变(单测逐行遍历整张词表守住这一条)。机器接口只
增字段。`[toolchain] default = "system"`(PATH 编译器这条逃生口)不被替换。

### 判据

- e2e `299`–`303`,全部走 `--format json` 分类而非字符串搜索,并接入
  `target matrix` 的第一层 —— **四台构建机各跑一遍**。
- `301` 的判据是 `~/.mcpp/config.toml` 的 **sha256**,不是「构建成功」:构建成功
  与配置被改写可以同时为真,而那正是这次要消除的行为。
- `299` 的第二半是对照 —— `x86_64-linux` 必须仍是 gnu。只测 aarch64 会让「把
  linux 的默认整个换成 musl」这种过头实现看起来是对的。
- 单测 `TripleRequest.*` 七条,含一条遍历整张词表的「每个受支持的行都能从它自己的
  拼写到达」。

## [2026.8.26.1] — 2026-08-26

写出 `--target` 这个动作,曾被当成「这个构建的系统来自依赖图」。完整分析见
[`.agents/docs/2026-08-26-cross-target-implies-graph.md`](.agents/docs/2026-08-26-cross-target-implies-graph.md)
与[六张矩阵表](.agents/docs/2026-08-26-target-matrix-six-tables.md)。

### 修复

- **命名宿主自己的目标,曾让构建失败。**

  同一台机器、同一个编译器、**同一个目标**,只差写不写 `--target`:

  ```
  $ mcpp build                            → ELF 64-bit LSB pie executable
  $ mcpp build --target x86_64-linux-gnu  → hermetic link check failed
  ```

  不带 `--target` 时宿主目标**就是** `x86_64-linux-gnu`。显式那条丢掉了
  `-stdlib=libc++`、`--rtlib=compiler-rt`、`--unwindlib=libunwind`,以及指向已装
  `xim:glibc` 的 `-B`/`-L`/`--dynamic-linker`,于是 clang 回退到默认,启动对象落到
  载荷之外。

  根因是 `crossTarget` 非空被当成「系统来自图」。那处代码**自己的注释**写的正是
  后者;而 `crossTarget` 只是 `--target=<三元组>` 这个字符串,任何命名目标都非空
  ——包括命名宿主目标、且完全不依赖任何包的工程。

  **同一个错误的问题被问了三遍**,分散在三处:`link_toolchain_flags`、
  `payload_ld`、`atomic_ld`。而其中一处的注释只预告了**两**条通道:

  > the C-runtime group reaches the link line through TWO channels, and a reader
  > who fixed one saw the identical error and could reasonably conclude the fix
  > had not worked.

  三条全部改问 `targetSide.cAbi.prebuilt()` —— 与 `2026.8.25.1`
  把另外三处决定迁过去的**同一个谓词**。这是该族的第六至第八条。

- **目标行声明的 sysroot 从不被安装(#510)。**

  一行目标表声明两样东西,只有一样被兑现:`pin` 走
  `resolve_xpkg_path(…, autoInstall=true, …)`,`sysroot` 是纯查询,查不到就静默
  跳过整块。干净环境实测:

  ```
  Target riscv64-none-elf
         c-abi  picolibc-riscv (…, prebuilt)
  error: 'stdio.h' file not found
  ```

  报告点名了这个目标的 C 库,而构建找不到它的头。mcpp 自己的裸机 CI **手工装
  它**并在注释里说明了原因,于是每一条裸机 e2e 都跑在缺陷已被抹平的机器上。

  改为走同一个 `autoInstall` 通道;离线与 `MCPP_NO_AUTO_INSTALL` 由 `Fetcher`
  判定,不在此处再问一遍。

- **裸机行的 pin 是能力陈述,不是偏好。**

  ```
  [toolchain] default = "gcc@16.1.0"
  $ mcpp build --target riscv64-none-elf
    g++: error: unrecognized argument in option '-mabi=lp64d'
  ```

  一条关于选项的消息,而决定在一百行之前。宿主行的 pin 说的是「哪个载荷供给这个
  目标的 C 库」,作者自带编译器时理应让位;裸机行说的是「哪个编译器能发出这个
  目标」——宿主 g++ 发不出 riscv64,谁声明都不行。现在在决定处拒绝,并指出出路。

  **约定仍然可以被推翻**:hosted 目标上显式声明 gcc 照常生效。

### 目标矩阵在四台构建机上找到的

**116 格,0 个 `mismatch`。** 四台各自的实测写在
[`tests/matrix/expected.tsv`](tests/matrix/expected.tsv),每一行都来自它自己那台
机器 —— 从别的宿主推断出来的一行,断言的是推断而不是那台机器。

| 宿主 | 格数 | ok |
|---|---|---|
| `linux-x86_64` | 40 | 14 |
| `linux-aarch64` | 16 | 1 |
| `macos-arm64` | 20 | 10 |
| `windows-x86_64` | 40 | 9 |

| status | reason | 格数 |
|---|---|---|
| ok | none | 34 |
| unsupported | tier-planned | 36 |
| unsupported | capability-pin | 16 |
| unsupported | convention-unreplaced | 12 |
| unsupported | host-cannot-serve | 7 |
| unsupported | layer-requirement | 5 |
| unsupported | host-tool-toolchain | 4 |
| unsupported | lld-required-absent | 1 |
| unsupported | other | 1 |

最后那一格是**诚实的** `other`:`std module precompile failed` 由
`stdmod.cppm` 发出,那里够不到拒绝记号的沉淀点。它是**构建失败经拒绝通道浮出**,
不是一条规则 —— 把它硬塞进邻近的理由才是错的。



**把 `linux-aarch64` 加进构建机轴之后,四台各自交出了一台机器上看不见的缺陷。**
轴取自 `release.yml` 发布的那一组(linux-x86_64 / linux-aarch64 / macos-arm64 /
windows-x86_64),而不是手头有哪几台 runner —— 一台拿到发布二进制却从没被扫过的
宿主,它的目标表是一句没人核过的话。

- **`--rtlib=compiler-rt` 在 aarch64 上是 codegen 事实,而它被声明在只覆盖
  一侧的键上。**

  ```
  error: precompiled file 'std.pcm' was compiled with the target feature
         '+outline-atomics' but the current translation unit is not
  ```

  `openkal-llvm-runtime` 把它写在 `std-module-flags` 里 —— 那个键到达 `std.pcm`
  的命令,不到达任何消费者的 TU。**与 `-fdwarf-exceptions` 是同一个缺陷,换了
  一个 flag**;那次的解法是提升成图级的 `graph_runtime_compile_flags`,这次同样。

  x86_64 上两侧都列空,所以直到第二个架构被构建才可见。

- **PE + musl 在任何宿主上都没有载荷,而 `host_can_serve` 在 Windows 上说有。**

  ```
  c-abi    musl(payload)
  c++-abi  msvc-stl(payload)
  lld-link: error: undefined symbol: __mingw_vfprintf
  ```

  musl 的 C 库、MSVC 的 STL、MinGW 的符号,一格里三个 C 运行时。
  `triple::pin_is_capability()` 与 docs/16 都已写明这一行只能由依赖图供给;
  Linux 上同一格早就答 `host-cannot-serve`。

- **宿主服务不了的目标,仍然去装它的载荷。** 拒绝被决定在早、释放在晚(因为
  图供不供给系统只有解析后才知道),而安装夹在中间,于是先失败且失败得更硬:
  `xlings install of 'xim:x86_64-linux-musl-gcc@16.1.0' failed`。跳过安装让两条
  后续路径都完好;尝试安装帮不了其中任何一条。

- **aarch64 Linux 只支持 `musl-gcc`,其余显式延缓。** 上游 LLVM 从 20.x 起停发
  `linux-aarch64`,索引里也没有,所以 `available_toolchain_indexes()` 在非 x86_64
  Linux 上不再列 `llvm` 与 `mingw-cross-gcc`。这是**政策陈述**不是索引数据的
  抄本,并且 `check_aarch64_llvm_deferral.sh` **在理由不再成立时变红** ——
  没人复查的延缓与缺陷无法区分。计划见
  [`.agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md`](.agents/docs/2026-08-26-aarch64-linux-ecosystem-closure.md)。

- 交叉 musl 与 mingw 的载荷都按宿主架构发布,`host_can_serve` 对两者也改为按架构
  回答。「自足」讲的是载荷装了什么,不是它为哪些宿主发布 —— 那行注释是对的,
  代码把它读错了一层。

### 机器接口

- **两条命令进入 `--format json`,矩阵与四条 e2e 不再匹配任何一句话。**

  ```
  mcpp toolchain list --format json          → mcpp.toolchain.list
  mcpp why toolchain --target T --toolchain C --format json
                                             → mcpp.why.toolchain
  ```

  后者只解析不构建,给出五层、驱动器、三元组、C 库模型,以及 `status` 与
  `reason`。`reason` 是一个记号——`capability-pin` / `convention-unreplaced` /
  `tier-planned` / `host-cannot-serve` / `os-mismatch` / `layer-requirement` /
  `layer-ordering`,由 `mcpp.build.refusal` 在每一处拒绝的 `return` 之前记下。

  **代价是当场量到的**:本次会话里我把 `cannot emit it` 改成
  `cannot be emitted by`,e2e 297 的断言随即变成空转——它仍然「通过」,只是不再
  匹配任何东西。消息**仍然是承诺**(点名目标、规则与出路,e2e 照旧断言这一点);
  换掉的是**分类**:一个答案集有限的问题,不该用子串搜索来问。

  而**查询不能取代构建**。`llvm × x86_64-windows-gnu` 解析得完全正常,失败
  在链接期的封闭性检查上——只查不建会把它报成绿。矩阵两样都做:分类取自
  `reason`,结论取自构建的退出码。

  `why toolchain` 声明的 effects 故意偏宽(`network` / `write-global-cache` /
  `exec-build-script`):它的答案来自与构建同一次的解析。客户端是在运行**之前**
  读这张表的,漏报一项就是一句不成立的安全承诺。

- **`refused` 曾与 `none` 同读数。** 一处没有记号的拒绝分支让矩阵写下
  `unsupported / none`——「拒绝了」和「没有理由」共用一个词,正是本次发布在修的
  那个形状,重现在为发现它而造的机器里。现在无记号的拒绝报 `other`:一句可见的
  承认,而不是并进邻近的理由。

- **新增 `ci-target-matrix.yml`,三个宿主 × 两层。** 第一层跑上述恒等式,
  不需要期望表;第二层用 `tests/matrix/scan.sh` 全表扫描 × 两种体系,与仓库里的
  `tests/matrix/expected.tsv` 比对。

  **「跳过」必须是期望表说的,不是运行时发现的**:一格因为「今天这台机器没装
  某载荷」而跳过,与「这个组合本就不支持」是两回事,前者会让矩阵在缺件机器上悄悄
  变绿。比对脚本还先断言**扫描真的跑了**——一格没跑与全部通过,在退出码上没有
  区别。


### 测试

- **e2e 295 是一条恒等式,不是一个阈值。** `mcpp build` 与
  `mcpp build --target <宿主自己的目标>` 描述同一次构建,链接线必须逐 flag 相同。
  它不需要期望表、不取决于机器上装了什么,任何宿主都成立。

  **它把「修了一半」直接指出来**:差异 7 项 → 5 项 → 3 项 → 0,每一步指向下一
  条通道。没有它,修完两条会看到「还是红」,而那句注释会让人以为已经找全。

- e2e 296:报告说 `c-abi (payload)` ⇒ 链接线必须含该载荷;说 `(graph)` ⇒ 不得含
  宿主的 C 库。两向。
- e2e 297:能力 pin 不可被推翻,且约定 pin 仍可被推翻——只断言前一半时,一个把
  所有声明都拒掉的守卫同样能通过。

## [2026.8.25.2] — 2026-08-25

一个谓词族的收尾。`2026.8.25.1` 修了其中四条,本次修余下三条,并补上让它们
存活至今的两个 CI 空洞。完整分析见
[`.agents/docs/2026-08-25-the-two-layer-predicate-family.md`](.agents/docs/2026-08-25-the-two-layer-predicate-family.md)。

### 修复

- **一个包声明「我供给哪一层」,让裸机目标丢掉了唯一能产出它的编译器。**

  实测,三行清单就够:

  ```toml
  provides = ["mcpp:kernel-abi=openkal"]
  ```
  ```
  $ mcpp build --target riscv64-none-elf
    Resolved gcc@16.1.0 → riscv64-none-elf → …/bin/g++
    g++: error: unrecognized argument in option '-mabi=lp64d'
    g++: error: unrecognized command-line option '--target=riscv64-none-elf'
  ```

  `graphSuppliesSystem` 是一个跨 kernel-abi ∪ c-abi 的 OR,它取消目标行的编译器
  pin。**对宿主行这是对的**——那一行指的是「哪个载荷供给这个目标的 C 库」,图供给
  了就用不上它。**对裸机行不是**:表里自己写着「pin 是 llvm,因为 clang/lld 按构造
  就是交叉编译器」——宿主 g++ 根本发不出 `riscv64-none-elf`,图里有什么都不改变
  这件事。

  于是 pin 分成两类,而这个区分**在读取那一行的同一处**记下,不在决定点重新推导。

  与 2026.8.25.1 修的四条同型:**一个跨两层的谓词,决定了一件不取决于这两层的事**。
  这是第五条。

  发现方式:2026.8.25.1 发布后重钉七个下游 pin PR,openkal-opensbi 红在
  `g++: unrecognized`。它在 2026.8.24.6 那轮红在**同一条**,所以既非 25.1 引入,
  也非 25.1 修掉——是同一跨度里的遗留。

- **图供给了 C 库,不等于目标平台的 SDK 不再需要。**

  ```
  kernel-abi   openkal   (openkal-macos@0.3.4, graph)
  c-abi        musl      (openkal-musl@0.3.5, graph)
  ld64.lld: error: library not found for -lSystem
  ld64.lld: error: undefined symbol: clock_gettime_nsec_np
  ```

  macOS 分支给链接线加 `-isysroot`,它自己的注释写明了为什么(「否则 ld64.lld
  会死在 library not found for -lSystem」);而 80 行之后的图分支把整条 `f.ld`
  换掉,`-isysroot` 随之消失——**注释预言的那个失败,由它下面的代码造成**。

  **Linux 上二者恰好重合而 Darwin 上不重合**:Linux 的内核接口是一条指令
  (`syscall`),所以自足的 musl 真的替换了一切;Darwin 的内核接口**本身就是一个
  库**(libSystem),所以 Mach-O 链接无论 libc 从哪来都要 SDK。新增
  `platformAnchor`:写一次、读一次,两个分支不可能对「什么该活下来」有分歧。

- **请求的目标与解析出的目标必须是同一个操作系统。**

  ```
  Target x86_64-windows-gnu → x86_64-unknown-linux-gnu
  …
  src/stream.cpp:68:9: error: 'GetFileType' was not declared in this scope
  ```

  一行里两个操作系统,而没有任何提示。交叉载荷缺席时解析回落到宿主编译器,
  Windows 源码被按 Linux 编译,失败在一百行之后——报出的是一个 Win32 函数名,
  不是做出这个决定的那一处。openkal-uefi 撞在链接器上:
  `ld: unrecognized option '--subsystem'`。

  **报告里早就有证据,现在对它下断言**,而不是把问题重新推导一遍。范围刻意
  只取 OS:`x86_64-windows-gnu → x86_64-w64-windows-gnu` 的差异正是这一行要报告
  的归一化,拿整个三元组比会拒掉每一次正确的交叉构建。

- **「首次运行」那条路把 `--target` 丢了。**

  ```
  First run  no toolchain configured — installing gcc@16.1.0 (glibc, native ABI)
   Resolved  gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++          ← 路径里没有目标
  ```

  同一条命令在已有工具链的机器上是
  `Resolved gcc@16.1.0 → x86_64-windows-gnu → …/mingw-cross-gcc/…`。这条分支回答
  的是「这台机器没有工具链,给它一个」,答案是一份**宿主**载荷;
  `overrides.target_triple` 在这条路上**从未被读取**。而载荷解析那条路上
  `autoInstall=true` **本来就在**,只是没被走到。

  修法不是加条件,而是让首次运行**汇入**那条已经会处理目标的路径。上面那条
  「同一个操作系统」的不变量因此有了配套:守卫让错配变成一句拒绝,汇入让本来就能
  服务的目标不再走到那句拒绝。

  **这一处的第一版是无限递归,而我的注释写着「深度为一」。** 闸放在了分支之外
  (必须放外面:它上面那段 Windows 代码自己会设置 target),而标志没有任何人复位。
  本机看不见——这一格只在「首次运行 + 交叉目标」出现,而开发机永远不是首次运行。
  抓到它的是两条 CI,症状还不同:生态仓库上 `Resolved` 打四遍后 **exit 139
  (SIGSEGV,爆栈)**,mcpp 自己的 `bare Windows` 上 `First run` 反复打印后 exit 1。
  现在的闸结构上不可能循环,标志在**调用之前**置位。

- **`toolchain list` 漏掉了本机能构建的目标。**

  它用 `host_can_serve`(问的是「有没有预制载荷」)去回答「能不能构建」。实测
  Linux 上 `x86_64-windows-musl` 不在列表里,而同一台机器能产出真正的 PE32+。

  **而不是每一行缺席都是这样**:`x86_64-windows-msvc` 与 `aarch64-macos` 在
  Linux 上缺席是**对的**,MSVC 与 macOS SDK 是宿主专有的,依赖替代不了。判据不
  需要新字段——**一行若指向本宿主装得上的编译器,那它缺的只是系统,而系统可以
  由图供给**。第三种状态:`via dependency graph`。

- **镜像完整性门:一个没有重试的 GET 判掉整条发布。**

  `2026.8.25.1` 的发布红了两次,两次都报 16 个资产「already mirrored」,然后因
  其中一个的 502 判失败。手工抓下来:5,772,395 字节,sha256 与发布的校验和逐位
  相同。补 `--retry-all-errors`(不是 `--retry`,后者盖不住这条路径也会遇到的
  传输层错误);并修掉 `502ERR` 拼接(`|| echo ERR` 是追加不是替换)。

### 测试

- e2e 292,两向断言:声明一层之后裸机目标仍解析到同一个编译器(先建立基线,
  否则分不清「修好了」和「这台机器没有 llvm」);以及宿主行**不得**顶掉项目自己
  选的工具链——不加区分地永不取消 pin 也能让前一半通过,而那正是这个谓词当初要
  防的替换。

- **e2e 292/293/294**,每条两向断言,且**都在修复前的二进制上验证过会失败**:
  292 声明一层后裸机目标仍解析到同一编译器 + 宿主行不得顶掉项目自己的工具链;
  293 拒绝跨 OS 的解析 + 四个正确交叉目标零误伤;294 列出图供给的目标 + 宿主
  专有的仍然缺席。

- **`285`–`289` 此前一条都没在 CI 跑过。** 它们声明 `# requires: llvm`,而两个
  linux e2e shard 的能力行里没有 `llvm`,`run_all.sh` 在 skip 时退 0。新增
  `openkal-cross.yml` 的 `ecosystem-e2e`:装 gcc + llvm + 两个模拟器,直跑六条,
  **逐条断言 PASS 行**,并对 287/288 **额外断言运行阶段那一行**(实测它们会降级
  成 SKIP 而 OK 行照印)。

- **判据的「否」与「没测成」同读数**:一次会话里我自己新写的六条 e2e 有四条犯了
  它(宿主 objdump 反汇编外架构得零指令零报错、`readelf` 读不了的文件输出零行、
  CI 自己的 PATH 本就以 subos/bin 开头)。判据一律带分母
  (`7 LSE instructions out of 148906`),工具取自产生该产物的工具链。

## [2026.8.25.1] — 2026-08-25

### 修复

- **图供给内核接口时,载荷那份 C 库被一起断开了。**

  ```
  error: hermetic link check failed
           crt1.o (bare name — the linker cannot resolve it)
           crti.o (bare name — the linker cannot resolve it)
           crtn.o (bare name — the linker cannot resolve it)
  ```

  三行清单即可复现:

  ```toml
  [dependencies]
  openkal-linux = "0.5.4"
  ```

  判据是一个**跨两层的 `OR`**,被用来决定只取决于其中一层的事:

  ```cpp
  bool system_from_graph() const {
      return kernelAbi.fromGraph() || cAbi.fromGraph();
  }
  ```

  链接侧在它为真时**整体替换** `f.ld`,丢掉载荷的 binutils 前缀、库目录与
  rpath —— 而这些全是通往**载荷那份 C 库**的路。两层在它被写出来时针对的场景里
  同进同退(openkal 目标的内核接口与 C 库都来自图),在另一个同样普通的场景里
  分开:**一个在平台之上实现 openkal 的后端,而程序仍用载荷的 C 库**。
  驱动照样索取启动文件,链接器却没有了可查的路径。

  **这个形状正是每个 openkal 后端被测试的方式** —— openkal-linux、
  openkal-macos、openkal-windows 三家的一致性套件都对着平台自己的 C 库构建。
  它们的 CI 钉在旧 mcpp 上,所以一直绿;pin 一动就全红,而这是缺陷被发现的
  唯一原因。

  **mcpp 278 条 e2e 里,「kernel-abi 来自图 + C 库来自载荷」这个组合一条
  都没有。** 新增 `285_kernel_abi_from_graph_keeps_the_payload_c_library.sh`,
  断言到**产物能跑**,而不只是链接成功。

  判据两向(2026.8.24.6 与本修复):

  ```
  已发布 24.6   error: hermetic link check failed — crt1.o (bare name)
  本修复        ok  it links against the payload's C library, and runs
  ```

  区间由 CI 侧三分支并行二分给出:`8.21.3 绿 → 8.24.1 红`,唯一实质提交是
  #486。

  **判据是层的来源,不是情形的列举。** `Origin` 有四个值
  (`Payload` / `Xpkg` / `Graph` / `None`),而链接线要问的是「C 库是不是来自
  解析之前就存在的目录」—— 这正是 `Layer::prebuilt()` 的定义,也是本模块开头
  那段注释划分世界的方式(prebuilt 在解析前可知,composed 在解析后才知)。
  因此**没有新增谓词**:两处改用 `cAbi.prebuilt()`,与既有的两处读法(C++ 层
  能否用载荷运行时、`check_layering`)成为同一个事实的第三次读取。

  一版写成 `fromGraph() || absent()` 的中间修法被否掉了:它答对三个来源、
  对 `Xpkg`(来自预构建 sysroot 的 C 库)沉默,而那同样不是载荷的。

- **ninja 后端的测试夹具补上了目标侧。**

  `minimal_plan()` 此前让 `TargetSide` 保持默认构造 —— 四层全 `Origin::None`,
  那不是「本机构建」,是「什么都还没解析」。它能一直蒙混过去,是因为旧判据
  对「全 None」与「载荷构建」给出同一个答案;换成链接线真正要问的问题,两者
  才分开,而夹具描述的于是变成「没有 C 库的目标」——它拿到 `-nostdlib -static`
  且不带任何运行期搜索路径,而这个文件里 44 条断言全是关于「有载荷 C 库」的。

  **没有任何生产路径会带着全 `None` 的 `TargetSide` 走到 flags**:`resolve`
  给普通本机构建的是 `cAbi = { Payload, … }`。夹具现在照实写。

- **缓存键漏掉了新参数,于是跨着一处不兼容命中了。**

  `compile_flags(spec)` 在 #486 长出第二个参数 `targetCxxRuntime`,而缓存键
  仍按一个参数算:

  ```cpp
  b.targetImpliedFlags = mcpp::freestanding::compile_flags(*spec);
  ```

  同一个键因此覆盖两套实际不同的编译 flag。**命中不是「跳过一次重编」,是
  「拿到一份为另一套 flag 建的产物」**——实测 `openkal@0.7.0` 出现 6 个槽位
  对应 5 个尺寸各异的 BMI。

  这类缺陷不会在加参数的那天失败,它在下一次缓存命中时失败,而那时改动
  已经不在视野里了。三条单元测试因此**直接打在 `build_axes()` 上**,而不是手
  搭一个 `BuildAxes`——后者表达不出「推导过程本身错了」这件事。

- **载荷的 C++ 运行时,服务的是载荷的 C 库。**

  ```
  undefined reference to `__cxa_allocate_exception'
  undefined reference to `std::runtime_error::runtime_error(char const*)'
  ```

  契约表用 `system_from_graph()` 决定要不要取载荷的 C++ 归档 —— 又是那个跨两层的
  OR。一个「内核接口来自包、而 C 库与 C++ 运行时都来自载荷」的程序本该由那些归档
  服务,OR 却说不是,`-nostdlib++` 于是砍掉了它要用的那一份。

  **这一处在两个方向上都错过。** 最初是 `targetCxxRuntime`,对 C 程序失败
  (它没有 C++ 运行时,答「否」被读成「载荷的是对的」);#486 换成 OR,又矫枉过正。
  **C 库才是决定它的那一层** —— 理由 `check_layering` 早已反向陈述:载荷的 C++
  运行时是对着载荷的 C 库配置的,所以当且仅当那份 C 库在用时它才可用。

- **第四条同型:`linkage = "dynamic"` 的「无效」诊断在说谎。**

  实测 2026-08-25,在 285 的形状上(kernel-abi 来自图 + C 库来自载荷):

  ```
  warning: `linkage = "dynamic"` has no effect … The artifact is static.
  $ file    → dynamically linked
  $ readelf → NEEDED libm.so.6, libgcc_s.so.1, libc.so.6
  ```

  谓词用的是 `system_from_graph()`(跨 kernel-abi 与 c-abi 两层的 OR),而这条
  警告自己给的理由——「那些包被当作对象编进本次构建,没有共享对象可链接」——
  是**C 库单独一层**的性质。载荷的 libc 有共享对象,`dynamic` 就被兑现了,这里
  本来无话可说。改为 `cAbi.fromGraph()`。

  prepare.cppm 里另外两处 `system_from_graph()` 保留:它们问的是「图有没有供给
  系统的任一部分」,那确实是两层的问题。

- **`build.mcpp` 的 `PATH` 前置项目声明的那个环境。**

  ```
  PATH=<被声明环境的 bin>:<mcpp 启动时的 PATH>
  ```

  构建程序想用某个工具,此前只能像 shell 脚本一样去问 `PATH` —— 而 `command -v`
  回答的是「这台机器有什么」,不是「这次构建用什么」。实测代价:
  `command -v qemu-system-riscv64` 命中一个 shim,执行时答
  `[error] qemu-system-riscv64 is not installed in this subos` —— 找到了、报告
  为存在、却跑不了,而可用的那份就在项目自己的环境里,不在 `PATH` 上。

  **只对声明了 `[xlings].subos` 的项目生效,没声明的逐字节不变。** 一个更早
  的草案无条件前置 mcpp 共享的 `subos/default/bin`,那会让「构建看见什么」取决
  于这台机器上还装过什么 —— 同一台机器上的两个项目彼此一致,而同一个项目在两台
  机器上不一致。是声明本身把它放到前面的。

  **前置而非替换。** 构建程序合理地会调 `git`、`python3`、shell,这些都不在
  SubOS 里;只有被声明目录的 `PATH` 会把它们全部弄坏。

  **没有新的决定点。** `mcpp::xlings::runtime` 早就是「项目用哪个 SubOS」的
  唯一策略,`RuntimeBinding::subosDir` 是它已解析的答案;本次只是把这个答案多交
  付给一个消费者。`projectSubosBin` 在绑定解析后算**一次**,两个交付点各自取用。
  本次发布修的三条缺陷全部来自「一个事实在多处各自推导」。各包的载荷路径由
  `MCPP_XPKG_*_DIR` 另行回答,与 `PATH` 是两个问题。

  新增 `examples/07-project-subos/` 与 [第 17 章](docs/17-the-project-environment.md)。

### 测试

- 五条单元测试,**按 `Origin` 的四个值各一条**,外加一条把缺陷本身写成断言
  (内核接口的来源不参与这个决定)。按枚举写而不按想到的情形写:后者只覆盖
  作者想到的,前者在枚举新增取值时会留下可见的缺口。

- 三条缓存键测试,**直接打在 `build_axes()` 上**而不是手搭 `BuildAxes`:
  那个夹具表达不了这个缺陷,因为缺陷在推导里。两种配置的 flag 必须不同、
  缓存键必须不同,而**宿主目标必须不受影响** —— 最后一条是防止修过头的控制项。

- 五条真实依赖 openkal 包的 e2e。此前八条提到 openkal 的脚本里有七条用的是
  **合成清单**(当场编造一个自称 `provides = ["mcpp:kernel-abi=…"]` 的包),
  测的是引擎对一句声明的处理,测不到生态实际的形状 —— 今天的两条回归都从这条缝
  里出去。

  | | 覆盖 |
  |---|---|
  | 285 | kernel-abi 来自图 + C 库来自**载荷**(后端跑在平台之上) |
  | 286 | 三层全来自图,断言静态、无 INTERP、能跑 |
  | 287 | 交叉到 aarch64,断言 outline-atomics 辅助函数与 LSE 指令数,qemu 真跑 |
  | 288 | 无 OS 无 C 库,断言 c-abi 那一行的**值**是 `—`(不是断言它缺席),并在 qemu 里真启动 |
  | 289 | **一台宿主横扫四个目标** —— 这个体系本就是通用交叉构建,传统栈要六个 runner 的覆盖,这里一个循环 |
  | 290 | 声明把环境放到 `PATH` 前面,**而且只有声明会** —— 两半都对着**继承的那个值**比对,不是比对一个模式 |
  | 291 | `dynamic` 只在 C 库来自图时被拒 —— 且断言产物的 `DT_NEEDED` 而非只断言文案 |

- **上面这张表里的 285–289,此前一条都没在 CI 跑过。**

  它们声明 `# requires: llvm`,而两个 linux e2e shard 报的能力行是

  ```
  Detected capabilities: elf unix-shell fresh-sandbox gcc patchelf pack …
  ```

  没有 `llvm`——shard 的 workflow 从不装。`run_all.sh` 在 skip 时退 0,于是
  套件一直绿,而专门用来衡量这个生态的五条测试一次都没执行。**「我加了测试」
  和「测试跑过」是两件事**,这一条我自己又犯了一次。

  修法用仓库已有的范式,而不是新造一个 token:`run_all.sh` 自己的注释写明了
  为什么没有 hard-requires——一个 token 分不清「这台 runner 配错了」和「这个
  平台本来就没有」。有效的守卫必须知道自己在跟哪台 runner 说话,所以它住在
  job 里。新增 `openkal-cross.yml` 的 `ecosystem-e2e`:装 gcc + llvm,直接跑
  这六条,再逐条断言它们的 PASS 行真的出现了。与 `ci-linux-e2e.yml` 的
  `baremetal` job 同形,同因。

  **这个 job 第一次跑就抓到 287 在说谎。** 它用
  `command -v llvm-objdump || command -v objdump` 找反汇编器,而 CI 上前者不在
  PATH、后者是宿主 GNU binutils —— BFD 只编了 x86_64。让它反汇编 aarch64 会打印
  一个文件头、**零条指令、零报错**:

  ```
  $ objdump -d a-aarch64.o | grep -cE '^\s*[0-9a-f]+:'
  0
  $ llvm-objdump -d a-aarch64.o | grep -cE '^\s*[0-9a-f]+:'
  5
  ```

  `grep -c` 得 0,脚本报「`+outline-atomics` 看起来是被关掉了」。它在写它的那台
  机器上通过,因为那里恰好有 `/usr/bin/llvm-objdump`。现在工具取自**编译这个
  产物的那条工具链**,并且先数指令总行数:零条 = 工具读不了这个文件,那不是
  关于 LSE 的证据。判据也随之带上分母(`7 LSE instructions out of 148906`)。

  同一轮还发现 287/288 的「运行」两步在 CI 上都降级成了 SKIP 而 OK 行照印。job
  因此装上两个模拟器,并对这两条**额外断言运行阶段的那一行**。

  290 的两半只有一半是特性:无条件前置能通过前一半,而那正是被撤回的设计。

## [2026.8.24.6] — 2026-08-25

### 新增

- **`x86_64-windows-musl`:Windows 上的 musl 有名字了。**

  同一个 `--target x86_64-windows-gnu`,两种体系下的 C 库完全不同 ——
  而 mcpp 用同一个名字称呼它们。实测同一份源码:

  | | MinGW CRT | musl / openkal |
  |---|---|---|
  | 体积 | 587,894 | **9,815,552**(16.7×) |
  | 依赖 DLL | `KERNEL32`、**`msvcrt.dll`** | `ntdll`、`KERNEL32`、`SHELL32` |

  MinGW 的 C 库不是自足的:`printf`/`malloc` 的实现在目标机自带的
  `msvcrt.dll` 里。musl 是自足的,整个 C 库静态链入,只经 openkal 调
  Win32 原语。**两者是不同的东西,不该共用一个名字。**

  **这个名字 LLVM 拼不出来。** 实测 llvm 22.1.8:

  ```
  clang++ --target=x86_64-pc-windows-musl -c t.cpp
      #5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)
  ```

  崩在 COFF 写出器,不是诊断而是 ICE。Windows 上四个非 MSVC 环境
  `gnu`/`cygnus`/`itanium`/`musl`,前三个能编,只有 `musl` 死;
  预定义宏显示它从未被建模(无 `__MINGW32__`)。

  **于是 mcpp 的名字与交给 clang 的三元组必须是两个字符串**,而它们
  本来就是 —— 构建报告里那个箭头两侧就是:

  ```
  Target x86_64-windows-musl → x86_64-w64-windows-gnu
         ^ 回答「C 库是谁」      ^ 回答「遵循哪套对象 ABI」
  ```

  该行 pin 为 `llvm@22.1.8`,不是偏好:全局默认为 gcc 时,空 pin 会让
  它落到 musl-gcc 载荷并报「没有 C++ 前端」—— 一条关于缺前端的消息,
  而真正的问题是只有 clang 能发这个目标。裸机各行同理。

  档为 `preview`:`verified` 的定义是「构建**并运行**过」,而端到端
  可用还差一环 —— `openkal-musl@0.3.3` 精确钉死 `openkal-windows = "0.1.3"`,
  索引里已有的 0.1.4 到不了消费者。生态链条另行推进。

### 修复

- **构建期体系下 `x86_64-windows-gnu` 的名实不符,此前静默。**

  ```
  x86_64-linux-gnu     名字说 gnu，事实是 musl  →  警告
  x86_64-windows-gnu   名字说 gnu，事实是 musl  →  静默
  ```

  `check_request()` 豁免了非「C 库轴」的平台,理由是「Windows 上 `gnu`
  命名对象 ABI 而非 C 库」—— **只对了一半**:那一段捆着对象 ABI(被兑现)
  与 MinGW 的 C 运行时(被图替换),第二件正是该函数存在的意义。

  判据改为「轴 ∈ {CLibrary, ObjectAbi}」。裸机的 `elf` 继续豁免 ——
  它在任何平台上都不命名 C 库,对它说「请求了 `elf` C ABI」是胡话。
  消息在对象 ABI 轴上额外说明 ABI 那一半**未受影响**。

  误伤由既有的 `fromGraph()` 挡住:实测传统预构建路径下
  `c-abi gnu (payload)` 不进警告。

- **暂存跨主版本副本时,与源码相邻的私有头没有跟着走。**

  当同一个包的两个主版本同时出现在一张图里,解析器会把其中一份改名暂存到
  `target/.mangled/` 下。暂存只搬源码 —— 这对经 `[build].include_dirs`
  找到的头是对的,它们靠绝对化后的路径仍指回原处;但对**写在源码旁边**的私有头
  不对:`#include "detail.h"` 是相对**包含它的那个文件所在目录**解析的,搬走源码
  就搬走了搜索起点。包从来没有声明过这条路径,因为它从来不需要。

  ```
  target/.mangled/openkal-opensbi/__self__/src/time.cpp:44:10:
      fatal error: 'sbi.h' file not found
  ```

  这条诊断指向的东西全是错的:路径是作者没写过的暂存目录,头文件就躺在源码
  期待的位置,而触发它的构建没有要求任何不寻常的事情 —— 一张图里有两个主版本是
  受支持的安排,这是它最普通的后果。

  现在,凡是包含了被暂存源码的目录,其中未被暂存的文件一并原样带过去。没有被
  暂存源码的目录不会被访问,所以代价与暂存量成正比。

### 文档

- docs/16 新增「三套词表,以及它们为何不同」(中英双份):GCC / LLVM /
  mcpp 三套的形状、`x86_64-w64-mingw32` 为何把实现放在 OS 位、
  clang 如何把它重拼为 `windows-gnu`,以及 mcpp 为何要保留自己那一套。

## [2026.8.24.5] — 2026-08-25

### 改进

- **`<arch>-<os>` 现在在每个平台上都是一个完整的目标。**

  ```
  mcpp build --target x86_64-windows    # 此前:error: unknown target
  mcpp build --target riscv64-none      # 此前:error: unknown target
  ```

  2026.8.24.3 立的规矩是「三元组既是身份也是请求,而请求必须能什么都不说」,
  **而它只在 Linux 与 macOS 上成立**。被强制写出第三段的两个平台
  (windows、none),恰恰是构建期体系最常用的两个。

  **真正的分界是两种体系,而不是平台。**

  | | 传统预构建体系 | 构建期体系 |
  |---|---|---|
  | 目标侧来自 | 工具链**载荷** | 依赖**图**,从源码构建 |
  | 第三段 | **挑选载荷**,解析期承重 | **不选中任何东西**,图已决定 |
  | 该不该写 | 该写,它是作出选择的方式 | 不该写,它陈述一个不被查询的请求 |

  在构建期体系下,`x86_64-windows` 不只是比 `x86_64-windows-gnu` 短 ——
  它**更准确**,因为那次构建里编译器是 clang、链接器是 lld、C 库是 musl、
  C++ 运行时是 libc++,**没有任何东西是 GNU 的**。

  **填 `gnu` 而不是宿主自己的 env。** `host_triple()` 在 Windows 上答
  `msvc`,按它填会让同一条命令在不同宿主上产生不同的输出目录与缓存键 ——
  目标的身份不允许依赖于它在哪里被构建。

  **判据是一个指纹,不是「能构建」。** 同一个二进制、干净的 `target/`,
  两种拼法都落进 `target/x86_64-windows-gnu/<同一指纹>/`,第二次构建
  0.10s 全缓存命中。若各自产生一个指纹,短拼法就只是「少打四个字符、
  多编译一遍」。

  连带:下面那条加注退化为**只在使用者主动写出第三段时出现** ——
  那时它才是一句有用的回答,而不是对一个本不必出现的词的辩解。

- **两张按三元组索引的表,一致性现在由机器保证。**

  `kKnownTargets` 说某行存在、属于哪一档、C 库由谁供给;
  `freestanding::kTable` 说它的 ISA 档与链接如何驱动。此前**没有任何东西
  检查这两份描述覆盖同一批行**:只在前者里的行会一路走到代码生成才失败,
  且错误信息谈的是 flag 而不是缺行;只在后者里的行是读起来像支持的死数据。
  实测删掉 `kTable` 任意一行,新增的两条测试立刻转红。

- **三元组的 env 段在每个平台上命名不同的轴,而报告此前只是对此保持沉默。**

  ```
  Target x86_64-windows-gnu → x86_64-w64-windows-gnu
         c-abi   musl   (openkal-musl@0.3.3, graph)
  ```

  沉默作为**诊断**是对的 —— 在 Windows 上报「名字请求了 `gnu` C ABI」
  会在每一次合法的 MinGW 构建上出现,而且说的是错的。作为**报告**则不够:
  读者在其中找不到一行叫 `gnu`,于是把它映到最像 C 库名字的那一行。

  于是在该段不命名 C 库的平台上,报告直接说出它选中的是什么:

  ```
  Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
  ```

  **它不对应报告里的任何一行,而这正是要点。** 对这次构建的产物实测:

  | 观测 | 值 |
  |---|---|
  | 导入的库 | `ntdll`、`KERNEL32`、`SHELL32` —— 无 `msvcrt`,无 `ucrtbase` |
  | Itanium 修饰符号(`_Z…`) | 4507 |
  | MSVC 修饰符号(`?…`) | 0 |

  第一行说明 `c-abi musl` 是老实的:MinGW 的 C 运行时一点没链进来。
  五层记录的是每一层**由谁供给**;`gnu` 命名的是这些**对象遵循哪套约定**。
  把它读成 `c++-abi libc++` 是第二个错误答案 —— libstdc++ 同样坐在
  Itanium ABI 上。因此报告命名那套 **ABI 本身**,它的名字不出现在任何一行里,
  于是不会被误当成某一层。

  名词取自**值**而非仅取自轴:`gnu` 与 `msvc` 同轴而选中相反的 ABI,
  按轴取名会给 MSVC 构建打印「Itanium」—— 那不只是含糊,是假的。

  内部把 `envNamesCAbi` 这个布尔换成 `EnvAxis`,因为布尔是对事实的有损编码:
  该段在 Linux 上是 C 库、Windows 上是对象 ABI、无操作系统时是对象格式。
  一个只回答「是否为第一种」的布尔,能压住错误的警告,却给不出正确的名字。

  该提示**不出现**在 C 库来自载荷时:那种情况下 C 库正是三元组选中的,
  `gnu → ucrt` 是可见的因果,加注就成了每次普通 Windows 构建上的噪声。

### 文档

- 新增[第 16 章 —— 目标三元组](docs/16-the-target-triple.md)(中英双份):
  三段各是什么、第三段在每个平台上命名的是不同的轴、两种体系的分界、
  以及该用哪种拼法。

## [2026.8.24.4] — 2026-08-24

### 修复

- **`mcpp run --target X` 把 X 的构建记进了宿主的槽。**

  损害不在这条命令 —— 它构建得完全正确 —— 而在下一条:

  ```
  $ mcpp run --target riscv64-none-elf        # 正确,经 qemu 跑起来
  $ mcpp run
       Running `target/riscv64-none-elf/…/bin/openkal-same-source`
  exit=1
  ```

  缓存文件里逐字可见:一次 riscv 交叉构建写下的键是 `[target=]`。空键的
  含义是「为这台机器构建」,而这正是快路径允许自己**直接 exec 缓存产物**
  的唯一依据。裸的 `mcpp run` 命中它,跳过整个 prepare_build —— 因此既没有
  构建宿主目标,也没有解析 runner —— 把另一个目标的二进制在本机执行了。

  真因是一个丢掉的实参:`build_run_target` 收到 `target_triple`、用它建好
  overrides 并正确交叉构建,唯独没把它传给写缓存的那一步。`mcpp build` 的
  两个调用点都传了。

  同一个缺陷有**两条到达路径**:目标写在清单的 `[build] target` 里(已守),
  以及目标来自 `--target` flag(本次)。修好一条不会暴露另一条。

  回归测试的判据是**路径而不是退出码**。异架构产物 exec 会失败,所以
  只看退出码的测试在那里因错误的理由通过,**而在同架构上完全测不到** ——
  后者更危险:用户不会看到崩溃,只会拿到一个 musl 产物冒充宿主产物。

## [2026.8.24.3] — 2026-08-24

### 修复

- **三元组是请求,而解析把「未指定」抹掉了。**

  ```
  $ mcpp build --target x86_64-linux          # 我写的是「不指定 C 库」
        Target x86_64-linux-gnu → x86_64-unknown-linux-gnu     ← 被改写
               c-abi   musl   (openkal-musl@0.3.3, graph)      ← 名字自相矛盾
  ```

  三元组同时充当**身份**(输出目录、缓存键、`cfg()` 的主语)与**请求**。
  身份必须是全的,请求必须能说「没指定」;`parse` 用自动填充让身份变全,
  代价是请求消失 —— 两态在下游不可区分。

  修法是窄的:保留填充,另记 `Triple::envExplicit`。而请求**必须在规范化
  之前捕获** —— `str()` 渲染的是填好的身份,之后再 parse 就分不出来了。

  未指定 ⇒ 报告显示工程写的那个拼写;写了且与图矛盾 ⇒ **拒绝**。

- **目标行的约定在图之前就被应用,而它要回答的问题在图之后才有答案。**

  `x86_64-linux-musl → gcc@16.1.0` 说的不是「偏好 gcc」,是「musl-gcc 载荷
  供给这个目标的 C 库」。工程的 C 库若来自依赖图,该载荷根本不被使用。

  早决定被**双向实测**否掉:无条件应用会替换用户用 `mcpp toolchain default`
  设下的工具链;不应用会让一个零依赖的交叉构建从可用变为不可用。

  判据换成它本来就该是的那个:**图供给 `kernel-abi` 或 `c-abi` 时,约定不适用。**
  工具链解析因此移到依赖解析之后。

  代码不搬,只搬执行时机 —— 原地包成 lambda,在图已知处调用。
  先前记录的「39 处读写挡着」是**没测就写下的**:实测依赖解析段读 `tc` 仅 1 处,
  而那一处要的是三元组不是编译器。

  实测三格:

  | 场景 | 结果 |
  |---|---|
  | openkal 工程 + 全局 llvm 默认 + `--target x86_64-linux-musl` | `Resolved llvm@22.1.8` |
  | 无依赖工程 + 同一目标 | `Resolved gcc@16.1.0`(约定生效)|
  | 无依赖工程 + `x86_64-windows-gnu` | `Resolved gcc@16.1.0` |

## [2026.8.24.2] — 2026-08-24

### 新增

- **目标侧是五层,而其中最大的一层此前没有名字。**

  实测 `openkal-llvm-runtime` 编译的 729 个对象里,**498 个是 compiler-rt 的
  builtins**、21 个是 libunwind 的。这个包最大的一块此前声明在 `mcpp:c++-abi`
  名下 —— 而 `__udivti3` 及其同类是一个**纯 C 程序**需要的东西,与 C++ 无关。

  把 builtins 算作 C++ 运行时的一部分,与 `targetside` 模块开头记录的缺陷同形:
  一个交叉到 macOS 的 C 程序被问「有没有 C++ 运行时」,答「没有」,链接行因而
  保留了载荷自带的 libc++。**一个只有部分程序需要的层仍然是层。**

  五层为 `compiler` / `compiler-runtime` / `kernel-abi` / `c-abi` / `c++-abi`。
  `compiler` 是唯一一个包不能供给的层 —— 族与族之间的差异(flag 拼写、模块模型、
  BMI 格式、驱动 cfg)是引擎必须持有的事实,不是数据能描述的。

  `compiler` 层上报**族名**(`llvm`)而非驱动名(`clang`):使用者书写的每一处
  都用族名,报告用驱动名会让 `requires = ["mcpp:compiler=llvm"]` 永远不可满足。

- **`requires = ["mcpp:<层>=<实现>"]` —— 在引擎里不出现实现名的前提下执行分层规则。**

  libc++ 的源码由 clang 编译;递给 gcc 的实测结果是
  `fatal error: __config: No such file or directory` —— 一个命名了读者从未打开过的
  文件、且不提任何 mcpp 决定的消息。在引擎里写
  `if (stdlib == "libc++" && compiler == gcc)` 会把两个实现名放进引擎;
  写在包里,引擎只需检查一条它能一般性陈述的关系。

  检查在**编译开始之前**运行,这正是声明它的全部意义。

- **规则一:每层恰好一个供给者。**

  此前两个包供给同一层时,**图遍历顺序里第一个静默胜出** —— 那个顺序既不是
  作者写的,也不是他能预测的 —— 而落选者的 `[build]` 段仍然进入命令行。
  判据是失败模态:选错不会让链接失败,会得到一个能跑、偶尔崩的程序。
  `[build] runner` 早已按同一条规则处理。

- **`docs/14-target-side.md`(中英)与 `docs/spec/target-side.md`(SPEC-002)。**

### 变更

- **`std-module` / `std-compat-module` / `std-module-flags` 移入 `[build]`。**

  模块源是这个包的一个翻译单元 —— 引擎自己的注释早就这么写。放在 `[package]` 下
  损失的恰恰是位置所决定的那件事:`[build]` 可条件化而 `[package]` 不可,于是
  一个在多种 C 库之上供给同一 C++ 运行时的包无法为不同 C 库给出不同 flag。
  `-D_GNU_SOURCE` 对 musl 与 glibc 是对的,对 picolibc 是错的。
  `[package]` 写法保留为别名。

- **报告按需暴露。** 零配置构建的五个层全部来自同一份载荷,五行 `(payload)`
  回答的是无人提出的问题。默认只列出来源不是编译器载荷的层;`MCPP_VERBOSE=1`
  列出全部;**诊断始终列出它所依据的每一层**。

- **`Family` 去掉 `OpenkalLlvm`,拼写归一为 `llvm`。**

  保留枚举项的代价不止一条死分支:可用工具链列表按族枚举,一份载荷挂在两个
  族名下就出现两次,而安装状态按族记录 ⇒ **第二份被报成未安装,并被推荐给已经
  装了它的人。**

- **词表 pin 替换用户默认时,状态行说出替换与修法;图已知后指出这次替换本可不必。**

  让全局默认压过词表 pin 的做法被实测否掉:一个无依赖的工程、全局默认
  `llvm@22.1.8`、`--target x86_64-windows-gnu`,**从能构建变成不能构建**。
  行所 pin 的不是「偏好的编译器」而是「供给该目标 C 库的载荷」。
  结构性修法(把 pin 的决定与目标侧一样后移)未在本版落地:`tc` 在解析后到图之间
  被读写 39 处。

### 兼容性

- `hosted-standard-library` 继续表示 C++ 层;
- `openkal-llvm` 拼写继续解析;
- `[package]` 下三个 std-module 键继续被接受;
- 实测:**旧引擎(2026.8.24.1)读带 `requires` 的清单构建成功** ——
  TOML 侧忽略未知键,xpkg 侧警告而非报错。已发布的包因此可以先行声明。

## [2026.8.20.2] — 2026-08-20

### 新增

- **`[target.<triple>].sysroot` —— 目标的 C 库可按工程覆盖,并由此有了零 libc 档。**

  目标表把一份 C 库绑在每个 triple 上,而三条互不相干的需求都想和它分歧:内核要
  一份都不要、厂商 SDK 上的工程要换成 newlib、`std-freestanding` 坐到 openkal 的
  C 库上也要换。**一个旋钮解锁三条线。**

  字段是可选值而不是字符串,因为**缺席与空是两个不同的答案**:缺席继承目标表行,
  `sysroot = ""` 是零 libc 档。用普通字符串两者不可区分,而空串正是没有 sysroot 的
  目标行本来的样子 —— 内核工程会静默地把 picolibc 拿回去。

  实测两侧:不覆盖时 picolibc 头可用;`sysroot = ""` 后 `'stdio.h' file not found`;
  零 libc 自包含镜像 **`text 108`**,在 qemu 中打印 `zero-libc ok`。

- **三个目标查询,解除板级包对编译器与 C 库的隐式耦合。**

  `mcpp::target_builtins_lib()` / `mcpp::target_libc_profile()` /
  `mcpp::target_libc()`。

  这层耦合**在任何 manifest 里都看不见**:`riscv-virt-rt` 既不声明 LLVM 也不
  声明 picolibc,却依然服务不了第二种工具链或第二份 C 库,因为
  `clang_rt.builtins-riscv64` 与 `rv64gc/lp64d` 写进了它的 build.mcpp。**声明出来的
  依赖可评审;写死的名字只在换东西时才失败。**

  判据不是「能编过」(只有一种取值时永远成立),而是板级包代码里两个字面量**归零**
  且 rv32/rv64 双档仍通过。

- **裸机上缺 `operator new` 时的具名诊断。**

  裸机工程一用 `std::vector`,链接就死在标准库深处某个头文件里的 mangled 符号上,
  而消息里没有一处说明哪个包提供它。现在追加说明,并给出激活 feature 的写法。
  该建议**不带版本字面量** —— 它点名的是包与 feature,因此跨该包的每个版本都成立。

### 测试

- **e2e 机器校验 `import std` 诊断里那条可粘贴的依赖行。**

  同一个缺陷发过两次(先是包不存在,后是版本过期),而两次的修法都是「改字面量
  + 加注释」;第二次发生时,第一次留下的注释就在断掉的那一行正上方。**注释强制不了
  跨仓库不变量。**

  新守卫不写版本号,而是触发诊断、从输出里抠出那一行、原样粘进 manifest、再构建。
  revert-A 探针确认它对**历史缺陷本身**变红。

### 生态

- `mcpplibs/std-freestanding` 0.3.0 —— `alloc` / `alloc-kal` / `alloc-libc` feature。
- `mcpplibs/std-freestanding-alloc-kal` 0.1.0、`-alloc-libc` 0.1.0、`-nolibc` 0.1.0(新)。
- `mcpplibs/riscv-virt-rt` 0.4.0 —— 解耦、openkal 裸机后端、零 libc 内核模板。

## [2026.8.20.1] — 2026-08-20

### 修复

- **`mcpp pack` 打出的 `kind = "shared"` 库带走了构建机,产物在别人机器上起不来
  (#460)。**

  打包只是把链接产物 `copy_file` 进包里,于是 `.so` 保留了链接期的
  `DT_RUNPATH`——一串指向**构建机** `~/.mcpp/` 的绝对路径。消费者在另一台机器上
  拿到的是 `libstdc++.so.6: cannot open shared object file`。

  **issue 里建议的 `$ORIGIN` 修不好它,空串也修不好。** 在真实包 + 真实消费者上
  实测(把构建机 store 变成不可达):

  | 发货 `.so` 上的状态 | 消费方 `DT_RPATH` 被继承 | 结果 |
  |---|---|---|
  | 失效绝对路径的 `DT_RUNPATH`(此前的行为) | 否 | rc=127 |
  | **没有这条 tag** | **是** | ok |
  | `DT_RUNPATH = $ORIGIN` | 否 | rc=127 |
  | `DT_RUNPATH = ""` | 否 | rc=127 |

  关掉继承的是这条 tag 的**存在**而不是内容,所以判据是「tag 不存在」。删掉它也
  不是妥协:消费方自己的 `DT_RPATH` 是同一个闭包,只不过是在真正要运行它的机器上
  解析的。

  实现是**进程内改写 `PT_DYNAMIC`**(删槽、后移、补 `DT_NULL`,文件尺寸不变),
  不是调 patchelf——库打包支持 `--target` 且没有宿主门,非 Linux 宿主上
  `sandbox_patchelf` 会解析为空,照抄应用侧的 `if (!patchelf.empty())` 等于把这个
  缺陷留给一半的宿主。新增 `mcpp.pack.relocate`,覆盖 ELF32/64 × 大小端(单测),
  Mach-O 只读报告 `LC_RPATH`。

- **`mcpp pack` 一个 Mach-O 程序会**执行用户的程序**,然后报告 `Packed`。**

  非 PE 路径靠 `LD_TRACE_LOADED_OBJECTS=1 '<binary>'` 向动态链接器要依赖表,而这个
  变量是 glibc 的;dyld 不认它,于是那条命令在 macOS 上就是把程序跑起来,程序的
  输出被当成依赖表解析(解析出零条),然后写出一个只含二进制的包。有副作用的程序
  会把副作用做一遍,交互式的会把打包器挂住。现在按**产物格式**(而非宿主)拒绝,
  理由与旁边那条 `_WIN32` 拒绝完全同源。macOS 上 `kind = "lib"` / `"shared"` 照常
  打包——库打包从不运行产物。

  为什么此前没人发现:e2e 的 `pack` 能力 = `elf` + `patchelf`,只有 `Linux)` 分支
  给,所以应用打包在 macOS 上**一条 e2e 都没跑过**。

- **共享库包的 SONAME 别名在符号链接失败时会拷到未处理的原始产物。**

  别名的 copy 回退读的是 `leg.artifact`(构建树里的文件)而不是暂存后的 `dst`。
  在没有重定位/strip 之前两者逐字节相同,所以看不出来;之后它会在
  `create_symlink` 失败的机器上,用装载器真正要打开的那个名字,发出一份未重定位、
  未 strip 的库。

### 变更

- **`mcpp pack` 默认走 release 并 strip 发货产物。**

  此前两个打包器发的都是 dev 构建:未 strip、带着发布者的绝对源码路径。现在
  profile 的**兜底**从 `dev` 改为 `release`——其余优先级不变(`--profile` >
  `[build] default-profile` > 兜底),所以声明过 profile 的工程仍然拿到它声明的
  那个。

  剥什么取决于产物**是什么**,用的是 dh_strip 的分档:

  | 产物 | 参数 | 为什么不能更狠 |
  |---|---|---|
  | 可执行文件 | `--strip-all` | 没有人链接它 |
  | 共享库 | `--strip-unneeded` | 保留 `.dynsym`——那**就是**导出表 |
  | 静态归档 | `--strip-debug --enable-deterministic-archives` | `--strip-all` 会删掉归档的**符号索引**,消费方链接时报 `archive has no index; run ranlib to add one`(实测) |

  被捆绑进 bundle 的第三方 `.so` **不**剥——它们不是 mcpp 构建的。

  新增 `--profile` / `--no-strip` / `--debug-symbols <DIR>` 与 `[pack] strip`、
  `[pack] debug_symbols`。`--debug-symbols` 是分离而不是丢弃:写出
  `<dir>/<triple>/<产物>.debug` 并给发货产物加 `.gnu_debuglink`——**按 triple 分目录**,
  因为 fat 包的各条 leg 产物同名(`gnu` 与 `musl` 两条腿都叫
  `libmathkit-shared.so` 是常态),扁平布局会让后一条覆盖前一条,而前一个产物的
  `.gnu_debuglink` 会静默指向另一个目标的符号。

  **一条要知道的后果**:裸 `mcpp build` 与裸 `mcpp pack` 现在写进**不同的**
  `target/<triple>/<fingerprint>/` 目录(指纹把 profile 算进去了)。手工放到构建
  产物旁边的文件只在两条命令解析到同一个 profile 时才被 `pack` 看见;声明式通道
  (`[runtime] deploy_files`、`runtime_search_dirs`)不受影响。e2e 240 因此在
  fixture 里显式写了 `[build] default-profile`。

  > `[pack] strip` 与 `[profile.<name>].strip` 是两个决定:后者给**链接**加 `-s`
  > (碰不到静态归档,也分离不出任何东西),前者管**包里带什么**。

### 内部

- `mcpp::toolchain::binutils_tool(tc, name)`:四个工具链家族对同一个 binutils 工具
  的四种拼法,此前只有 `ar` 知道。`archive_tool` 现在由它表达(MSVC 的 `lib.exe`
  仍是特例,因为它不是 binutils 的名字)。
- `tests/e2e/_elf_tag.sh`:215 与新增的 264 共用同一个 ELF 读取器,两份拷贝会变成
  「构建机路径」的两个定义。
- **判据只查动态段,不查文件字节**:重定位删的是条目,字符串留在 `.dynstr`
  (`patchelf --remove-rpath` 实测残留完全相同,`.dynstr` 有尾部合并,删不安全)。
  一个 `grep` 式的判据会把正确重定位的产物报成脏的,而且会在 strip 落地那天因为
  另一个原因变绿——两次都不是因为重定位。

## [2026.8.18.3] — 2026-08-18

### 新增

- **原生 `cl.exe` 可以消费打包库了 —— 而且这条此前就已可用,是文档没跟上。**

  机制(方言中立的 `[target.<pred>.runtime]`)在 2026.8.18.2 就落地了,而
  docs/12 的边界表仍写着 ,同一份文档的正文却在描述解决方案。现在两端都验证了:
  渲染侧有可移植单测,端到端的 e2e **消费方钉死 `msvc@system`** —— 这一点是判据:
  若中立形式被忽略而 `ldflags` 生效,clang 消费者**照样能过**,只有 cl 会因为
  一个 `-L` 失败,所以只有它能证明这件事。e2e 还断言生成的图里**没有**该腿的 `-L`。

- **「一个 flag 由哪根轴决定」写进了 docs/08 §7.5。**

  2026.8.18 那一轮改的四个 flag 分属**三根不同的轴**(目标格式 / 目标 ABI / 方言),
  而每次挂错的表现都相同:**在恰好一个平台上莫名其妙地失败**,报错既不点名那个
  flag,也不点名它背后的决定。

  其中一条是反直觉的:`-L` vs `/LIBPATH:` **按方言判才是对的** ——
  它交给的是 mcpp 直接调用的那个程序,而不是链接器。面向 MSVC ABI 的 clang
  是同时区分这三根轴的反例:它说 GNU 方言、产 MSVC ABI 对象、出 PE 映像。

### 修复

- **lib root 约定在**所有**调用点跟随已声明的扩展名。**

  上一轮只修了打包器用的那个解析器,另外两个调用点仍是非探测版本 ——
  **而「修了主路径」不等于「修了这个决定」**:

  - `validate.cppm`:`.ixx` 工程每次构建都收到**虚假警告**。控制对照(用已发布的
    2026.8.18.2 二进制跑同一工程):
    `warning: src/mathkit.cppm: lib target without conventional lib root`;
  - `prepare.cppm`:接口是 `.ixx` 的 host-module 依赖被交出一个指向不存在文件的路径。

  e2e 263 **按调用点各钉一条**,并带负向对照(真的缺 lib root 时仍须告警),
  否则这条测试对「验证器干脆不检查了」也会通过。

- **host-module 依赖不再收到虚假的 `module_extensions` 死条目告警。**
  这类依赖的源码 glob 是**被刻意清空**的(那正是把构建规则挡在消费者二进制之外的
  机制),于是它声明的每个扩展名都显得是死的 —— 规则包作者会在每个消费者的构建里
  看到一条关于自己**正确** manifest 的告警,而且无从修起。

### 文档

- docs/05 增加一条可引用的规则:**`sources` 匹配到的每一项都必须产出会被链接的对象**
  —— 让 2026.8.18.2 引入的硬失败有出处,而不是凭空多出一条禁令。
  中文版此前连 `sources = []` 那条注记都没有,一并补齐。
- docs/12 的边界表改为 (中英);MSVC **数据符号仍需 `dllimport`** 这条限制
  由 e2e 258 **做成可复现对照**,不再只是散文 —— 断言钉的是「有/无 `dllimport`
  行为不同」,而不是某条随工具链版本变化的报错文本。

## [2026.8.18.2] — 2026-08-18

### 新增

- **`kind = "shared"` 在 MSVC ABI 上可用了 —— mcpp 自己生成 `.def`。**

  MSVC 在没有 `__declspec(dllexport)`、也没有 `.def` 时,DLL 什么都不导出;
  导入库为空,消费者拿到一堆 unresolved externals,而符号明明就在对象里。
  **拒绝的理由成立,结论不成立** —— CMake 的 `WINDOWS_EXPORT_ALL_SYMBOLS` 自 3.4
  起就是这么做的,而且它的 `bindexplib` **直接读 COFF、不依赖 dumpbin**。这一点是
  决定性的:`dumpbin` 只在 Visual Studio 开发者环境里,而 mcpp 在 Windows 的默认
  工具链是 clang,`mcpp build` 根本不在那个环境里。

  `mcpp.build.coff_exports` 是那个读取器,写成**对字节的纯函数**,于是它能在任何
  平台上被测试 —— 16 条单测逐字节构造对象(那是唯一能按需改变存储类的办法),
  外加一个**真实的 mingw-cross 对象**:只喂自己测试输出的读取器,只会与自己一致。
  超过 **65535** 个可导出符号时**拒绝而不截断**:被截断的导出表能干净链完,
  然后在「恰好需要那个掉出去的符号」的消费者那里失败。

  **标注优先。** 对象里已带 `/EXPORT:` 指令(即 `__declspec(dllexport)` 的产物)时,
  mcpp 让开、不生成任何东西 —— 再加一份列表会把同名符号导出两次(`LNK4197`),
  更糟的是把其余所有符号也导出,用「全部」替换掉作者选定的公开面。
  **这件事靠检测而不是配置**:为「我标注过了」加一个 manifest 键,就是给对象已经
  说过的事再加一个说法,而两者可以不一致。

  仍有两条限制是工具消不掉的(与 CMake 记录的同两条):导出的**数据**在消费端仍需
  `__declspec(dllimport)`;**vtable 被引用的类**要整类标注。两者都写进了 docs/12。

- **打包库可以被原生 `cl.exe` 消费。** 生成的 manifest 现在同时带方言中立的
  `[target.<pred>.runtime]`(`link_library_dirs` / `libraries`),mcpp 会按 target
  渲染成 `/LIBPATH:` + `<n>.lib` 或 `-L` + `-l<n>`。**两种拼写都带**:旧版 mcpp
  只读 `ldflags` 并静默忽略新段,去掉它会让旧客户端一个 flag 都拿不到;
  而新版读到中立形式时**忽略**同腿的 `ldflags` 而不是叠加 —— 叠加会把 `-L`
  送回 `cl` 的命令行。

### 修复

- **`mcpp pack` 现在跟着工程的 `module_extensions` 走,不需要再配一次。**

  实测:接口是 `.ixx` 的库打出来的包是**静默错的** ——

  

  两半都错且都不出声:没有接口,消费者 `import` 不了;而**空的发布集合正是打包器
  判定「C 表面」的依据**,于是这个包同时不再约束 C++ ABI,兼容性闸门也不再检查
  编译器与标准库。真因是 lib root 约定把 `.cppm` 写死了。现在按**已声明的每个扩展名**
  各给一个候选并取存在的那个;生成的 manifest 也会**自己声明** `module_extensions`
  —— 从**发布的文件**算出来,因此不可能与 `sources` 不一致。

- **`sources` 命中却分类不出角色的文件,现在当场拒绝。**

  未声明的 `.ixx` 过去会编译出一个**没人链接的对象**,报错是
  `undefined reference to mk::answer@mathkit()` —— 既不点名扩展名也不点名那条键。
  根因是「这是不是模块接口」有**两个答题者**:扫描器读到 `export module` 记下
  `provides`(所以边上挂了 `bmi_out`),而分类器说 `Other`,链接集合只读后者。

  这是**行为变化**:`sources` 里混进 `.md` / `.txt` 的工程会开始报错。

## [2026.8.18.1] — 2026-08-18

### 新增

- **`mcpp pack <target>` 可以把一个库打成「接口 + 预编译二进制」的包(#433)。**

  闭源库、离线环境、以及「构建农场已经编过一遍了」这三种场景,过去都只能自己
  写脚本收集产物。现在:

  ```bash
  mcpp pack mathkit                              # 静态库包
  mcpp pack mathkit --target x86_64-linux-gnu \
                    --target aarch64-linux-gnu   # 一个包,两条腿
  ```

  **产出的是一个普通的 mcpp 包** —— 一份正常的 `mcpp.toml`,走 mcpp 早就有的
  「载荷自带 manifest」通路。**新增 manifest 段 0 个、键 0 个**:打什么由
  `[targets.<n>].kind` 决定(所以没有 `--lib`、没有 `--artifact`),发布哪些接口
  由 `[lib]` 约定 + 模块图决定,公开头是 `[build].include_dirs` 全量,
  每条腿的 ABI tag 与 digest 记在既有的 `[[runtime.artifacts]]` 上。
  一个**老版本 mcpp 照样能构建**这种包 —— 它只是不执行下面那两道闸门。

  一个包可以同时带**两种接口**:`include/`(文本,`#include`,不编译)与
  `interface/`(模块,消费者编译它)。实测同一个包被「只 #include」/「只 import」/
  「两者都用」三种方式消费,静态与动态两种形态,六格全过。

  发布哪些 `.cppm` 是**算出来的** —— lib root 的模块闭包,不是「所有 `.m.o`」。
  实现分区(`module M:secret;`)照样产 `.m.o`,按扩展名挑会**泄露闭源源码**;
  同一个闭包反过来决定归档里要删哪些对象,按 `.m.o` 删则会删掉真代码、
  三个平台全部链接失败。两条清单都会打印出来。

  详见 `docs/12-binary-distribution.md`、`examples/05-lib-distribution`。

- **`kind = "shared"` 不再只有 Linux:PE/MinGW 与 Mach-O 都能产、能打包、能跑。**

  过去这条路只在 ELF 上验证过,其余一律拒绝。**这是能力的增加,不是修一个洞** ——
  早前的提交信息把那道守卫描述成「在原生构建上失效」,那是**错的**:
  `tc.targetTriple` 由编译器的 `-dumpmachine` 填,原生构建上**非空**
  (实测 `resolution.json` 记的是 `x86_64-linux-gnu`),所以原生 macOS / 原生 Windows
  本来就被它拦住。

  真正缺的东西各不相同,而且都不是 flag 拼写:

  - **PE 缺导入库。** 一个 PE 共享库是**两个文件**:加载器打开的 `.dll`,以及
    链接器消费的桩归档。mcpp 只写了前者,消费者直接链 `.dll` —— mingw 的 ld 容忍这个,
    别的链接器都不容忍,于是**能用的那种情况把坏掉的那种遮住了**。现在链接边把导入库
    作为隐式输出声明出来,包里两个都带,生成的 manifest 指向导入库。
    另外 PE 可执行文件带 `-static`,而 `-static` 会让 ld 进入纯静态模式并拒绝导入库,
    报的是 `have you installed the static version of the mathkit library?` ——
    既没点 DLL 也没点 `-static`;所以那条 `-l` 之前要先 `-Wl,-Bdynamic`。
  - **Mach-O 缺 install name。** `.dylib` 记录的是**链接时的路径**,而原先只在声明了
    `soname` 时才发 `-install_name` —— 一旦放开 macOS,**每个没写 soname 的 `.dylib`
    都会把构建目录烙进去**:在打包机上完好,换个地方就 `image not found`。
    现在无条件发 `@rpath/<file>`。这个选择原先还是用宿主的 `#if defined(__APPLE__)`
    做的(在旧的拒绝之下不可达,但放开之后就会发错),现在按 target 决定,
    和 `target_output` 早就做的一样。
  - **PE/MSVC 仍然拒绝,但换了个理由,而且是真理由。** 不是链接器 ——
    `link /DLL /IMPLIB:` 一直都在规则表里。是**符号导出**:没有 `__declspec(dllexport)`
    或 `.def`,MSVC 的 DLL 什么都不导出 ⇒ 导入库是空的 ⇒ 消费者拿到一堆
    unresolved externals,而那些符号明明在对象里。产出这个比拒绝更糟。

- **`--target` 不能服务时直接拒绝,而不是悄悄按宿主构建。**

  实测(Linux):`mcpp build --target x86_64-windows-msvc` 解析到**原生 g++**、
  写进 `target/x86_64-linux-gnu/`、报告成功 —— 一个 ELF 被当成 Windows 构建交付。
  词表的 tier 说的是「mcpp 支持这个 target」,从来没说「这台机器能产出它」;
  后者是 `host_can_serve` 的问题,现在 `prepare.cppm` 会问它,并把
  **这台宿主能构建的清单**列进错误信息。逃生口保留:显式
  `[target.X] toolchain = "…"` 表示交叉链是你自己提供的,mcpp 的载荷矩阵无权否决。

- **`[package] platforms` 会与实际打出的腿对账(设计里承诺过、实现里没有)。**

  四种比较只有两种值得打印:**打了却没声明**永远可行动;**声明了却没打,
  且这台宿主本来能构建它**才可行动。正常发布流程是 CI 上每平台各跑一次 `mcpp pack`,
  所以 Linux runner 不产 macOS 腿不是遗漏、是每一次 —— **永远触发的告警会把真正该看的
  那条盖掉**。所以判据用的是 `host_can_serve`,与 `--target` 是同一个函数。

- **消费预编译包时的两道闸门。** 都是不检查就会静默出错的:

  **接口与二进制是否仍然配对。** 这条闸门存在是因为另一种结果被实测过:把随包
  接口里一个结构体的两个 `int` 成员互换 —— Itanium ABI 不 mangle 字段顺序 ——
  消费者**编译过、链接过、运行过、打印出交换后的错数据**,任何工具都没有一句诊断。

  **二进制是否为这套工具链所编。** 失配时诊断会**列出包里确实有哪些 tag** ——
  一句「找不到」会让人去找一个就在自己硬盘上的包。

  另外,在解开的分发包目录里直接 `mcpp build` 会被拒绝:那儿的 `interface/`
  是声明,定义在旁边的归档里,构建会产出一个几乎空的库然后报告成功。

### 修复

- **「谁也没判定过」这个状态过去被拼成了「是接口」,于是闭源实现分区会静默发布出去。**

  一个分区的源码能不能发布,取决于一个关键字:`export module M:api;` 可以走,
  `module M:impl;` 不能。而三条建图路径里有两条读不到它:
  `[scan_overrides."<glob>"]` 声明了文件提供哪些模块,**没有地方能说它是否 export**;
  P1689 的 `is-interface` 是可选键,mcpp 把它解析进了一个**从来没人读**的字段。

  两者都以 `providesInterface = true` 到达,而字段自己的注释把这叫「保守方向」,
  理由是「这个标志只会产生一条警告」。**这恰好说反了**:`true` 正是那个**不产生
  任何警告**的值 —— 于是用 `[scan_overrides]` 声明的实现分区被**一声不响地发布**。

  现在它是三态的,每条路径只说自己真的知道的事:文本扫描器读关键字并显式写 true/false;
  P1689 读取器把编译器的答案(**包括它的沉默**)原样带过来;`scan_overrides`
  **留空**,因为 schema 表达不了。未知会告警,而且和已知那条**说的是不同的话**。

- **实现分区(`module M:part;`)在 Windows 上构建不了,而根因在扫描器里。**

  `module M:part;` 与 `module M;` 共用一个拼写,却是两种不同的声明,而扫描器把
  它们当成了一种:前者被记成**「requires `M:part`、provides 空」** ——
  一个文件 requires 自己的名字。于是图里**没有**从「import 分区的单元」到
  「定义分区的单元」的边,构建顺序无约束:GCC 与 macOS clang 靠各自的依赖扫描
  兜住了,**Windows clang 以 `failed to read compiled module` 失败**。

  同一处还有第二半:`import :part;` 的解析读的是 `u.provides`,而实现单元
  (`module M;`)没有 provides ⇒ 它里面的 `import :secret;` 停留在字面的
  `:secret`,没有任何单元提供。两个平台都会刷的那条
  `module 'M:part' imported but not provided in this build` 就是这两件事的
  合并症状 —— **它读起来像一条提示,其实是病因**。

  实现分区在此之前**mcpp 里任何地方都没有测试覆盖**,是库分发的 e2e 第一次
  用到它才暴露出来。现在扫描器记 `provides = M:part` 并标 `providesInterface
  = false`;`import :part;` 按 TU 自己所属的模块名解析。

  **两处行为变化**:①两个文件声明同一个分区(`module m:p;` × 2)现在会被
  **拒绝并点名两个文件**,此前是静默接受 —— 那种程序本来就 ill-formed,
  但它是一条新的失败路径;②`sources = []` 从「等于不写」变成「什么都不编」,
  一个真写了 `sources = []` 又依赖默认 glob 的工程会发现产物变空(此前无法表达
  「什么都不编」,所以这种写法只可能是误解)。

- **`[target.'<三元组>'.build]` 在没有 `--target` 时从不命中。**

  同一个语句的两种拼写互相矛盾:`cfg(linux)` 在原生构建上命中,
  `[target.'x86_64-linux-gnu'.build]` 不命中。根因是 `matches()` 拿着原始的
  `--target` 字符串(原生构建下是空的)短路返回 false,而同一文件的
  `context_for()` 对 `cfg(...)` **回落到宿主三元组** —— 一个决定两处推导。
  `manifest/types.cppm` 的注释从写下起承诺的就是回落那一种。

  **形状是最坏的那种**:CI 传 `--target` 是绿的,开发者本机的 `mcpp build`
  静默丢掉那一段,失败在链接期出现、点的是符号而不是谓词。

  修法是**删掉第二个答题者**:解析后的三元组进 `cfgpred::Ctx`,`matches()`
  只有一个来源。

- **`sources = []` 与不写 `sources` 逐字节等价。**

  解析器在向量为空时一律填默认 glob,于是作者**没有任何写法**能表达
  「什么都不要编」。二进制分发需要这个:一个纯头文件的包不编译任何东西,
  而 `src/` 下任何遗留文件都会被扫进消费者的构建,并可能与预编译库里的符号
  重复定义。改成记录**键是否出现**(`BuildConfig::sourcesDeclared`),
  与 `XlingsConfig::subosDeclared` 同一个模式。


- **卸载后的清扫会波及**别的版本**,而那可能正在被另一个进程解压。**

  `sweep_parked_payloads` 原来把整个 family 目录扫一遍,把**任何**没有文件的
  版本目录删掉。但安装是**逐步**往版本目录里写文件的(所以 package_fetcher
  用标记文件而不是"目录在"来判断装完没有),于是**另一个正在解压的版本**在那
  短暂窗口里和"残骨架"长得一模一样 —— 共用一个 `MCPP_HOME` 的机器上
  (自托管 runner、共享开发机)两个 mcpp 进程同时跑是常态。

  `.trash-*` 照旧全扫(那个名字只有这段代码会写);"没有文件的骨架"这一条
  收窄成**只扫这条命令点名的那一个版本**。

- **`msvc_available_here()` 每次构建都要为每个已装 payload 起一次 cl.exe。**

  它只想知道"这儿有没有能用的 toolset",却走了完整的 `installation_at()`,
  那里面会跑一次 cl 拿 banner 来定版本。而这个判据在**每次构建**的 MSVC ABI
  门上都会被问到 —— 正好是装了多个 toolset 的机器最慢。

  `installation_at(..., identifyVersion=false)` 跳过 banner。**布局仍然只有
  一份实现** —— 不是再抄一遍路径拼接。

## [2026.8.16.3] — 2026-08-16

### 修复

- **macOS 上任何带 `build.mcpp` 的工程都构建不了(#437)。**

  同一个工具链、同一个环境,**主构建能过,host helper 不能过** ——
  这才是它是 bug 而不是"macOS 环境问题"的地方。

  主构建在 macOS 上**刻意**用 `-fuse-ld=lld`,`flags.cppm` 的注释原文就写着
  *"Xcode 15.4's ld aborting at launch on macos-14 CI when its libc++
  resolution was diverted"*。而 host helper 走 `hostflags.cppm` 的 trust-cfg
  分支,**返回空 link token**,于是用 Xcode 的 `/usr/bin/ld` —— 那个 ld 自己
  就是链 libc++ 的 Mach-O,跑在 payload 工具链设好的 `DYLD_*` 里,dyld 把它的
  libc++ 解析到 payload 那份,缺 `__ZdaPv`,**还没开始链接就 abort**。

  cfg 选的是 **runtime**,从来没选过**链接器**。所以"信任 cfg"是对的但不完整:
  继续信任它,同时在 macOS 上把链接器点名。lld 随做编译的那套工具链一起发布,
  不可能被解析到它没链过的 libc++ 上。

  根因链记在 `.agents/docs/2026-08-13-build-optimization-status.md` §9a-3
  —— 那份文档原来停在"没有继续猜,本机复现不了"。

- **Windows 上 `mcpp toolchain remove <msvc toolset>` 报 "Access is denied"。**

  `remove_all` 直接上,不清只读位、不重试、报错也不说是哪个文件。
  payload 是从 .vsix/.msi 解出来的,归档条目带只读属性;POSIX 只要**目录**
  可写就能 unlink 子项,所以这个问题在 Linux/macOS 上根本不出现。
  另外 `/Zi` 构建之后 mspdbsrv.exe 还会在 payload 里活几秒。

  两个原因表现完全一样(都是 "Access is denied"),所以两个都处理:
  先清可写位重试,再给活着的进程一个**有上限**的等待窗口(10 × 300ms)。
  报错现在会指出卡在哪个文件 —— 光一句 "Access is denied" 没法处理。

  这个诊断当场就派上用场了:它点名的是
  `bin\Hostx64\x64\Microsoft.VisualStudio.Telemetry.dll` —— 既不是只读位
  也不是 mspdbsrv,而是 cl.exe 拉起的后台遥测进程 `vctip.exe` 占着它。
  **占用者不是 mcpp 能删掉的东西**,真正的修复在 payload 那边
  (openxlings/xim-pkgindex#637 不再安装 vctip.exe);这边留下的是通用兜底
  和那句能读懂的报错。

  报错同时会说清楚:**失败的 remove 不是空操作** —— `remove_all` 会先删掉
  能删的,所以剩下的是一个有洞的工具链,得重来一次而不是接着用。

  修完 vctip 之后占用者换成了 `mspdbcore.dll` —— `/Zi` 构建拉起的
  **mspdbsrv.exe**,它构建完还活几十秒,而且就住在正要被删的 payload 里。
  等它不现实(等多久都是猜),所以改成**挪走**。

  第一版挪错了东西:去重命名 payload **目录**。Windows 允许重命名一个
  **打开着的文件**(更新器就是这么替换运行中的 .exe),但**不允许**重命名
  一个**含有**打开文件的目录 —— CI 当场否掉了这个前提。

  正确的形状是反过来的:把**文件**逐个挪到旁边的 `.trash-*`,再删掉此时
  只剩目录的那棵树。要是某个文件连挪都挪不动,就把 `.trash-*` 清掉、整体报失败
  —— 挪了一半的 payload 比原封不动更糟。

  最后一层:文件都挪走了,**空目录骨架**仍可能删不掉(sharing violation ——
  某个进程把它当工作目录,mspdbsrv 正是在 payload 里被拉起来的)。判据因此定为
  **「没有文件残留 = 已卸载」**:一个没有文件的工具链就是没装,这正是 `remove`
  承诺的事;报失败等于报了相反的事实。骨架在下一条生命周期命令里清扫。

  顺带修掉对称的那半句谎:`toolchain default` 原来只查目录**存在**,
  会把骨架当成装好的工具链交给构建。现在它问 `payload_frontend` 要一个
  **能解析出来的编译器** —— 和 `installed()`、`find_windows_sdk()` 同一条规则:
  **装好了 = 能用,不是"在那儿"**。

- **半装的 Windows SDK 被当成装好的,链接到最后才炸。**

  `find_windows_sdk()` 认一个根的条件是 `Include\<v>\ucrt\corecrt.h`
  存在 —— 只看头文件。而 SDK 是头文件**和**导入库两半。

  托管的 `xim:windows-sdk` payload 少了带 `kernel32.lib` 的那个 MSI 时,
  这个根照样"找到了",而且因为版本号更高,**排在机器自己那套完整 SDK 前面**。
  于是每个 TU 都编过了,一直到最后一步:

      LINK : fatal error LNK1104: cannot open file 'kernel32.lib'

  日志里没有任何一行提到 SDK。

  现在两半都要:`Include\<v>\ucrt\corecrt.h` 和
  `Lib\<v>\um\<arch>\kernel32.lib`。半装的根被跳过,搜索落到下一个,
  本来就能用的构建就能用了 —— 和 `has_usable_msvc()` 坚持"两半都要"是同一条
  理由,只是这次轮到 SDK 自己。

- **装好的 msvc toolset 在 `mcpp doctor` 里看不见,而 `toolchain list` 看得见。**

  doctor 还在用 #436 修掉的那个写法 —— `toolchain_frontend(<root>/bin, ...)`。
  cl.exe 在 `VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/`,深四层,`bin/` 下什么都没有
  → `continue` → 整个 toolset 被丢掉。#436 把 list 换成了 `payload_frontend`,
  doctor 没跟着换,于是**同一台机器上两个命令互相矛盾**,而且都"成功"。

  这是同一条布局规则的**第四份拷贝**。

- **`has_usable_msvc()` 把"这台机器有 VS"当成了"这里能用 MSVC"。**

  它只探测机器,却门控三处对**受管 toolset 同样适用**的决策(首次运行是否转向
  mingw、离线文案、MSVC ABI 修复门)。于是一台钉了 `msvc@14.44.35207`、
  没装 Visual Studio 的机器,判据回答 `false` —— 工具链明明就在 payload 里躺着,
  却被转去 mingw。

  新增 `msvc_available_here(pkgsDir)`:两条来源都算。原来那个保留,
  它回答的是另一个问题。

- **默认的 `/MD` 构建在只有受管 toolset 的干净 Windows 上跑不起来。**

  产物链 `vcruntime140.dll` / `msvcp140.dll`,这两个**不是** Windows 自带的
  (自带的是 `ucrtbase.dll`)。而全仓库搜不到一处 `Redist` / `vcruntime140` /
  `msvcp140` —— 我们把 `CRT.Redist.X64.base.vsix` 下载下来,一个字节都没用。

  CI 看不到,因为 runner 上装着 Visual Studio。

  新增 `vc_redist_dir()`,把 toolset 自带的那份 CRT 放进 `linkRuntimeDirs`
  —— Windows 上它就是 PATH,和 gcc 把私有 libstdc++ 放进 `LD_LIBRARY_PATH`
  是同一件事。5 个测试:能从编译器路径单独找到、**redist 版本号不等于 tools
  版本号**(14.44.35112 vs 14.44.35207,推导会找不到)、取最新、
  **`debug_nonredist` 永不返回**(不可再分发)、没有 redist 不算错误。

## [2026.8.16.2] — 2026-08-16

### 修复

- **装好的 msvc toolset 在 `mcpp toolchain list` 里不出现。**

  枚举问的是 `toolchain_frontend(root / "bin", pkg)`,而 cl.exe 在
  `VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/` —— 深四层。拿不到就 `continue`,
  于是每一个 msvc payload 都装得好好的、然后**看不见**。

  这个布局有三个地方需要知道:安装知道、构建知道、列表不知道 ——
  一条规则被内联抄了第三份时的典型结果。三者现在共用
  `payload_frontend(payloadRoot, pkg, family)`。

  `2026.8.16.1` 带着这条(tag 打在修复之前),其余部分不受影响 ——
  install / build / default / remove 都是好的,只有 list 少一行。

## [2026.8.16.1] — 2026-08-16

### 工具链

- **MSVC 不再是「唯一版本无法声明」的工具链。**

  gcc / llvm 由 mcpp 自己安装、按声明解析;MSVC 是体系里唯一的例外——**每一个**
  msvc spec 都是系统 spec,manifest 写了版本也会被丢掉。后果不是不够优雅,是
  **同一份源码在两台机器上会被不同的编译器编译,而且不报错**。

  xrgui#3 实测:同一轮 CI 里 mcpp 用 14.51、xmake 用 14.52,直到 14.51 触发
  ICE 才暴露。导出完整 vcvars 环境**无效**,最后只能在 CI 里把 `vswhere.exe`
  挪开,逼 mcpp 落到 `VSINSTALLDIR` 那一步。

  现在由 **spec 的版本轴**决定来源,两条来源并存:

  | spec | 来源 | 用哪个编译器 |
  |---|---|---|
  | `msvc@system`(或裸 `msvc`) | 机器自己的 Visual Studio | 这台机器上装的那个 |
  | `msvc@<toolset>`(如 `msvc@14.44.35207`) | mcpp 安装的 xlings payload | **声明的那个,每台机器都是** |

  `msvc@<toolset>` 与 `gcc@16.1.0` 在每个方面都同构:多版本共存、
  `toolchain remove msvc@<toolset>` 可卸载、manifest 里写了就自动安装。
  payload 自带编译器、STL,并通过 `xim:windows-sdk` 依赖带上 ucrt/um 头与库,
  机器上**什么都不必预装**。

  实现上,「获取」与「解析」被拆成两条正交的轴:获取与 gcc 共用一条
  (xim 安装),解析与 `msvc@system` 共用一条(`installation_from_tools_dir`)。
  所以受管 toolset 不是第二条代码路径,也就不会长出自己的 bug。

- **破坏性变更:`msvc@19.44` 不再是 pin-verify。**

  它过去表示「用系统 MSVC,并校验 banner 前缀」——而且只有
  `mcpp toolchain default` 会校验,**构建路径完全忽略它**。版本轴现在到处都表示
  toolset。写成 `19.x` 时,mcpp 会用这台机器自己的 cl 版本说清楚,并给出两个
  替代写法(`msvc@system` 或该机器实际的 toolset 版本)。

- **`VSINSTALLDIR` 现在优先于 vswhere 探测。**

  vswhere 在几乎每台开发机上都能返回点什么,于是 `VSINSTALLDIR` 事实上不可达:
  一次已经导出了完整 vcvars 环境的构建,仍然用 vswhere 排第一的那个编译器。
  **猜测不该压过答案。** 顺带给 vswhere 加了 `-prerelease`——没有它,只装了
  Insiders 的机器会被报告成「没有 MSVC」,而磁盘上明明有一个可用的 cl.exe。

  `VS*COMNTOOLS` 仍排在 vswhere **之后**:那是机器全局的残留(2017 的
  `VS150COMNTOOLS` 不该压过当前安装),而 `VSINSTALLDIR` 是有人为这个 shell
  设的。

- **Windows SDK 不再只认两个写死的绝对路径。**

  顺序改为:`WindowsSdkDir`(+ `WindowsSdkVersion`,vcvars 本来就导出这两个)
  → 受管 toolset 在 mcpp 自己 store 里的 `xim:windows-sdk` payload
  → 原来的绝对路径(降为回退)。

  第二条不需要任何配置:**编译器自己的路径就说明了它来自哪个 store**,
  SDK 是它在那里的邻居。所以 mcpp 里没有任何地方写死 SDK 版本。

### 接口一致性

- **`cxx_runtime = "self-contained"` 在 MSVC 上真的生效了。**

  过去两个旋钮只有一个管用:`linkage = "static"` **确实发** `/MT`,而
  `cxx_runtime = "self-contained"` 报「未实现」——对着同一个物理开关。
  两处注释也互相矛盾(`flags.cppm:605` 说发了 `/MT`,`distribution.cppm:202`
  说「根本没有 /MT」)。

  在 MSVC ABI 上这两条不是可以二选一的旋钮:`/MT` 把 C 运行时和 C++ 运行时
  从同一个库里链进来,**它们本来就是一个开关**。现在两种写法都选中它,由
  `msvc_wants_static_crt()` 统一推导——项目的 TU 与 std 模块问的是同一个函数,
  不再各写各的表达式(#422 正是这样分叉的)。

  默认仍是 `/MD`:判据取的是**manifest 里写下的字面值**,不是解析后的
  contract——后者对多数 role 默认就是 self-contained,拿它做判据会把每一个
  Windows 构建都翻成 `/MT`。

  MSVC 的 CRT 模型是**整个项目**的属性(一个项目只编一份 std 模块,cl 把
  `_MSVC_MT`/`_MSVC_MD` 烤进去),所以按 role 覆盖会被明确拒绝并说明原因,
  而不是在 ucrt 头文件里炸出 C5050/C2375。

### 测试

- msvc 的发现逻辑第一次可以在 Windows 之外测试:`installation_at()` 接受目录
  而不是去探测机器,`find_windows_sdk()` 接受 root 列表。6 个新单测在 Linux CI
  上跑真实的 fixture 目录树,包括「两个 toolset 都在,要老的那个」这条——
  「取最新」的实现会在这里失败。
- 新增 e2e `239_msvc_managed_toolset.sh`。它的每一条断言都写成
  **系统编译器来应答就会失败**:cl.exe 必须在 mcpp 的 store 里、toolset 目录
  必须是 spec 声明的那个、同一台机器上换回 `msvc@system` 必须仍然解析到系统
  的 cl(两条来源互不污染)。

## [2026.8.13.1] — 2026-08-13

### 性能

- **「接口没变就不级联重编」的机制从设计之日起从未生效过 —— 现已修复。**

  `cxx_module` 规则会保留上一份 BMI、重编、然后在内容相同时把旧文件换回去,让
  ninja 的 `restat` 判定输出未变、从而**不重建导入者**。这套机制 2026-05-12 就设计
  并实现了,判据是 `cmp -s`。

  但 GCC 把 wall-clock **写进了 BMI 的内容**:

  ```
  buildtime: 2026/08/12 02:25:01 UTC
  localtime: 2026/08/12 02:25:01 UTC
  ```

  同一份源码相隔一秒的两次编译,BMI 差**恰好 4 个字节** —— `cmp` 于是永远报「变了」,
  这条快路径**一次都没有走通过**。当年的设计说明只预见到 GCC 会重写文件(mtime 抖动)
  并据此开出内容比较的药方,没有预见到时间戳本身就是内容,所以药方按原样写出来就不
  可能生效。

  新增 `mcpp bmi-equal`(内部子命令,由 ninja 规则调用),比较时掩掉这两个字段。
  刻意**不用 `SOURCE_DATE_EPOCH`**:那会把整个编译的 epoch 钉死,从而改变**用户代码**里
  `__DATE__` / `__TIME__` 的展开结果;掩码只改变 mcpp 认为「什么算相等」,别的什么都不动。

  构造上保守:找不到预期字段、或两份文件对字段位置的判断不一致时,回落为严格比较 ——
  它可以把等价的 BMI 判成不同,但**绝不会**把不同的 BMI 判成相同。

  实测(用 `bench/` 套件测 mcpp 构建 mcpp 自身,touch 一个被 46 个模块导入、**内容未变**
  的文件):

  | 场景 | 2026.8.11.3 | 2026.8.12.1 |
  |---|---|---|
  | `noop` | 0.27s | 0.19s |
  | **`touch-hub`** | **73.99s** | **0.45s** |

  ~164×。正确性由单测从**两侧**钉死:只测「等价的判相等」会放过一个恒返回 true 的实现,
  而那比原缺陷更糟 —— 它会静默吞掉所有真实的级联。

### 新增

- **`bench/` —— 构建引擎基准套件(顶层目录,用 mcpp 自己写)。**

  起因是一次实测:mcpp 的自举构建**不是吞吐瓶颈,是延迟瓶颈** —— 关键路径 = 100%
  墙钟,后 55% 的时间里 32 个硬件线程上只有 1 个编译进程在跑;而这条关键路径上
  **77% 的时间在生产没有任何下游需要的 `.o`**(BMI 在编译进度 22.8% 处就原子落盘
  了)。同样的病理在 xlings(110 模块、独立作者)上完全复现,说明这是
  「C++23 命名模块 + GCC 单阶段 + 边完成即释放」的结构性结果,不是某一家的实现问题。

  详见 `.agents/docs/2026-08-12-modular-build-performance-deep-analysis.md`。

  上一轮用的是一次性 bash + hyperfine 脚本,有四个致命缺陷:只支持两个引擎、
  **Windows 上根本跑不了**、被测对象只有 mcpp 自己、结果格式随手加字段。新套件:

  - **跨平台**:C++23 写成、由 mcpp 构建,三平台同一套逻辑。平台差异按 xlings
    `src/platform/*.cppm` 的约定拆成**模块分区 + 整文件宏控** —— 非目标平台不导出
    任何符号,同名定义全局只有一份,编译期自动选中。`#if defined(_WIN32)` 只出现在
    两个分区里,runner / engines / protocol / fixture 全部零平台条件。
  - **协议先行**:`bench.protocol` 带 `protocol_version`,并把三条不变量写进类型 ——
    失败不得伪装成数据(非 ok 的格**没有** timing 字段,而不是 0)、跳过必须带原因
    (`unavailable` ≠ `failed`)、结果与宿主同生共死(含**异构 CPU 标记**:13900K 的
    32 线程不能当 32 个同构核读)。
  - **加一个引擎 = 加一个文件**:`bench.engines.Engine` + `registry.cppm` 一行。
    已接入 mcpp / mcpp-opt / cmake / xmake / meson / bazel。
  - **同一工程三种形态**:`headers` / `modules` / `modules-impl`,由生成器产出而非
    手写 —— 手写两份「等价」代码几乎必然在某处不等价,而那正是被测量的东西。
  - **`--analyze`**:同一个二进制还能剖析任意 ninja 构建目录(工作量 / makespan /
    关键路径 / 并发曲线),固化了五个会**反转结论**的解析陷阱。

  CI:`.github/workflows/bench.yml`,**仅手动触发**、覆盖 linux/macOS/windows、
  **不设性能阈值**(宿主方差远大于多数真实回归,把噪声变成红叉只会让人忽略它)。

### 说明

本版本不改变 mcpp 的构建行为;`bench/` 是独立工程,`mcpp build` 不受影响。
分析报告给出的优化方案(BMI 落盘即释放、BMI 时间戳归一、实现移出接口单元)
按收益/风险排序记录在文档中,尚未实施。

## [2026.8.11.3] — 2026-08-11

### 修复

- **回归:产物加载的库不是它链接的那一份 —— `$ORIGIN` 被 SubOS 库视图遮蔽。**

  `2026.8.11.2`(PR #413)首次把 SubOS 库视图(farm)写进产物的 `DT_RPATH`,但它
  落在 **`$ORIGIN` 之前**。于是 imgui/GLFW 应用链接的是 mcpp 从 `compat.x11` 源码
  构建、部署到产物目录的 `libX11.so`,运行期加载的却是 farm 里 xlings 装的
  `xim:libX11` —— **链接期用 A,运行期加载 B**,程序在 main 之前就死:

  ```
  undefined symbol: _ZNKSt13runtime_error4whatEv
  ```

  真因不是「放错了位置」,而是**一条链接命令行的顺序由两个互不知情的生产者用
  `+=` 决定**:`flags.cppm` 把 farm 拼进全局 ldflags(并注释「so it is LAST」),
  `plan.cppm` 把 `$ORIGIN` 拼进 per-unit,而每条链接规则渲染的是
  `$ldflags $unit_ldflags`。三处各自都对,合起来是错的。

  新增 `mcpp.build.link_line`:把 per-unit 尾部声明成**具名槽位**,相对顺序写在
  类型里、由单测钉死。新增一个生产者必须先选一个槽 —— 而"选"正是"在产物自己的
  目录之前还是之后"这个问题被提出来的地方。

- **共享库不再把自己的 C++ 运行时导出给别人(ELF)。**

  `SharedLibrary` 此前与可执行文件共用 `Distributable` 角色,于是拿到同一份
  self-contained 契约:`-static-libstdc++`。在 ELF 上这不是"私有一份" —— 只有一个
  全局符号命名空间,共享对象会导出它定义的每一个全局符号。一个**纯 C** 的 compat 包
  因此导出了 777 个 GLOBAL 标准库符号(`libXau.so`:39KB 的 Xau + 9.5MB 的 libstdc++)。

  可执行文件链接时 `-lX11` 排在驱动的 `-lstdc++` 之前,ld 就用它满足了
  `std::runtime_error::what()`,归档成员从不拉入 —— **可执行文件的
  `-static-libstdc++` 变成空操作,它的 C++ 运行时事实上是那个 `.so`**。上一条的
  库替换之所以致命,根源在这里。

  共享库默认契约改为**按目标格式分档**:ELF `toolchain-coupled`,
  Mach-O / PE 维持 `self-contained`(两者都没有这个危害 —— Mach-O 的机制本就是
  `-load_hidden`,PE 没有全局命名空间)。显式 `cxx_runtime = { shared = "…" }`
  仍可选回自包含,此时自动补 `-Wl,--exclude-libs`,让内嵌的运行时留在动态符号表之外。

  实测(helloegui,imgui + GLFW + X11):`libXau.so` 9.5MB → 39KB,
  `libX11.so` 导出 std 符号 2931 → 0,GUI 正常启动。

### 内部

- `dist::default_contract` 从**没有任何生产调用方**变成唯一真源:角色→契约的策略
  此前在 `flags.cppm` 被第二次推导,而这正是 `distribution.cppm` 开篇声讨的那类债
  (「used to be derived independently in five places」)换个位置复发。

- e2e 219 的断言由「farm 是最后一个**绝对路径**条目」收紧为「**字面**最后一项」,
  并补一条**行为**不变量(`LD_DEBUG=libs` 实测同名 SONAME 解析到 `$ORIGIN`)。
  旧断言把 `$ORIGIN` 过滤掉了,对坏顺序与好顺序给出同一个结论 —— 它在一个 farm
  并非最后的二进制上报告「farm is last」。测试工程也从 `int main()` 换成消费依赖
  共享库,否则它连 `$ORIGIN` 都不产生,整条断言链是空转的。

## [2026.8.11.2] — 2026-08-11

### 修复

- **回归:SubOS 没有自我描述时,`mcpp build` / `mcpp test` 直接失败
  ([xlings#543](https://github.com/openxlings/xlings/issues/543))。**

  Windows 上 xlings 不写 `subos_info` 块,而 mcpp 把「缺声明」当成了错误,于是
  **每一次构建都停在一条讲 GL 驱动的消息上** —— 在一台没有 ELF、没有 `PT_INTERP`、
  没有私有 libc 的机器上。`2026.8.10.2` 引入(PR #400),`2026.8.8.4` 正常。

  判据改为:**矛盾报错,缺席降级。** 点名的 SubOS 不存在仍是硬错误(该请求无法被
  满足);SubOS 存在但没描述自己则记 `declared=false` + 一条必被打印的 note,
  runtime 规则报 `inconclusive` 而不是给出判决,构建继续。

  同一位置还有第二颗雷:schema 检查是 `!=`,而它的**读取器**明写着「更高的 schema
  照读」。xlings 写出 schema 2 的那天,全平台所有构建会同时停摆。已改为上限语义 ——
  **发布数据不得使读它的程序失效**。

- **图形/系统库链得上却跑不起来:运行期搜索路径补上 SubOS 库视图。**

  mcpp 在编译与链接两条线上都发 `--sysroot=<subos>`,所以 `-lGL` 零 flag 就解析得到;
  但运行期的搜索路径是**另一套独立推导**(只有工具链载荷目录)。结果是
  `mcpp build` rc=0、`./bin/app` 报 `libGL.so.1: cannot open shared object file`。

  ```
  $ readelf -d bin/app | grep RPATH
  before:  [<store>/glibc/2.39/lib64 : <store>/gcc/16.1.0/lib64]
  after:   [<store>/glibc/2.39/lib64 : <store>/gcc/16.1.0/lib64 : <subos>/lib]
  ```

  新增 `mcpp.platform.runtime_search` 契约模块:一条搜索目录有**来源**
  (`payload` / `package` / `subos_farm` / `host_default`)、**次序**与**是否机器本地**。
  **次序 = 不可变性递减,farm 在最后** —— 载荷目录装一次不再动,`<subos>/lib` 每次
  `xlings install` 都重写;载荷在前,`libc`/`libstdc++` 永远从被 pin 的载荷解析。
  交叉目标与非 ELF 格式不发 farm。

- **`validation: pass` 曾对一个跑不起来的产物成立。**

  闭包解析在搜完 rpath 后**回落到宿主默认目录**,而宿主通常自带 `libGL.so.1` ⇒
  模型认为解析到了。但产物跑在**私有加载器**下,它的默认路径里没有宿主目录。
  现在宿主默认目录只在**非 hermetic** binding 下参与;hermetic 产物上一个谁都提供
  不了的 `DT_NEEDED` 是**可证的**失败(新判决 `unresolvable`),会让构建变红并指名。

  实测佐证(`LD_DEBUG=libs`):私有加载器的内建默认路径是 glibc 载荷**自己的构建期
  前缀**(`…/fromsource-x-glibc/2.39/lib`),这台机器上根本不存在;`/usr/lib` **从不**
  被查。因此 e2e `206` 里那条「安全的宿主 DSO 对照」此前报 `pass` 也是假绿 —— 它
  刻意不运行产物,而产物其实 127。已改为断言 `inconclusive` 并说明原因。

  **`[build] allow_host_libs` 同时退出两个阶段。** 它本就关掉链接期 hermeticity 检查;
  既然用户已声明「我有意伸到沙箱外」,mcpp 就不能再断言产物起不来(他们可能用
  `LD_LIBRARY_PATH` 跑,或装在私有加载器会看的地方)。⇒ 该档下未解析的 `NEEDED`
  报 `inconclusive` 并指名,而不是变红。一条声明,一个含义。

  另两处精度修正(CI 抓到的):`unresolved` 此前混装三种东西 ——「找不到的 SONAME」
  「读不了的对象」「512 上限」。只有第一种可证,故拆出 `unresolvedSonames`;
  且**产物的格式由产物决定,不由 binding 决定** —— Linux→Windows 交叉构建拿的是宿主
  binding(hermetic),产物却是 PE,「不是 ELF」落进 `unresolved` 后被判成「缺库」,
  让 `crosswin.exe` 构建失败。

### 变更

- xlings 强相关模块归入 `src/platform/xlings/`:`mcpp.platform.xlings`、
  `mcpp.platform.xlings.subos_info`、`mcpp.platform.xlings.runtime_selection`
  (命名空间不变)。`RuntimeBinding` 与 `runtime_search` 留在 `src/platform/` ——
  它们是 provider 中立的契约类型,不是 xlings 专属。
- `resolution.json` 的 `runtime.search` 增加 `closure` 数组(路径 + origin +
  machine_local,**保序**);`mcpp why runtime` 按加载器次序打印它。
- mcpp 为自己启动的每个进程声明 `XLINGS_SUBOS_LD_PATHS=0` —— xlings 链接器包装器
  路径注入的退出声明([xlings#540](https://github.com/openxlings/xlings/issues/540))。
  今天是无操作;它落地后 mcpp 的 DT_RPATH 仍只含 mcpp 自己决定的内容。
  **不读 `$XLINGS_SUBOS_LIB`**:实测它指向当前 shell 的 subos,而 mcpp 有自己的
  registry home,两者通常由**不同物理 glibc 载荷**支撑。
- 内带 xlings pin → `2026.8.11.2`。

## [2026.8.11.1] — 2026-08-11

### 新增

- **`[build] module_extensions` —— 哪些扩展名是模块接口,由工程声明。**

  ```toml
  [build]
  module_extensions = [".ixx", ".ccm"]
  ```

  追加到内置的 `.cppm`。声明一个扩展名会**同时**做三件事:默认 sources glob 跟着
  变宽(文件才能被**找到**)、这些单元走**模块**规则(产 BMI、`.o` 无条件进链接)、
  新鲜度快路径**扫描**它们(加 `import` 会让构建图作废)。一个键而不是三处配置。

  任何扩展名都接受,唯独拒绝已代表其他角色的(`.cpp` `.c` `.h` `.S` …)——
  manifest **错误**而非警告,因为宣称 `.c` 是模块接口会把 C 文件送进 C++ 模块规则,
  最终失败在一个既不提文件也不提这个键的地方。

  `.ccm`/`.cxxm`/`.ixx` **不进内置默认**。进了的话默认 glob 会跟着变宽,
  于是 `src/` 下躺着 vendored MSVC-only `.ixx` 的**已发布包会在一次 mcpp 升级后
  突然开始编译它** —— 而包作者改不了已经发出去的 tarball。

- **`[build] build_program_timeout` —— `build.mcpp` 的运行上限可配置。**

  优先级 `MCPP_BUILD_PROGRAM_TIMEOUT` > **该包自己的** manifest > 内置 600s,
  与 `macos_deployment_target` 同构。超时报错**点名要改的那份 `mcpp.toml`** ——
  依赖超时时改自己的那份不会有任何效果,这正是 [#410](https://github.com/mcpp-community/mcpp/issues/410)
  从外面看到的样子。不写这个键与写 `0` 不是一回事:不写=用默认上限,`0`=不设上限。

- **`mcpp self doctor` 报告构建策略。** 生效的模块接口扩展名表、生效的超时值
  **及其来源**、以及本平台的 deadline 是否**真的**强制执行。
  另外 `module_extensions` 里零命中的条目会告警 —— 否则打字错误(`.ixxx`)与
  「这个工程还没有」无法区分。

### 修复

- **超时上限在 Windows 上从来就是空操作。** `capture_exec_deadline` 只在 POSIX
  生效,其余平台直接回落到无界启动器 —— 于是 `mcpp test --timeout`、
  `--build-timeout`、以及这个新键在 Windows 上**设了等于没设**。现在两侧各有实现:
  Windows 把子进程放进 **Job 对象**并在到期时关闭它,杀掉的是**整棵进程树**而不只是
  直接子进程(否则一个还攥着捕获管道的孙进程会让杀掉之后的读取一直挂住)。

- **`.mm`(Objective-C++)的对象编了但永远不进链接** —— `is_implementation_source`
  的清单漏了它。

- **stage 一个含汇编的依赖会静默丢掉那些源文件** —— 兜底 glob 漏了全部三种汇编扩展名。

### 架构

- **「扩展名 → 角色」此前在 9 个文件 20 处推导,分成 8 份互不一致的清单**
  (三份「什么算实现单元」、四份「什么算源文件」)。
  [#272](https://github.com/mcpp-community/mcpp/pull/272) 修了链接侧,却漏了
  `pick_rule` —— 边上**声明**了 BMI 产物(那行读 `providesModule`),命令行却丢了
  `-fmodule-output=`。

  收敛的形状不是「大家都调同一个函数」,而是**分类只发生一次**(文件进图时),
  之后当数据传递(`SourceUnit::kind` → `CompileUnit::kind`)。扫描器手里本来就有
  所属包的 manifest,所以这一步没有新增任何管道。

- **mcpp 现在每次都显式告诉编译器某个单元是模块接口**(`-x c++` / `-x c++-module` /
  `/interface /TP`),而不是去维护「哪个驱动认哪个后缀」。实测(GCC 16.1 / Clang 22.1):
  **Clang 根本不认 `.ixx`** —— 把它当链接输入、警告、**退出码 0 且不产 BMI**;
  而显式旗标在已识别后缀上**幂等**(Clang 的 `.cppm` BMI 逐字节相同)。
  一张会过期、错了还静默的表不该存在。

- `src/platform/` 拆成 `unix/ windows/ linux/ macos/`;`mcpp.platform.process`
  对有界运行**单点 `if constexpr` 分派**,取代此前散落的平台分支。

### 兼容性

- 未配置时**构建图零差分**:同样的文件被编、同样的 BMI、同样的对象进链接、
  同样的指纹目录。
- `module_extensions` **进**指纹(它改图的形态);`build_program_timeout` **不进**
  (它不改任何一条边 —— 进了会让「抬高超时」重建全世界)。
- 旧版 mcpp 遇到 `module_extensions` 会警告+忽略,然后把那些文件当普通翻译单元
  编译 —— **错误的构建**而不是干净的失败。发布用了这个键的包必须声明 mcpp 版本下限
  (见 `docs/10-publishing-a-library.md`)。

## [2026.8.10.3] — 2026-08-10

### 修复

- **宿主能力清单从「解析后的图」取,不再从根 manifest 取。** `2026.8.10.2` 引入的
  「自带 libc 的档拒绝宿主能力」只在**根工程自己声明**能力时生效 —— 而几乎没有应用
  会自己声明 `capability:opengl.glx.driver`,它依赖某个声明了的包(glfw / SDL 封装 /
  GL runtime),resolver 会给每条需求盖上请求者身份。读根 manifest 回答的是
  「作者写没写」(几乎总是没写),而该问的是「解析出来的图需不需要」。

  实测:一个真实 imgui 工程的 `mcpp why runtime` 列着
  `capability:opengl.glx.driver [run] <- compat.glfw@3.4 (required)`,
  而 `mcpp pack --mode self-contained` **照打不误**。修复后它正确拒绝,并列出
  `abi:glibc, opengl.glx.driver, x11.display` 三条;`--mode vendored` 正常打包
  并把三条写进 `HOST-REQUIREMENTS`。

  同一处也修好了 `mcpp pack` 的 `HOST-REQUIREMENTS`:此前对真实工程是空的
  (空文件会被读成「什么都不需要」,而它现在根本不写空文件)。

  **为什么原来的测试没抓到:它的 fixture 在根工程声明了能力 —— 恰恰是真实工程
  唯一不具备的形态。** `216` 已改成依赖声明、消费方什么都不声明,并加了一条
  前置断言:那条需求必须先出现在 `mcpp why runtime` 里,否则测试等于什么都没验。

## [2026.8.10.2] — 2026-08-10

图形栈闭合与分发档位。完整设计与实施计划见
`.agents/docs/2026-08-10-graphics-closure-and-distribution-tiers-design.md` 与
`.agents/docs/2026-08-10-graphics-closure-implementation-plan.md`。

### 修复

- **依赖 BMI 缓存命中时,被恢复的包传递依赖的 `std` 没进构建图([#405])。**
  消费方自己不 `import std` 时,`gcm.cache/std.gcm` 的 stage 边没有任何消费者,
  ninja 从不执行它,于是被恢复的 BMI 报 `No such file or directory` /
  `Bad import dependency` —— 一条完全不指向缓存的错误。缓存 **miss** 时依赖在本地编译,
  这个动作把 std 边带进图,所以**第一个构建它的人永远是好的、之后每个人都坏**,
  看起来像升级回归。修法把 std BMI 放进 `_mcpp_staged_cache` 聚合 ——
  那个聚合本来就是为「stage 边丢掉编译边携带的次序」而存在的。
  不动 cache key、不改 entry schema,**现有缓存全部继续有效**。

- **`mcpp build` 会重放 `mcpp test` 留下的构建图([#407])。** 三种模式写同一个
  `build.ninja`(指纹不含 dev-deps 与 test targets),而快路径只比源码 mtime、
  从不校验这张图是哪一种。于是 `build → test → build` 链测试、不链 target、
  还报 `Finished`;改坏一个测试文件会让**普通 `mcpp build` 失败**,而 `src/` 一个字没动。
  修法是让 `build.ninja` 自己声明形态(`# mcpp:graph=normal|test`),快路径校验它即将
  重放的那张图。**这是读取侧不变式**:#387 那半边的写入侧修补被同时删掉了 ——
  写入侧修补要求每一个未来的图重写者都记得调它,而这正是 `mcpp test` 那半边
  在 `--configure-only` 修好之后仍然坏着的原因。

### 新增

- **加载器标签契约(`mcpp.build.loader_contract`)。** 可执行文件必须 **DT_RPATH**,
  共享库保持 **DT_RUNPATH**。DT_RUNPATH 只对携带它的对象**自己发起**的 `dlopen` 生效;
  图形程序到驱动要经三到四层 `dlopen`,而**这些 dlopen 都不是它发起的**,
  是 `libGLX.so.0` / `libEGL.so.1` 代发的 —— 所以标签(不是路径)决定它能不能拿到 GPU。
  同一路径只翻标签,egl / gles2 / egl-surfaceless 从 llvmpipe 变 NVIDIA。
  反过来在**库**上强制 RPATH 有害(传递性会打断 `eglInitialize`),所以这是一分为二、
  不是一起翻。链接期与 `mcpp pack` 的 patchelf 期读**同一条契约**。
- **rule E:标签校验落在产物上,并写进 `resolution.json` 的 `loader_tags`。**
  warn-first,与既有闭包规则同一推进节奏。记录而不只是告警 ——
  一个只在沉默中通过的检查,和一个根本没跑的检查,输出完全相同。
- **`mcpp pack`:产物里不再残留构建机路径。** 此前只有主二进制被重写,
  bundle 进来的每个 `.so` 都保留着链接时的 RUNPATH,而在这个生态里那是一串指向
  **构建机 xlings store** 的绝对路径。「依赖 xlings 生态」是设计选择,
  「依赖这一台机器的这一份 store」是缺陷,而且它在构建它的机器上跑得好好的。
  另外 `patchelf --set-rpath` 默认写 DT_RUNPATH —— 对可执行文件而言是同一个缺陷晚一层。
- **`HOST-REQUIREMENTS`:自包含也有底。** 驱动类库只能来自目标机器
  (与内核模块锁步,且专有栈禁止再分发),所以打包这类程序的诚实产出不是一个悄悄
  少了它的 bundle,而是 bundle **加上一份声明**。带 `discovery` 一列,因为几种发现
  机制互不通用 —— 一种是烙进派发库的搜索路径,另一种是 JSON 里的**绝对**路径,
  「把目录搬过去」只满足其中一个。
- **自带 libc 的档遇到宿主能力硬拒。** `--mode self-contained` 与 `--mode static`
  在存在 run 期能力需求时于 plan 期失败,并指出改用 `--mode vendored`。
  两者坏在同一件事上:那个 `.so` 带着对**宿主 libc** 的要求进来,而进程里没有那份
  (#392 / #401 的两个方向)。此前两档都链得过去、然后运行时崩或静默降级。
- **`[[runtime.requirements]] discovery`。** 声明式,**mcpp 绝不推断** ——
  从能力名推断机制就是把 provider 专属知识写进 mcpp(`test_runtime_contract` 正是
  为此设的门),而且它是 provider 的属性、会脱离 mcpp 变化。未声明报 `unknown`。
- **声明的 runtime artifact 带身份判决。** `resolution.json` 每个 artifact 增加
  `identity`:`ok` / `mismatch` / `missing` / `unverified`。这正是 mcpp **已经**
  对私有 libc 执行的规则(`glibc@2.44` 只解析那一个载荷,陈旧即错误)的推广,
  纯路径事实、跟随符号链接、不需要认识 GL。`mcpp why runtime` 的
  `artifacts: (none declared)` 改为 `(not declared by the environment —
  nothing to verify)`:一个有名无物的 provider 是**未验证**,不是验证通过。

### 变更

- **内带 xlings 升到 `2026.8.10.4`。** 其中 `2026.8.9.2` 的 semver 重写让四段版本可以
  参与范围比较 —— `xim:libglvnd@>=1.7.0.1` 此前解析成 `package not found`,
  这正是 mcpp-index 全量跑里 8 个图形成员全红的原因(不是数据缺陷)。

[#405]: https://github.com/mcpp-community/mcpp/issues/405
[#407]: https://github.com/mcpp-community/mcpp/issues/407

## [2026.8.10.1] — 2026-08-10

包身份、开发运行时与发布链收敛为同一组可验证事实。完整设计与验证记录见
`.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-focused-design.md` 与
`.agents/docs/2026-08-09-mcpp-template-runtime-graphics-aur-validation.md`。

### 新增

- **模板 selector 与 `mcpp add` 同源。** `mcpp new --template` 统一为
  `[namespace.]name[@version][:tname]`；省略 namespace 只表示 `mcpplibs`，不再做
  全索引短名搜索。未写 `tname` 时选择唯一显式 default；若只有一个模板且没有
  `default = true`，该单模板自动成为默认。多模板歧义明确失败并提示
  `--list-templates`，不引入 `--variant`。
- **事务化 scaffold。** 包身份、版本、模板、变量与 hook 全部在 sibling 临时目录中
  完成解析/渲染/校验，成功后才提交目标目录；失败不会留下半成品。
- **根项目级 RuntimeBinding。** 未声明 `[xlings].subos` 使用 mcpp 初始化并经 release
  验证的 `McppDefault`；也可在 `mcpp.toml` 选择命名 SubOS。无 CLI/env override，
  workspace root 决定整体环境，member/dependency 的 SubOS 不传递。不同 SubOS/glibc
  contract 进入独立构建指纹，可在同一机器并存。
- **provider-neutral 图形运行时契约。** mcpp-index 描述通用 requirement，xlings/xim
  选择 OpenGL/Vulkan、Mesa/NVIDIA、WSL、ICD/driver provenance；mcpp 只消费已解析的
  RuntimeBinding/LinkIntent、记录 resolution，并做平台通用链接与闭包校验，绝不探测 GPU。
- **不可变 release manifest。** 全平台资产完成后生成 `mcpp-release.json`，逐项重算
  SHA256，并在公开 GitHub inventory 上重放比较；它是下游发布的唯一 desired state。
- **`mcpp-bin` AUR reconciler。** 校验 manifest、双 Linux 资产/sidecar、AUR RPC/git 与
  Arch `vercmp`，先 dry-run exact diff，再以固定 host key 做普通 fast-forward push，
  有界重试并验证 RPC/公开 HEAD/干净 Arch 安装。定时恢复瞬时故障；`mcpp-m`、
  `mcpp-git` 不在自动化作用域。
- **`mcpp build --configure-only`:不编译也能拿到 `compile_commands.json`。** 走的是
  真实 `prepare_build()` 与真实 `BuildPlan`,以 Ninja dry-run 收尾 —— 编译参数只有一条
  推导路径,selector(`-p` / `--workspace` / `--profile` / `--target` / `--features` /
  `--cap` / `--static`)与普通构建同解。CDB 同时覆盖普通源码与 `tests/**/*.cpp`,测试 TU
  带 dev-dependencies 与匹配的 `[build].flags`,所以源码还编不过时 clangd 就已经能索引。
  它**不是只读操作**:`build.mcpp`、缺失依赖/工具链、lock 与构建目录元数据仍可能被写,
  只在可信 workspace 中运行。稳定契约只有退出码和 `compile_commands.json` 两项,
  stdout 仍是面向人的文本。设计见 `.agents/docs/2026-08-08-configure-only-cdb-design.md`。
- **CDB 改为原子发布。** 同目录临时文件 + `rename`/`MoveFileExW(MOVEFILE_REPLACE_EXISTING)`,
  从不先删目标,所以发布失败时磁盘上留下的仍是上一份完整可用的 CDB —— clangd 不会读到
  半截 JSON。符号链接形式的 CDB 替换的是链接目标而非链接本身。内容不变则不写,避免无谓
  重索引。普通 build/test 遇到发布失败只警告并继续,`--configure-only` 则直接失败:
  它唯一的产物就是那个文件。

### 修复

- **私有 glibc 不再泄漏进子进程环境（#401）。** `mcpp run` 曾把私有 glibc 目录放进
  `LD_LIBRARY_PATH`，而该变量会被程序派生的**每一个**进程继承；`/bin/sh` 由**宿主**
  loader 加载（`PT_INTERP` 写死在可执行文件里，任何环境变量都改不了），于是在重定位
  阶段就死于 `undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE`。
  该目录本来只为「可执行文件 DT_NEEDED 闭包覆盖不到的 dlopen」而存在，而产物的
  RUNPATH 已经覆盖了它（link model 在 `--dynamic-linker` 旁就发了
  `-Wl,-rpath,<glibc>`，改动前后产物 RUNPATH 逐字节相同），所以这条环境项没有收益、
  只有代价。决策收敛在 `mcpp.platform.runtime_env_contract`：私有 libc 是
  **binary 作用域**，不是 environment 作用域。
- RuntimeBinding 不再把 stale 的声明文本误当成实际 payload：有效 SubOS view 可规范化到
  唯一受管 payload 时记录真实身份；旧 view 断链时只解析声明精确指名的 payload，
  仍绝不枚举目录挑版本。
- Linux ELF/glibc 闭包规则的单测只在 Linux 断言相应物理语义；macOS/Windows 原生 CI
  固定验证 typed no-op 边界，不再拿 Linux 结果误判其他平台。

### 迁移

- **省略 namespace 的依赖获得一个版本的过渡期。** 省略 namespace 依然精确表示
  `mcpplibs`，但当精确坐标未命中时，`compat.<name>` 与「不声明 namespace 的上游
  descriptor」两级会再被尝试一次；命中会打印弃用警告、给出可直接粘贴的 manifest
  片段，并把**规范身份**写入 lock/install/cache（歧义拼写只留在用户 manifest 里）。
  `mcpp add <裸名>` 直接把规范点分形式写回 `mcpp.toml`。
  写明的身份（如 `mcpplibs.gtest`）不进入过渡期，未命中即失败；第三方 namespace 仍
  不可被裸名触达。该过渡期在 `2026.9` 移除。
  - 理由：索引里已发布的 `compat.*` 包与既有用户 manifest 全部使用裸名写法。
    「已发布的数据不得让程序失效」与「已发布的程序不得让既有数据失效」是同一条判据，
    两个方向都必须降级而不是变砖。#278 修掉的缺陷是**静默**回退，不是回退本身。

### 其他

- xlings pin 保持在 `2026.8.9.2`，自举 mcpp pin 提升到已发布的 `2026.8.8.4`。
  曾提升到 `2026.8.10.1` 并回退：该版本在**冷 home** 上装不上 `xim:gcc@16.1.0`
  （依赖解析到 glibc 2.44，而 gcc 的 config hook 找不到该 payload），
  同一 workflow 的冷缓存 A/B 已确认，见 openxlings/xlings#524。修复发布后再提升。

## [2026.8.8.4] — 2026-08-08

机器可读输出有契约了。设计与实测见
`.agents/docs/2026-08-08-machine-readable-output-protocol-design.md`,用户文档见
`docs/11-machine-output.md`。

### 新增

- **`--format json`:统一的输出信封(`mcpp.wire`)。** 覆盖 `self env`、`xpkg parse`、
  `cache list`,含 `schemaVersion` / `kind` / `kindVersion` / `effects` / `data` /
  `diagnostics`。信封与 kind 的版本**分开** —— 单一全局版本号会让给 `mcpp.env` 加一个
  字段推高客户端为 `mcpp.xpkg` 读到的版本。

- **`mcpp --protocol-version`。** 回本 build 支持的信封版本、各 kind 版本,以及
  **命令 → 效应**的静态表。它是优化而非地基:在它出现之前的每个 mcpp 上,它自己就是
  未知选项,失败与成功同走 stdout —— 所以客户端的判据只能是**正向识别**(stdout 解析
  得出 `schemaVersion` + `kind`)。这条写进了文档的第一节。

- **效应集合,不是 `destructive` 布尔。** 实测:全新 `MCPP_HOME` 上 `xpkg parse` 与
  `cache list` 什么都不建,`self env` 建 6 项。布尔分不开「mcpp 给自己做初始化」和
  「执行工作区里的代码」,而 IDE 的 untrusted 门只在乎后者。

- **`mcpp self env --format json` 走独立只读路径。** 不调 `load_or_init`:客户端问
  「东西在哪」不该成为把东西放到那儿的原因。全新 home 上返回完整路径 +
  `initialized: false`,创建项数 0(人类路径不变,仍会初始化)。

### 修复

- **CDB 的 `arguments` 带着 shell 引号。** 消费者(clangd)逐字 exec 它,不经 shell,
  于是带引号的 token 不是 flag 而是不存在的文件名。Windows 上每个带路径的 flag 都中招
  (`shell_quote_arg` 的触发集含反斜杠);带空格的路径上更糟 —— token 在引号**内部**
  被切断,一个参数变成两个,其中一个带着永不闭合的开引号。

- **usage 错误不再进 stdout。** 未知选项过去打在 stdout、rc=1、stderr 为空,于是
  `mcpp cache list --format json | jq` 拿到的是人类文本。现在未知选项与不支持的值
  统一 stderr + rc=2,stdout 一字不写。

### 兼容

- **`--json` 永久保留它的 payload,不打 deprecation 警告。** 拼写兼容不等于 payload
  兼容:`cache list --json` 顶层是 `{root, entries}` 且本仓库 e2e 已断言。两种拼写由
  同一来源产出,不会漂移。
- `pack --format tar|dir` 是产物形态而非输出格式,**声明为本协议的例外**。

## [2026.8.8.3] — 2026-08-08

### 修复

- **subos 的 `prepend` 把调用方的值整个丢掉了(#382 顺带发现)。**

  解析出的键值对是**替换**子进程里的变量的(作为 extraEnv 进去),而 `prepend` 只发出
  声明值 —— 对 PATH 形状的变量,丢掉的是用户的整条搜索路径。现在它接在调用方的值
  前面,且对已存在元素幂等(嵌套运行不会让列表增长)。空值不算「用户设过」。

  同一处的 `set` **保持不变**。`set` 与「默认值」是两种意图:有些变量 subos 必须说了
  算 —— 指向它自己 loader 配置的那类,用户 shell 里一个陈旧值就能把环境弄坏。而且
  `envs` 的 op 词汇表是 xlings 的契约,消费方悄悄给一个 op 加第二种含义,会让同一个
  subos 因为「由谁启动」而行为不同。#382 想要的「用户可覆盖的默认值」应由 xlings 新增
  一个 op,mcpp 认它;因为 mcpp 会丢弃未知 op,那个 op 必须两侧同时到位。

### 其他

- xlings pin 提升到 2026.8.8.1。

## [2026.8.8.2] — 2026-08-08

编译器交付的是**能力**,不是配置。设计与实测证据见
`.agents/docs/2026-08-08-payload-version-and-contract-drift-design.md`。

### 修复

- **产物加载哪个 glibc,现在有权威,不再靠目录顺序。**

  payload-first 的构建要链接到某个具体 glibc。旧规则是问目录"那个 glibc"、
  取 `readdir` 的第一项——只装了一个时它永远对,所以从没有东西逼它对。一条带
  `xim:glibc@>=2.38` 的依赖就足以装进第二个:编译侧取了 2.44,而产物的
  interpreter(装机时冻结在 gcc specs 里)仍是 2.39,于是二进制引用
  `GLIBC_2.42` 符号却跑在没有它的运行时上,报错还落在与那条依赖无关的包上。

  现在由 subos 自述的 runtime 作答(`[xlings] subos` 可指定),没有答案就**拒绝**
  走 payload-first,而不是挑一个。该绑定计入工具链指纹(11 字段),两次只在
  runtime 上不同的构建不再共用缓存。旧 subos 回落到工具链自身烙入的值。

- **不再改写 GCC 的 `specs`,产物也不再带别人机器的死路径。**

  旧的 specs 重写用单路径 needle 配双路径 replacement,每个跑过它的 home 都漏下
  一条:一台开发机产出的每个 gcc 产物里都有 **68** 条陈旧 `RUNPATH`,全指向已删除
  的 `mktemp` 目录。改为 `-specs=` 一份逐构建生成的干净文件(`-dumpspecs` 取内建
  `*link:`,去掉 loader/rpath),loader 与 rpath 由 mcpp 显式写在链接行上。逐构建、
  不需写权限,继承来的只读 payload 也能用。e2e `201_gcc_no_specs_pollution.sh`
  断言的是产物而非 specs 文件。

- **落后于 pin 的 vendored xlings 会被替换——但只在替换品确实更新时。**

  `acquire_xlings_binary` 过去见到文件存在就返回,于是一个 home 会永远留着它第一次
  获取的 xlings(实测 2026.8.2.1 对 pin 2026.8.6.3)。`subos_info` 是 2026.8.5.1
  才有的,所以那台机器上 subos 的自述一直被一个太旧的客户端丢弃。修复的第一版直接
  删了重取,结果把 2026.8.2.1 换成了系统的 0.4.51——更旧,且同样没有那个特性;现在
  先给替换品定价再动手。

- **`--sysroot` 的判据从"存在"改为"归属"。** gcc 把
  `--sysroot=<...>/.xlings/subos/default` 当字符串烙进去,而一台机器上有很多同名
  目录,所以那条烙入的路径经常存在、却属于另一个 checkout(实测:mcpp 里的构建解析
  到了无关仓库下的 sysroot)。

### 其他

- `mcpp self doctor` 新增两条检查:vendored xlings 落后于 pin;sysroot 既不属于本
  mcpp home 也不属于当前项目。
- xlings pin 提升到 2026.8.7.1。

## [2026.8.8.1] — 2026-08-07

xlings 作为运行时底座:subos 环境到达程序,以及 self-contained 的
`/proc/self/exe` 陷阱(#352、#375)。设计见
`.agents/docs/2026-08-07-xlings-as-runtime-substrate-design.md`。

### 修复

- **subos 声明的环境变量现在真的到达被运行的程序(#352)。** 之前 mcpp 读的是自己
  想象出来的 `subos_info.envs` 线格式(数组),而 xlings 写的是以 binding 为键的
  对象,于是整条修复是空转的。现在按 xlings 实际写出的格式解析,并用逐字取自真实
  xlings 输出的 fixture 钉住。
- **升级后的 mcpp 会重放升级前的缓存,导致 subos 环境丢失。** 快路径命中时不带
  subos 环境;该缺陷是靠一条"必须走快路径"的自证断言抓到的,而那条断言随即又抓到
  第二个:缓存行写在 `profile=` 之前却在 `cacheMode=` 之后解析。
- **self-contained 打包的 `/proc/self/exe` 陷阱(#375)。** 新增 `MCPP_BUNDLE_DIR`
  契约。

## [2026.8.7.1] — 2026-08-07

两处「模型比生态少一层」。设计与实测证据见 `.agents/docs/2026-08-07-windows-resources-and-version-identity-design.md`。

### 新增

- **`[resources]`:exe 图标与版本信息现在是 `mcpp.toml` 里的一行(#365)。**

  ```toml
  [resources]
  icon = "assets/app.ico"
  ```

  `FILEVERSION` / `ProductName` / `FileDescription` / `CompanyName` / `LegalCopyright` 全部从 `[package]` 取默认值,资源脚本由 mcpp 生成。自写 `.rc` 写 `files = [...]`,mcpp 编译并**跟踪**它。

  **只有 PE 目标消费这一节**;在 Linux/macOS 上它「不适用」——不是降级、不是带警告地跳过:没有消费者,构建逐字节不变,也不说话。所以**不需要(也不能)加 `cfg(windows)` 谓词**。节名不叫 `[windows]` 是因为「图标」作为概念不是 Windows 专有的,将来 macOS `.icns` 扩同一节而不是把这条轴按 OS 切三份。

  **声明了却不存在的文件是硬错误**,这是对 issue 第 3 条请求的**有意偏离**:mcpp 里每个「声明过的输入」都是这个规则(`main = "…"` 必须匹配恰好一个文件、nasm 缺失是硬错误),而「缺失就跳过」会把这个 feature 要消灭的失效模式写成规定行为——一个没有图标、没有版本信息、且什么都没说的正式二进制。不要图标已经可表达:把那一行删掉。

  **这条校验在每个目标上都跑**,「不适用」只停在*编译*那一步。路径存不存在是关于工作树的事实、不是关于目标的事实;按 PE 设门会让 Linux/macOS 的构建与 CI 完全看不见 `icon` 里的拼写错误,只有 Windows job 变红——正是这条硬错误要消灭的「太晚才知道」。

- **`role = "object"`:build.mcpp 的 action 现在能把产物接到链接输入上。** 角色表原本三格接在「编译输入 / 无 / 链接输出」上,缺的正是「链接输入」——一个构建图显然有的接线点。后果不是理论上的:预编译对象只能塞进 `[build].ldflags`,而那是链接命令里的一串字符、不是图里的文件,于是改了图标得到 `ninja: no work to do`。

  可选 `.target("name")` 指定接哪条边;**省略是推荐写法**,它接到本次构建产出的每个镜像——可执行、动态库**与测试二进制**。测试二进制在默认集合里不是顺手加的:它链接的是同一份库代码,排除掉会让 `mcpp build` 通过而 `mcpp test` 在这个 action 本来要提供的那个符号上报 `undefined symbol`;而改成显式点名也不成立——测试链接单元是从 `tests/*.cpp` **发现**出来的,名字不在 `mcpp.toml` 里,写了它的 build.mcpp 在普通 `mcpp build` 下会直接构建失败。**每一个**匹配不到链接单元的名字都是错误,包括写在一个匹配得上的名字旁边的那个(拼错的真实形状)。本次构建里没有任何镜像可接时报 degradation——这条边只能经由链接被达成,没有链接就意味着命令一次都不跑。

### 修复

- **解析出的版本现在是索引里的字面键,不是重新渲染的数字(#363)。** `resolve_semver` 一直把索引的字面版本键读到手里,然后 `return parsed[i].str()` —— 从解析出的数字重造一个地址。渲染器复现不了的东西就变成了不存在的地址:

  | 上游键 | 旧行为 |
  |---|---|
  | `1.92.8-docking` | 截断成 `1.92.8`,与非 docking 那个**塌成同一个可比较版本**(两个不同 tarball) |
  | `25.0.4.7.1`(jdk-corretto,五段) | 截断成 `25.0.4.7` —— **索引里没有这个键** |
  | `pre-v0.0.5`(khistory,唯一的发布) | 静默跳过,然后报「no valid versions in index」——把责任推给一个发布得好好的包 |

  现在字面键与序一起传递,`version_req` 只负责**排序**。连带修的:

  - **预发布按 SemVer 排序**,且范围按 npm/Cargo 规则**看不见预发布**,除非约束自己在同一数值元组上带了预发布。`^1.92.8` 因此确定性地选 `1.92.8`,不再在两个 tarball 之间由一个看不见差别的序做取舍。同一条规则顺带修掉 `^1.2.3` 会漏进 `2.0.0-alpha`。
  - **数值段不再截断在第四段**。真实索引里 `jdk-corretto` 发五段键;截断让 `25.0.4.7.1` 与 `25.0.4.7.2` 比较相等。
  - **别名条目(`{ ref = "…" }`)不再是范围候选**。`jdk-temurin` 的 `["25.0.4"] = { ref = "25.0.4+7" }` 曾与它自己的目标构成一次「平局」。精确寻址不变。
  - **不可排序的键**(`b10069`、`latest`、`pre-v0.0.5`)成为一等公民的一类:只参与精确匹配,范围约束下报**指名的**错误并给出可粘贴的 pin 行。
  - **真平局硬错**。只差 build metadata 的两个键(`1.0.0+a` / `1.0.0+b`)是两个 tarball、两个 sha256,序说不出该要哪个;旧行为按描述符里的行序取第一个,意味着索引的一次排版调整会改变构建出来的东西。

- **mcpp.lock 记录解析结果,并覆盖传递依赖。** 它记的一直是**约束本身**(`version = "^1.92.8"`),而一个记录范围的 lock 不锁定任何东西;`Compiling compat.imgui v^1.92.8` 这行也一样。两者读的都是 `m->dependencies`(未解析的输入、且只有直接依赖),而解析结果 `ResolvedRecord` **早就覆盖整张图**——修法是把两个消费者都指过去,而不是补第三处回写。

  lock 头部现在自己声明**它还不 pin 后续构建**(index 依赖仍每次从约束重新解析)。一个记着真实版本却不生效的文件,比一个明显记着范围的文件更容易被误当权威。

  **dev-dependencies 不进 lock。** 解析结果覆盖整张图,而 `mcpp test` 解析 dev-deps、`mcpp build` 不解析——照单全收会让一个进 VCS 的文件取决于「上一条命令是什么」,build/test/build 写出三个不同的文件。判据:**lock 是 manifest 的函数,不是命令的函数**。头部注释也写了这一条。

- **`[resources]` 在 MSVC 下找不到 `rc.exe`。** 工具链 PATH 覆盖按 `find_first_of(";:")` 切分,而 Windows 路径的**盘符冒号**就在下标 1——`C:\Windows Kits\…` 被切成 `C` 加一段「当前盘相对路径」,同盘时侥幸命中、跨盘必然找不到。而这条 PATH 遍历正是 msvc 下的**主路径**(`rc.exe` 属于 Windows SDK,从不在 `cl.exe` 旁边)。两个调用点各自推导同一条规则、且已经彼此不一致,现在收敛成一个 `split_env_list`。

- **`.rc` 现在能穿过工程级 fast path。** `sources_newer_than` 只扫 `src/**/*` 的 C++ 扩展名,一次只改资源脚本的构建因此报 `Finished dev in 0.15s` —— 而 `.rc` 的 implicit input 集合来自扫描它、扫描发生在 prepare,所以往脚本里新加一行 `#include "ids.h"` 那个头文件永远不会被跟踪。与 `build.mcpp`、glob 输入(#359)是同一类:**mtime 扫描看不见,但改了它图就该长得不一样**。只扫 `files`——`icon` 与 `extra-inputs` 已经是 ninja 的 implicit input。

### 其他

- 版本号 2026.8.6.3 → **2026.8.7.1**。
- **行为变化(生态可见)**:`cc-connect` 这类「稳定版 + 预发布版」并存的包,`^1.3` 从 `1.3.3-beta.1` 改为解析到 `1.3.2`;`jdk-corretto`/`jdk-temurin` 这类带别名的包,范围解析改为选中真条目(`25.0.4.7.1` 而非别名 `25.0.4`),store 目录名随之变化。
- **诊断口径**:`resources/versioninfo`(版本资源 Windows 读不到)与 `resources/no-image`(声明了却没有任何镜像可嵌)由 warning 改为 **degradation** —— 它们的 impact 正是本批要消灭的静默失效,`--strict` 必须看得见;`role = "object"` 无消费者新增 `action/no-target`,同口径。
- README 的包索引链接由仓库地址改为 <https://mcpplibs.github.io/mcpp-index/>。

## [2026.8.5.4] — 2026-08-06

命令长度这一族缺陷的**第七次**,这次不再补洞。架构分析见 `.agents/docs/2026-08-06-command-length-architecture.md`。

### 修复

- **windows 上的 clang 链接改用 lld,消掉最后一个随对象数增长的上限。** 2026.8.5.3 把 mcpp**自己**写的响应文件改成按行分隔,但 clang 作为 driver 时会**再生成一个**响应文件转发给链接器,而那个是单行的 —— 我们改不到它。mcpp-index 的 `opencv-module` 因此在 2026.8.5.3 上仍然 `LNK1170`,而且是在编译完 356 秒之后。

  **为什么现在才出现**:#344 给每个依赖的对象加了一层包目录(缓存正确性的要求),路径因此变长——同一条边在 linux 上从 56 840 涨到 161 687 字节。而 mcpp-index 的 CI 一直 pin 在 **2026.8.3.3**,正好是那之前一版,所以它的 windows 腿从没用长路径链接过。**把 pin 抬上来才第一次撞到**。

  **为什么这不是 workaround**:lld 用 LLVM 的 tokenizer 解析响应文件,**没有单行上限** —— 消掉的是一整类,不是把某个数字调大;路径也缩不短(那层包目录正是 #344 需要的,其余是源码树自身结构)。而且 **linux 与 macOS 早就在用 lld**,windows 是唯一还在用系统链接器、也是唯一有单行上限的平台 —— 这是消除平台不一致。原生 cl.exe 保持 link.exe:那条路径上响应文件是我们自己写的。

### 改进

- **命令长度预算现在是一张表,不是七处注释(`src/build/cmdlimits.cppm`)。** 前七次每次都在注释里写下「构建系统不该有靠崩溃才发现的规模上限」,然后换个地方再犯。根因是:命令构造层对「这条命令要穿过哪些通道、每个通道上限多少」**一无所知**,而这份知识只存在于注释与 CHANGELOG 里 —— 是「同一决策在 N 处推导」的镜像:**一个关键约束在零处被表达**。

  表里除了字节数,还记**症状**:这一族最贵的从来不是修,而是**认出**。`posix_spawn: Argument list too long`、`LNK1170`、cmd.exe 的裸 `127` 三种表现毫无共同点,下次遇到第四种时表能直接把人指过来。新增执行通道必须回答「你的上限是多少」,与 `directives::kTable` 里「Scope 是必填字段」同一个手法。

- **超限改为在计划期拦下并指名道姓。** 以前是 ninja 或 link.exe 在构建末尾崩,而报错的是别人的程序,信息里没有 mcpp 的上下文(哪条边、哪个包、多少对象)。现在生成完 `build.ninja` 就统一扫描,超限时报出**边名、通道、实测字节、上限、解法、文档路径**,且发生在还没编译任何东西的时候。

  两处判断在实施中被纠正:校验点选在「manifest 生成后统一扫描」而不是「逐个 emit site 插桩」——后者在新增一种边时没人会想起来加,而那正是前七次的漏法;`phony` 必须排除,否则会误报到 **#274 的修复本身**(它为解决 argv 超限,正是把几千个目标收进一条 phony 聚合边,而 phony 根本没有 command)。

### 其他

- 内带 xlings 升到 **2026.8.5.2**(`.github/` 下 16 处 pin 由 `check_version_pins.sh` 机器校验同步)。

## [2026.8.5.3] — 2026-08-05

### 修复

- **链接响应文件按行分隔(`$in_newline`)。** 一条链接边和操作系统之间有**两道**上限,而「改用响应文件」只拆掉了第一道:

  1. **命令行**:Windows `CreateProcess` 32 KiB;POSIX 下 ninja 用 `sh -c "<整条命令>"`,整条命令是**一个** argv 项,撞的是 `MAX_ARG_STRLEN` 128 KiB。这道在 #344 / PR#345 已经拆掉。
  2. **响应文件的单行长度**:`link.exe` 上限 128 KiB。所有对象写在一行,于是

     ```
     fatal error LNK1170: line in command file contains 135135 or more characters
     ```

     mcpp-index 的 `opencv-module` 与 `opencv-module-dnn` 在 windows 上正是死在这里 —— 链接前的 795s / 1166s 编译全部白做。

  改成 `rspfile_content = $in_newline` 之后,**没有任何上限再随对象数增长**。全平台同一条规则形状:GNU 与 LLVM 的响应文件解析把任何空白(含换行)当分隔符,而 link.exe / lib.exe 要的正是这种写法。

  > 同一族的第四次(#274 / #247 / #344 / 本条)。前三次的教训写的是「命令有多长不该有人放在心上」;这次补上的是它的孪生兄弟 —— **一行有多长同样不该**。e2e 190 两面都钉:既断言生成的规则,也断言 ninja 真正写出来的文件(`-d keeprsp`),因为只断言前者的话,ninja 哪天改了 `$in_newline` 的展开方式测试仍会绿。

## [2026.8.5.2] — 2026-08-05

修复 `host-module = true`(规则包)的两个缺陷。二者都是 2026.8.5.1 引入的,合起来的效果是:**规则包只能写「手工 printf 指令」的玩具规则**,一旦规则要用它本该用的 API 就编不过。第一个真实使用者(`grpc-m` 的 protoc/gRPC codegen 规则)在第一分钟就同时撞上了这两个。

### 修复

- **规则里的 `import std;`。** `build_program.cppm` 先编 host module、后建 std 模块,所以规则拿到的 `stdFlags` 是空的,报 `module 'std' not found`。而且「是否需要 std」只扫了 `build.mcpp` 的源码 —— 一条只有**规则**用 `import std;` 的构建根本不会去建 std 模块。现在两处都修:扫描把规则接口一并计入,std 那一段整体挪到编译 host module **之前**。

  这里的顺序是承重的,不是风格问题:BMI 必须先存在、`stdFlags` 必须先指向它,规则才可能 import。

- **规则里的 `import mcpp;`。** `host-module = true` 只做了「注册这个模块」,从没把这个包移出消费者的**普通依赖图** —— 于是同一个 `.cppm` 又被当作消费者的一个普通库编译了一遍,而在那次编译里内置 `mcpp` 模块并不存在,报 `fatal error: module 'mcpp' not found`。

  一条构建规则是**纯构建期**的东西,本来就不该进消费者的二进制(Cargo 用 `[build-dependencies]` 划的是同一条界线)。现在只经由 root 的 host-module 边到达的包会被清空源码集,不再参与编译与链接;仍会被解析落盘,因为 lib 根要从那里读。

  **有护栏**:同一个包完全可以既是规则、又是别处(或 root 另一种拼法)的普通库,此时不清空 —— 否则会变成一个离现场很远的 undefined reference。

### 文档

- `docs/05-mcpp-toml.md` 的示例此前写的是 `import mcpp.rules.protobuf;`,**做不到**:mcpp 用依赖的裸 `package.name` 注册 host 模块,而 SPEC-001 要求 `name` 是单一原子段。随之澄清一条会咬人的约束:规则包的名字必须是**合法 C++ 模块名**(`grpcgen` 可以,`grpc-rules` 不行),否则报的是 `module 'grpc_rules' not found`,不会提示你名字有问题。

## [2026.8.5.1] — 2026-08-05

`build.mcpp` 机制的**架构地基**:把「一条指令是什么」收敛成一张表,并补上三个今天就存在的稳定性缺口。架构分析见 `.agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md`(本次实现其中的步 0 与步 1)。

### 改进

- **新增一条 directive 从改 9 处降到改 1 处。** 一条指令原本要在九个地方定义:`Directives` 结构体字段、`parse_line` 分派、`write_cache` 序列化、`read_cache` 反序列化、`apply` 落到 manifest、`cache_fresh` 的产物存在性校验、`prepare.cppm` 的 `DirectiveMark` 字段、`markDirectiveTail`、`foldDirectiveTailIntoPrivateBuild`,再加内置 `mcpp` 模块的类型化包装。`prepare.cppm` 自己的注释还承认这个拆分**仍不完整**(「link/source/fingerprint residues stay at the call sites」)。

  这是本仓库反复付过学费的「同一决策在 N 处推导」形态(#233/#240/#242/#344):它**不在你新增指令时失败**,而是过一阵子在别的地方失败。现在一条指令就是新模块 `src/build/directives.cppm` 里 `kTable` 的**一行**,解析、缓存读写、落盘应用、声明产物契约、私有作用域折叠全部由该行驱动。

  **作用域是必填字段**,不是可选注释:`include-dir` 之所以是 `PackagePrivate`,是「构建期程序不得静默拓宽包的公开接口」这条供应链规则(Cargo 纪律),把它变成表里一列意味着**下一条指令不回答这个问题就加不进来**。

  > 为什么是新模块而不是继续写 `build_program.cppm`:那个文件的匿名 namespace 在 clang 22 + C++20 modules + `-O2` 下会**误编译自己的邻居**(PR#332 证实一个**从未被调用**的新函数就足以破坏 `contract_env`,PR#334 复现)。`mcpp.build.hostprogram` 当初就是为此拆出去的。

- **`build.mcpp` 有了线协议版本(S1)。** 用 `import mcpp;` 的程序在 `main` 之前自动声明 `mcpp:protocol=<N>`(你不用自己写)。于是:

  - 程序声明的协议**高于**本 mcpp 所理解的 → **拒绝执行**并给出升级提示。原先的行为是警告后忽略,那会让「构建成功了但那个 flag 根本没到」——最难查的一类构建 bug——静默发生。
  - 既然双方已被证明一致,**未知指令即错误**:同一协议版本内它只可能是拼写错误。

  手写 `printf("mcpp:…")` 的程序什么都不声明,**保留**历史上的「警告并忽略」。这条不对称就是兼容性契约本身:那一面**冻结在现有 11 条指令**上,新能力只在类型化 API 里落地。

- **`build.mcpp` 缓存带上了语义 epoch(S2)。** 缓存条目里的指令是命中时**原样重放**的,所以当引擎对某条指令的**解释**改变时,旧条目必须失效而不是被按新含义重放。纪律照抄 `cache_key::kCacheEpoch`:只在旧条目真的不可用时 bump,且**与 mcpp 版本号解耦**(否则每次发布都会让所有构建程序白重跑一遍)。一条本 mcpp 不认识的 `d` 记录(更新的 mcpp 写的)同样让整条条目作废——只重放认识的那部分,等于应用了程序所要求的一个**真子集**。

- **`build.mcpp` 的运行有了时间上限(S3)。** 默认 600 秒,超时杀掉并让构建失败,错误**点名是哪个包**、并说明怎么改。原先没有任何上限:一个死循环或卡在网络读上的构建程序会让整个构建**挂死且毫无诊断**。

  **编译**这一步刻意不设上限——与 `mcpp test` 同一条不对称纪律(run 有限 / build 无限):编译跑得久通常是正当的(首次构建 `std` 模块就是分钟级),杀掉它只会产生莫名其妙的失败;构建**程序**跑得久通常是卡住了。`capture_exec_deadline` 顺带补上了 `cwd` 形参——没有它,加超时会**静默改变**构建程序相对写入的落点。

### 新增

- **依赖产出的 host 工具:`tools = ["protoc"]`(#355)。** 一个包能构建出消费者在**构建期**需要的二进制(protoc、grpc_cpp_plugin、flatc、moc、转译器),但消费者此前完全拿不到它 —— `mcpp::dep_dir()` 给的是**源码树**,而依赖的 `kind = "bin"` target **从不被构建**(`plan.cppm` 只遍历 root 的 targets;唯一的例外 `kind = "shared"` 是按 `--target` 构建的,当 host 工具用不了)。

  **为什么它不能是主图里的一个节点**:时序。`build.mcpp` 跑在 prepare 内,那时 BuildPlan 还不存在、build.ninja 更在其后 —— 主图产出的东西对需要它的程序来说**永远来得太晚**。再叠上交叉编译,它连架构都不对。所以工具由**嵌套的、面向 host 的子构建**产出,落进全局 store。这正是 Cargo `[build-dependencies]` / Bazel exec configuration / vcpkg `"host": true` / Conan `tool_requires` 的形状。

  **为什么它便宜**:工具是**可执行文件**,与主构建零 ABI 接触。子构建因此可以用工具包自己的 `[toolchain]`、自己的 profile、自己的依赖解析,不必与消费者一致 —— 对照 `kind = "lib"` 依赖,这几条**必须**一致。

  **单一版本轴**:工具的版本就是依赖的版本,所以「protoc 与 protobuf 运行时错配」这类**运行期**才炸的问题结构上不可表达。默认关闭(成本由消费者付),成本门复用已有的 `[features]` + `required_features`。全局 store 按 包版本 × host 工具链 × feature × 自身依赖闭包 缓存。

  **逃生舱** `[tools.overrides]` / `MCPP_TOOL_<PKG>_<TOOL>`:直接指一个现成二进制,**完全跳过构建**。每个同类系统都提供这一条(vcpkg `VCPKG_HOST_TRIPLET`、CMake `LLVM_NATIVE_TOOL_DIR`、Qt `QT_HOST_PATH`),理由一样 —— 源码在本机构建不出来的工具不能是死路。它**刻意不进 cache key**:逃生舱不是可复现输入。

- **`mcpp:action=`:声明构建图节点,而不是在 build.mcpp 里干活。** 在程序里直接写生成逻辑,是每次 prepare 跑一遍、全量、串行,失败报「build.mcpp exited 1」。**声明**成节点后它是图里的一条边 —— 增量、并行、失败可归因到具体那条边。

  **一个原语,三种接线**(`role` 只决定输出接到哪,不是三套机制):`source` 进编译集(protoc、转译器)、`check` 产出 stamp 且**默认与编译并行**(clang-tidy、格式/ABI 检查;`blocking` 可改成前置)、`artifact` 的**输入**是链接产物(签名、打包、size budget)。顺序完全由 ninja 的文件依赖决定,不需要任何 phase 机制 —— 这也是为什么 `artifact` 不会像朴素的「post 钩子」那样把自己重复施加一遍。

  **必须写出输出文件名**:mcpp 在 prepare 期就定死源码集、fingerprint 与模块图,名字未知的产物无法进图。内容可以晚到,名字不行。畸形 action 是**硬错误**而非静默跳过。生成**模块接口**时用 `.provides()/.imports()` 声明,mcpp 会按该声明播下占位文件让 prepare 期的扫描与生成器将要产出的内容一致 —— 与 `[modules].scan_overrides` 同一条「声明+验证」的取舍,build 期由编译器自己的 P1689 复核。

  命令是 **argv 而非 shell 字符串**(不假设存在 shell),插值只有封闭的四个:`${mcpp.out_dir}` / `${mcpp.bin_dir}` / `${mcpp.compile_db}` / `${mcpp.target_file:<name>}`。

- **`host-module = true`:可复用的构建规则以普通包分发。** 「跑 protoc」这类规则应该写一次,而不是每个消费者的 build.mcpp 复制一遍。把它做成普通 mcpp 库包,消费者 `import mcpp.rules.protobuf;` 即可。规则因此**有版本、能测试、能发布**,走的是已有的包管理机制,而且是用 **C++** 写的 —— 不引入第二门语言(xmake 用 Lua rule、Bazel 用 Starlark),这正是 build.mcpp 存在的理由。

  实现上的关键:规则模块与 build.mcpp **在同一条命令里、用同一套 flag** 编译。这不是优化 —— BMI 只对「在 standard / dialect / 编译器身份上与它一致」的编译可用,分成两次独立解析的构建则毫无理由一致,而不一致的表现是 `module X CRC mismatch` 而不是一条清楚的错误。

- **工作目录可外置(`BuildOverrides::work_dir`)。** 一次 `mcpp build` 会往工程根写 5 处(`target/`、`mcpp.lock`、`compile_commands.json`、`.mcpp/`、`target/.build-mcpp`),而「注册表包根是共享的、可能只读、绝不写入」是 `build_program.cppm` 自 G2 起的明文不变量 —— 在此之前没有任何东西能对 build.mcpp 的临时目录之外兑现它。把「源码在哪」与「往哪写」拆开,是 host 工具子构建**能够存在**的前提。五处一起搬:只搬一部分比一处都不搬更糟,那等于照样写进共享目录、只是更不显眼。

### 改进

- **内带 xlings 升级到 `2026.8.5.1`**(自 `2026.8.4.1`)。13 个 pin 点由 `check_version_pins.sh` 机器校验并全部更新。

### 修复

- **自举 pin 指向了索引已不再提供的版本。** `ci-aarch64-fresh-install` 自 2026-08-03 起在 main 上就是红的:`version '2026.8.3.2' not found for 'mcpp'`。`.xlings.json` 的自举 pin 是**自举起点**、故意滞后、不随发布走(docs/09 §4),平时不该动 —— 但它有一条硬约束是**必须命名一个可安装的版本**,这条被打破时正是该 bump 的场合(而不是「发版顺手 bump」那种误用)。改到 `2026.8.4.1`。

## [2026.8.4.1] — 2026-08-04

### 修复

- **`ci-fresh-install` 11 个 job 全红 —— 两个独立缺陷。** 都不是某次发布引入的,而是**每次发布之后必现**;两个各自都能单独把整个矩阵打红。分析见 `.agents/docs/2026-08-04-ci-fresh-install-two-defects.md`。

  **A. 仓库的 workspace pin 伏击了被测版本。** job 第一步就 checkout,于是仓库的 `.xlings.json`(声明**自举** mcpp,手工维护且**故意滞后**)落在工作目录里。它是目录作用域的,在 checkout 内部**压过全局安装**。于是「装的是 `MCPP_PIN`,跑的是自举版本」——而后者根本没装:

  ```
  1 package(s) installed
  [error] xlings: version '2026.8.3.2' not found for 'mcpp'
  [error]   available: 2026.8.3.4
  ```

  `ci-aarch64-fresh-install.yml` 早就遇到过并靠「checkout 放最后」规避,注释写得很完整 —— 但那招在这里用不了:`build mcpp` 步骤要在仓库里跑 `mcpp clean && mcpp run`,checkout 必须在场。所以改为中和那个 pin:这个 workflow 验证的是**已发布**的 mcpp,自举 pin 在这个问题上没有发言权。

  **B. `wait-index` 守的是另一条分发通道。** 它轮询索引的 **git 真源**(`raw.githubusercontent.com/openxlings/xim-pkgindex`),而 job 从**发布出来的 artifact**(`xlings-res/xim-index` → 指针 → tarball)安装。两条通道延迟完全不同:git 在 PR 合入瞬间更新,artifact 还要打包 + 过 CDN。2026.8.3.5 那次实测:守卫报「就绪」,11 个 job 随后全部 `package 'mcpp@2026.8.3.5' not found`。**测量一条没人从那里安装的通道,不叫守卫。** 现在改查 artifact 通道。

  两处修复连同「装了 ≠ 跑的是它」的激活(`-u` + `xlings use`)与**断言**,收敛进一个共享脚本 `.github/tools/install_released_mcpp.sh`(5 处调用点)。断言是唯一能防住**下一个**的部分:它对着 **PATH 解析出来的** `mcpp` 求值 —— 也就是后续步骤真正会执行的那个 —— 于是任何未来的静默重定向(workspace pin、陈旧激活、PATH 意外)都会变成一条点名的失败,而不是一个悄悄测了错二进制却报绿的矩阵。

  > CDN 是**按边缘节点**传播的:轮询的 runner 不是安装的 runner。所以守卫收窄窗口、脚本内的有界重试兜住残差 —— 两层分工,缺一个要么留偶发红、要么让整个矩阵陪跑等待。

### 改进

- **内带 xlings 升级到 `2026.8.4.1`**(自 `2026.7.28.4`)。该版本落地了索引快照的**版本契约与自动路由**(openxlings/xlings#476,由本仓库提出):索引声明它需要的客户端版本,客户端自动路由到自己能用的**最新**快照,版本错配从**硬失败**变成**路由决策**。

  对 mcpp 的直接意义是它**补上了 mcpp 自己做不到的那一半** —— mcpp 不下载索引(`update_index` 就是 shell out 给 `xlings update`),此前无法要求「给我索引版本 X」。新增的 `xlings index list --json` 会把 `requires` 里非 `xlings` 的键**原样透传**给对应消费者,`xlings index use` 提供钉选。mcpp 侧消费这套接口(按 `min_mcpp` 自动路由)是后续工作,需要索引侧先声明 `requires.mcpp`;本次只做版本升级与验证。

  16 个 pin 点由 `check_version_pins.sh` 机器校验并全部更新;已实测 mcpp 在 `2026.8.4.1` 下 `new`/`build`/`run` 正常。


## [2026.8.3.5] — 2026-08-03

### 修复

- **索引侧的改动不再能让 mcpp 从「能用」变成「不能用」。** 一个索引树可以声明客户端版本下限(`index.toml` `min_mcpp`)。此前 `xlings update` 会**原地覆盖**本地索引,一旦上游抬了下限,一台一分钟前还好好的机器就报

  ```
  error: index requires mcpp >= 2026.8.3.3 but this is mcpp 0.0.109 [E0006]
  error: ... package 'compat:compat.libarchive@3.8.7' not found
  ```

  而且**没有退路** —— 旧的、能用的那份树已经被覆盖掉了。**刷新本身就是把机器弄坏的那个动作。**

  刷新现在是**单调**的:判据是「刷新前能用 ⇒ 刷新后必须仍能用」。`mcpp::xlings::update_index`(全进程唯一的刷新入口)在同步前归档每一棵可读的索引树,同步后复核;变得不可读就回滚,并提示可以升级 mcpp 拿新包。2026-07-08 的索引设计里写过这个行为("staged refresh keeps the last compatible snapshot"),**从未被实现** —— 下限只在描述符读取处检查过一次,刷新路径对它一无所知。

- **不再把「这个索引读不了」伪装成「这个包不存在」。** 违反下限会让该树的每次描述符读取返回空,与「包确实不在」不可区分。后果有两层:终止构建的那条错误是 `dependency '...': not found`,**把责任推给包**(指向发布/命名),而真正的答案(升级 mcpp)早已滚出屏幕;同一个无法区分的 miss 还会喂给刷新策略,被读成「本地缺东西,去拉」—— **不可用状态自己在驱动重复刷新**。

  「这个索引不可读」现在是一个可查询的事实:刷新策略据此不再徒劳重试,而每一处终止构建的 not-found 都会带上真正的原因。

### 改进

- **`mcpp cache`/索引之外的一处分层修正:`MCPP_VERSION` 移到叶子模块 `src/version.cppm`。** 它此前住在 `mcpp.toolchain.fingerprint` 里,而后者 `import mcpp.toolchain.detect` → `import mcpp.xlings`,于是「这个二进制是什么版本」这件事**传递依赖了整个工具链探测与包管理子系统**。这不是洁癖:`mcpp.pm.index_contract` 只为一次比较需要这个常量,却因此依赖上 xlings,反过来使得 xlings **不可能**依赖 index_contract —— 而刷新守卫恰恰必须装在那里。**编译器报的那个循环依赖,就是分层在告诉我们常量放错了地方。** 版本号的唯一真源随之移动;`check_version_pins.sh` 与发布文档同步更新。

- **补上了从未存在的端到端覆盖。** 下限行为此前只有纯谓词单测(`floor_violation()`),`tests/e2e/` 里一条都没有 —— 这正是上面两个缺陷能长期存在的原因:谓词是对的,**谓词周围的行为从没被端到端跑过**。新增 `tests/e2e/185_index_floor_degrades.sh`(错误必须点名真正的原因、不可用索引不得波及健康索引、逃生舱仍有效)与 `tests/unit/test_index_snapshot.cpp`(9 条,含「刷新弄坏索引必须回滚」)。


## [2026.8.3.4] — 2026-08-03

### 修复

- **全局 build cache:同一个 key 下第二个消费者必挂 `missing and no known rule to make it`(#344)。** 一个依赖的 `.o` 存在 cache 条目里的**路径**,此前取自消费方 build dir 的相对路径。而 build dir 里的对象布局由 #233 的 basename 消歧决定,消歧的判据是一次**跨越整个 build dir**的普查 —— 也就是说,它取决于消费方还拉了哪些别的包:

  - 同时拉了 `compat.zlib` 与 `compat.bzip2`(两者上游各有一个 `compress.c`)→ `obj/compat_zlib/zlib-1.3.2/compress.o`
  - 只拉了 `compat.zlib` → `obj/compress.o`

  cache key **刻意不含消费方**(这正是跨工程共享成立的前提),于是两种布局落进同一个条目,后跑的那个工程按自己的布局去取,必然缺一个文件。失败发生在 ninja 的 **graph 加载**阶段 —— 一条命令都还没跑,上一行却刚打印过 `Cached … (15 units)`。mcpp-index 全量 workspace 在三个平台上共 32 个成员因此失败。

  条目内的对象地址现在是**包自身的纯函数**:镜像源文件相对它**自己**包根的路径,不含任何消费方信息。构建目录里,依赖包的对象一律落在 `obj/<pkg-slug>/…` 下 —— 跨包撞名结构性地不可能发生,依赖包因此完全退出消歧普查。根工程(永不入 cache)保持沿用至今的扁平 `obj/<name>.o`。

  #233(编译边撞名)、#240(链接输入未跟改名)与本条是同一台机器的三个产物:**布局由一次全局普查决定**。所以修的不是再补一处同步,而是把依赖包从普查里彻底拿掉。

- **链接/归档命令一律走 response file,不再有「项目大到一定程度就崩」的隐形上限。** 此前只有 Windows(CreateProcess 32 KiB)与 msvc 方言用 rspfile,POSIX 走内联 `$in`,理由写的是「ARG_MAX 很宽裕」。两半都错:ninja 在 POSIX 上是 `sh -c "<整条命令>"`,整条命令是**一个 argv 项**,撞的是 `MAX_ARG_STRLEN`(32 页 = 128 KiB)而不是 2 MiB 的 `ARG_MAX`;而且它从来就不宽裕 —— 实测 mcpp-index 的 `opencv-module`,内联链接行**本来就已经 56840 字节**,占那条无人看守的上限的 43%。

  上面 #344 让依赖对象路径变长(多一层包目录),同一条边到了 161687 字节,于是 ninja 直接 `ninja: fatal: posix_spawn: Argument list too long` —— **不报是哪条边、哪个文件、什么原因**。构建系统不能有一个「靠崩溃才被发现的项目规模上限」,「这条命令有多长」也不应该是选对象路径时需要有人记在脑子里的事。clang/gcc driver、link.exe、GNU ar、llvm-ar 全都认 `@rspfile`,现在全平台一个规则形态。

### 改进

- **cache 条目与本次构建的布局分歧,现在降级为 miss 并明确报告,而不是让 ninja 崩在图加载阶段。** `is_cached` 此前校验的是条目**自述的**文件表,而消费方随后按**自己算的**地址去取 —— 两处独立推导,从不比对。命中判据现在校验「本次实际要读的那批产物」,任何不匹配都只是一次重编。同时新增一行 warning:一个系统性的分歧否则会表现为「cache 永远不命中」而毫无信号,这正是 v2026.7.30.2 之前那个假 `Cached` 骗了三个月的失败模态。

- **可缓存性改判磁盘出处,不再只认标签。** 规则一直写着「无法证明来自不可变的 xpkgs store 就不准入」,但代码实现的是更弱的代理(`sourceKind == "version"`)。多版本共存(mangling)会把消费方包的根重锚到 `<project>/target/.mangled/…` 并**改写其源码**,而标签仍是 `"version"` —— 它今天不出错只靠轴 F 侥幸。现在按包根的实际位置判定。同一批还加了「全有全无」:任何一个单元拿不到与机器无关的条目地址,整个包退出缓存,不留下半 staged 的包(那会表现为三条边之后的 BMI CRC mismatch)。

- **`mcpp cache verify` 现在会报告逃出条目的对象地址。** 让「条目地址必须是包内相对路径」这条不变量可以离线审计,而不是只能通过复现一次双工程构建才看得见。

- **`kCacheEpoch` 1 → 2。** 产物布局变了,旧条目描述的是本版本不会去要的布局。它们本来就会被判为 miss,但让两套布局共用一个目录会让 `cache gc` 的体积统计和 `cache verify` 的输出失去意义。用户侧表现为一次全量重建,无需任何手工步骤。

## [2026.8.3.3] — 2026-08-03

### 修复

- **交叉构建每次都重新链接(增量构建对 PE 目标实际失效)。** `plan.cppm` 的 `target_output()` 用 `mcpp::platform::{exe_suffix,lib_prefix,static_lib_ext,shared_lib_ext}` 拼产物名 —— 这四个是**主机**常量,由 `#if defined(_WIN32)/__APPLE__` 选择。主机构建下「这台机器叫什么」与「产物该叫什么」恰好同解,所以它一直没被发现;`host ≠ target` 时两者分岔。

  后果不是命名难看:Linux 交叉到 PE 时,ninja 被告知产出 `bin/foo`,而 mingw 的 GCC driver 写出的是 `bin/foo.exe` —— **声明的那个文件从来不存在**,ninja 每次都发现输出缺失并重跑链接边。这条路径正是 CI 每天在跑的。

  产物命名现在由 `ArtifactNaming` 决定,每个 plan 从 target triple 求值一次。**它是 (os, env) 二元函数,不是 os 一元**:`x86_64-windows-gnu` 用 GNU 约定(`libfoo.a`),`x86_64-windows-msvc` 才是 `foo.lib`。空 triple 表示「为本机构建」,只有那时主机答案才是对的,因此它作为回退传入而非被直接读取 —— **主机构建逐位不变**。

  同样的处理给了 `shared_library_link_flags`:消费者链接一个共享库时用完整路径(PE)、`@loader_path`(Mach-O)还是 `$ORIGIN`(ELF),是**产物**的属性;按主机求值在交叉时方向就是反的。

- **`windows-gnu` 静态库命名错误(行为变更)。** 在 Windows 主机上用 mingw 工具链构建静态库,产物此前叫 `foo.lib` —— 一个 GNU archive 顶着 MSVC 的名字,MSVC 拿不去用。现在按 GNU 约定命名为 `libfoo.a`。这修正的是一个**今天就是错的**名字,与交叉编译无关。

### 改进

- **非 ELF 目标上声明 `SharedLibrary` 现在明确报错,而不是产出未经验证的东西。** 全部 5 个共享库 e2e 都声明 `# requires: elf`,而这个 capability 只在 Linux 上授予 —— 也就是说共享库在 PE 与 Mach-O 上**从未被端到端验证过**。相关分支是推测代码:mingw 的 ld 容忍直接链 `.dll`,MSVC 的 `link.exe` 不行,而两者都没有 import library 可链,因为 mcpp 还没有建模它。

  一段既没有测试覆盖、又不肯明确拒绝的分支是最难清理的债 —— 它既不能被信任,也不能被删除,因为没人知道谁在依赖它。先把边界写死,等真要支持时再补(前置条件是先有 PE/Mach-O 的共享库覆盖)。

## [2026.8.3.2] — 2026-08-03

### 新增

- **在 Windows 上构建 Linux 程序(`--target x86_64-linux-musl`)。** 补上 `host ≠ target` 最后一个空象限:一台 Windows 机器直接产出**完全静态的 Linux ELF**,不需要 WSL、不需要容器、不往系统里装任何东西。

  ```bash
  mcpp build --target x86_64-linux-musl        # 在 Windows 与 Linux 上拼写完全相同
  ```

  命令两边一字不差,因为「cross」在 mcpp 里从来不是一个名字,只是 `host ≠ target` 这个关系的取值。由谁来服务这个目标是自动解析的:Linux x86_64 主机装原生 `musl-gcc`,Windows 主机装 **canadian cross**(build=`x86_64-linux-gnu` / host=`x86_64-w64-mingw32` / target=`x86_64-linux-musl`)。两者都是 GCC 16.1.0,都带 `bits/std.cc`,所以 `import std` 在哪边都一样能用。产物无 `PT_INTERP`,任何 Linux 发行版上都能跑,与它的 libc 无关。

  不支持的两种组合是明确拒绝而非碰运气:Windows → `linux-gnu`(glibc 需要 `xim:glibc` / `xim:linux-headers` 两个 sysroot 载荷,只为 Linux 主机发布),以及 Windows → 跨架构(canadian cross 载荷按主机架构构建)。`mcpp toolchain list` 只列当前主机真能装的目标,所以某个目标没出现在 Targets 块里,就是这台机器确实服务不了它。

### 修复

- **`-static` 由构建主机而非目标决定,导致 Windows→Linux 交叉产物根本不是静态的。** `supports_full_static` 是一个**主机**常量(`is_linux`),描述的是「这台机器自己的二进制能否全静态」;`flags.cppm` 却拿它来决定**产物**要不要 `-static`。在 Linux 主机上这两个问题对所有 Linux 目标恰好同解,所以它一直没被发现 —— 只有当非 Linux 主机交叉编译到 Linux 时才会现形:`-static` 被静默丢弃,而 `x86_64-linux-musl` 这个目标存在的全部理由就是产出可移植的静态 ELF。没有报错,没有警告,那个 flag 就是不在。

  判据改读解析后的 triple:空 triple(目标即主机)沿用主机答案;PE 目标返回 false,它们的 `-static` 来自 C++ 运行时分发契约,在这里也答 true 会让两套机制都发一遍;macOS 返回 false(libSystem 必须动态);Linux 返回 true。今天所有能工作的路径逐位不变。

- **Windows 主机上交叉工具链的前端永远找不到。** 候选名是 `{ "<triple>-g++", "g++" }`,用 `filesystem::exists` 解析,而 Windows 上的文件叫 `<triple>-g++.exe` —— 载荷装得好好的,然后不可用。`archive_tool` 的 musl 分支(`<triple>-ar`)有同样的遗漏。

### 改进

- **「这台主机能否服务这个目标」收敛为单一判据 `host_can_serve`。** 它此前在两处独立推导:`registry.cppm` 选载荷时一次,`lifecycle.cppm` 决定 `toolchain list` 能否把目标标为 `available` 时又一次 —— 而且两者**已经漂移**:载荷侧会解析出一个可用性侧宣称不可能存在的 windows-hosted musl 包。现在判据只有一份,就放在它必须与之一致的载荷解析旁边。

## [2026.8.3.1] — 2026-08-03

### 修复

- **`[build] static_stdlib = false` 对测试二进制静默失效(#336)。** #124 在 0.0.52 明确写下这个 opt-out,`docs/05-mcpp-toml.md` 至今也还这么写;但 #202(0.0.86)把测试二进制改成与分发目标相同的静态 `-load_hidden` libc++ 时,新增的那条推导**没有带上 `staticStdlib` 门**。结果是约一年时间里 macOS 上的 `mcpp test` 没有任何办法回到动态 libc++,而文档一直在承诺它可以。

- **macOS 上全局对象在静态初始化期访问 `std::cout` 必崩(#336)。** Mach-O 没有按优先级排序的初始化段(`init_priority` 只在单个 TU 内有效),归档成员的初始化器按链接顺序排在最后;而 libc++ 的 `<iostream>` 不像 libstdc++ / MSVC STL 那样自带 `ios_base::Init` 守卫,流的构造只存在于库内对象里。两件事叠起来的后果是:默认配置下,任何在构造函数里碰 `std::cout` 的全局对象都会读到 vptr 为零的流,进程启动即 SIGSEGV —— 而且**包侧无法修复**,因为 `std::ios_base::Init` 在 libc++ 的头文件里只有前向声明(`ios:70`),标准为静态初始化次序提供的官方解药在 libc++ 上用户根本写不出来。

  修法是让静态链接恢复动态链接本来就有的保证:macOS + 自包含时,mcpp 生成一个极小的 C 翻译单元并把它的对象排在链接行**最前**,由它先把流顶上去。它对 `ios_base::Init::Init()` 的引用是 **weak** 的 —— 换一份不这么拼这个 ABI 符号的工具链,链接与今天完全一样,shim 退化为空操作。

### 新增

- **C++ 运行时分发契约 `[build] cxx_runtime`。** 三档:`self-contained`(默认)/ `toolchain-coupled` / `host-coupled`,可按角色(`{ default = ..., tests = ... }`)也可按目标三元组(`[target.<triple>] cxx_runtime`,与 `linkage` 并列 —— 它们本就是同一根轴)。`static_stdlib` 保留为忠实别名(`true`↔`self-contained`,`false`↔`host-coupled`)。

  这个字段替换的旧字段名描述的是**手段**("静态链接 stdlib"),而它承载的其实是**意图**(产物能在哪些机器上跑)—— 这正是同一个 `true` 在四种配置上展开成四种不同结果的原因,其中一种是**静默空转**:Linux + clang/libc++ 工具链上 `static_stdlib = true` 一个 flag 都不发,交付的是工具链耦合的产物,而 manifest、文档和 `--version` 都说它是自包含的。

- **Linux + libc++ 工具链现在真的能自包含**:显式链入 `libc++.a` / `libc++abi.a` / `libunwind.a`(缺 libunwind.a 时产物仍会拉 `libunwind.so.1`,所以它是机制的一部分而不是可选项)。实测 `NEEDED` 只剩 libc / libm / loader。

- **兑现不了的契约一定会被报出来。** 工具链不带 `libc++.a`、macOS 没有 deployment floor、MSVC 运行时没有 `/MT` 机制、macOS 上没有可用的 `toolchain-coupled` 形态(LLVM 的 libc++abi/libunwind dylib 向上链 `/usr/lib/libc++`,会把第二份 libc++ 载进进程)—— 这些格子现在都会打印实际退到了哪一档。

### 变更

- **五处独立推导收敛成一处。** "这个产物自带 C++ 运行时吗"过去在 `flags.cppm` 的 `ldStdlibDefault` / `ldStdlibTest` / `-static-libstdc++` / MinGW `-static` 四处,加上 `ninja_backend.cppm` 里那个按 `LinkUnit::TestBinary` 的二分派,各推一遍 —— 这正是新语义只落到其中一处的成因。现在是 `src/build/distribution.cppm` 里的三层模型:角色(由链接单元内在决定)→ 契约(按角色取默认,可覆盖)→ 机制(唯一放 flag 的地方,且是**总函数**)。

- 相应地,C++ 运行时相关的链接 flag 从全局 `ldflags` 移到了**每个链接单元**的 `unit_ldflags` —— 两个角色在同一次构建里可以持有不同契约,这一点全局通道表达不了。它们都是驱动级 flag,相对库的位置无意义,Linux/Windows 的链接语义不变。

## [2026.8.1.2] — 2026-08-01

### 新增

- **`mcpp.lock` 成为 git 依赖的解析输入,而且是权威的那一份。** 在此之前 lock 只写不读:`branch = "develop"` 这类依赖每次进 `prepare_build` 都要打一次 `git ls-remote`,而写进 lock 的那个 commit 从来没有人看过。现在**先读 lock**——分支已经解析过就直接用记录的 commit,不再有网络往返。

  「权威」是这里的关键词,不是「缓存的提示」。一个只在本地克隆恰好还在时才生效的 lock,等于把「构建哪个 commit」这个决定交给了缓存目录的存活状况:清一次 `~/.mcpp/git`、或者换一台机器 clone 同一个工程,构建就会静默滑到分支的新头上,并把 lock 改写成新值——**而 `mcpp new` 生成的 `.gitignore` 并不忽略 `mcpp.lock`,它本来就是要提交的**。实测(见 e2e 24 新增断言):分支移到 v2、缓存删除后,旧行为构建出 v2,新行为构建出 lock 里记的 v1。要新的分支头依然只有一条路——`mcpp update <dep>`,它丢掉 lock 条目,下次构建重新解析。

  连带把这块的结构收敛成两个各自独立的问题,各带**恰好一个** `--offline` 闸:**(1) 是哪个 commit** —— `tag`/`rev` 自带答案,`branch` 由 lock 回答、否则 `ls-remote`;**(2) 它在不在盘上** —— commit 选定缓存目录,miss 就 clone。原来 tag/rev 有一条自己的分支腿、打一行每次构建都出现的 `from cache` 噪声,并且在 dep 根本没进 lock 时报「is locked but its local cache is missing」;这条腿现在整个不需要了。

### 修复

- **`--offline` 不再拒绝本地 git 远端。** `docs/05-mcpp-toml.md` 写明 `--offline` 的语义是「完全不碰网络……已安装的东西照常构建」,`prepare.cppm` 里依赖下载闸的注释也把线画在同一处:「这条线以上全是本地操作,一个依赖齐备的离线构建必须成功」。但 `git = "../sibling-repo"` 这种指向本地目录的远端,`ls-remote`/`clone` 都只是文件系统读取,拒绝它买不到任何隔离性。现在按远端形态判定——`file://`、以及不带 scheme 也不是 `git@host:path` 的存在路径,算本地(Windows 盘符 `C:\repo` 含冒号但不含 `@`,因此仍归本地)。

- **克隆被中途杀掉后不再永久提供错误的 commit。** 缓存目录以 commit 命名,但内容是 `git clone` 之后再 `git checkout` 两步做出来的;进程死在两步之间,目录名和 HEAD 就对不上了,而后续构建只检查目录存不存在。现在分支依赖会比对 `git rev-parse HEAD`,不符即删除重克隆(tag/rev 以 ref 名为身份,无可比之物)。

- **git 克隆在 Windows 上不再依赖 `cd` 跨盘。** 克隆命令用的是 `git clone … && cd <dir> && git checkout`,而 cmd.exe 的 `cd` 不带 `/d` 是不换盘的——缓存根(`MCPP_HOME`)与工程不同盘是常态。仓库里其它跨盘位置(`process.cppm`、`msvc.cppm`)都写了 `cd /d`,这里漏了。改用 `git -C <dir>`,连 shell 都不需要。

- **`mcpp.lock` 读取失败改用 degraded 通道。** 按 `src/diag.cppm` 的划分,「引擎做得比要求的少」属于 Degraded 且必须给出 impact——lock 读不出来,每个 git 分支依赖都会退回网络解析、并可能越过记录的 commit。原来记的是普通 warning,`--strict` 提升不到它。

- **`parse_git_source` 不再切开名字里带 `@` 的 tag/rev。** 写侧只对 `branch` 追加 `@<commit>`,解析侧却无条件按最后一个 `@` 切分,于是一个叫 `v1.0@rc1` 的 tag 会被读成 ref `v1.0` + commit `rc1`。现在只有 branch 走这条切分(且仍按最后一个 `@`,以容纳 `feat@v2` 这类分支名)。

## [2026.8.1.1] — 2026-08-01

### 修复

- **`mcpp` 的进度输出在非 TTY 下不再被 libc 缓冲区吞掉。** 全部状态行此前都是裸 `std::println` 且**无一处 flush**,仓库里也没有 `setvbuf` —— 于是「什么时候能看见」完全由各平台 libc 的缓冲区大小决定,而这个数字三个平台都不一样:musl(Linux 发行版的链接方式)写死 `BUFSIZ = 1024` 且忽略 `st_blksize`,Apple libc 取 `st_blksize`(管道上是 65536),MSVCRT 用 4096 且**不支持行缓冲**。

  实测同一个 97 成员工作空间的同一段 ~13 KB 状态输出:Linux 冲了 13 次,macOS **0 次**。macOS 的 CI 日志因此只有测试二进制自己的输出(它们是独立进程,各自退出时 flush),mcpp 自己的**一行都没有**;等这一步被 job timeout 杀掉时,整个缓冲区连同那 13 KB 一起没了 —— 一个 45 分钟、零可归因输出的步骤。

  现在 `main()` 把 stdout 设为行缓冲,`mcpp::ui` 的每个写 stdout 的函数额外显式 flush(Windows 上 MSVCRT 会把 `_IOLBF` 当 `_IOFBF`,所以两条腿都要有)。实测:同一条命令从 3 次块写变成 391 次逐行写。

- **`mcpp test` 的 `finished in` 不再漏算构建时间。** 计时起点此前在包级构建与批量测试构建**之后**,只覆盖逐测试循环。实测一个成员打印 `finished in 6.53s`,真实墙钟 **93.5s** —— 14 倍的低报,而且**恰好在构建最重、最需要这个数字的成员上最不准**。现在起点移到 Phase A 之前,并拆成 `finished in 93.5s (build 87.0s + run 6.5s)`:测试只要几毫秒、链接要 90 秒的成员,在合并数字里和「测试套件很慢」长得一样,拆开才分得出。

- **`--message-format json` 的 stdout 不再被非 JSON 行污染,且污染是不对称的。** 静音标志此前由 `run_tests` 自己设置,而 `--workspace` 的 `Workspace testing member` 表头由扇出层在调用**之前**打 —— 第一个成员的表头漏进 NDJSON 流,第二个起被静音。现在扇出层在第一个成员之前就静音。

### 新增

- **`mcpp test` 默认有界。** `--timeout` 的默认值从 `0`(不限)改为 **300 秒**;`--timeout 0` 仍然表示不限,只是现在要显式要求。无人值守的 CI 不该被一个挂住的测试吃掉整个 job,而「不限」作为默认值恰恰保证了它可以。

- **`--build-timeout`(默认关闭)—— `--timeout` 覆盖不到的另一半。** 此前 `--timeout` 只包住测试**进程的运行**,而 `mcpp test` 的三次 ninja 驱动(包级构建、批量测试构建、逐测试构建)**全都没有期限**。实测一个 macOS lane 卡在某成员的 14 次可执行链接上超过 44 分钟,`--timeout` 设多大都无效。现在每次驱动各自独立计时,超时报成该成员的构建失败并继续下一个成员,失败行明确写「build timeout」而非笼统的 compile 失败。

  **它与 `--timeout` 不同,默认不开,这个不对称是实测出来的而非风格选择**:单个测试跑过 5 分钟不寻常,而冷依赖构建跑过 15 分钟很平常 —— mcpp-index 有一个成员要从源码建 OpenCV,实测 linux 1019s、windows 1289s。给它一个默认上限会把「慢但正确」的构建判红并让人以为是 mcpp 的错。构建可以跑多久是工程自身的性质,所以由工程来说;mcpp 只需要让「说得出来」成为可能 —— 而这正是原先缺的东西。

  平台限制如实说明:deadline 执行器在 Windows 上没有 kill-by-handle 路径,该值被忽略(POSIX only)。

- **`--workspace-timeout`(默认 0 = 不限)。** 扇出是串行的,一个没有上界的成员会拖住它后面的所有成员。超时后停止扇出、**如实汇总已跑完的成员并列出未运行的**,而不是把进程留给 CI 去 kill(那会把它想说的话一并丢掉)。

- **`mcpp test --workspace` 有了进度与计时。** 逐成员 `M/N` 进度、逐测试耗时(`t1 ... ok (0.31s)`)、逐成员耗时,以及 workspace 级汇总 + `slowest:` 榜:

  ```
   workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
      slowest: libs/jsc 93.5s, libs/install 32.2s, libs/http 24.1s
  ```

  此前扇出只打 `all N member(s) passed` —— 定位「哪个成员吃掉了墙钟」只能靠反解 CI 日志时间戳。

- **JSON 记录带成员限定。** 每条 test 记录新增 `"member"`,`summary` 新增 `"member"` / `"build_ms"` / `"run_ms"`,流末尾新增一条 `workspace_summary`(含 `failed_members` / `not_run`)。一旦两个成员都有名为 `smoke` 的测试,裸测试名就不再可归因。

- **macOS 上不注入运行期库路径这件事,现在会说出来。** `runtime_library_path_key()` 在 macOS 返回空串(理由正确:`DYLD_LIBRARY_PATH` 会波及 ninja 启动的每个可执行文件,可能让系统框架加载到私有 libc++)。后果是依赖 `[runtime] library_dirs` 的测试在 Linux/Windows 通过、在 macOS 以 dyld 错误失败,而这条差异此前**零诊断**。现在当计划里确有 `runtimeLibraryDirs` 时给一条点名平台与 rpath 兜底的警告。

分析与实施计划见 `.agents/docs/2026-07-31-test-workspace-observability-analysis.md` 与 `.agents/docs/2026-07-31-test-observability-implementation-plan.md`。

## [2026.7.31.1] — 2026-07-31

### 新增

- **`standard = "c++20"` 成为一等档位(默认仍是 `c++23`)。** 白名单新增 `c++20` / `c++2a` / `gnu++20` / `gnu++2a`。C++20 是这个白名单的地板 —— 命名模块本身就是 C++20 特性,再往下 mcpp 的构建模型不存在。

  **`import std;` 在 c++20 依然可用。** 它虽然是 C++23 的*库*特性,但三家实现都在 C++20 模式下提供 `std` 模块:libstdc++ 的 `bits/std.cc` 与 libc++ 的 `std.cppm` 都没有 `__cplusplus` 守卫,MSVC STL 在 [microsoft/STL#3977](https://github.com/microsoft/STL/pull/3977) 解禁(修复 [#3945](https://github.com/microsoft/STL/issues/3945),原话:C++20 的封锁"是纯策略选择,没有技术原因")。本次实机验证了 gcc 15.1.0 / 16.1.0 × glibc / musl / mingw-cross 与 clang 22.1.8 + libc++(含 `std.compat`):在 `-std=c++20` 下建 std 模块、编译消费者、链接、运行全部通过(musl 与 mingw 分别以 `-static` 在本机与 wine 跑通)。

  **不改默认、不改模板**:未声明 `standard` 的工程指纹逐字节不变,`mcpp new` 与所有示例继续用 C++23。C++20 是给外部约束(公司内规、只到 C++20 的第三方 API)准备的逃生舱,不是新推荐值 —— 它意味着放弃 `std::print` / `std::expected` 等 C++23 库设施。

  缓存零改动:标准早已进入指纹、`import std` 的 BMI 身份与依赖构建缓存键,所以 c++20 与 c++23 各自拥有产物目录与 std BMI。这一点由编译器本身兜底 —— 跨档位复用 BMI 会被直接拒绝(`language dialect differs 'C++20', expected 'C++23'`)。

- **`import std` 的可用性判定从「有没有」升级为「从哪一档起」。** `Toolchain::importStdMinLevel` 由各 provider 自己填(GCC / libc++ 答 20;MSVC 按 cl banner 版本答 20 或 23)。STL#3977 之前的 MSVC 仍然封锁 C++20,那些机器上 `import std;` + `/std:c++20` 现在得到一句指名工具链与工程档位的可行动错误,而不是 `std.ixx` 内部的报错。

### 修复

- **`build.mcpp` 在 `standard = "c++fly"` / `"c++latest"` 下拼出非法的 `-std=` 旗标。** 它此前用 `"-std=" + canonical` 直接拼接,而这两个值的 canonical 不是合法的 `-std=` 拼写,宿主编译因未知方言失败。现在与主构建走同一个方言层(`cppfly::std_flag`),按真正执行编译的宿主工具链解析。

## [2026.7.30.3] — 2026-07-30

### 变更

- **索引刷新从「时间驱动」改为「解析驱动」(#315)。** `mcpp build/run/test` 此前只要刷新 marker 超过 1 小时,就无条件跑一次 `xlings update`(同步**全部**索引仓库,失败还带 3 次 2s/4s 退避重试)——不管有没有东西真的缺失。网络差时这就是「每小时一次、为已经在本地的数据等几分钟」。

  这不是「功能缺失」:offline-first 的策略**早就在仓库里**了(`xlings.cppm` 的 xim 安装门,注释里甚至点名了 Termux 上的 build hang),但 `prepare.cppm` 那道 TTL 门先开火,把它变成了不可达代码。同一个决策此前在 **5 处各推导一遍**,其中两处互相矛盾。

  现在只有一个真源 `mcpp.pm.index_refresh`:`mcpp build` / `mcpp add` / xim 安装门 / 安装失败重试全部走它。刷新只在三种情况发生 —— 本地没有索引、描述符不在本地、SemVer 约束在本地版本集内无解。**依赖全部能在本地解析的构建零网络请求,无论本地索引多旧。**

  **判据换轴的理由**:「索引够不够新」不可判定,而 mtime 是它的坏代理(CI 缓存恢复、时钟回拨、tar 保留时间戳都会让 marker 两个方向说谎)。可判定的问题是「解析器能不能用磁盘上的东西干活」——而**所有**解析输入都在磁盘上(描述符是文件,`resolve_semver` 解析本地 xpkg.lua)。marker 因此降级为纯去抖计时器。

  **最危险的一条判据**(`SuppressedInconclusive`):「本地查不到」单独不能推出「需要刷新」。xim 描述符不写 `namespace`,`(xim, x)` 永远匹配不上身份门 —— 把这种 miss 当真,任何带 xim 依赖的工程会**每次构建都刷**,比被删掉的 TTL 更糟。判据复用 `IndexRoute::authoritative_for`(#307),单测 + e2e 双闸锁住。

  语义变化:`^1.2` 对**本地索引已知的版本**求解。上游新发的 1.3.0 需要 `mcpp index update` 或 `mcpp update` 才可见 —— 这正是那两个命令存在的意义,已写进 `docs/05-mcpp-toml.md`。

- **`mcpp update` 不再是空操作。** 它此前只删 mcpp.lock 条目、然后叫用户去跑 `mcpp build` —— 而构建路径**从不读 mcpp.lock**(`prepare` 只写不读),所以删了等于没删,行为影响为零。它现在先强制刷新索引(显式意图 ⇒ 不看 TTL、不看去抖),并报告索引 rev 的变化;工程里没有任何走共享 registry 的依赖时跳过(刷了也没用)。

- **新增 `--offline` / `MCPP_OFFLINE=1`。** 一次调用内完全不碰网络:不刷索引、不下载包、不自动装工具链。已安装的东西照常构建 —— 检查点都放在「真要下载」的那一刻,而不是更早。`MCPP_NO_AUTO_INSTALL` 作为它的旧式窄化拼写保留(只管工具链),文档标注 deprecated:同一个概念此前有三个名字。

- **新增 `[index] auto_refresh`(全局 `config.toml`)。** 机器级关闭自动刷新,下载仍可用。刻意做成布尔而**不**保留旧 TTL 模式:为模拟一个正在删除的策略而留第二条代码路径,正是本次要还的那笔债。策略归全局配置而非 `mcpp.toml` —— 它描述机器的网络环境,写进工程清单会让工程在内网/家里/CI 之间不可移植。

### 修复

- **未来时间戳的索引 marker 被判为「永远新鲜」。** `age < ttl` 对负数恒真,所以时钟回拨、容器时间、`tar` 保留 mtime、CI 缓存恢复产生的未来 marker 会让索引一直「新鲜」到墙钟追上为止。不可用的时间戳现在一律读作 unknown,而 unknown 必须是 stale。

- **索引刷新此前没有任何并发保护。** BMI 缓存早就用着 `platform/fs.cppm` 的跨平台 flock/LockFileEx,索引一个锁都没有 —— 并行 CI job 共享 `MCPP_HOME`、多个终端同时构建,就会并发重写同一棵树并并发写 marker。改为非阻塞互斥:**拿不到锁就跳过,不排队、不报错**(持锁者正在做的就是我们想要的事,排队只会把本 issue 要消除的症状原样复制一遍)。

- **项目级 env 下刷新永不打标。** `mark_known_indexes_refreshed` 在 `projectDir` 非空时整段早退,于是带自定义 `[indices]` 的工程刷新完全局索引却不留痕,`mcpp index status` 对这些机器永远显示 unknown。

### 新增

- **`mcpp index status` 增加 revision 列。** 索引自带内容身份 `.xlings-index-version`(artifact 分发把 rev 打进文件名:`mcpp-index-8d67478.tar.gz` → `8d67478`),mcpp 此前**零引用**。它是唯一能回答「两台机器/CI 缓存与本地是不是同一份索引」的信号,age 永远回答不了。按**不透明字符串**处理:子索引的值是日期版本号(`2026.7.30.1`)而不是 sha,探针实测,绝不解析。

- 依赖解析失败时附带索引身份与年龄(`index: local index 8d67478 (refreshed 12d ago)`)+ `mcpp index update` 建议 —— 变懒之后,「没这个包」和「你的索引是上个月的」必须能被区分开。

## [2026.7.30.2] — 2026-07-30

### 修复

- **依赖的全局缓存此前净收益为零 —— 命中也全量重编。** 命中的依赖产物由 `prepare_build` 直接拷进 build dir,而这些路径在 `build.ninja` 里仍然是 **compile edge 的输出**;ninja 对「输出存在但 `.ninja_log` 里没有该输出的命令行记录」一律判脏,而新 build dir 天然没有 `.ninja_log`。ninja 自己的话:

  ```
  $ ninja -C target/x86_64-linux-gnu/<fp> -d explain -n
  ninja explain: command line not found in log for obj/zutil.o
  ninja explain: obj/zutil.o is dirty
  ```

  于是每一个「命中」的 TU 都被重编一遍,而 CLI 照旧打印 `Cached <pkg>` —— 缓存只在「同一个 build dir 已经有 `.ninja_log`」时"生效",而那时本地产物本来就在。

  修法不是加 `restat`(log entry 根本不存在,restat 只在有 entry 时改写 mtime 判定),也不是给 compile edge 打 `generator = 1`(那会把「命令行变了要重编」整条规则关掉,用错误换性能)。命中的依赖**改发 `stage_file` 边、不发 compile 边、不发 scan/dyndep 边**:产物落在 compile edge 本来会产生的同一个路径上,所以链接行、消费者 TU 的 BMI implicit input、运行期部署边全都不变,变的只是「这些文件由谁产生」—— 而 ninja 从此有了可比对的命令行记录。

  状态行同时带上省下的 TU 数(`Cached compat.zlib v1.3.2 (15 units)`):光秃秃一个 `Cached` 能骗人三个月,一个必须与 ninja 实际跳过的边数吻合的数字骗不了。

- **`mcpp build --release` 之后裸 `mcpp build` 会 0.00s「成功」并交付 release 产物。** 与缓存无关的独立缺陷,同根于下面的 profile 轴:`.build_cache` 的条目**只按 target triple 去重**,而 fast path 的准入条件只挡显式 `--profile/--dev/--release`,挡不住裸 `mcpp build`(那恰恰是「我要默认 profile」的意思)。于是 fast path 拿着 release 的 `build.ninja` 跑,`build.ninja` 里写着 `-O2`,产物无 debug info。

  条目改按 `(target triple, resolved profile)` 去重,fast path 先用同一条纯规则(新的 `resolve_profile_name`,manifest 之外无输入)算出本次请求的 profile 再匹配;不匹配即当 miss 走完整 `prepare_build`。旧条目没有该字段 ⇒ 读成空 ⇒ 任何 profile 都不匹配 ⇒ 自愈,无需迁移。

- **`Finished release [optimized]` 是硬编码的。** 唯一调用点传字面量 `"release"`,`ui::finished` 又硬编码 `[optimized]`,所以 `--dev`(`-O0 -g`)也自称优化过的 release 构建。现在 profile 名来自本次真正解析出的 profile,描述符来自本次真正解析出的开关(`unoptimized + debuginfo` 等),没有调用方需要猜。

- **传递的 `path` / `git` 依赖被写进全局缓存。** 排除谓词在 **root manifest** 的 `dependencies`/`dev-dependencies` 里查该包并在 spec 为 path/git 时跳过 —— 但传递到达的包两个表里都没有,于是 `specIt == end()` 让谓词返回「不跳过」,本地源码被缓存,且 `indexName` 回落到默认索引(一个 workspace 成员 `B` 于是以 `mcpplibs/B@0.1.0` 的身份落在磁盘上)。它的源码可以在 `name@version` 不变的情况下改变,缓存键看不见这种变化。

  唯一可采信的证据改为**解析期记录的来源**(`DepCacheIdentity::sourceKind`,两个 push 点都在作用域内拿得到):非 `"version"` 一律不进缓存。判据方向与 `mcpp add` 的存在性门**相反** —— 那里「不可证伪就放行」,缓存这里「不能证明来自不可变的 xpkgs store 就不缓存」,因为这里的错误代价是静默的错误对象,不是被拒绝的命令。

  此前这个缺陷被上面第一条掩盖着(ninja 反正会重编);修好第一条之后它就是静默错误产物,所以两者必须同批。

### 变更

- **依赖缓存键:全工程指纹 → 每包 Merkle 键。** 旧键是整个工程的 fingerprint,而它的 flags 字段序列化了图里**每一个包,含 root** —— root 的包名、版本、`[build]` flags 都在里面。后果:改自己的版本号(`0.1.0` → `0.1.1`,实测 `3b8a8ae4fc217233` → `2138d7ce160e1154`)就让 std BMI 与**全部**依赖缓存整体失效;两个工程只要包名不同,即使依赖集与工具链完全一致,也一条都不共享。本机实测:26 GB / 1198 个指纹目录,`compat.zlib@1.3.2` 存了 162 份,`compat.x11@1.8.13` 73 份共 1.49 GB。

  新模块 `mcpp.build.cache_key` 按七个轴逐包成键:工具链身份 / 语言与方言 / **profile** / 包身份 / 该包自身的构建配置(含 features 折叠后的结果)/ **每个直接依赖的键(递归,Merkle)** / cache epoch。不含 root 的身份与 flags —— 实测 root 的 `[build] cflags/cxxflags` **不会**下发到依赖 TU,这正是跨工程共享成立的机制依据。

  F 轴之所以递归而不是「枚举上游的 public 接口」:GCC 把被导入模块 BMI 的 CRC 烙进导入者的 BMI,上游接口或 ABI 一变,旧的导入者 BMI 就 `module 'B' CRC mismatch` + `Bad import dependency`(手工三组对照实测:只改函数体通过,改签名/`-D` 改布局都硬失败)。可枚举清单必然漏项(上游 re-export 出去的传递模块接口在上游自己的 manifest 里根本看不出来),而失败模态是不对称的 —— BMI 轴取窄是编译器硬报错,`.o` 轴取窄是静默错对象。

  递归的代价是上游的**私有**变化会级联下游;对真正吃这个缓存的人群 ≈ 0:index 包的描述符按版本冻结,`B` 的键只能因版本 bump(那本就该让消费者失效 —— 它链接 `B` 的对象)或全图轴而变,而唯一能不改版本就动私有 flag 的 path/git 包整体不进缓存。

- **std 模块缓存键:全工程指纹 → std 自己的身份。** `stdmod.cppm` 写在产物旁边的 15 字段 metadata 一直就是正确完整的 std 身份,`metadata_matches` 一直在按它校验命中 —— 唯一错的是**目录名用的不是它**。本机实测代价:1014 个目录里只有 **15 个真身份**,16.10 GB(需要 ~0.5 GB),且 `mcpp version bump` 会重新 precompile 一次 std(几十 MB、十几秒)。(实现上有个自指要解:build commands 里含 cacheDir,而 cacheDir 由含它的 metadata 算出 —— 先用占位段生成一份规范化 metadata 算键,再用真目录生成落盘的那份。)

- **缓存布局迁到 `$MCPP_HOME/build-cache/v1/`,条目自描述。** `pkg/<index>/<pkg>@<ver>/<key16>/` 与 `std/<identity>/`。刻意不放 `$MCPP_HOME/cache` —— 那个名字已经归 `GlobalConfig` 的索引元数据缓存所有,而它的 reset 路径会整目录删除;把编译产物塞进别人会清空的目录里,会以「构建缓存有时会自己空掉」的形态浮现。`v1` 是布局版本段,所以换布局永远不需要迁移或删除旧树。

  每条目写 `entry.json`,记下**键的全部输入**;命中判定从「目录存在 + 文件齐」升级为「schema 匹配 ∧ 键匹配 ∧ 记录的输入与本次算出的输入**逐字段相等** ∧ 清单文件齐」。这是把 std 侧一直做对的事搬到依赖侧:此前依赖条目只有一份文件清单,所以怀疑命中错了的时候什么都审计不了。缺字段视为不匹配,**永不**因为 hash 相等就放行。

  旧的 `$MCPP_HOME/bmi/`(按全工程指纹分目录)**不读不写不自动删**:`mcpp doctor` 报出它的体积,`mcpp cache clean --legacy` 回收。它从来没产生过收益(见上面第一条),但删几十 GB 必须是用户的显式动作。

- **profile 成为失效轴。** `[profile.<name>]` 把开关落在 `buildConfig.optLevel/debug/lto/strip`,由 flags.cppm 变成 `-O<n>`/`-g`/`-flto`,而 `canonical_compile_flags` 只序列化 cflags/cxxflags/ldflags —— 于是 `--dev`、`--release`、`--profile dist` 三者**同一个 fingerprint**、同一个 `target/<triple>/<fp>/`、同一条缓存条目。

  **可感知的行为变化**:`target/<triple>/` 下从此每个 profile 一个哈希目录。好处是 dev↔release 来回切从「每次全量」变成增量,两个 profile 的二进制可以并存;代价是磁盘随实际使用的 profile 数增长(每个 build dir 带一份 staged std BMI,本机 `std.gcm` = 31,466,112 字节)。

- **缓存失效不再挂在 mcpp 版本号上。** 缓存键的兼容轴改用独立的 `kCacheEpoch`,只在缓存布局/键算法/staging 契约真的不兼容时递增。`fingerprint.cppm` 的 `MCPP_VERSION` 保持在 fingerprint 里(build dir 命名与本地增量归它管,宁可保守),但每次发版把整个缓存作废对纯 C 目标文件是过度失效。

### 新增

- **`--cache <mode>`:三种构建模式。** `global`(默认,读+写全局缓存)/ `local`(不读不写,所有依赖在本工程 `target/` 内编 —— 排障时一次性排除「是不是缓存的问题」,也给 CI 一个无共享的可复现基线)/ `off`(不读不写,并先清掉本次的 `target/<triple>/<fp>/` 做冷构建)。优先级 `--cache` > `MCPP_BUILD_CACHE` > `[build] cache` > `global`;无法识别的值会被报出来(`--strict` 下为错误),而不是静默回落到 `global`。

  `--no-cache` 保留为 `off` 的兼容别名。它的旧 help 文案「Force-clear target/ before building」两处不准:它清的是**构建目录**(`target/<triple>/<fp>/`)而不是整个 `target/`,而且名字与缓存无关。`mcpp run` / `mcpp test` 一并补上这两个 flag(此前它们连 `--no-cache` 都没有)。

  **`--no-cache` 的语义有一处收紧**:此前它只清构建目录、**仍然会回填全局缓存**;现在它等于 `off`,即**不读也不写**。想要「从零重编但仍然刷新缓存」的,用 `mcpp clean` 或 `rm -rf target` 后正常构建。这个收紧是为了让三个模式正交:一个叫 `off` 的模式还偷偷写缓存是说不通的。

- **`mcpp cache` 补齐到可运维。** `cache dir`(缓存到底在哪 —— 此前 `cache *`/`doctor`/`clean --bmi-cache` 各自解析根目录,而 config 的 reset 路径用 `GlobalConfig::bmiCacheDir`,两者可能不是同一个目录)、`cache gc --max-size <N>{MiB,GiB} / --older-than <N>{s,m,h,d}`(**真 LRU**)、`cache clean --deps|--std|--all|--legacy`、`cache list --json`、`cache verify`(逐条目校验清单与磁盘,残缺条目非零退出)。`cache info` 现在打印该条目的键输入 —— 怀疑命中错了时第一件想看的东西。

  `prune` 此前按**目录 mtime** 排序,而那只记录条目被**写入**的时间:一个每次构建都命中的热包,和一个一个月没人碰过的冷包一样「陈旧」。`entry.json` 的 `accessed` 由每次命中刷新(只重写 `entry.json`,**不动产物 mtime** —— 那些 mtime 参与 ninja 的 restat 判定),`gc` 按它排序。`cache clean` 开头那句 `remove_all(<root>/"deps")` 指向一个从不存在的路径(dep 条目在 `<root>/<fp>/deps`),是死代码。

  `gc` 刻意**不动 std 条目**:一份 std BMI 全机共享、重建要几十秒,为了腾一点磁盘赶走它是拿很多时间换很少空间。达不到容量预算时会明确说出来,而不是静默少删。

## [2026.7.30.1] — 2026-07-30

### 修复

- **[#311](https://github.com/mcpp-community/mcpp/issues/311) Windows 上 clangd 映射住 std BMI 会让整个构建报 `build failed`。** mcpp 把 staged std BMI 的路径写进 `compile_commands.json`,好让 clangd 解析 `import std;` —— clangd 于是把这个几十 MB 的文件 mmap 住;而 staging 步骤(`rule cp_bmi`)用 `powershell Copy-Item -Force` **原地覆写同一个文件**,Windows 拒绝替换带 user-mapped section 的文件(error 1224)。也就是说,mcpp 一边把这个路径交给别人读,一边在原地重写它。

  POSIX 侧看不见:GNU `cp -f` 在目标打不开时会 unlink 重建,POSIX 本身也允许覆写被 mmap 的文件 —— 所以 CI 一直全绿,只有 Windows 用户中招。

  staging 改为走 mcpp 自己的内部子命令 `mcpp stage`(形态对齐既有的 `mcpp dyndep`):**目标已等价就一个字节都不写**,否则先写同目录临时文件再 rename,再退化为原地覆写,失败按退避重试,最终失败时给出点名文件与可能持有者(clangd / 编辑器索引 / 杀软 / 上一次 `mcpp run` 还在跑的程序)的诊断。**绝不降级为 warning** —— staged BMI 过期或缺失会变成难以归因的 `module 'std' not found`,或者更糟:旧 BMI 配新 `std.o` 静默链接。

  「已等价」由**逐字节比较**判定 —— 无条件正确:内容相同就是不需要写。这个判断只在 ninja 已经认定 edge 脏了才会跑,所以代价可以忽略。(`--verify size` 保留给确知源是 fingerprint 作用域的调用方:build dir 与缓存目录共享同一个 fp,而 fp 已覆盖编译器身份/target triple/stdlib/std 源哈希/标准与方言 flag。但它**不是默认** —— 同一条 rule 还搬 DLL,而 PE 的节对齐让「真的重建了、大小却一样」十分常见。)dep BMI 缓存一直就是「已存在就不动」的(`bmi_cache.cppm`: *"Existing project outputs are left untouched"*)—— std staging 是全仓库唯一强制覆写的那一处,这个不对称本身就是缺陷。

  **verify 档位是 per-edge 的**:std BMI / `std.o` / `std.compat.*` 走 `--verify size`,Windows DLL 部署与 `runtime_alias` 走默认的逐字节比较。原因是判等要读目标就必须 open 它,而持有者可以连读都不给(Windows `FileShare.None` 映射 → open 即 `ERROR_SHARING_VIOLATION`);size 来自目录元数据、不需要 open。于是 #311 那条真实路径(clangd 映射 std BMI)**连排他锁都扛得住**,而可能过期的 DLL 仍逐字节把关 —— 前者靠 fingerprint 作用域保证等长即等价,后者没有这个保证。

  同一条 rule 也用于 **Windows 运行期 DLL 部署**,以及 Windows 上的 `runtime_alias`(PE 没有 soname 符号链接,别名就是刚建出来的 DLL 的副本),因此「往上一次 `mcpp run` 还加载着的 DLL 上覆写」这个同类失败一并治好。POSIX 的 `runtime_alias` 保持符号链接不变 —— 那里符号链接是语义,不只是写法。

- **重新 stage 一个未变的 std BMI 不再重编整张模块图。** staged BMI 是每个 importer 的 implicit input,而 staging rule 既不保留 mtime 也没有 `restat`,于是缓存侧 BMI 只要 mtime 变新(在下面那个缓存根缺陷下,**换个 cwd 跑就会发生**),所有 `import std` 的 TU 全部重编 —— 即使字节完全相同。

  修法是「不写字节」+ `restat = 1`。实测:`touch` 缓存侧 BMI 后,只有 staging edge 重跑,`main.cpp` 不再重编,下一次构建回到 `no work to do`。

  一条记录在案的实现约束:**跳过时对 mtime 的任何触碰(包括对齐到 src 的 mtime)都会让 restat 失效并重新引发级联** —— ninja 的 restat 只把「mtime 未被命令改变」的输出视为从未需要构建。所以跳过路径不动任何时间戳。

- **私有 glibc 的 strip 不再被「组合出的显式覆盖」绕过。** `process.cppm::merged_environ` 会把继承来的 `LD_LIBRARY_PATH` 里的私有 glibc payload 条目剥掉,但**显式 `extra` 覆盖是原样采用的**;而 `env::prepend_path_list` 组合该覆盖时,把继承值(含毒)整段追加了进去 —— 于是 strip 恰好在它唯一有用的场景下失效:嵌套的 `mcpp run` → `mcpp test` 链里,子工具拿到一个**版本不匹配**的 libc.so.6,在动态链接器里 main 之前 SIGSEGV(签名:一行裸 `__vdso_time`)。

  修法是把判据下沉到 `mcpp.platform.env`(路径列表组合的所在地),并让 `prepend_path_list` 只清洗**继承来的尾部**:调用方显式传入的 payload 目录必须保留 —— 那正是沙箱二进制需要的那一条。PATH 不受影响。

  这个缺陷与 #311 无关,是排查 e2e 156 在本 PR 上失败时找出来的:两次 CI 恢复了**不同的 sandbox 缓存**(`…-01baa227…` vs `…-0e74cc64…`),payload 版本集不同,于是同一个潜伏缺陷只在一侧显形。本改动前该测试的绿灯取决于「毒化的 payload 版本恰好与工具期望的一致」。


### 变更

- **BMI 缓存根统一为 `$MCPP_HOME/bmi`。** `toolchain/stdmod.cppm` 的 `default_cache_root()` 是 home 解析逻辑的一份私有拷贝,自 v0.0.1 起一字未改:**没有 Windows 的 `USERPROFILE` 分支,也没有 self-contained 安装探测**。后果是 Windows PowerShell(不设 `HOME`)下 std BMI 缓存落进**当前工作目录**的 `.mcpp-bmi/`,而 dep BMI 缓存在 `%USERPROFILE%\.mcpp\bmi` —— 一个缓存两个根,其中一个还随 cwd 漂移(从子目录跑就重编一次 std);release tarball 形态的安装在 Linux 上同样分家。

  新增叶模块 `mcpp.home` 作为唯一解析器,`config` / `stdmod` / `prepare` 的 git 缓存,以及 `doctor` 里另外两份拷贝(其中 `self init --force` 那份在 self-contained 安装下会去删错的树)全部收敛过来。单测锁死 `default_cache_root() == mcpp::home::bmi_root()`。

  **升级影响**:Windows 用户与 self-contained 安装的用户首次构建会重新编一次 std 模块(10–60 s);遗留的 `.mcpp-bmi/` 不再使用,`mcpp doctor` 会指出它,可手动删除。

- **`FAILED: <target>` 不再被输出过滤器整行丢掉**,归一成 `failed: <target>` 保留。#311 的报告读不出「失败的是 BMI staging 而不是编译」,一半原因就是这行被丢了。同时把 mcpp 自身的可执行路径纳入命令行前缀集合,于是被回显的命令行被过滤、mcpp 打印的诊断保留。

- **`mcpp new` 生成的 `.gitignore` 加上 `.mcpp/`**(per-project xlings sandbox,以及解析不出 MCPP_HOME 时的本地 BMI 缓存)。

## [2026.7.27.1] — 2026-07-27

### 变更

- **版本号改为日期格式 `YYYY.M.D.N`。** 与 xlings 生态对齐(xlings 于同日从 `0.4.70` 迁入)。月/日不补零。

  第 4 段的约定:**`.0` 保留给正式版本 / 稳定版本,日常迭代默认从 `.1` 开始** —— 一天内可以有若干次常规发布,`.0` 这个槽位只在该版本被认定为正式 release 或稳定版时使用。

  跨方案的序是单调的:`0.0.109` < `2026.7.27.1`,第一段由 `0` 变 `2026`,不存在回退。

### 修复

- **[#291](https://github.com/mcpp-community/mcpp/issues/291) 私有 glibc payload 不再无条件出现在运行目标的 `LD_LIBRARY_PATH` 里。** 该变量会被**整棵进程子树**继承。当运行目标是个会 shell out 的程序(例如课程 provider 调 `popen("mcpp test ...")`)时,`/bin/sh` 是**宿主二进制** —— 它的 `PT_INTERP` 烙死在文件里,装载它的永远是**宿主 ld.so**,而这个变量却把 **payload 的 `libc.so.6`** 递给它。

  glibc 的 libc 与 ld.so 之间通过 `GLIBC_PRIVATE` 版本锁定(实测:payload `libc.so.6` 对 `ld-linux-x86-64.so.2` 有 `GLIBC_PRIVATE` 依赖),二者必须同一次构建。于是**只要宿主 glibc 与 payload 不同版本**,shell 就在 `main` 之前死于装载器内的 SIGSEGV —— stdout 全空,没有任何诊断。报告者是 Ubuntu 22.04(glibc 2.35)对 payload 2.39。

  注意这**不是**「构建机不同导致 ABI 不兼容」:它是纯粹的版本错配,任何宿主 glibc ≠ payload 的用户都会中招。也正因为版本相同就不复现,它一直没被发现。

  payload 目录是 `LD_LIBRARY_PATH` 里**唯一不同时出现在可执行文件 RUNPATH 中**的一项(`flags.cppm` 刻意把它排除,以保持静态/musl 链接干净)。它存在的唯一理由是:被 `dlopen()` 的库其自身的 DT_NEEDED 闭包**不会**查主程序的 RUNPATH。因此现在只在构建确实存在这类依赖库时才注入(`depRuntimeLibraryDirs` 非空)——真实的 host-GL 透传场景走 `compat.glx-runtime` 的 `[runtime] library_dirs`,正好落在这个条件内,行为不变;而零依赖的二进制不再拿到它。

  `process.cppm` 的 `strip_private_glibc` 早已保护 mcpp **自己的**子进程,但它够不到再外一层:变量是 mcpp 有意设给目标的,目标之后再 fork 什么已超出 mcpp 的控制 —— 能控制的是**不必要时就不发**。

  e2e 166 双向锁住(零依赖工程必须拿不到、有 `[runtime] library_dirs` 的工程必须拿得到)。它断言的是**发出的环境变量**而非「shell 是否崩溃」:后者在宿主与 payload 版本相同的机器上会因为错误的理由通过,那正是这个 bug 长期存活的原因。

- **4 段版本号在比较时被静默截断,致 E0006 索引底线检查失效。** `version_req::Version` 原本是严格三段,`parse_version("2026.7.27.1")` 解析出 `{2026, 7, 27}` 后**丢弃第 4 段且不报错** —— 同一天内的所有版本互相比较相等。

  后果落在 `pm/index_contract.cppm`:索引写 `min_mcpp = "2026.7.27.5"`、用户跑 `2026.7.27.1`,两者比较相等,`have >= need` 成立,**底线检查放行**。用户拿着不够新的 mcpp 去读新索引,得到的是描述符读取返回空这类难以归因的次生故障 —— 而挡住这种情况正是该检查存在的唯一理由。

  修法是把 `Version` 扩到 4 段,并新增 `components` 记录源串实际写了几段。两条约束:比较**只看 4 个数字**(`components` 若参与比较,`"1.2"` 就不再等于 `"1.2.0"`);`str()` 必须能回写第 4 段 —— 它是**载荷性的**,`pm/resolver.cppm` 用它重建已解析的依赖版本串,该串会流向 lock 文件与 xlings wire 地址,而 `.0` 结尾的版本一旦塌成三段就会指向一个不存在的索引 key。既有三段依赖的解析逐字节不变。

### 维护

- **所有 xlings pin 升至 `2026.7.27.2`,并加机器校验。** `src/xlings.cppm` 的 `pinned::kXlingsVersion` 现为唯一真源,`.github/tools/check_version_pins.sh` 强制 `.github/` 下全部 16 个 pin 点与之一致,并同时校验 mcpp 自身版本在四处的一致性。

  此前靠常量上方一句「keep in lock-step with release.yml / cross-build-test.yml / ci-linux-e2e.yml」的注释人工同步,而**那个清单本身就是不全的** —— 漏掉了两个 composite action,CI 沙箱因此在 0.4.30 上静默跑了数周。落实检查时又找出三处从未被记录的 pin(`release.yml` 里硬编码的 aarch64 xlings tarball 字面量)和三处落后的 bootstrap pin(`ci-fresh-install.yml` 的 `v0.4.38`×2 / `v0.4.51`)。

## [0.0.108] — 2026-07-26

### 修复

- **Windows:显式 ninja 目标集不再撑爆命令行(0.0.104 起的回归)。** `mcpp test` 会把每一个共享前置(除测试自身 main 外的全部编译单元 + 非测试链接产物)列为 ninja 目标,好让包内源码出错时报**一次**包级错误,而不是 N 次雷同的逐测试编译失败。对大包来说这就是几千个对象路径 —— FFmpeg 的 2281 个编译单元拼出 **50,781 字符**的 argv。Windows 把 argv 合并成单条命令串交给 cmd.exe,而后者上限 **8191 字符**,于是命令**根本没有执行**:返回的是 cmd.exe 的裸 127,ninja 和 mcpp 都没有机会打印任何东西。

  症状因此极难归因:mcpp-index 的 Windows CI 自 0.0.104 起在 `ffmpeg` 成员上失败,日志里最后一行是无关的下载进度条,既没有 `build failed` 诊断,`target/.build_cache` 也没写出。它是全仓唯一大到能触发的成员,`mcpp build` / `mcpp run` 走 `default`(不带目标参数)则始终正常。

  修法是把目标集合写进 build.ninja 的一条 phony 聚合边,命令行只留一个词。清单文件没有长度限制,ninja 仍在**一次调用**内构建整个集合,并行度不变。e2e 164 锁住该机制。

  与 0.0.107 修掉的 soname 别名回归**同源**:都来自 0.0.104 引入的"显式 ninja 目标"([#274](https://github.com/mcpp-community/mcpp/pull/274))—— 一处设计改动,两个只在特定条件下现形的症状。

## [0.0.107] — 2026-07-25

### 修复

- **`mcpp test` 不再漏建共享库的 SONAME 别名(0.0.104 起的回归)。** 声明了 `soname` 的共享库产物写作 `bin/libX11.so`,但记录的 SONAME 是 `libX11.so.6`,链接器解析传递 `NEEDED`、加载器启动程序时找的都是后者 —— 别名不是附属产物,而是**任何链接该库的目标的前置条件**。此前别名是一条无人依赖的独立 ninja 边,只能经 `default` 到达;0.0.104 的 test 能力批次([#274](https://github.com/mcpp-community/mcpp/pull/274))让 `mcpp test` 改为显式指定 ninja 目标以隔离逐测试编译,于是这条边被静默跳过。

  症状离根因很远,这也是它潜伏三个版本的原因:消费者链接期报 `libX11.so: undefined reference to xcb_connect`(伴 `libxcb.so.1 ... not found`),或测试二进制以 `exit 127` 退出。`mcpp build` / `mcpp run` 走 `default`,一直正常,所以只有以 `mcpp test` 驱动的工程会中招 —— mcpp-index 的 CI 正是如此,`gui-stack` / `imgui-module` / `imgui-window` 三个成员自 0.0.104 起持续失败。

  修法是把别名挂到消费者的隐式输入上(`plan.cppm` 的 `append_direct_shared_deps`),与被链接的 `.so` 并列。无论 ninja 的目标是 `default` 还是某个测试二进制都会生成,且不会多编译任何东西。e2e 64 补了 `mcpp test` 一段覆盖此路径 —— 它此前只覆盖 `build` + `run`,正是缺口所在。

## [0.0.106] — 2026-07-25

> 落地 **SPEC-001**(`docs/spec/package-identity.md`)—— 包身份的规范形态。0.0.105 为修 #278 引入的「`name` 必须写成完全限定名」是**编码约束而非设计规则**:它的唯一成因是 mcpp 构造安装目标时丢弃了已读到的字面 `name`、改用 `<ns>.<短名>` 重新渲染一遍。本版本改为直接使用字面值,规范形态回归 **`namespace` 承载层级、`name` 是单一原子段**。配套 xlings 0.4.69([#381](https://github.com/openxlings/xlings/issues/381),索引改按 `(namespace, name)` 建键)。

### 新增

- **规范文档目录 `docs/spec/`**:存放规范性文档(语义、约束、匹配机制),每条规则标注实现状态,与使用文档 `docs/*.md`、设计文档 `.agents/docs/*.md` 分工明确。首篇 **SPEC-001** 覆盖 `package.namespace`/`package.name` 形态、`[dependencies]` 四种书写文法及其候选阶梯、完整匹配流程、派生量公式与端到端示例。
- **描述符文件名自由**:描述符按**声明的身份**被发现,文件叫什么都行。推荐文件名仍作为**快路径**优先探测,全部落空时才回落到按身份扫描 `pkgs/**/*.lua` —— 符合推荐命名的索引**零扫描开销**。这补齐了 identity-first 解析长期只兑现「验证」半边、「发现」半边受固定候选文件名约束的缺口。
- **同一索引内同短名不同命名空间的包各自可寻址**:`(alpha, widget)` 与 `(beta, widget)` 共存于一个索引,分别安装到 `alpha-x-widget` / `beta-x-widget`。需要 xlings >= 0.4.69。

### 变更

- **`package.name` 规范形态改为单一原子段**,层级一律放 `namespace`:`namespace = "chriskohlhoff", name = "asio"`。**已发布的完全限定拼写(`name = "chriskohlhoff.asio"`)仍被接受**,前缀会被剥离后再判定 —— 现网全部描述符无需改动即可继续工作。反过来,`namespace = "mcpplibs", name = "capi.lua"` 这类**短名仍带点**的写法被拒绝,因为它描述的是一个没人声明过的命名空间。
- **安装目标改为 `<namespace>:<字面 name>@<版本>`**。冒号前缀是包的命名空间(xlings 的 *effective namespace*),不是索引名 —— 这正是同索引同短名得以消歧的原因;无命名空间的上游包用裸字面名。
- **身份归一化不再 split-on-last-dot**:身份就是描述符声明的两个字段。旧规则会从 `name` 反推命名空间(`ns="a"` + `name="a.b.c"` 静默变成 `(a.b, c)`),现在改为拒绝而非猜测。
- **xlings 依赖升到 0.4.69**(`kXlingsVersion` 与 release/CI 的 `XLINGS_VERSION` 同步)。

### 修复

- `mcpp xpkg parse` 与安装路径的 `name` 形态校验语义随规范反转,共用同一谓词,lint 与运行期不会漂移。

## [0.0.105] — 2026-07-25

> 包身份口径双侧收敛(#278)。事故:mcpp-index 把 `chriskohlhoff.asio` 的 `name` 从 `"chriskohlhoff.asio"` 改成 `"asio"`(namespace 不变),lint 全绿,三个平台的 workspace job 跑满 20~58 分钟后全挂 `E_NOT_FOUND` —— 描述符能解析、能过 mcpp 的身份闸门,却没有任何消费写法能装上。根因是身份归一化(容忍三种拼写)与安装目标构造(只支持一种)口径断层,契约只写在注释里、无人执行。设计见 `.agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md`。

### 破坏性变更

- **裸依赖名不再解析到第三方命名空间的包。** 命名空间缺省时,`[dependencies]` 里的裸名**只**解析三类:`mcpplibs`(默认)、`compat`(包装)、无 `namespace` 声明的上游包。此前裸名会跨命名空间命中(例如裸 `tensorvia-cpu` 能装上 `aimol` 下的包),现在必须写全:`"aimol.tensorvia-cpu" = "…"` 或 `[dependencies.aimol] tensorvia-cpu = "…"`。
  取舍理由:全域按名发现的便捷性换来三条稳定性损失——同名包的裁决依赖索引优先级(而用户 `[indices]` 添加的索引之间**无全序**)、**新增一个索引可能悄悄改变既有依赖解析到的包**(供应链隐患)、同一份 `mcpp.toml` 在不同机器上可能解析到不同包。依赖解析的可复现性优先于书写便捷性。
  迁移无需查文档:失败时 mcpp 会扫描索引并直接给出应当改写成的那两行(见下)。

### 新增

- **INV-NAME 校验(`mcpp xpkg parse`)**:描述符声明了非空 `package.namespace` 时,`package.name` 必须是完全限定名(以 `<namespace>.` 开头),否则报错退出。索引是以 `package.name` **字面值**为键的扁平空间,而 mcpp 按 `<ns>.<short>` 寻址,两者不相交即永久不可安装。诊断直接给出应写的字面量(`fix: name = "chriskohlhoff.asio"`),`--json` 同步 `error` 字段供索引 CI 机读。mcpp-index 的 CI 本就跑 `mcpp xpkg parse pkgs/*/*.lua`,免费获得秒级防护。
- **`mcpp xpkg parse --allow-split-name`**:跳过 INV-NAME 检查。xlings 原生索引(xim-pkgindex、-scode)里 `package.namespace` 是**安装目录分类**(`config`/`scode`/`awesome`)而非包命名空间,索引按裸 `package.name` 建键,split 形式在那个世界里是正确的——这些树用此开关 lint。
- **运行期 fail-fast**:`mcpp build` 在构造安装目标**之前**用同一个谓词校验手上已有的描述符(零额外 I/O),把"三平台一小时后的 E_NOT_FOUND"变成秒级自解释失败。lint 与运行期共用一份判定,不会再各自推导。已装旧快照的路径降级为 warning,让"本机绿、干净 CI 红"的遮蔽陷阱可见。
- **依赖解析失败的 did-you-mean**:候选全部落空时(且**仅**在此时)扫描索引,若该短名存在于其他命名空间,直接列出 FQN 与两种可直接抄写的正确写法。该扫描是**纯诊断**——结果只进错误文案,绝不回灌解析、lockfile 或安装层(否则就退化成被否决的全域模糊匹配)。
- **`mcpp emit xpkg --namespace <NS>`**:为归档场景提供命名空间,无需改 `mcpp.toml`。

### 修复

- **`mcpp emit xpkg` 不再生成破损雏形**:此前只写裸 `name`、完全不输出 `namespace`,维护者归档进命名空间索引时手补一行 `namespace = "<org>"`,那一刻描述符就变成无法安装的 split 形式(`aimol.tensorvia-cpu` 正是这么来的,文件头还留着"AUTO-GENERATED, do not edit by hand")。现在 `[package] namespace` 非空时同时输出 `namespace` 与 FQN `name`;为空时给出明确警告,说明不要事后手补。此修复在 2026-06-26 设计 §4.5 里就已写明,一直未落地。
- **依赖候选全部落空时不再静默回退**:此前会退到第一个候选并把 mcpp **自己编造的**命名空间当作结论继续跑,失败被推迟到下载/安装阶段,错误文本里还带着用户从未写过的命名空间。现在当场失败并列出试过的每一个身份。
- **discovery 档不再泄漏空命名空间(P3)**:`(∅, name)` 命中后写回的是**描述符声明的**命名空间,而非候选的空值——空命名空间此前会流进 lockfile 与安装层。无 `namespace` 声明的上游包(`opencv`/`musl-gcc`)保持空命名空间,那是它们的合法身份。

## [0.0.104] — 2026-07-24

> `mcpp test` 能力批次(两轮):逐测试编译隔离、子目录路径命名、过滤器、JSON 输出——四项均为通用能力(cargo/ctest 同形),首个下游消费者是 d2mcpp「练习即测试」重设计(见 d2mcpp 仓 `.agents/docs/2026-07-23-exercises-as-tests-design.md` §4,验收标准即出自该文档)。实施计划见 `.agents/docs/2026-07-23-test-isolation-json-plan.md`。

### 新增

- **`mcpp test` 逐测试编译隔离**:两阶段构建——Phase A 先建包级共享前置(全部非测试 main 的编译单元 + 非测试 link 产物),失败是**包级错误**(报 build failed,绝不渲染成 N 个红测试);Phase B 每个测试作为独立 ninja goal 构建,编译失败只标记**该测试** `FAIL (compile)`(诊断按测试分组输出到 stderr),其余照常编译运行。此前一个编译不过的测试会让整轮 `error: build failed` 零执行。
- **测试按 `tests/` 相对路径命名**:`tests/00-a/0.cpp` → `00-a/0`,子目录下同名 stem 不再冲突(此前直接 `duplicate test name` 硬错);平铺布局名字不变。
- **`mcpp test <pattern>`**:按路径名子串过滤要构建/运行的测试。过滤只作用于构建/运行阶段——计划始终含全部测试,`compile_commands.json` 保持完整(clangd 依赖)。无匹配时报错退出码 2。
- **`mcpp test --message-format json`**:NDJSON 输出,逐测试一条记录流式发射(`test`/`status`=`pass|compile_fail|run_fail`/`exit_code`/`signal`/`compile_output`/`run_output`),包级失败单独 `{"error":"package",...}` 记录,末行 `{"summary":...}`;stdout 纯协议流,人读输出全部静默。供 CI/IDE/d2x Provider 消费。
- **`[build].flags` glob 覆盖测试 TU**:glob 指名文件,是 source 还是 test 是正交的——匹配的条目经既有 per-target flag 通道(#131)挂到合成测试 target 上,per-test 编译选项从此有 toml 承载。配套:死 glob 警告改为「磁盘上无文件匹配」才触发(此前只数被扫描的 source,指向 tests/ 的合法 glob 会被误警)。

### 新增(批次二,设计评审见 `.agents/docs/2026-07-24-mcpp-test-design-review.md`)

- **Phase B 并行化**:逐测试隔离改为「keep-going 全量预构建(-k 0,跨测试并行)+ 逐测试缓存命中验证/失败重试取诊断」——语义不变,49 测试全量墙钟 29s → 3.7s。
- **`mcpp test --list`**:只枚举(可过滤的)测试,不解析工具链、不构建;`--message-format json` 输出 `{"test":…,"main":…}` 逐行记录 + total 汇总,坏文件同样可列出。供 d2x Provider 等工具做单一真相源发现。
- **`mcpp test --timeout <secs>`**:每测试运行截止,超时 SIGKILL 并计为 `FAIL (timeout)` / JSON `"timed_out":true`(POSIX;Windows 暂为尽力而为,不生效)。
- **JSON 记录增加 `duration_ms`**(该测试构建+运行墙钟);schema 只增不改。
- **信号退出码规范化**:`WIFSIGNALED → 128+WTERMSIG`(shell 惯例)。此前返回原始 wait status——SIGSEGV 仅因 core-dump 位碰巧显示 139,SIGTERM 死亡会伪装成 `exit 15`;JSON 的 `signal` 字段推导从未可靠触发。属 bug 修复。

### 修复

- **#273 沙盒围栏**:post_install 的所有改写者(`patchelf_walk`/`fixup_gcc_specs`/`fixup_clang_cfg`)以显式信任根(`cfg.registryDir` canonical 化一次后穿透)逐文件设防——物理位置(符号链接解析后)在根外的一律拒绝改写并 fail-closed。修复既有入口 guard 的三处残余缺口:error_code 复用导致的错误掩蔽、裸字符串前缀无组件边界(`registry-evil` 可穿过)、binutils 兄弟目录 walk 不在 guard 覆盖内。`13_toolchain_pin.sh` 的 symlink 种子保留为金丝雀。单测 test_post_install_containment 覆盖事故拓扑。
- **嵌套 mcpp 的 `LD_LIBRARY_PATH` 段错误**:外层 `mcpp run` 为其子进程指向私有 glibc payload 的 loader 路径,子进程再 spawn mcpp 时(如课程 Provider 驱动 `mcpp test`),毒化值继续流入内层 mcpp 的工具子进程——sandbox ninja/gcc 加载错配 libc 后在动态链接器里段错误(残片签名 `__vdso_time`)。现在 `merged_environ` 从继承的 `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` 里**只剥离** `xim-x-glibc` payload 条目:用户自己的条目保留,per-child 显式 override 一如既往优先。下游(d2x `platform.cppm`、d2mcpp `runner.cppm`)的 `unsetenv` workaround 可随本版删除。

### 备注

- e2e 新增 152(子目录命名)/153(隔离与包级归因)/154(过滤器)/155(JSON)/156(嵌套 loader 路径免疫)/157(测试 TU 吃 glob flags + 死 glob 判定)/158(信号退出码+duration)/159(--list)/160(--timeout)。
- 首个下游消费者已完成端到端接入:d2mcpp「练习即测试」迁移(104 练习 zh/en 52/52 全绿)+ d2x checker 引导链路,全程使用本分支 musl 静态二进制。
- docs 措辞修正:"(gtest style)" → 框架无关表述;记录合成测试名含 `/` 的命名豁免(zh+en)。
- `BuildOptions::ninjaTargets`(构建计划子集)为支撑性通用接口,空 = 原行为。
- cross-build CI 的 wine 安装改为缓存 .deb 依赖闭包(首跑回填,命中免 apt update/下载)。
- musl 静态构建(`--target x86_64-linux-musl`)已验证:五个新 e2e 全绿;静态 mcpp 本体对 loader 毒化免疫,与上述修复共同覆盖嵌套链路的两端。

## [0.0.103] — 2026-07-22

> #267/#269:索引侧健壮化一批——mcpplibs 索引 URL 组织迁移(不再依赖 GitHub 重定向)+ 采纳 xlings 0.4.68 per-repo artifact 来源(索引同步不再硬依赖可用的 git)。设计见 `.agents/docs/2026-07-22-issue267-269-index-artifact-and-org-migration-design.md`。

### 修复

- **mcpplibs 索引 URL 仍指向已迁移的旧组织**(#267-A):索引仓已迁 `mcpplibs/mcpp-index`,mcpp 4 处硬编码旧 `mcpp-community` URL 全靠 GitHub 仓库重定向才能工作——旧名一旦被再次占用即静默指向错误内容。新/旧 URL 与 artifact base 收敛为 `mcpp::config` 三常量;`canonicalize_legacy_index_names` 导出并承担"URL 归一→名字迁移→artifact 填充→去重"单循环(**URL 归一先行**,老配置的名字迁移仍命中、新旧条目在去重处折叠);存量安装经既有文本迁移钩子治愈(config.toml 与 registry `.xlings.json` 的 URL 就地替换),publish Fork 提示与 docs 链接同步。

### 新增

- **采纳 xlings 0.4.68 per-repo artifact 来源**(#269,#267-B):默认 `mcpplibs` 条目(config.toml 模板 + 内存级默认)声明 `artifact = xlings-res/mcpp-index`,`source = "git"` 为显式退出口;seed 管线由 `(name,url)` pair 升级为 `SeedRepo{name,url,artifact,source}`,非空才发射——无 artifact 的仓输出逐字节不变(测试锁定)。存量 registry `.xlings.json` 唯一治愈通道是文本迁移:URL 归一后就地注入 artifact 字段,幂等闸=res base 已出现即跳过,双间距变体覆盖 mcpp/xlings 两个写者,`subos`/版本绑定等无关状态逐字节保留(**不整文件重生成**)。旧 xlings(<0.4.68)安全忽略新字段,发布端 `xlings-res/mcpp-index` 布局实测零改动。
- **项目级 `[indices]` artifact/source**(#269 可选项):`IndexSpec` 增两字段,mcpp.toml 与 config.toml `[indices]` 长格式同步解析,经项目 seed 透传;`artifact_applicable()` 编码设计规则——**rev/tag/branch pin(及本地 path)强制 git**(artifact 通道只跟 latest pointer),声明了 artifact 但不适用时告警而非静默同步错误版本。
- **捆绑 xlings pin 0.4.67 → 0.4.68**(release/cross-build/e2e 三 workflow):0.4.68 携 openxlings/xlings#377(自定义 index artifact)/#380(shim env 去重)。陈旧无消费的 `kXlingsVersion`(0.4.51)对齐至 0.4.68 并由 `mcpp self env` 打印,常量不再能静默漂移。

### 备注

- e2e 新增 151:fresh init 播种新 URL + artifact;老 org URL/无 artifact 的存量 config.toml 与 `.xlings.json`(pretty/compact 双间距)就地治愈且幂等,无关 `.xlings.json` 状态保留。
- CN region 对象形态(`{"GLOBAL":..,"CN":..}`)待 gitcode 侧 `xlings-res/mcpp-index` 镜像仓真实部署后再扩(设计文档 P5),避免声明 404 base 白付探测。

## [0.0.102] — 2026-07-22

> 四条 issue 一个批次:#261 windows 命令行长度、#257 clang 依赖跟踪缺口、#258 条件段表达力、#254 host/target 双轴不自洽。贯穿主线是**同一决策不许两处推导**,以及本批次新立的第二条不变量**失败必须响,不许静默降级**。设计见 `.agents/docs/2026-07-22-v0.0.102-batch-254-261-design.md`,issue 裁决见 `.agents/docs/2026-07-22-issue-triage-254-261.md`。

### 修复

- **windows scan-deps 撞 8191 字符上限**(#261):clang 扫描规则此前用 shell 重定向产出 P1689 JSON,ninja 在 windows 走 CreateProcess 不解释 `>`,于是整条命令被 `cmd /c` 包裹——而 cmd.exe 的命令行上限是 8191,只有 CreateProcess 的 1/4。改用 `clang-scan-deps -o`(LLVM 17+,两个自带工具链均实测可用),重定向与包裹一并消失,与 GCC 的 `-fdeps-file=`、MSVC 的 `/scanDependencies` 三条分支形态统一。`cmd /c` 自此在全仓 ninja 规则中绝迹。
- **windows 编译/扫描命令行无长度兜底**(#261):`local_include_flags()` 每依赖一个 `-I`、无上界,而 `cxx_module`/`cxx_object`/`c_object`/`asm_object`/`cxx_scan` 全部内联该载荷;扫描命令包住编译命令,只是恰好先炸。把 #247 给链接规则的 response file 推广到所有携带无界载荷的规则(gcc/clang 驱动与 cl.exe 均展开 `@file`,clang-scan-deps 会把 `--` 之后的 `@file` 透传给驱动;NASM 因为拼写是 `-@ file` 而排除)。顺带把 `escape_flag_path` 改用 `generic_string()`——这是 #247 那个坑往下一层:response file 内容按 GNU 文法分词,反斜杠是转义符,windows 的 `-I` 路径一旦离开命令行就会丢分隔符。POSIX 逐字节不变。
- **clang 路径 purview `#include` 不触发重建**(#257):编辑模块接口 purview 内 `#include` 的文件后,构建静默复用陈旧 BMI,导入方对着旧接口编译——最差的一类失效,错误结果看起来像一次正常的增量构建。根因是 0.0.97 把两个决策捆在一个谓词里:**是否发 depfile** 与 **是否剥离 GCC `-fmodules` 附加的 reversed make-rules**。实测两个自带工具链,clang 的 module-TU depfile 就是一条普通规则(`x.o: x.cppm ops.inc`),没有任何需要过滤的东西——当年那道闸在防一个不存在的形状,代价是把与它捆在一起的正确性契约一起关掉了。现拆为 `posixDepfile`(所有 POSIX 非 MSVC)与 `needsGnuModuleFilter`(仅 GCC)。同时补上同一处不对称的另一半:`c_object`/`asm_object` 此前在**任何**工具链上都没有 depfile。windows 非 MSVC(mingw gcc / 托管 clang)仍无 depfile(GCC 过滤器依赖 awk),但现在经 `diag::degraded` 明确上报影响,不再静默。
- **xpkg per-OS 段按宿主而非目标拼接**(#254):per-OS 段的 sources/flags/deps 与 xpm 版本/资产表全部按 `xpkg_platform` 这个**编译期宿主常量**选取,而 `[target.'cfg(...)']` 按**解析后的目标**求值——同一决策两处推导。交叉编译时依赖包因此被拼进宿主那条腿。原生构建 host == target 恰好掩盖了它,所以三平台 CI 从未发现。

### 新增

- **`mcpp::diag` 统一降级/告警通道**:`degraded()` 强制填写 `impact`——作者必须回答"用户会因此遭遇什么",而这正是每一个静默降级 bug 当年缺的那句话。记录按整条载荷去重、报告即渲染(告警与 `ui::status` 的交织顺序、以及提前返回前已报内容都不变),`--strict` 提升策略收敛到单点(此前在 `prepare.cppm` 里 copy-paste 了 5 处)。告警内容首次可单测。`prepare.cppm` 的 8 处裸 `println(stderr, "warning: ...")` 已迁入;渲染仍走 `ui::warning`,输出逐字节不变。
- **`[target.'cfg(...)'.build]` 支持 per-glob `flags` 与 `include_dirs`**(#258,xpkg `target_cfg` 同步):此前 `sources` 可按 OS 条件化而 `flags` 不能,于是一份覆盖三 OS 的 manifest 必须让每个 OS 都看到另外两个 OS 的 flag 条目,而它们按构造必然零命中。vendored-opencv 移植为此付出 **703 个 stub 文件**(唯一用途是给 windows TU 制造 OS-唯一路径好让全局 flag 表 key 上去)、32 条指向它们的 glob,以及每次构建 ~23 条结构性死 glob 告警。条件条目追加在 base 之后,GNU last-wins 使**撤销**可表达(windows 需要 `-UHAVE_UNISTD_H` 对抗 base 的 `-D`,而无条件覆盖层做不到——`-U` 反条目自身也需要按 OS 条件化,递归回同一缺口)。死 glob 告警由**结构**消除:不匹配当前 OS 的条目根本不进 `globFlags`,与 #253 在 feature 轴上的做法同构;issue 里提的 `optional = true` 逃生口**明确不做**,那会成为第二套抑制机制。
- **`mcpp.platform.axis`:host / target 两轴成为不同类型**(#254):互不可转换,且都不能由裸字符串构造;API 声明自己要哪一轴,调用点必须**指名**自己给的是哪一轴。`synthesize_from_xpkg_lua` 的平台参数去掉宿主默认值改为必填——静默默认正是这个 bug 得以存活的方式。三段 triple 与 xpkg 的词汇差异("macos" vs "macosx")收进 `TargetPlatform::for_os`。分类:依赖包 manifest 合成与版本/资产选择走 **target**;工具链版本列举走 **host**(其注释本就写明了理由);`--all-os` lint 走刻意起得别扭的 `for_lint_of()`。
- **`BuildInputs` 类型**(#258 的架构面):cfg 轴与 feature 轴此前各自手挑可条件化的字段子集,**且挑得不一样**(cfg 拿 cflags/cxxflags/ldflags/sources,feature 拿 sources/defines/flags)。"哪些构建输入可被条件化"这一决策被提炼成一个**类型**而非一张要靠人记得更新的表。入选判据两条:合并语义是**追加**;消费点在条件合并**之后**。据此排除选型轴(`target` 用来*选定* triple,而谓词要靠该 triple 求值——构造性循环)与策略标量(需 last-wins,是另一场设计);`generatedFiles` 因为是写磁盘的**副作用动作**而非输入、`featureDefines` 因为是沿 Public 边**传播的接口贡献**而非私有构建输入,各自按范畴排除。`BuildConfig` 以**继承**方式承载(该字段在 ~150 处被读,嵌套只会是 150 次无收益的机械改动),`ConditionalConfig` 则**嵌套**——保证需要落在那里。合并收敛为单一 `append(BuildInputs&, const BuildInputs&)`。
- **#256 clang 模块运算符模板 canary + 文档**:mcpp 侧无缺陷(`--precompile` 成功,崩溃在 clang 前端的导入侧),但 mcpp 自带 LLVM,且该 hazard 正落在本项目推荐的模块包模式上。文档给出可操作规则(导出的运算符模板,所有模板参数应由**第一个实参**绑定;否则改为对整个推导操作数类型建模)与可直接套用的改写范式。e2e 150 是**状态** canary 而非通过/失败契约:期望表记录 LLVM 20-22 崩、18-19 正常,一旦现实与期望不符即失败——未来某次 clang bump **修好**它与**再弄坏**它同样是需要被看见的信号。

### 备注

- 新增不变量写入批次总账:**任何"因条件不满足而少做一步"的分支,必须要么返回错误,要么经 `diag::degraded` 上报;`log::debug`/`log::verbose` 不算用户可见;丢弃 `std::expected` 返回值视为缺陷。**
- **#254 的覆盖是单测级**:per-OS 段按 target 选段、host==target 逐字节不变、轴类型互不可转,三条单测锁定;原计划的"cross 腿消费带 per-OS 段的 xpkg dep"e2e 未落地(需要 e2e 侧先有"复用宿主 registry + 本地索引 + 预置 payload"的夹具,临时 MCPP_HOME 会触发整套交叉工具链重装),记为已知缺口。
- e2e 新增 148(64 个 include dir 的宽 include 表,POSIX 验内联形态、windows 验 response file)、149(条件段 per-glob flags 四象限:命中生效/覆盖 base 的 removal/非命中零告警/真死 glob 仍告警)、150(#256 canary);118 去掉 `# requires: gcc` 并加 clang 腿。
- **未包含**:#259(根因在 xlings 侧的 dep 静默丢弃,调查结论已发 issue 评论;mcpp 侧另发现 sysroot 兜底是死代码——`resolve_xpkg_path("xim:glibc")` 缺版本必然失败且两处都丢弃返回值——一并另行安排);manifest 未知键策略五处不一致([#263](https://github.com/mcpp-community/mcpp/issues/263),有兼容性面,与 #258 无依赖)。

## [0.0.101] — 2026-07-20

> #253:feature 模型两缺口收口——per-feature per-glob `flags` + per-OS `features` 语义锁定,解锁 compat.opencv `dnn` off-linux 腿并消除 feature-off 构建的死 glob 告警。设计见 `.agents/docs/2026-07-20-issue-253-feature-flags-and-per-os-features-design.md`。

### 新增

- **`features.<name>.flags`**(#253):feature 级 per-glob 编译旗标,与 `[build].flags` 共用同一 entry 文法(`glob` 必填 + `cflags`/`cxxflags`/`asmflags`/`defines`),xpkg 与 mcpp.toml 双文法一数据模型(共享解析 helper)。激活时折入既有 globFlags 单一漏斗(scanner 匹配/per-TU 落旗标/死 glob 告警/fingerprint 四个下游零改动),追加在 base 规则之后、feature 按名序,"last flag wins" 使 feature 规则可覆盖 base;折入点在 `includeDevDeps` 门外(0.0.94 双路径不变量)。**私有 per-TU、不传播**(与接口开关 `defines` 的语义分界)。feature off 时规则不存在 → opencv mlas 一类"结构性必死 glob"的告警自然消失;feature on 而 glob 空仍告警,且文案点名归属 feature(`features.<f>.flags glob '...' matched no source file`)。TOML 侧 `[[features.<name>.flags]]` AOT 拼写同步进 #227 封闭文法 allowlist(`features.*.flags` 通配段)。
- **per-OS `features` 语义锁定**(#253):`mcpp.<os>` 段的文本拼接 additive overlay 本就覆盖 `features` 键——同名 feature 逐子键 append(中性段在前、OS 段在后),per-OS 段可注册 OS-only feature;现以单测(逐 OS `osOverride`)+ e2e 锁死并写入文档,配合 `features.<f>.flags` 构成 opencv `dnn` 的 common/delta 形态(中性段放跨平台公共载荷,per-OS 段放 mlas x86 / NEON 差集与其旗标)。

### 修复

- **依赖包 per-glob flags 此前不入 per-package fingerprint**:`canonical_package_build_metadata` 只序列化 cflags/cxxflags/ldflags/genfiles,globFlags 靠"descriptor 随版本冻结"间接成立;feature 折入使该向量随构建变化,现与根侧同款全量有序序列化(`globflags:/gc:/gxx:/gas:/gd:`)。

### 备注

- e2e 新增 146(feature flags 四象限 + 死 glob 告警消除/点名 + 私有不传播对照)/147(per-OS features 端到端,宿主段生效、非宿主段投毒不可见);单测新增 xpkg/TOML 解析与 per-OS 合并锁定。host/target 轴缺陷(per-OS 拼接键=宿主常量,交叉编译选段错误)另开 issue 跟踪,不入本版。

## [0.0.100] — 2026-07-19

> 大型源码直编包(ffmpeg/opencv 级,数千 TU)全平台化批次:#247/#248/#249 平台三修 + 增量构建修复 + build.mcpp 指令面补全(P1)。设计见 `.agents/docs/2026-07-19-large-source-pkg-platform-fixes-and-buildmcpp-generation-design.md`。

### 修复

- **windows driver-style 链接命令行溢出**(#247):gnu 方言(g++/clang++ 作链接驱动——windows 托管 clang-MSVC 即此)的 `cxx_link`/`cxx_archive`/`cxx_shared` 此前内联 `$in`,数千对象直接溢出 CreateProcess 32 KiB 上限。现 windows 上 driver-style 也走 response file;两方言分支收敛为单一 `link_rule` 发射器(msvc 规则文本逐字节不变),POSIX 保持内联零变化。配套:ninja 节点名统一 `generic_string()` 正斜杠——rsp 内容按 GNU 文法分词,反斜杠是转义符(`obj\cli.o` 会被吃成 `objcli.o`)。
- **macOS dep/root build.mcpp 收不到 G3 契约环境**(#248,launcher-unify):非 Linux 的 `capture_exec` 走 shell 字符串拼接,`ENV… cd <cwd> && bin` 里 env 只绑给 `cd`(全仓唯一 env+cwd 双非空调用点恰是 build.mcpp)。现 macOS 与 Linux 同走 posix_spawn 直启路径(child-only env + `addchdir_np`,`environ` 经 `_NSGetEnviron()` portable-correct),顺带消掉 macOS 的 shell quoting/注入面。windows shell 回退补 `cd /d`(跨盘符)。
- **generated_files 每次构建无条件重写毁增量**:materialize 此前不比内容直接写,mtime 抖动令 ninja 把 include 该头的全部 TU 判脏——冻结快照包(config.h × 数千 TU)每次 build 都全量重编。现内容逐字节相同即跳写(变更检测本就由指纹负责)。
- xpkg feature 表未知子键(如 `features.X.include_dirs`)此前静默吞掉,现进 `xpkgUnknownKeys` 走统一告警面。

### 新增

- **`[build] include_dirs_after` → `-idirafter`**(#249):排在工具链系统目录**之后**搜索的 include 目录(descriptor/xpkg 与 mcpp.toml 双文法、`*` tarball 根 glob、沿 Public/Interface 边传播且不升级为 `-I`)。治大小写不敏感 macOS 上依赖源根 `VERSION` 顶替 libc++ `<version>` 一类系统头遮蔽;`-isystem` 不解此症(仍先于默认系统目录)。方言降级单点收敛:cl.exe → 末尾 `/I`,NASM 单元 → 普通 `-I`(NASM 会把 `-idirafter<p>` 误吞成 `-i dirafter<p>`)。附带统一主工程 include 路径的 glob 展开(此前与 dep 路径两套推导)。
- **build.mcpp 指令面补全**(P1,0.0.100+):`mcpp:source=`(选既有 payload 源入编译集,`generated=` 的"绝对路径灰色用法"正名)、`mcpp:include-dir=` / `mcpp:include-dir-after=`(私有 include,Cargo 纪律不进公共接口;typed 通道享方言降级);typed `import mcpp;` 同步 `source()/include_dir()/include_dir_after()`。契约环境拆分 `MCPP_TARGET_OS/ARCH/ENV`(免手撕三元组)。**root build.mcpp 后移至依赖解析之后**,与 dep 一样拿到 `MCPP_DEP_<NAME>_DIR`;指令落通道收敛为 root/dep 共享的单一 fold(防"同一决策两处推导")。顺修:root `build.mcpp` 变更此前被整库 fast-path 吞掉不触发重跑。
- **`mcpp xpkg parse --all-os`**:按 xpm 声明的平台集逐 OS 校验 per-OS 段(构建路径只 splice 宿主段,windows 段的 typo 在 linux CI 上原本不可见);mcpp-index CI 可单 runner lint 多平台描述符。

### 备注

- e2e 新增 141–145(-idirafter 语义/增量跳写/source= /include-dir/root dep-dirs);linux 全量 e2e 134 过(3 项为既知环境性失败)。macOS/windows 断面由平台 CI 与 mcpp-index spike 复现件(PR#89/90/91)收口。

## [0.0.99] — 2026-07-19

> #230–#243 批次收尾:#243 feature 转发落地(0.0.98 只出了设计)+ #238 根因修复随 xlings 0.4.67 vendored 入包 + #230 windows build.mcpp 次生面补齐。设计见 `.agents/docs/2026-07-19-v0.0.99-feature-forwarding-238-230-design.md`。

### 新增

- **feature 依赖 feature 转发 `dep/feat`**(#243,Cargo 平价):`[features]` 里含 `/` 的 token(或表格形专用 `forward = ["dep/feat"]` 键)表示"本包该 feature 激活时,顺带打开依赖 `dep` 的 `feat` feature"——一个 feature 既能本地拉源、又能开依赖的重档 feature,解阻塞模块包的可选模块接口(opencv-m 的 `import opencv.dnn;`:`dnn` 一档同时拉 `dnn.cppm` 源集并把 `compat.opencv` 以 `features=["dnn"]` 参与构建,而非对所有消费者全量 +309 TU)。收敛为**一数据模型 `featureForwards` 两文法**(TOML 与 xpkg 描述符共享唯一切分点 `split_feature_forward_token`);转发**注入 0.0.98 既有的 `aggregatedRequest` 依赖边漏斗**(在子依赖 push 进 worklist 前把转发 feature 并入其请求集),一处注入同时覆盖解析(`mergeActiveFeatureDeps` 拉被转发 feature 的条件依赖)与激活(边图 union → `apply()` 发宏/源集)两个消费点;沿 BFS 前向边天然**传递**(root→mid→leaf)。与 #242 `default-features = false` 加性组合(转发进显式请求集、不受默认门控影响),`mcpp build`/`mcpp test` 双路径一致。转发到未声明依赖 strict 报错 / 非 strict 告警;转发未声明的依赖 feature 复用既有 "does not declare requested feature" 门。单测 3 例、e2e 128(含双路径)。

### 修复

- **≥2 项目级 index_repo 时 `install_packages` 静默失败**(#238,**根因修复上游落地**):vendored xlings 由 0.4.62 升至 **0.4.67**,携 openxlings/xlings#374 的多仓安装修复(`fix(xim): surface multi-repo install failures + best-effort catalog`,commit `cf9b60d5`)。此前 workspace 根级 `[indices]` 继承(#224)× default 重定向(R6)组合会给每个成员播下 ≥2 个 `index_repos`,任一未缓存包安装裸 `exit 1` 无 error 事件;0.0.98 已在 mcpp 侧把它变成可操作诊断,0.0.99 随 bundle 带上真正的解析修复。发布/交叉构建/e2e 三处 workflow pin 同步 0.4.67。
- **windows 下依赖/成员 `build.mcpp` 产物名缺 `.exe` 无法执行**(#230 次生面):`build.mcpp` 编出的宿主程序此前恒名 `build.mcpp.bin`;windows 的 `capture_exec` 走 cmd.exe,`.bin` 不在 PATHEXT 故无法按名启动 PE。现按平台取后缀(windows=`build.mcpp.exe`,其余保持 `.bin`,`is_windows` 为 constexpr,非 windows 字节不变)。此面在 0.0.96 的 scanner symlink-逃逸崩溃(`df985df`,裸 127 的真凶)修复后才会被 workspace 的 build-mcpp 成员在 windows 走到。

### 备注

- #230 主因(scanner glob 顺 `.mcpp/.xlings` symlink 逃逸进 vendored 索引、CJK 文件名触发 MSVC 窄串转换抛异常→`__fastfail`→裸 127)已于 0.0.96 根治并在 0.0.98/0.0.99 在库;`src/main.cpp` 顶层 catch 兜底(未捕获异常→exit 70,不再裸 127)。0.0.99 补齐 build.mcpp 次生面后,mcpp-index 的 workspace(windows)CI 从临时钉回的 0.0.94 升到 0.0.99 复验全绿即关闭。

## [0.0.98] — 2026-07-19

> #230–#243 批次(单 PR 统一发布,逐 commit):#233 对象路径消歧的两个后续缺口(#239/#240,解阻塞 mcpplibs #79 opencv 收录)+ #237/#241/#242 根因级实现 + #238 mcpp 侧诊断(根因在 openxlings/xlings#374)+ #243 设计。总账 + 架构评估见 `.agents/docs/2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md`;各设计文档见 `.agents/docs/2026-07-19-*`。

### 新增

- **`MCPP_DEP_<NAME>_DIR` build.mcpp 契约**(#241):包的 `build.mcpp` 现可经 `mcpp::dep_dir("<name>")` 拿到依赖的安装目录(verdir/payload 根),不必再逆向 store 布局(实例:compat.opencv 的 `unifont` feature 读数据资产包 compat.opencv-unifont 的字体 blob)。用权威的 consumer→dep 边图注入,覆盖 feature 激活的依赖;canonical + short 双发(`dep_dir("compat.zlib")`/`dep_dir("zlib")` 皆可)、同款 sanitize、碰撞守卫、自动进 rerun hash。作用域为依赖侧 build.mcpp(root 工程 build.mcpp 早于依赖解析运行,列为后续项)。
- **消费端 `default-features = false`**(#242):依赖 spec 支持关闭该依赖的默认 feature 集(`{ default-features = false, features = ["x"] }`),Cargo 平价。根因收敛在 `feature_closure` 的单一 `seedDefault` 门:关闭时不 seed 该依赖 `[features].default`,仅显式请求 + `implies` 激活;root 包/工程 build.mcpp 仍默认 seed。(挡住 compat.ffmpeg 裁剪档形态。)

### 修复

- **消歧后链接输入未跟随改名,依赖与消费者同名源即挂**(#240):当依赖包与消费者存在同名源(近乎必现——双方都有 `src/main.cpp`,如 OpenCV 自带的 sample `main.cpp` × 消费者的入口)时,#233 已把被扫描的消费者 `main` 编到 `obj/<pkg>/src/main.o`,但链接步骤仍引用消歧前的扁平 `obj/main.o` → `ninja: error: 'obj/main.o' … missing and no known rule`。现将对象路径分配收敛为**单一来源**:被 glob 进来的入口复用其编译边已消歧的对象;未被 glob 的入口也纳入同一碰撞普查后消歧——链接输入与编译边永不背离。常见单二进制工程(`main` 唯一)仍为扁平 `obj/main.o`,字节不变。
- **绝对/越根路径源的消歧对象逃逸出 `obj/`**(#239):依赖 `build.mcpp` 写进 OUT_DIR(`target/.build-mcpp/deps/<name>@<ver>/out/`,在包根之外)的生成源,其 `relPath` 携带 `..`,#233 曾原样拼进 `obj/<pkg>/../…` 致对象路径爬出构建树(甚至在 CWD 镜像整棵绝对路径树);且路径里的 `@` 会被 ninja 单引号包裹,连带压垮 #235 的 `"$out.d"` depfile 重定向。现消歧前缀逐分量净化:去绝对根、`.` 丢弃、`..`→`__up`、非可移植字符(如 `@`)→`_`——对象永远向下且 shell 安全;逐分量单射,保住 #233 的唯一性(L1b 断言兜底残余)。
- **xpkg 描述符 mcpp 段未知键 build 时静默忽略**(#237):`dependencies` 误写(正确键 `deps`)等未知键此前只有 `mcpp xpkg parse` 报,build 路径静默丢弃致依赖消失无诊断。现描述符被采纳为依赖时按键**响亮告警**并给 did-you-mean(封闭词表别名 + Levenshtein 回退);沿用 0.0.97 封闭文法先例,告警而非硬错以保前向兼容。
- **feature 请求集收敛到依赖边图(#242 传递边 + #241 命名;架构评估头号优化)**:此前 feature 请求集在解析(`mergeActiveFeatureDeps`,读 per-edge spec)与激活(`apply()`,只扫 root 直接依赖)两处独立推导且对**传递边**不自洽——传递依赖的请求 feature 与其消费者的 `default-features = false` 被静默丢弃(激活仍 seed 该依赖默认 feature,定义其宏/保留默认门控源,而解析已跳过)。现 `DependencyEdge` 携带 per-edge `requestedFeatures + defaultFeatures`,新 `aggregatedRequest` 对某依赖包所有入边做 union/OR(Cargo 菱形语义),激活与依赖 build.mcpp 共享之;直接依赖行为不变,顺带补掉长期的传递 feature 未传播缺口。e2e 127。
- **多 index repo 下 `install_packages` 失败无诊断**(#238,**根因在 xlings**):裸 `fetch failed (exit 1)` 现重建为可操作诊断——点名目标、已配置 index repos 清单、`≥2` 仓的已知 xlings 解析缺口提示、保留子进程输出、`MCPP_VERBOSE=1` 看原始调用。**仅诊断改进**;多仓解析的根因修复须落在 openxlings/xlings(已开 **openxlings/xlings#374**)。

### 设计(未实现,后续 PR)

- **#243 feature 依赖转发(`dep/feat`)**:核实条件依赖半边已存在(`[feature-deps]`/xpkg `features.x.deps`),真正缺口是转发;设计见 `.agents/docs/2026-07-19-issue-243-feature-forwarding-design.md`,收敛为单一 per-package 请求 feature 漏斗(顺带补 `prepare.cppm:2653` 传递性缺口),与 #242 opt-out 加性组合。

### 备注

- #230(windows workspace exit 127)已于 0.0.96 修复,待 mcpp-index windows CI pin ≥0.0.98 复验后关闭。

## [0.0.97] — 2026-07-18

> 架构级修复批次(单 PR,逐簇 commit):ffmpeg-m/opencv-m 全源码直编暴露的第二层缺口 + workspace 测试基建语法空洞。

### 新增

- **`[[build.flags]]` 数组表写法**(#227):`[build].flags` 现同时接受 TOML 标准数组表 `[[build.flags]]` 与内联表数组两种等价写法(声明顺序=应用顺序不变),长 per-glob flags 条目不再挤单行。纯解析层扩展(自研 TOML parser 补全 array-of-tables);并加清单层 closed-grammar 守卫——非白名单段落误写 `[[x]]`(如 `[[dependencies]]` 手滑)现**硬报错**而非静默丢数据。
- **glob 花括号交替 `{a,b}`**(#228):sources/flags glob 支持 `libavcodec/{aac,bsf,hevc}/**` 笛卡尔展开(嵌套/多组均可),vendored 大库 per-glob 声明不再重复。
- **默认命名空间索引重定向**(R6):`[indices] default = { path = "..." }`(亦接受空引号键 `""`)可把默认命名空间(`namespace = ""` 的模块包)指向本地 checkout,补上此前只能重定向具名命名空间的语法空洞——使 index 仓能以 `mcpp test --workspace` 声明式验证 imgui/ffmpeg/opencv 等模块包,替代逐包 smoke shell。(`url` 形式的默认命名空间重定向暂不支持,解析期显式报错而非静默失效。)
- **`mcpp run -p <member>`**:run 命令支持选择 workspace 成员并运行其二进制(与 `build`/`test` 的 `-p` 对齐)。

### 修复

- **源发现全树遍历致 `mcpp run` 前慢 + 缓存复用**(#225):glob 遍历改从字面前缀起(`src/**` 从 `src/` 起走,不再从项目根扫全树),并排除 `.git`/`target`/git 子模块边界;`mcpp run` 复用 `mcpp build` 已解析的缓存,不再每次重扫大子模块(此前含大 `compat/` 子模块时每次 ~9.5s)。
- **相对 include 族 flag 未按项目根重写 + 含空格值未转义**(#226 #234):`-iquote`/`-isystem`/`-idirafter`/`-iprefix`/`-L` 现与 `-I` 一样按项目根绝对化(joined 与 separated 两拼写);`defines = ["T=long long"]` 等含空格值发射时 shell 引号保护,不再被拆成孤立参数。含 `[build] include_dirs` 在 MSVC 方言下的相对路径绝对化亦一并修正。
- **对象路径按父目录名折叠致同名源冲突**(#233):不同目录同名源(`a/src/util.cpp` vs `b/src/util.cpp`,OpenCV/LLVM 式 `modules/<mod>/src/*` 布局)对象路径改按碰撞时镜像源**相对路径**消歧 + 构建后唯一性断言;非碰撞项路径字节不变。
- **模块 purview 内文本 `#include` 改动不触发重编**(#235):编译边补 depfile 追踪(过滤 GCC `-fmodules` 注入的反向规则以规避 ninja `inputs may not also have inputs`),purview/GMF/普通头改动均正确触发重编——顺带根治此前非 MSVC 下普通头改动也不重编的潜伏问题。
- **依赖包 cfg 条件 sources 被消费时不展开**(#229):path/git 依赖的 `[target.'cfg(...)'.build].sources` 现与 root/version-dep 同样按已解析 target 求值(`mcpp build` 与 `mcpp test` 双路径),不再 undefined reference(与 #218 同类,收敛为统一 per-package 求值)。
- **nasm 冷环境惰性自举时序**(#232):nasm 供给改走工具链同款同步门 `Fetcher::resolve_xpkg_path("xim:nasm@3.02", autoInstall=true)`(索引刷新前置 + 硬报错 + payload 校验),不再先判死后台补装;config 自举错误不再被 `if(cfg)` 门吞成误导性的 "no usable nasm"。
- **workspace 根配置无法一次声明全员可用**(#224):`[workspace.dependencies]` 的 path 依赖可被成员经 `.workspace = true` 继承;根 `[indices]` 的相对 `path` 按 **workspace 根**解析(而非消费成员目录),成员不再需重复声明各自 `../` 相对路径。

### 备注

- #230(windows workspace 崩溃)已于 0.0.96 修复。#215(cppfly Clang 反射行)待上游 Clang 落地 P2996,不排期。

## [0.0.96] — 2026-07-18

### 修复

- **[windows] `mcpp test --workspace` 静默崩溃(裸 exit 127,实为 0xC0000409)**
  (#230,修复 #231)。三层根因:
  1. 0.0.95 扫描器的 glob walk 新增 `follow_directory_symlink`,而项目本地
     `.mcpp/.xlings/data/<index>` 是指回索引根的**符号链接** → walk 逃逸出成员
     目录、扫遍整个索引 checkout(CI 里含 vendored xim-pkgindex);
  2. `path_matches_glob` 用 `generic_string()` 拼窄串,MSVC 在非 CJK ANSI 代码页
     (runner ACP=1252)下遇到中文文件名 `bug-report---问题反馈.md` 抛
     `std::system_error`;
  3. 异常逃出 `main` 未捕获 → `std::terminate` → `__fastfail`(0xC0000409),
     git-bash 显示为无任何输出的 exit 127。
  - 修复:`expand_glob`/`expand_dir_glob` **按名剪枝 `.mcpp` 目录**(mcpp 自身
    元数据目录永远不是源码目录,从源头切断符号链接逃逸,顺带避免每个成员把
    整个索引树白走一遍);`path_matches_glob` 对无法窄化的文件名按"不匹配"
    跳过而不是摧毁构建;`main()` 增加最后防线 catch,逃逸异常打印真实错误并
    以 70 退出,不再静默 fastfail。
  - 取证:runner 开 WER 全内存 dump,崩溃栈+被转换字符串逐帧还原(记录见
    mcpplibs/mcpp-index `debug/mcpp230-windows-repro` 分支及 #230)。
  - 回归测试:`tests/e2e/113_scanner_mcpp_dir_prune.sh`(`.mcpp` 符号链接逃逸
    必须被剪枝;CJK 文件名不得致命)。linux 行为对照:0.0.95 会顺着该符号链接
    把项目外源码编进来(链接期 duplicate `main`),修复后构建干净。

## [0.0.95] — 2026-07-17

- 见 GitHub Release v0.0.95:声明式清单能力(features.sources / generated_files /
  cfg 条件 sources / per-glob flags,#223)、汇编源一等公民(.S/.s/.asm 进
  sources,NASM 按目标推导 `-f`,#220)、build.mcpp 补全(环境契约 + cross 下
  运行 + 依赖包执行,#222)。
  已知问题:#230(本版 windows workspace 崩溃,0.0.96 修复)、#232(冷环境
  `xim:nasm` 自举产出空载荷,待修)。

## [0.0.94] — 2026-07-15

### 修复

- **依赖包被激活 feature 的 `sources` 在 `mcpp test` 下不编译**。`prepare_build()`
  把 feature 源集解析(drop + add)**整段**门在 `!includeDevDeps`,而 `mcpp test`
  走 `includeDevDeps = true` → 激活 feature 的 sources **从不被加回**构建图。
  descriptor 若把某个 glob **只**写在 `features` 下(xpkg 的 `features.X.sources`
  只落进 `featureSources`、从不进 base `sources`),该包在 `mcpp build` 下正常、
  在 `mcpp test` 下必然链接失败(`undefined reference`)。
  - 命中面:`compat.cjson` 的 `utils`(`cJSONUtils_*`)、`compat.eigen` 的
    `eigen_blas`(`dgemm_`)、`compat.spdlog` 的 `compiled`。
  - **`eigen_blas` 的 `dgemm_` 一直被记为「把 feature 编出的依赖目标链进 test
    二进制是 follow-up」——定性是错的**:不是链接问题,是**源集解析**问题。
  - 修复:**drop 仍只在 build 模式做**(`mcpp test` 需要保留完整源面,让 dev-dep
    轨的 per-test main 检测看得见 `gtest_main.cc` 并逐 test 剪枝——见
    `tests/e2e/79_gtest_regular_dep_feature_main.sh`);**add 改为两模式都做**,
    并**去重**,使 gtest 那种 base/feature 双列的 glob 不会进两次。
  - 回归测试:`tests/e2e/100_feature_sources_test_mode.sh`(cjson `utils` 在
    `mcpp test` 下必须编译并链接;`mcpp build` 仍正常;不请求 feature 时 gated
    源仍被排除)。

## [0.0.93] — 2026-07-15

### 变更(命名统一,全部旧拼写永久兼容)

- **工具链 × 目标 命名统一 —— 二轴身份模型**。toolchain = `family@version`
  (family 只剩 **gcc | llvm | msvc**),target = mcpp 自有三段 triple
  `arch-os[-env]`(Zig 式砍 vendor)。变体(gnu/musl/msvc)进 triple env 段,
  **"cross"/"musl"/"mingw" 不再是工具链名字**——`mingw-cross 16.1.0` 的本体是
  `gcc@16.1.0 → x86_64-windows-gnu`,交叉只是 host≠target 的关系。业界对照
  (rustup 零 "cross" 命名/Zig 三段 triple/musl.cc 分发层先例)与决策记录见
  `.agents/docs/2026-07-15-toolchain-target-naming-unification-design.md`。
  - **canonical triple**:`x86_64-windows-gnu` 为正典(D1);GNU 拼写
    `x86_64-w64-mingw32` 及 4 段 Rust 拼写为**永久别名**,归一后进同一
    `target/<canonical>/` 目录(同一构建缓存)。macOS 产物目录随 canonical 变为
    `aarch64-macos`。
  - **`--target` 封闭词汇表校验**:打错字**硬错 + did-you-mean**
    (`did you mean 'x86_64-linux-musl'?`),不再静默 fall through 编成宿主产物
    (最坏失败模式根治);自定义 triple 走显式 `[target.X]` 节逃生舱;planned
    档位(riscv64 等)报「registered but not yet supported」。两条硬编码约定
    (`*-musl`、`x86_64-w64-mingw32`)改为词汇表数据行(pin + 默认 static)。
  - **单一 triple 解析器 `triple.cppm`**:cfgpred/abi/model 谓词/registry 四处
    平行解析收敛;abi 的 os 维 `darwin`→`macos`(与 cfg 词汇分叉消灭,
    `darwin`/`arm64` 作为约束别名接受)。
  - **`compat.cppm` 兼容层**:唯一知道旧拼写的文件(musl-gcc/gcc@V-musl/
    <triple>-gcc/mingw/mingw-cross/clang),归一 + 单行 `note:` 提示;xim 分发包名
    (`mingw-cross-gcc` 等)不动——"cross" 在分发层合法(musl.cc/Debian 先例)。
  - **CLI 单名词 + `--target` 选项**(D4,不设 `mcpp target` 子命令):
    `toolchain install [gcc 16] --target <triple>`(family 可省→约定 pin)、
    `toolchain default gcc@16 --target <triple>`(默认变 **pair**,持久化
    `default` + `default_target` 两键)、`toolchain remove … --target <triple>`;
    主路径仍是 `mcpp build --target <triple>` 自动装链(零仪式)。
  - **`[build] target = "<triple>"`** 新 manifest 键(≙ cargo `build.target`):
    「默认全静态 musl」的正确归宿(产物属性,非编译器家族属性);优先级
    `--target` flag > `[build] target` > 全局 `default_target` > host。
  - **`toolchain list` 两轴重排**:Toolchains 块(family@version)+ Targets 块
    (target × 状态 installed/available/**planned**,planned 行使词汇表用户可见);
    修版本字典序排序 bug(9.4.0 不再排在 15.1.0 前);`gcc X-musl` 行不再被
    `llvm` 劈开。README 平台表从词汇表重画(target × tier 维度,补 MSVC=与
    windows-gnu 行——旧表 MSVC 仍标 planned 是错的)。
  - 修 Windows host 上 `mingw` 的门:Linux 上 `toolchain install mingw` 现在
    合法(= 装交叉 payload,同一身份 host 分流);`mcpp run` 位置参数 help 改为
    「Binary name」消除与 `--target` 的语义撞名。
  - 验证:单测 35(新增 triple/compat 套件);e2e 新增 103(typo/planned/逃生舱/
    `[build] target`/别名同目录)、102 双拼写断言;本机实测双拼写同 Resolved
    行+同缓存(alias 二跑 0.07s 全命中)、PE wine 真跑、musl 静态链、typo
    did-you-mean。

## [0.0.92] — 2026-07-15

### 新增

- **Linux → Windows MinGW-w64 交叉工具链(`mingw-cross`)—— host≠target 一等公民**。
  在 Linux 主机上交叉编译出 Windows x86_64 PE,含 `import std`。补 0.0.89
  `msvc-mingw-design` §4.4/§7 登记的延期项(交叉维:host≠target)。
  - **工具链**:从源码构建的 GCC 16.1.0 mingw-w64 **MSVCRT** 交叉链(triple
    `x86_64-w64-mingw32`,跟随 Rust Tier-1 `x86_64-pc-windows-gnu` 的 CRT 选型);
    自包含(自带 binutils/CRT/libstdc++ 含 `bits/std.cc` + `libstdc++exp`);
    发布于 `xlings-res/mingw-cross-gcc`(GitHub+GitCode)+ `xim-pkgindex`。
  - **mcpp 侧**:用户名 `mingw-cross` → xim `mingw-cross-gcc`(前端
    `x86_64-w64-mingw32-g++`);解除 MinGW 的 Windows-host 门(Linux 主机可装,native
    `mingw` 仍 Windows-only);`--target x86_64-w64-mingw32` 约定解析(默认 static);
    跳过 glibc/linux-headers(PE 自带 CRT)。
  - **host≠target 化**:PE 产物形态判断一律按 target 而非 host constexpr——std 模块源
    探测补 `<prefix>/<triple>/include/c++/` 子目录;自包含工具链跳过外部 binutils `-B`
    (musl + mingw);MinGW link 分支(`-static` / `-lstdc++exp`)由 `if constexpr(is_windows)`
    host 门提为运行期 `is_mingw_target` target 判定。
  - **验证**:e2e `102_mingw_cross_wine.sh`(`# requires: mingw-cross wine`,build --target
    → PE 静态自包含断言 → wine 跑 import std + 多模块);CI `cross-build-test.yml` 加
    `mingw-cross-wine` job(OS-cross,wine)。全链实测闭环(install→build→wine)。
  - 设计:`.agents/docs/2026-07-15-mingw-linux-cross-windows-design.md`。

## [0.0.91] — 2026-07-15

### 新增

- **`standard = "c++fly"` — 一行启用"最新标准 + 全部实验特性"(语言 + 标准库)**。
  语义三件套,全部按 resolved 工具链自动判定:①族最新 `-std=` 档位(GCC16→c++26、
  Clang→c++2c、MSVC→/std:c++latest);②该工具链支持的全部实验性语言特性门
  (GCC≥16:反射 `-freflection`;契约随 `-std=c++26` 默认启用,实机探测定案);
  ③标准库实验门(libc++→`-fexperimental-library`)。不支持的特性**软跳过**并打印
  summary(`c++fly on <toolchain>: <std>; enabled: ...; skipped: ...`)。
  新模块 `src/toolchain/cppfly.cppm` 承载三张数据表(族×版本×stdlib)——首个真正
  使用 `Toolchain::version` 做门控的查询点;产物汇入 0.0.90 的图全局方言旗标通道
  (全图 TU + P1689 扫描 + std BMI 预构建同源),派生旗标并入指纹。
  设计:`.agents/docs/2026-07-14-std-features-experimental-gate-design.md`。
  e2e 100(gcc16 硬路径:零手写旗标跑通 std::meta 反射)/ 101(clang 软路径:
  c++2c + skipped summary)。

### 修复

- **`standard = "c++latest"` 在 GNU 族误拼 `-std=c++latest`**(GCC/Clang 不识别,
  构建必败):canonical 现经 cppfly 的"族最新档"表解析为真实档位(GCC16→
  `-std=c++26`)。e2e 100 尾段回归覆盖。

## [0.0.90] — 2026-07-13

### 新增

- **MSVC 原生构建后端(cl.exe)落地,0.0.88 的构建门移除**。选定 `msvc@system`
  后 `mcpp build/run` 直接用系统 MSVC 编译链接:
  - 环境模型:`find_windows_sdk()` + 从检测到的 VC tools/SDK 直接合成
    INCLUDE/LIB/PATH(+`VSLANG=1033`),不跑 vcvarsall;SDK 缺失时检测/选择仍可用,
    构建报带指引的明确错误;doctor 新增 SDK 与 mingw 检查行。
  - 模块管线:std/std.compat.ixx 单命令 staging(`/ifcOutput` → ifc.cache),
    命名模块 `.cppm` 经 `/interface /TP` 编译、`/ifcSearchDir` 消费;
    `/scanDependencies` 作为第三个编译器内建 P1689 扫描驱动接入 dyndep。
  - 链接:link.exe/lib.exe(SeparateLinker)+ 响应文件(绕 cmd 8191 限制),
    DLL=`/DLL /IMPLIB:`;`deps=msvc`(/showIncludes)头文件依赖;`/MD|/MT` CRT
    随 linkage;`/std:c++20|c++latest` 映射;`.obj` 扩展名全链路。
  - fast-path 增量:构建缓存 env 槽新增 `@env` 多变量编码,增量构建重建
    INCLUDE/LIB 环境。e2e 99(模块/import std/增量)+ 95 改造为真实构建断言。
- **`[build] dialect_cxxflags` + 方言旗标全图化(issue #210 修复)**。
  `-freflection` 等"改变标准库头声明集"的 flag 现随 `-std=` 的通道到达:
  全局 cxxflags(项目+依赖所有 TU)、std/std.compat BMI 预构建命令、P1689 扫描。
  known-list 自动提升(reflection/contracts/char8_t/`_GLIBCXX_USE_CXX11_ABI`)+
  显式 `dialect_cxxflags` 逃生舱;指纹早已包含这些 flag,修的是命令构造。
  实证:#210 的最小复现(gcc16 + `import std;` + `std::meta`)输出 `x 2/y 3`;
  e2e 98 含依赖模块变体。

### 修复与优化

- mingw 在非 Windows 主机的 `toolchain install/default` 现在明确报
  windows-only(此前是 `invalid xpkg target 'xim:mingw-gcc@'`)。
- std 模块 staging 命令在 Windows 用 `cd /d`(跨盘;工作区 D: + 缓存 C: 的
  真实 CI 布局)。
- release 的 publish-ecosystem:镜像脚本改为批量上传+带耐心的 ranged-GET 验证、
  **验证超时不再删除资产**(0.0.89 因逐资产"18s 即删重传"+全量 GET 探测触顶
  20min 被杀);timeout 兜底 20→30。
- stdmod 执行层支持工具链声明环境(capture_with_env);shell 引用平台化。

## [0.0.89] — 2026-07-13

### 新增

- **MinGW-w64 工具链入 xlings 生态(Windows 原生 GCC,无需 Visual Studio)**。
  `mcpp toolchain install mingw 16.1.0` / `default mingw@16.1.0`:xim 包
  `mingw-gcc`(winlibs GCC 16.1.0 + MinGW-w64 14.0.0 UCRT 独立构建,镜像于
  xlings-res/mingw-gcc,GitHub+GitCode 双端)。复用既有 GCC 后端
  (gcm 模块管线、libstdc++ `bits/std.cc` 的 `import std`);Windows 上
  libstdc++/libgcc 默认静态链接(产物免带 DLL,`[build] static_stdlib=false`
  可关),`linkage="static"` 升级全静态。e2e 97 覆盖 install→default→
  多模块 build/run→独立 exe 验证;ci-windows 新增专项步骤。
  连带修复:`toolchain list` 的 Available 段与部分版本解析此前硬编码按
  "linux" 平台读取 xpkg 版本(Windows/macOS 上恒空);`toolchain install`
  在 Windows/macOS 不再错误安装 glibc/linux-headers 依赖。

### 重构

- **工具链后端抽象层(Part A,对 GCC/Clang 零行为变化,build.ninja 零 diff
  实证)**。新增 `mcpp.toolchain.dialect`(命令行拼写 traits:gnu/msvc 两行
  数据,`-I/-D/-std=/-c/-o/-O/-g/ar` 及归档命令模板经其发射);`BmiTraits`
  并入模块旗标拼写(`compileModulesFlag`/`stdBmiUsePrefix`/
  `moduleOutputPrefix`/`bmiSearchPrefix`),flags.cppm 的 is_clang 分支改由
  数据驱动;`ProviderCapabilities.has_builtin_p1689_scan` 取代 ninja 后端的
  is_gcc 门;`Toolchain::envOverrides`(EnvVar 表,注入 ninja 子进程环境,
  为 MSVC 后端 INCLUDE/LIB/PATH 预留);gcc provider 增加与 clang 同形的
  `std_module_build_commands`(命令序列);`resolve_link_model` 在 Windows
  显式返回 PE 空模型。设计:`.agents/docs/2026-07-13-toolchain-backend-
  abstraction-msvc-mingw-design.md` + 同日触点审计文档。

## [0.0.88] — 2026-07-13

### 新增

- **MSVC 系统工具链支持(`msvc@system`,detection-first)**。MSVC 作为首个
  "系统工具链"接入:mcpp 负责**定位与识别**(vswhere → `VSINSTALLDIR`/
  `VS*COMNTOOLS` → 标准安装路径三级发现;cl.exe banner 解析出编译器版本/架构,
  容错本地化 banner),**从不安装/卸载** MSVC 本体。
  - `mcpp toolchain default msvc`:检测系统 MSVC,打印 VS 产品/VC tools/
    cl 版本与 `std.ixx`(import std)可用性,持久化稳定 spec `msvc@system`
    (不落具体版本,VS 升级后配置依然有效);未安装时输出安装指引
    (VS Installer C++ 工作负载 / `winget install …BuildTools`)并退出非零。
    `msvc@19.44` 形式为 pin 校验(仍取最新 VC tools,前缀不符则报错)。
  - `mcpp toolchain list`:Windows 上新增 `System:` 段展示检测到的 MSVC;
    `install msvc` 报告已装现状或给指引;`remove msvc` 明确拒绝(系统组件)。
  - `mcpp self doctor`:Windows 上新增 "msvc (system)" 检查段。
  - manifest 支持 `[toolchain] windows = "msvc@system"`(types.cppm 注释中的
    既有 schema 首次落地);非 Windows 主机使用 msvc spec 时给出明确报错。
  - `mcpp build`:原生 cl.exe 构建(.ifc 管线)**本版暂不支持**,在工具链解析
    后以单一 owned 错误信息拦截,并提示可用的 `llvm@20.1.7`(MSVC-ABI Clang)。
  - detect() 新增 cl.exe 分类路径(文件名短路,banner → 版本/triple),
    `bmi_traits` 预置 MSVC `.ifc` 分支;e2e 95/96 + 单测覆盖。
  - 设计文档:`.agents/docs/2026-07-13-msvc-system-toolchain-detection-design.md`。

## [0.0.87] — 2026-07-09

### 修复

- **项目本地模式:不再把官方全局索引 `xim` 注入项目作用域**。此前带自定义
  `[indices]`(本地 path 索引)的工程进入项目本地模式时,`ensure_project_index_dir`
  会无条件把官方 `xim` 索引 append 进项目 `.xlings.json` 的 `index_repos`
  (`config.cppm`),意在让 `xim:*` 依赖在项目模式可解析。但 xlings 按"repo 落在
  哪一组"决定作用域——项目 `index_repos` 里的包一律 `PackageScope::Project`,于是
  `xim` 的**全局工具**(cmake/glibc/gcc/make/binutils 等)整体被错误地"项目化"、
  装进项目 store 而非共享 registry。由此 build-dep 工具(如 `xim:cmake`)的 ELF
  interpreter 被指向项目 store 里未物化的 glibc → `cannot execute`,任何在
  `install()` 里执行 glibc-动态 build-dep 工具的 compat 包(如从源码 CMake 构建的
  OpenCV)在 `mcpp test` 下必现,且与宿主历史无关(fresh `MCPP_HOME` 亦复现)。
  **修复**:移除该注入及配套的项目 data dir `xim` 副本暴露。`xim`(及其动态发现的
  sub-index)是 xlings 全局默认索引,**global 即默认作用域**——`xim:*` 经全局
  index_repos + registry 本地 clone 正常解析、装 registry,并经 additive 对项目
  可见;只有用户在 `[indices]` 声明的本地自定义索引才项目化。设计与分析见
  `.agents/docs/2026-07-09-project-index-scope-global-infra-fix.md`。

## [0.0.84] — 2026-07-08

### 修复

- **clang 驱动配置文件(cfg)补全头文件搜索路径**:`fixup_clang_cfg` 再生成的
  cfg 此前仅包含链接相关条目(`-B`/`-L`/动态链接器/rpath),缺少 C 标准库头文件
  与内核头文件的搜索路径。该 cfg 服务于直接调用打包内 `clang`/`clang++`
  (不经由 mcpp)的场景:缺少这两项时,此类调用仅在宿主系统存在
  `/usr/include` 时可编译(依赖宿主环境,违背沙箱自包含约束),在无宿主开发头
  文件的环境中直接报头文件缺失错误。本次补充
  `-isystem <glibc payload>/include` 与 `-isystem <linux-headers payload>/include`,
  置于 libc++ 头文件条目之后以保持 `#include_next` 搜索链;生成内容与
  xim-pkgindex 侧 `llvm.lua` 安装期生成的 cfg 保持一致,消除两个生成端之间的
  内容差异。fixup 修订号升级至 `hermetic-3`,既有 payload 在下一次构建时自动
  重新收敛,无需重新安装。验证方式:以 `--sysroot=<空目录>` 屏蔽宿主头文件后,
  由 cfg 驱动的 `clang`/`clang++` 直接调用编译与运行均通过;移除上述搜索路径的
  对照组按预期失败。mcpp 自身构建路径不受影响(构建 flags 由 linkmodel
  独立提供,不读取 cfg)。


### 修复

- **Linux llvm 工具链链接失败 `cannot open Scrt1.o/crti.o/crtn.o`(#195)**:clang-with-cfg
  的 payload 链接路径此前只带 `-L/-rpath/--dynamic-linker`,缺少 CRT 启动对象的发现前缀
  `-B<glibc payload lib>`——driver 查找 `Scrt1.o/crti.o/crtn.o` 只走 `-B` 前缀与 sysroot
  派生路径,不查 `-L`。在装有宿主 libc6-dev 的机器上 driver 会静默兜底宿主 `/lib` 的 CRT
  (污染式"假绿"),在没有的机器(如全新 WSL2)上则把裸文件名传给 lld 直接失败。

### 新增 / 架构

- **工具链链接模型单一化(hermetic toolchain link model)**:新增 `mcpp.toolchain.linkmodel`
  作为「如何对该工具链的 C 库编译/链接」的唯一解析器(payload-first,--sysroot 回退),
  `flags` / `stdmod` / `build_program` / cfg 再生全部消费同一模型,消除四份漂移实现;
  动态链接器名按 声明式 payload 元数据 → 按 triple 的 arch 映射 → glob 三级解析,全链
  不再硬编码 `ld-linux-x86-64.so.2`(aarch64 glibc 的 loader 障碍随之消除)。详见
  `.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`。
- **post-install fixup 归位为统一管线**:`ensure_post_install_fixup` 成为所有工具链安装
  路径(显式 install / 默认工具链 auto-install / manifest `[toolchain]` auto-install)共享
  的唯一 fixup 入口,内容指纹 marker 幂等;此前 manifest 路径不跑任何 fixup。clang cfg
  由行级补丁改为从链接模型**确定性再生**(同一 payload 在任何机器/安装路径产出一致 cfg,
  人类直接使用 `clang++` 同样获得 hermetic 的 CRT 发现)。
- **hermetic 链接校验**:构建前用 `-###` 干跑断言 CRT 对象与生效 dynamic linker 全部解析
  在沙箱(xpkgs registry)内,越界即报错并指明泄漏路径;逃生阀
  `[build] allow_host_libs = true` / `MCPP_ALLOW_HOST_LIBS=1`。按 flag 集缓存判定。
- **测试与 CI**:新增 e2e `86_llvm_hermetic_link.sh`(`-###` 前缀断言,双向防「链接失败」
  与「宿主污染」回归);llvm e2e 解除 20.1.7 硬 pin(`MCPP_E2E_LLVM_VERSION`,默认最新
  已装 payload);ci-linux-e2e 新增 **无宿主工具链容器 job**(debian:stable-slim,无 gcc /
  无宿主 CRT)——唯一能真实复现 #195 环境类的 CI 形态。

## [0.0.71] — 2026-06-29

### 新增

- **Feature 系统 v2 Stage 2a — 由 feature 激活的可选依赖**:声明于 `[feature-deps.<name>]`
  段(或 Lua 描述符中 feature 的嵌套 `deps` 表)的依赖为**可选**依赖,仅当该 feature 处于激活
  状态(根 `--features` 或依赖 spec 的 `features=[...]`)时才进入解析;声明于 `[dependencies]` 的
  依赖始终解析。可选性由声明位置表达,无需额外的 `optional=true` 标志。实现上,`prepare_build`
  在为根包播种解析 worklist 之前、以及在每个依赖的 manifest 加载之后,将该 manifest 的活跃
  feature-deps 合并进其 `dependencies` 映射,后续既有的 worklist BFS 与 Stage 3 能力绑定即自动
  接管——一个 `backend-openblas` feature 可同时**拉取** provider(`compat.openblas`,
  `provides=["blas"]`)并**开启**消费开关(`implies=["use_blas"]`,`requires=["blas"]`),图中单一
  provider 时能力自动绑定。Lua 描述符的 feature `implies` 亦补齐解析(此前仅 TOML 支持)。详见
  `.agents/docs/2026-06-29-feature-optional-dependencies-s2-design.md`。

  > 实现注记:上述两个 helper(`activateFeatures`/`mergeActiveFeatureDeps`)必须为 prepare_build
  > 内的局部 lambda,而非文件作用域函数。若作为模块接口单元中的导出(inline)函数,其 `std::map`
  > 实例化会泄入发射的 BMI,触发 GCC 16 modules 缺陷——另一导入 `std` 的翻译单元随即报
  > `fatal error: failed to load pendings for __normal_iterator`。局部化可将实例化限制在实现单元内。

## [0.0.70] — 2026-06-29

### 修复

- **首次初始化在海外网络与 GitHub 托管 CI 上的冷启动失败(`index missing`;patchelf / ninja
  bootstrap 失败)**:`mcpp self env` 为新建的 `MCPP_HOME` 播种 `.xlings.json` 时,将 `mirror` 字段
  硬编码为 `"CN"`。xlings 的 `normalize_mirror_` 仅接受 `GLOBAL` 与 `CN` 两个合法取值,故 `"CN"`
  被直接采用并解析至 gitcode,致使 xlings 内置的区域探测 `detect_install_mirror_()` 被跳过——该例程
  经 `tinyhttps::probe_latency` 测量 github 与 gitcode 的连接延迟,择可达且更低延迟者。在美国区域的
  runner 上,gitcode 不可达或显著较慢(实测 github 70 ms、gitcode 1060 ms),由此索引与沙箱的冷
  bootstrap 失败。本版将播种值改为 `"auto"`:`normalize_mirror_("auto")` 判定为非法取值,xlings 视其
  为未设置并执行自身探测,在美国区域解析至 GLOBAL、在中国大陆解析至 CN。镜像选择的职责由此归还
  xlings(其已基于 tinyhttps 实现该机制),mcpp 不再代为决策。播种仅在 `.xlings.json` 不存在时发生,
  显式的 `mcpp self config --mirror CN|GLOBAL` 配置不会被覆盖。

### 新增

- **`MCPP_VERBOSE` 环境变量**:取非空且非 `"0"` 的值时,为每一次 mcpp 调用启用 verbose 日志,涵盖
  e2e 脚本中未携带 flag 的 `$MCPP` 调用,便于 CI 诊断。该变量与既有的 `MCPP_LOG_LEVEL`(仅控制文件
  日志级别)互补;显式 `--quiet` 仍具有更高优先级。该变量已在 fresh-install 等 workflow 中启用,但
  不含运行「默认静默」输出断言的 e2e 套件。
- **`update_index` 冷启动重试**:索引同步为网络 git 操作,单次瞬时故障原会直接导致冷启动失败。本版
  改为有界退避重试(至多 3 次,退避 2 s / 4 s);成功路径于首次尝试即返回,稳态无额外延迟,仅失败
  时方触发退避。

## [0.0.69] — 2026-06-29

### 新增

- **Feature 系统 v2 — feature 可贡献「包自有 defines」+ capability(provides/requires)能力绑定**:
  解决「`compat.eigen` 启用 `blas` 特性后,`compile_commands.json` 里只有 `-DMCPP_FEATURE_BLAS`、
  没有上游真正读的 `-DEIGEN_USE_BLAS`,特性形同未启用」这一类根因——旧版 feature 激活**只能**产出
  `-DMCPP_FEATURE_<NAME>` 宏 + 门控源文件,无法表达任意宏、更无法做 backend 选择。本次按
  「功能全覆盖 + 少即是多」收敛为**两个原语**(详见
  `.agents/docs/2026-06-29-feature-capability-model-design.md`):

  - **Stage 1 — feature `defines`**:`[features]` 条目可写成**表形式**
    `name = { defines = ["EIGEN_USE_BLAS"], implies = [...] }`(TOML 与 Lua 描述符两面均支持);
    激活时每个**裸名** define 脱糖为 `-D<x>` 加到该包编译标志,与自动的 `-DMCPP_FEATURE_<NAME>`
    并存。按行业经验(vcpkg)**刻意限制**为「包自有命名空间宏」,feature **不**注入自由
    `cflags`/`ldflags`,以保持 feature union 组合性。
  - **Stage 3 — capabilities**:包/特性可 `provides`/`requires` 一个**抽象能力字符串**(如 `blas`),
    解析器从依赖图中**绑定唯一 provider**——确定性:`[capabilities]` pin / `--cap` 指定者胜出;
    图中**恰好一个** provider 自动绑定;**零个**或**多个未指定**均**硬报错**(绝不静默猜测)。
    这把「静默用错/缺失后端」变成配置期显式报错。link/include 仍走既有依赖机制流动。

  > Stage 2(feature 触发的可选依赖自动拉取 + 全图 feature union 统一)作为下一阶段:它需要把
  > 特性计算提前到依赖解析之前(解析阶段重排),风险更高,且 capability/Eigen 用例并不依赖它
  > (provider 以显式依赖声明)。本次先发坚实的 S1+S3,符合设计文档「各阶段独立可发」原则。

## [0.0.67] — 2026-06-26

### 修复

- **带命名空间前缀的依赖解析失败 `index entry not found in local clone`(自定义 ns + 非规范文件名)**:
  当一个包以「裸 `name` + 独立 `namespace` 字段」形态声明(如 `aimol.tensorvia-cpu`:
  `name="tensorvia-cpu"`、`namespace="aimol"`),并以**非规范文件名**落盘在共享索引里
  (`pkgs/t/tensorvia-cpu.lua` 而非 `pkgs/a/aimol.tensorvia-cpu.lua`)时,限定请求
  `aimol.tensorvia-cpu` 报「索引条目缺失」,而裸名 `tensorvia-cpu` 却能解析。根因是
  **候选消歧 `selectDependencyCandidate` 用「规范文件名 `<ns>.<short>.lua` 是否存在」当身份
  判据**——描述符以非规范文件名落盘时,正确的 peer-root 候选 `(aimol, tensorvia-cpu)` 对消歧
  器隐形,请求被钉死在错误的首选候选 `(mcpplibs.aimol, …)` 上并被身份门拒绝。修复:候选消歧
  改为**身份优先**,经由加载路径同款的身份校验读取器(`read_xpkg_lua*`)按描述符**声明的
  `(ns, name)`** 定位候选,文件名不再参与身份判定——选择层与加载层从此不可能对同一候选产生
  分歧。详见 `.agents/docs/2026-06-26-identity-first-resolution-no-filename.md`。

## [0.0.66] — 2026-06-26

### 修复

- **LLVM 工具链产物运行期 `libatomic.so.1: cannot open` / 真用原子时链接报 `undefined __atomic_*`**:
  16 字节及超宽 `std::atomic` 会降级成 `__atomic_*` 外部调用,这些符号位于 **libatomic**
  (GCC 运行时库,LLVM 无对应物),而编译器驱动**不会自动链接** libatomic。mcpp 现在在
  Linux 链接行注入 `-Wl,--push-state,--as-needed -latomic -Wl,--pop-state`:真正用到原子的
  程序自动链上并保留依赖,未用到的程序经 `--as-needed` 自动丢弃、产物零额外依赖。注入是
  **自守卫**的——仅当工具链链接目录里存在可解析的 libatomic(动态链接 `libatomic.so`/`.a`,
  静态链接 `libatomic.a`)时才发出 `-latomic`,因此对不附带 libatomic 的工具链零回归。
  与之配套的 llvm 资源包需把 libatomic 打入 `lib/<triple>/`(详见
  `.agents/docs/2026-06-26-llvm22-libatomic-self-containment-design.md`)。

## [0.0.65] — 2026-06-25

### 修复

- **`mcpp add gtest` + `mcpp build` 报 `duplicate symbol: main` / `LNK2005`**(#168):
  gtest 作为**常规依赖**时,其 `gtest_main.cc`(自带 main)被链进应用,与应用自身的
  main 冲突。修复采用**通用的「feature 门控源」机制**:依赖描述符可声明
  `[mcpp].features.<名>.sources`,被某 feature 列出的源**默认不编译/链接**,仅在该
  feature 被请求(`dep = { version="…", features=["…"] }`)时纳入。gtest 描述符把
  `gtest_main.cc` 归入 `main` feature → **默认只链框架,不再撞 main**;需要 gtest 提供
  main 时 `gtest = { version="1.15.2", features=["main"] }` 显式开启。
  门控仅作用于 `mcpp build`;`mcpp test` 保持既有的 dev 依赖 main 检测(0.0.64)不变。
  详见 `.agents/docs/2026-06-25-gtest-main-feature-and-add-dev-design.md`。

### 新增

- **`mcpp add --dev <pkg>`**:把依赖写入 `[dev-dependencies]`(测试专属,如 gtest;
  由 `mcpp test` 消费,不链进 `mcpp build` 的应用)。

### 测试

- 单元 `SynthesizeFromXpkgLua.FeatureGatedSources`(描述符 feature 门控源解析);
  e2e `79_gtest_regular_dep_feature_main.sh`(#168 哨兵 + `features=["main"]` opt-in +
  `add --dev`)。

### CI

- release workflow 默认 xlings 版本 `0.4.58` → **`0.4.60`**(缓存键同步更新)。

## [0.0.64] — 2026-06-25

### 修复

- **`mcpp test` 在自带 `main()` 的测试 + gtest dev-dep 下 `duplicate symbol: main`**:
  gtest 的 `gtest_main.cc` 自带 `main()`,而 mcpp 此前把依赖的**全部对象内联**进每个
  测试二进制,于是测试自己的 `main()` 与 `gtest_main.o` 撞符号。修复:**兑现依赖
  描述符里已声明的 `kind="lib"`**——把这类依赖编译成静态归档 `lib<pkg>.a`,链接在
  测试对象**之后**;标准归档语义只在符号未定义时拉成员,故 `gtest_main.o` 的 `main`
  只在测试不自带 `main` 时才被拉入。`{自带/框架 main} × {用/不用 gtest}` 全部组合
  皆正确,用户无感。纯模块依赖(如 mcpplibs.cmdline,无非模块对象)行为不变。
  这是**通用** link-model 改进、由既有描述符 `kind` 驱动,**无 gtest 特例**,未来
  测试框架声明 `kind="lib"` 即自动适配。详见
  `.agents/docs/2026-06-25-dependency-archive-linking-design.md`。

### 测试

- 新增单测 `NinjaBackend.ArchiveInputsLinkedAfterObjects`(归档须排在对象之后)与
  跨平台 e2e `78_test_main_combinations.sh`(四种 main×gtest 组合 `mcpp test` 全绿)。

## [0.0.63] — 2026-06-25

### 修复

- **`tests/` 目录无代码提示**:clangd 在测试文件里对 `gtest::InitGoogleTest()`、
  `import std` / `import mcpplibs.*` 全无补全。根因:`compile_commands.json` 是当次构建
  plan 的镜像,`mcpp build` 的 plan 不含 `tests/**/*.cpp` 与 dev-deps,而它与 `mcpp test`
  写同一个 cdb——后写覆盖前写,日常「编辑→build」循环里测试条目几乎总被擦掉。修复:
  `write_compile_commands` 由「全量覆盖」改为「**合并保留**」——保留当前 plan 未覆盖但
  文件仍存在的旧条目(上次 `mcpp test` 写入的测试条目),剪除已删文件。`mcpp build` 自身
  **零改动**:不解析、不下载任何 dev-deps,build-only 用户与构建图均不受影响(offline-first)。
  跑一次 `mcpp test` 后,测试补全在后续所有 `mcpp build` 中持久生效。
  详见 `.agents/docs/2026-06-25-cdb-test-coverage-design.md`。

### 测试

- 新增单测 `tests/unit/test_compile_commands.cpp`(合并/剪除/去重/坏 JSON 回退)与跨平台
  e2e `77_cdb_preserves_test_entries.sh`(`mcpp test` 后真实重建 `mcpp build` 仍保留测试条目)。

## [0.0.62] — 2026-06-24

### 修复

- **macOS 链接 `library not found for -lSystem`**(#43):macOS 链接命令此前从不显式传 SDK,
  链接侧靠 clang 隐式探测(xcrun/`SDKROOT` → ld64 `-syslibroot`)去找 `libSystem`。干净的 CI
  Xcode runner 上探测正常、缺陷被掩盖;真机一旦 `xcode-select` 指向异常 / 只装 Command Line
  Tools / 新装 bundled clang,探测失效就 `ld64.lld: library not found for -lSystem` + 所有 libc
  符号未定义。修复:`f.ld` 显式追加 `-isysroot <SDK>`,并给 `macos::sdk_path()` 加多级回退
  (`SDKROOT` → `xcrun` → `xcrun --sdk macosx` → `xcode-select -p` 推导 → 固定路径),即便
  xcrun 返回空也能定位 SDK,把链接从「碰运气」变「确定」。(#162)
- **macOS 首跑需手动回车 / stdin 挂起**:装 POSIX 工具链时进程等待 stdin,POSIX 路径也 seal
  stdin(`</dev/null`),不再要求交互按键。(#163)

### 测试

- 新增跨平台 e2e `76_compile_commands_generated.sh`:`mcpp new` + `mcpp build` 一个最小工程,
  在 Linux / macOS / Windows 三平台断言根目录生成合法 `compile_commands.json`。因 `mcpp build`
  含链接步骤,它同时是 macOS `-lSystem` 链接缺陷的跨平台回归哨兵。(#165)

## [0.0.61] — 2026-06-24

### 新增

- **离线优先的索引刷新**:`mcpp build` 不再因 TTL 过期就自动联网 `xlings update`。改为
  **miss-triggered**——依赖在本地索引里就直接用(零网络,消除弱网/Termux 首跑卡顿);依赖在
  本地查不到时才刷新一次去拉它(打印 `Refreshing package index — \`<pkg>\` not found locally`,
  并有 120s 防重,避免一个 build 里多个缺包各跑一遍全量 git 同步)。
- **`mcpp index status`**:只读、全程不联网,显示 xim/mcpplibs 两索引的 present/fresh/age/path;
  缺索引时提示显式 `mcpp index update`。
- **install.sh 多架构**:新增 `linux-aarch64`(aarch64 / arm64),并支持 GitHub→GitCode(CN)
  镜像回退(`MCPP_MIRROR=CN` 强制 GitCode),让被墙网络下同一条安装命令可用。
- **first-init 细粒度计时日志**:`--verbose` 下首次初始化(sandbox 布局、patchelf/ninja bootstrap)
  各步带时间戳 + `ScopedTimer` 耗时(`[VERBOSE <ts>] … done (Δ=<ms>ms)`),便于定位"卡很久"的步骤。

### CI

- e2e 套件拆为独立的 `ci-linux-e2e.yml`,与 build/单测/工具链矩阵**并行**,缩短每个 PR 的关键路径。
- `tests/e2e/run_all.sh` 每个用例输出耗时 + 末尾「最慢优先」汇总,便于后续分片/优化。

### 杂项

- 自托管清单改用 TOML 原生命名空间依赖写法 `mcpplibs.cmdline = "0.0.1"`(去掉遗留引号)。

## [0.0.57] — 2026-06-20

### 修复

- 包描述符解析改为 **identity-first**:不再「按候选文件名跨索引无序扫描、撞上第一个就返回」,
  而是用描述符声明的规范 `(ns, name)` 二元组校验命中文件的身份。修复 `compat.zlib` 在全新
  CI 上偶发 `index entry has no mcpp field`(外来 `xim-pkgindex/.../zlib.lua` 因目录遍历
  顺序先被撞到而冒充 `compat.zlib`)。索引目录改为排序后确定性遍历。

### 重构

- 新增统一的 `canonical_xpkg_identity()` 归一器(身份 = 二元组 `(ns, name)`;`ns` 为可分层命名
  空间路径,`name` 为单一末段;点号名 `a.b` 本质 `(a, b)`)。归一三步:无声明 ns → 继承所属索引
  默认 ns;求 FQN;按最后一个点切分。匹配 = 限定请求精确相等 / 非限定请求按默认搜索路径
  `[mcpplibs, compat]`。`compat` 降级为搜索路径里的数据项(`kCompatNamespace`),不再是匹配
  分支。`[indices]` 路径索引的无命名空间描述符继承索引命名空间。

### CI

- `ci-{linux,macos,windows}.yml` 各加一步:用本次构建出的 mcpp `git clone` 并 `mcpp build` /
  `mcpp run` 外部 C++ 工程 xlings(openxlings/xlings),验证自托管 mcpp 能构建真实外部项目。

## [0.0.56] — 2026-06-19

### 修复

- `mcpp run` / `test` / `build` 不再把目标的捆绑 glibc `LD_LIBRARY_PATH` 注入到
  mcpp 自身进程,因而泄漏进它启动的宿主 `/bin/sh`。在 glibc 比捆绑版(2.39)更新的
  发行版上,`sh` 会被强制加载捆绑的旧 libc,无法满足宿主 `libtinfo` 的 `GLIBC_2.42`
  符号而在目标运行前崩溃(报错形如 `sh: ... version 'GLIBC_2.42' not found`)。新增
  `platform::process::run_exec` / `capture_exec`:直接 exec(不经 shell),额外环境
  只作用于子进程;run / test / 快速路径 ninja / 整次构建 ninja 四个启动点全部改走它。

### 变更

- `mcpp pack --mode` 模式更名,语义更清晰(旧名保留为永久别名,tarball 后缀冻结不变):
  `bundle-project`→`vendored`(默认)、`bundle-all`→`self-contained`;新增
  `system` 模式(完全依赖宿主提供所有共享库,用于发行版打包 / 同发行版部署)。
  `static` 不变。两轴模型:libc 由 `--target` 选(gnu/musl),`--mode` 只选打包深度。

## [0.0.55] — 2026-06-18

### 新增

- `[targets.<name>]` 新增按目标的键 `defines` / `cxxflags` / `cflags`,作用于该目标
  **独占的入口源**(它的 `main`)。用于二进制入口私有的标志(如 `-DBUILD_SERVER=1`、
  局部告警抑制),不影响共享模块/实现对象(compile-once 模型不变)。需要穿透共享代码的
  差异请用 workspace member 或 `[features]`(#131)。
- `[targets.<name>]` 新增 `required_features`:仅当列出的 feature 全部激活时才构建该目标,
  否则静默跳过。是构建选择门禁,不激活 feature。
- `mcpp test` 现在接受 `--profile` / `--features` / `--strict`,让被测代码与测试二进制
  在所选 profile/feature 下编译(适合 sanitizer、契约求值语义等整次构建模式)。

### 变更

- `[targets.<name>]` 下的不支持键不再被静默丢弃,而是产生 warning(`--strict` 下为 error),
  并指引到正确的机制(workspace / features / profile)。
- 文档 `docs/05-mcpp-toml.md`(及 `docs/zh`)新增"构建配置该放哪"的决策指引。
  设计记录见 `.agents/docs/2026-06-18-per-target-build-config-design.md`。

## [0.0.54] — 2026-06-10

### 修复

- `mcpp new <name> --template <pkg>`:对声明了命名空间的模板包(如
  `mcpplibs.llmapi` 以裸名 `llmapi` 引用)现在能从描述符派生出
  (namespace, shortName) 坐标,正确完成 semver 解析与安装(#130)。

### 其他

- 架构重构(零行为变更):`cli.cppm` 从 6192 行精简为约 480 行的纯命令
  分发层;`src/cli/cmd_*` 仅保留参数解析与路由,全部领域实现下沉到属主
  子系统 —— `mcpp.build.{prepare,execute}`、`mcpp.toolchain.{post_install,
  lifecycle}`、`mcpp.pm.index_management`、`mcpp.bmi_cache.maintenance`、
  `mcpp.scaffold.create`、`mcpp.publish.pipeline`、`mcpp.pack.pipeline`、
  `mcpp.doctor`、`mcpp.project`、`mcpp.fetcher.progress`。
  设计与迁移记录见 `.agents/docs/2026-06-10-cli-modularization.md`。

## [0.0.53] — 2026-06-09

### 新增

- 库 / 组件下载现在与工具链下载一样显示实时进度条、字节进度与速度。自定义 /
  项目索引依赖改经 xlings NDJSON `interface install_packages` 安装(仍落在项目
  本地数据根,不改变安装位置与 install hook 顺序),不再静默卡住。

### 修复

- 下载连接 / 预取大小阶段(`totalBytes` 尚未知)进度行不再"冻结"无反馈:
  新增不确定态渲染,显示 `connecting…` + 已用时,流式无 `Content-Length`
  时显示已下载字节,直到拿到总大小再切换为百分比进度条。

### 其他

- 内置 xlings 版本上调至 `0.4.51`。
- 下载进度的状态机与渲染集中到 `mcpp.ui`(`DownloadProgress`),工具链 /
  内置索引 / 自定义索引三条路径共用同一套 UI。

## [0.0.46] — 2026-06-03

### 新增

- 共享库 target 支持声明 `soname`,Linux 构建会传递 `-Wl,-soname,...`,
  并在运行产物目录生成 ABI 名称 alias,供下游 `DT_NEEDED` / `dlopen()`
  以标准 SONAME 加载。

### 修复

- `mcpp run` / `mcpp test` 会把工具链 runtime 目录加入进程库搜索环境。
  这修复了 GLX/OpenGL driver 这类经由 `dlopen()` 加载的库无法找到自身
  `DT_NEEDED` 闭包的问题。

## [0.0.45] — 2026-06-02

### 修复

- 修复裸依赖选择器无法 fallback 到独立 root 包的问题。现在
  `imgui = "0.0.1"` 会先尝试省略前缀的 `mcpplibs/imgui`,若候选包身份不匹配,
  会继续匹配独立 root `imgui`,避免把非 `mcpplibs` 体系的包误解析为
  `mcpplibs.imgui`。
- 选择候选 xpkg 描述时校验 `package.name` / `package.namespace`,并在 lockfile
  中保留独立 root 包的空 namespace 身份。

## [0.0.44] — 2026-06-02

### 修复

- 修复 git branch 依赖的缓存身份和 lockfile source 元数据。branch 依赖现在会先
  解析到具体 commit,缓存 key 会随远端 branch 更新而变化,lockfile 也会记录
  `git+<url>#branch=<name>@<sha>` 而不是错误落到 `index+mcpplibs@`。

## [0.0.43] — 2026-06-02

### 新增

- 支持在单个 `[dependencies]` / `[dev-dependencies]` /
  `[build-dependencies]` / `[workspace.dependencies]` 表中使用多段 dotted
  dependency selector,例如 `imgui.core = "..."` 会先尝试
  `mcpplibs.imgui/core`,未命中时再尝试同级根 `imgui/core`。
- `xpkg.lua` 的 `mcpp.deps` 支持同样的 dotted selector 规则,方便 compat、
  imgui 等生态根和 `mcpplibs` 并列演进。

### 改进

- `mcpp add` 默认保留用户写入的 dotted selector,显式 namespace 仍可使用
  `ns:name` 写入 `[dependencies.<ns>]`。

## [0.0.42] — 2026-06-01

### 新增

- 将 `[package].standard` 打通为一等 C++ 标准配置,默认仍为 `c++23`,
  并支持 `c++26` / `c++2c` 等写法。

### 修复

- 编译 flags、`compile_commands.json`、fingerprint 与 `import std` 标准库
  BMI 预构建命令现在使用同一个 active C++ 标准。
- `std.gcm` / `std.pcm` cache 增加元数据校验,只有 compiler、stdlib、target、
  standard、source 与 build command 匹配时才复用。
- `build.cxxflags` 回归附加 C++ flags 语义,若写入 `-std=` 会提示迁移到
  `[package].standard`。

## [0.0.41] — 2026-06-01

### 修复

- 修复 Objective-C `.m` 源文件在 Ninja 后端被路由到 C++ 编译规则的问题。
  `.m` 现在与 `.c` 一样使用 C/Objective-C 编译器与 `cflags`,避免 macOS
  GLFW 等上游 Objective-C 源被错误附加 `-std=c++23`。

## [0.0.40] — 2026-06-01

### 修复

- 修复 project-local index 包的 xpm hook 工具依赖无法解析官方 `xim`
  索引的问题。项目级 xlings 配置现在会在 custom/local index 旁边显式暴露
  官方 `xim` 索引,让 `xim:python` 等 hook 工具依赖可用。

## [0.0.39] — 2026-06-01

### 修复

- 修复 project-local index 包安装时没有走项目 xlings 数据根的问题,本地 path
  索引现在通过 xlings CLI 直接安装到项目数据目录,避免 hook 查找不到同索引包。
- 修复包 install hook 运行前 `mcpp.deps` 尚未安装的问题,库/头文件依赖可以继续
  留在 `mcpp.deps`,只有 hook 执行工具需要放入 xpm deps。

## [0.0.38] — 2026-05-31

### 新增

- 支持包描述拥有自己的 `ldflags`,依赖包声明的链接参数会随包源码编译
  一起进入最终链接命令,消费方项目不再需要手动补齐第三方 C/C++
  库的私有链接参数。

## [0.0.37] — 2026-05-31

### 修复

- 修复 xlings 项目构建时自动索引刷新泄漏 xlings 内部 `[N/M] index::path`
  输出的问题。mcpp 仍保留 `Updating package index (auto-refresh)` 状态行,
  且该状态行走统一彩色 UI 输出；内部 `xlings update` 现在在自动刷新路径中
  静默执行。
- 修复自动索引 freshness 依赖不稳定目录 mtime 的问题,改用 mcpp-owned
  `.mcpp-index-updated` marker,避免 full prepare 时重复刷新索引。
- 修复命名空间依赖命中 BMI cache 后仍显示 `Compiling mcpplibs.*` 的问题,
  cache key 与 UI 状态现在使用解析得到的 canonical dependency identity。
- 修复 `xim:` 工具链自动安装时官方索引/目标包文件/`.xlings-index-cache.json`
  可能陈旧或指向临时 sandbox 路径导致 `package not found` 的问题。

## [0.0.36] — 2026-05-31

### 修复

- 修复默认 `mcpplibs` 索引缺失时被其他 xlings 索引误判为 fresh 的问题。
  `mcpp build/search` 现在会要求默认索引自身存在并处于 TTL 内,避免
  `compat.*` 依赖在混合缓存状态下找不到。

## [0.0.35] — 2026-05-30

### 新增

- 支持包描述拥有自己的 `cflags` / `cxxflags`,依赖包源码编译时会继承所属包
  的构建宏,消费方项目不再需要集中声明第三方 C 库的私有宏。
- 支持 Form B `mcpp.generated_files`,官方索引包可以在包目录下生成少量配置头,
  用于承载平台兼容宏或库私有配置。

### 修复

- 修复本地 `path` 索引读取命名空间包时没有匹配
  `pkgs/<prefix>/<namespace>.<name>.lua` 的问题。
- 自定义索引首次同步时保留 mcpp 的 `Fetching custom index repos`
  状态提示,但静默 xlings update 的内部逐项输出。

## [0.0.33] — 2026-05-30

### 改进

- 将 legacy dotted dependency key 兼容解析移入 `mcpp.pm.compat.legacy`
  模块,保留 `mcpp.pm.compat` 作为 facade,并明确标注该兼容路径将在
  mcpp 1.0.0 移除。

## [0.0.32] — 2026-05-30

### 修复

- 修复 project-local `.xlings.json` 生成时未转义 JSON 字符串的问题,
  避免 Windows 本地 index 路径中的反斜杠导致 xlings 跳过项目索引。

## [0.0.31] — 2026-05-30

### 修复

- 修复 xlings 项目使用 mcpp 构建时 custom index 首次同步、project data
  root 查找和 local index 相对路径解析的问题。
- 支持 canonical nested dependency 写法:
  `[dependencies] capi.lua = "0.0.3"` 和
  `[dependencies.mcpplibs] capi.lua = "0.0.3"`。
- 将 legacy flat dotted dependency key 兼容解析集中到 `mcpp.pm.compat`,
  并标注该兼容路径将在 mcpp 1.0.0 移除。

## [0.0.14] — 2026-05-13

LLVM / Clang 工具链支持与 xlings 镜像配置完善。

### 新增

- **LLVM / Clang 工具链支持** —— 新增基于 `clang++`、`clang-scan-deps`、
  `llvm-ar`、`lld` 的工具链探测与构建路径，支持 xlings `llvm` 包提供的
  自包含 Linux LLVM 工具链。
- **`import std` 支持** —— LLVM libc++ 模块标准库可用时，自动发现
  `std.cppm` / `std.compat.cppm`，并接入标准库 BMI 预构建流程。
- **`mcpp self config --mirror`** —— 通过 xlings 抽象层配置 sandbox
  镜像，默认初始化为 `CN`，CI 可显式切换为 `GLOBAL`。

### 改进

- **工具链 provider 拆分** —— 将通用模型、探测逻辑、GCC、Clang、LLVM
  provider 与 registry 分离到独立模块，为后续更多工具链扩展预留入口。
- **xlings 索引兼容迁移** —— 自动将历史 `mcpp-index` 索引名迁移到
  `mcpplibs`，避免旧 sandbox 状态影响新流程。

## [0.0.4] — 2026-05-10

构建 / 环境体验优化三件套。

### 新增

- **Glob 排除模式** —— `[modules].sources` (以及 Form B 的 `sources`)
  现在支持 `!`  前缀的排除模式(类似 `.gitignore`):
  ```toml
  sources = ["src/**/*.cpp", "!src/**/*_test.cpp", "!src/**/*_fuzzer.cpp"]
  ```
  正向 glob 先展开、再减去 `!`-prefixed glob 命中的路径。解决了上游库
  test/fuzzer 文件与源混放时不得不逐文件列举的问题(典型如 ftxui)。

### 改进

- **xlings 布局调整** —— xlings 二进制从 `<MCPP_HOME>/bin/xlings`
  (与 mcpp 同目录)移至 `<MCPP_HOME>/registry/bin/xlings`
  (= `<XLINGS_HOME>/bin/xlings`)。由于 xlings 的 shim-creation guard
  恰好检查 `<XLINGS_HOME>/bin/xlings` 是否存在,新布局下
  `ensure_sandbox_xlings_binary` 自然变成 no-op,省去了之前的 hardlink
  步骤。

- **测试自动继承 sandbox PATH** —— `mcpp test` 在调用测试二进制前,
  自动把 sandbox 的 `subos/default/bin`(含 patchelf、ninja 等
  一次性自举工具)追加到 `$PATH`,使 test 代码 shell-out 到这些工具时
  不再报 "command not found"。

## [0.0.3] — 2026-05-10

依赖解析体系的三步演进:0.0.2 release tag 之后合入 transitive walker,
这一版补齐 SemVer 合并(Level 2)+ 多版本 mangling 兜底(Level 1)。

### 新增

- **依赖图传递性遍历** —— 直接依赖的子依赖(以及更深层)自动跟随入解析图,
  消费者不必再在自己的 `mcpp.toml` 里把 grandchild 也写一遍;子依赖的
  `[build].include_dirs` 也会沿链路传播,让中间层在编译时看得到 grandchild
  的头文件。冲突检测同时区分 path / git / version 三类来源,跨来源不允许
  混用。

- **SemVer 合并解析(Level 2)** —— 同一个包在传递依赖图里被多个消费者
  以不同版本约束声明时,resolver 会把两条原始约束 AND 合并(裸版本号视作
  `=X.Y.Z`),向 index 重新查询,选出同时满足两侧的具体版本。若该版本与
  此前已 pin 的不一致,旧的 manifest 与 `[build].include_dirs` 会被原地
  替换为新版本的内容,孩子依赖也按新 manifest 重新入队。新增 e2e
  `32_semver_merge.sh` 覆盖兼容合并 + 不可调和两条主链路。

- **多版本 mangling 兜底(Level 1)** —— SemVer 合并失败时(典型如
  `=0.0.1` ⨯ `=0.0.2` 这种无重叠的 pin),resolver 不再硬报错,而是把次要
  版本的源码 stage 到 `target/.mangled/<consumer>/...` 下,通过正则改写
  `(export )?module X;` / `(export )?module X:Y;` / `(export )?import X;`
  把模块名替换成 `<X>__v<M>_<m>_<p>__mcpp` 形式,让两个 BMI 在同一构建图
  里以不同模块名共存(C++23 module attachment 帮我们做 ABI 隔离,无需额外
  namespace mangle)。直接 consumer 的源码也一并 stage + 改写,让它的
  `import` 指向 mangled 副本。MVP 范围:仅处理 dep-as-consumer + 叶子
  secondary 两种情形,主包做 consumer 或 secondary 还有自己的 transitive
  deps 时报清晰错误并建议显式 pin。新增 `src/pm/mangle.cppm`(纯改写
  helper + 11 个单元测试)和 e2e `33_multi_version_mangling.sh`。

### 改进

- **构建后端按需为多包做 obj 路径命名空间** —— `plan.cppm` 检测到
  跨包同名源文件(多版本 mangling 后两个 `parse.cppm` 同时存在的常见情形)
  时,自动把 `obj/<file>.o` 改为 `obj/<sanitized-pkg>/<file>.o`,`.ddi`
  扫描产物随之放在 object 同目录下。无碰撞时仍是原始 `obj/<file>.o`
  布局,不影响现有缓存命中。

第二个公开版本。新增 C 语言一等公民支持、xpkg 风格依赖命名空间、包管理子系统骨架重构,以及 lib-root 约定。

### 新增

- **C 语言源文件支持** — `mcpp.toml` 的 `[build]` 段新增 `cflags`、
  `cxxflags`、`c_standard` 三个字段;ninja 后端探测 `.c` 源文件后自动派
  生兄弟 C 编译器(`g++ → gcc`、`clang++ → clang`、跨编译器前缀如
  `x86_64-linux-musl-gcc` 同样适用),发出独立的 `c_object` 规则。
  按文件扩展名分发:`.cppm → cxx_module`、`.c → c_object`、其它 →
  `cxx_object`;dyndep / 模块扫描自动跳过 `.c`。**实测可直接编译
  mbedtls 3.6.1 全部 108 个 `.c` 源文件**(SHA-256 测试向量与 FIPS
  180-4 一致)。

- **lib-root 约定** — 库项目(`kind = "lib"` / `shared`)的 primary
  module interface 默认在 `src/<package-tail>.cppm`,且必须
  `export module <full-package-name>;`(无 `:partition` 后缀);可用
  `[lib].path = "src/foo.cppm"` 显式覆盖(cargo `lib.rs` 风格)。
  违规组合(显式 path 但文件缺失 / 文件 export partition / module 名
  不匹配 [package].name)报 error;约定文件缺失只报 warning,给已有
  项目软迁移时间。纯 binary 项目跳过所有检查。

- **xpkg 风格依赖命名空间** — `mcpp.toml` 现在原生支持三种依赖书写形式:
  - 平铺默认命名空间:`gtest = "1.15.2"` ⇒ `(mcpp, gtest)`,无引号
  - TOML 子表命名空间:`[dependencies.mcpplibs] cmdline = "0.0.2"` ⇒
    `(mcpplibs, cmdline)`,无引号
  - 老式带点字符串(向后兼容):`"mcpplibs.cmdline" = "0.0.2"` 仍能解析
  - CLI 同步:`mcpp add mcpplibs:cmdline@0.0.2` 接受 `<ns>:<name>`
    冒号分隔形式,写出仍是子表写法
  - 解析层在 `DependencySpec` 增加 `namespace_` + `shortName` 结构化
    字段,fetcher / lockfile / cache 等下层逻辑沿用现有完全限定 key。

### 改进

- **`src/pm/` 包管理子系统(7 步重构,全部完成)** — 包管理相关代码
  从 `cli.cppm`(3510→2900 行) / `manifest.cppm` / `lockfile.cppm` /
  `fetcher.cppm` / `publish/xpkg_emit.cppm` 中抽出,集中到独立的
  `src/pm/` 目录下,跟 `build/` / `toolchain/` / `pack/` 平级。
  最终 8 个内部模块:
  - `pm/pm.cppm`(子系统门面,re-export 数据类型)
  - `pm/dep_spec.cppm` — `DependencySpec` + `kDefaultNamespace`
  - `pm/index_spec.cppm` — 占位,等索引仓配置实现
  - `pm/lock_io.cppm` — `mcpp.lock` IO
  - `pm/package_fetcher.cppm` — xlings NDJSON 客户端
  - `pm/resolver.cppm` — `resolve_semver` + `is_version_constraint`
  - `pm/commands.cppm` — `cmd_add` / `cmd_remove` / `cmd_update`
  - `pm/publisher.cppm` — `emit_xpkg` + tarball / sha256 / release helpers

  整个重构严格保持**零行为变更**:每一步独立 PR、独立 CI 通过、独立可
  回滚;旧模块名(`mcpp.lockfile` / `mcpp.fetcher` / `mcpp.publish.xpkg_emit`)
  保留薄 shim 透传到新模块,所有调用点零改动。规划与依赖图见
  `.agents/docs/2026-05-08-pm-subsystem-architecture.md` §3-§5。
- **新增设计文档** `.agents/docs/`:
  - `2026-05-08-package-index-config.md` — 多源包索引仓配置 +
    `mcpp.lock` 索引 commit 锁定 + 两层不可变性
    (L1 publish policy + L2 lock mechanism)
  - `2026-05-08-pm-subsystem-architecture.md` — 包管理子系统目标布局
    与 7 步落地计划

### 修复

- path 依赖的 `[package].name` 比对支持 xpkg 标准 `name` + 旧式
  `<ns>.<name>` 复合名两种形式,兼容当前 mcpp-index 描述符尚未迁移的
  状态。
- module 扫描器解析 partition import(`import :foo`)时,不再把当前
  TU 自己的 partition 后缀拼进 logical name。
  之前 `export module M:bar;` 里的 `import :foo;` 被解析成 `M:bar:foo`
  (没人 provide,产生 7 条 stale warning);现在正确解析为兄弟分区
  `M:foo`。GCC dyndep 实际能分辨,所以 build 不影响,但 mcpp 自己的
  warning 噪音消失。在 `mcpplibs/tinyhttps` 上验证(7 条 warning →
  0 条)。

### 兼容性

向后兼容。老的 `mcpp.toml` / `mcpp.lock` 不需要任何改动即可在 0.0.2 下
继续工作。带引号的 `"ns.name"` 形式继续被解析,只是新写出的 `mcpp add`
会用无引号的子表形式。

## [0.0.1] — 2026-05-07

mcpp 首个公开发版本。

### 已具备的能力

- 基础工程命令：`mcpp new` / `build` / `run` / `clean` / `test`
- C++23 模块（`import std` / `import foo.bar`）一等公民支持
- 跨项目依赖：[mcpp-index](https://github.com/mcpp-community/mcpp-index)
  远程仓库、git、本地 path 三种来源
- SemVer 约束：`"foo" = "^0.0.1"` / `"~1.2.0"` / `">=1, <2"`
- P1689 编译器驱动模块扫描 + ninja `dyndep`
- 跨项目 BMI 持久缓存
- 私有 toolchain 沙盒（`mcpp toolchain install / default / list`），
  跟系统 PATH 完全隔离；首次使用自动装 musl-gcc 默认工具链
- 部分版本号支持（`mcpp toolchain install gcc 15` 自动选最高匹配）
- `mcpp pack` 三种自包含发布模式：
  - `static` — musl 全静态，单文件可分发
  - `bundle-project`（默认）— 只 bundle 项目第三方 .so
  - `bundle-all` — 全自包含含 ld-linux + libc，附 `run.sh` wrapper
- `mcpp self {doctor,env,version,explain}` 自诊断
- 下载 / 安装实时进度（速度、字节数、终端宽度自适应）
- 项目相对路径显示（`@mcpp/...`、project-relative）

### 发布产物（GitHub Release）

- `mcpp-0.0.1-linux-x86_64.tar.gz` — bundled tarball（mcpp + 内置 xlings）
- `mcpp-linux-x86_64.tar.gz` — `latest` 别名
- `install.sh` — `curl | bash` 装机脚本
- `SHA256SUMS` + 各资产 sha256 sidecar
- 二进制为 musl 全静态 ELF，无 PT_INTERP / RUNPATH 依赖，任意 Linux x86_64
  直接可跑

### 限制

- 仅支持 Linux x86_64（glibc / musl 通用）
- macOS / Windows / aarch64 还在路上
- workspace、`mcpp publish --auto`（自动 PR 到 mcpp-index）等功能未发版

### 反馈

接口、命令、产物形态可能在后续小版本调整。issue / 想法 / 协作意向都欢迎到
[issues](https://github.com/mcpp-community/mcpp/issues) 来。
