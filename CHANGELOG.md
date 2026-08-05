# Changelog

> 本文件追踪 `mcpp-community/mcpp` 公开仓的版本演进。
> 格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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
  ✓ 1 package(s) installed
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

  ⚠️ **`--no-cache` 的语义有一处收紧**:此前它只清构建目录、**仍然会回填全局缓存**;现在它等于 `off`,即**不读也不写**。想要「从零重编但仍然刷新缓存」的,用 `mcpp clean` 或 `rm -rf target` 后正常构建。这个收紧是为了让三个模式正交:一个叫 `off` 的模式还偷偷写缓存是说不通的。

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

### ⚠️ 破坏性变更

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
    `llvm` 劈开。README 平台表从词汇表重画(target × tier 维度,补 MSVC=✅ 与
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

- ✅ **LLVM / Clang 工具链支持** —— 新增基于 `clang++`、`clang-scan-deps`、
  `llvm-ar`、`lld` 的工具链探测与构建路径，支持 xlings `llvm` 包提供的
  自包含 Linux LLVM 工具链。
- ✅ **`import std` 支持** —— LLVM libc++ 模块标准库可用时，自动发现
  `std.cppm` / `std.compat.cppm`，并接入标准库 BMI 预构建流程。
- ✅ **`mcpp self config --mirror`** —— 通过 xlings 抽象层配置 sandbox
  镜像，默认初始化为 `CN`，CI 可显式切换为 `GLOBAL`。

### 改进

- 🔧 **工具链 provider 拆分** —— 将通用模型、探测逻辑、GCC、Clang、LLVM
  provider 与 registry 分离到独立模块，为后续更多工具链扩展预留入口。
- 🔧 **xlings 索引兼容迁移** —— 自动将历史 `mcpp-index` 索引名迁移到
  `mcpplibs`，避免旧 sandbox 状态影响新流程。

## [0.0.4] — 2026-05-10

构建 / 环境体验优化三件套。

### 新增

- ✅ **Glob 排除模式** —— `[modules].sources` (以及 Form B 的 `sources`)
  现在支持 `!`  前缀的排除模式(类似 `.gitignore`):
  ```toml
  sources = ["src/**/*.cpp", "!src/**/*_test.cpp", "!src/**/*_fuzzer.cpp"]
  ```
  正向 glob 先展开、再减去 `!`-prefixed glob 命中的路径。解决了上游库
  test/fuzzer 文件与源混放时不得不逐文件列举的问题(典型如 ftxui)。

### 改进

- 🔧 **xlings 布局调整** —— xlings 二进制从 `<MCPP_HOME>/bin/xlings`
  (与 mcpp 同目录)移至 `<MCPP_HOME>/registry/bin/xlings`
  (= `<XLINGS_HOME>/bin/xlings`)。由于 xlings 的 shim-creation guard
  恰好检查 `<XLINGS_HOME>/bin/xlings` 是否存在,新布局下
  `ensure_sandbox_xlings_binary` 自然变成 no-op,省去了之前的 hardlink
  步骤。

- 🔧 **测试自动继承 sandbox PATH** —— `mcpp test` 在调用测试二进制前,
  自动把 sandbox 的 `subos/default/bin`(含 patchelf、ninja 等
  一次性自举工具)追加到 `$PATH`,使 test 代码 shell-out 到这些工具时
  不再报 "command not found"。

## [0.0.3] — 2026-05-10

依赖解析体系的三步演进:0.0.2 release tag 之后合入 transitive walker,
这一版补齐 SemVer 合并(Level 2)+ 多版本 mangling 兜底(Level 1)。

### 新增

- ✅ **依赖图传递性遍历** —— 直接依赖的子依赖(以及更深层)自动跟随入解析图,
  消费者不必再在自己的 `mcpp.toml` 里把 grandchild 也写一遍;子依赖的
  `[build].include_dirs` 也会沿链路传播,让中间层在编译时看得到 grandchild
  的头文件。冲突检测同时区分 path / git / version 三类来源,跨来源不允许
  混用。

- ✅ **SemVer 合并解析(Level 2)** —— 同一个包在传递依赖图里被多个消费者
  以不同版本约束声明时,resolver 会把两条原始约束 AND 合并(裸版本号视作
  `=X.Y.Z`),向 index 重新查询,选出同时满足两侧的具体版本。若该版本与
  此前已 pin 的不一致,旧的 manifest 与 `[build].include_dirs` 会被原地
  替换为新版本的内容,孩子依赖也按新 manifest 重新入队。新增 e2e
  `32_semver_merge.sh` 覆盖兼容合并 + 不可调和两条主链路。

- ✅ **多版本 mangling 兜底(Level 1)** —— SemVer 合并失败时(典型如
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

- 🔧 **构建后端按需为多包做 obj 路径命名空间** —— `plan.cppm` 检测到
  跨包同名源文件(多版本 mangling 后两个 `parse.cppm` 同时存在的常见情形)
  时,自动把 `obj/<file>.o` 改为 `obj/<sanitized-pkg>/<file>.o`,`.ddi`
  扫描产物随之放在 object 同目录下。无碰撞时仍是原始 `obj/<file>.o`
  布局,不影响现有缓存命中。

第二个公开版本。新增 C 语言一等公民支持、xpkg 风格依赖命名空间、包管理子系统骨架重构,以及 lib-root 约定。

### 新增

- ✅ **C 语言源文件支持** — `mcpp.toml` 的 `[build]` 段新增 `cflags`、
  `cxxflags`、`c_standard` 三个字段;ninja 后端探测 `.c` 源文件后自动派
  生兄弟 C 编译器(`g++ → gcc`、`clang++ → clang`、跨编译器前缀如
  `x86_64-linux-musl-gcc` 同样适用),发出独立的 `c_object` 规则。
  按文件扩展名分发:`.cppm → cxx_module`、`.c → c_object`、其它 →
  `cxx_object`;dyndep / 模块扫描自动跳过 `.c`。**实测可直接编译
  mbedtls 3.6.1 全部 108 个 `.c` 源文件**(SHA-256 测试向量与 FIPS
  180-4 一致)。

- ✅ **lib-root 约定** — 库项目(`kind = "lib"` / `shared`)的 primary
  module interface 默认在 `src/<package-tail>.cppm`,且必须
  `export module <full-package-name>;`(无 `:partition` 后缀);可用
  `[lib].path = "src/foo.cppm"` 显式覆盖(cargo `lib.rs` 风格)。
  违规组合(显式 path 但文件缺失 / 文件 export partition / module 名
  不匹配 [package].name)报 error;约定文件缺失只报 warning,给已有
  项目软迁移时间。纯 binary 项目跳过所有检查。

- ✅ **xpkg 风格依赖命名空间** — `mcpp.toml` 现在原生支持三种依赖书写形式:
  - 平铺默认命名空间:`gtest = "1.15.2"` ⇒ `(mcpp, gtest)`,无引号
  - TOML 子表命名空间:`[dependencies.mcpplibs] cmdline = "0.0.2"` ⇒
    `(mcpplibs, cmdline)`,无引号
  - 老式带点字符串(向后兼容):`"mcpplibs.cmdline" = "0.0.2"` 仍能解析
  - CLI 同步:`mcpp add mcpplibs:cmdline@0.0.2` 接受 `<ns>:<name>`
    冒号分隔形式,写出仍是子表写法
  - 解析层在 `DependencySpec` 增加 `namespace_` + `shortName` 结构化
    字段,fetcher / lockfile / cache 等下层逻辑沿用现有完全限定 key。

### 改进

- 🛠 **`src/pm/` 包管理子系统(7 步重构,全部完成)** — 包管理相关代码
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
- 📄 **新增设计文档** `.agents/docs/`:
  - `2026-05-08-package-index-config.md` — 多源包索引仓配置 +
    `mcpp.lock` 索引 commit 锁定 + 两层不可变性
    (L1 publish policy + L2 lock mechanism)
  - `2026-05-08-pm-subsystem-architecture.md` — 包管理子系统目标布局
    与 7 步落地计划

### 修复

- 🐛 path 依赖的 `[package].name` 比对支持 xpkg 标准 `name` + 旧式
  `<ns>.<name>` 复合名两种形式,兼容当前 mcpp-index 描述符尚未迁移的
  状态。
- 🐛 module 扫描器解析 partition import(`import :foo`)时,不再把当前
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

- ✅ 基础工程命令：`mcpp new` / `build` / `run` / `clean` / `test`
- ✅ C++23 模块（`import std` / `import foo.bar`）一等公民支持
- ✅ 跨项目依赖：[mcpp-index](https://github.com/mcpp-community/mcpp-index)
  远程仓库、git、本地 path 三种来源
- ✅ SemVer 约束：`"foo" = "^0.0.1"` / `"~1.2.0"` / `">=1, <2"`
- ✅ P1689 编译器驱动模块扫描 + ninja `dyndep`
- ✅ 跨项目 BMI 持久缓存
- ✅ 私有 toolchain 沙盒（`mcpp toolchain install / default / list`），
  跟系统 PATH 完全隔离；首次使用自动装 musl-gcc 默认工具链
- ✅ 部分版本号支持（`mcpp toolchain install gcc 15` 自动选最高匹配）
- ✅ `mcpp pack` 三种自包含发布模式：
  - `static` — musl 全静态，单文件可分发
  - `bundle-project`（默认）— 只 bundle 项目第三方 .so
  - `bundle-all` — 全自包含含 ld-linux + libc，附 `run.sh` wrapper
- ✅ `mcpp self {doctor,env,version,explain}` 自诊断
- ✅ 下载 / 安装实时进度（速度、字节数、终端宽度自适应）
- ✅ 项目相对路径显示（`@mcpp/...`、project-relative）

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
