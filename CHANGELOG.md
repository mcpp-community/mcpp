# Changelog

> 本文件追踪 `mcpp-community/mcpp` 公开仓的版本演进。
> 格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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
