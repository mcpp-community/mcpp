# 10 —— 发布打包

**读者：** 要把一个程序交付到没有 mcpp 的机器上的人。

**本章回答的那一个问题：** 一次构建怎样变成另一台机器能运行的东西，以及每一种
打包模式各自携带什么。

**不在这里：** 交付一个供其它包在构建时使用的**库**，那是
[12 —— 分发预构建库](12-binary-distribution.md)。在此之后：
[11 —— 发布一个库到 mcpp-index](11-publishing-a-library.md)。

> 本页讲的是打包一个**程序**。若要以「接口 + 预构建二进制」的形式发布一个*库*，
> 见 [12 —— 分发预构建库](12-binary-distribution.md)。
>
> `mcpp pack` 具体做哪一种，由 target 的 `kind` 决定，不由某个 flag 决定：
> `mcpp pack <name>` 打包 `[targets.<name>]`，一个 `bin` 会变成一个 bundle，
> 而一个 `lib`/`shared` 会变成一个库包。不给名字时，mcpp 选择唯一可打包的
> target。

> `mcpp build` 默认产出的动态链接二进制，其 loader 与 RUNPATH 都指向构建
> 沙盒。它是开发产物，不是交付物。有三条路径能把它变成交付物 —— 没有一条会
> 用到宿主的 C 库。

## 三种分发路径

下面每一种路径产出的产物，其 C 运行时都来自生态，从不来自 `/lib64`。这是有意
安排的：mcpp 之所以针对私有 glibc 构建，正是为了让产物的行为不取决于底层是
哪一个发行版；如果分发的最后一步又伸手去用宿主的 libc，前面这一步做的事就白
费了。

| | 路径 | 命令 | C 运行时的来源 | 适用条件 |
|---|---|---|---|---|
| **A** | 走生态 | `mcpp emit xpkg` → `xlings install <pkg>` | 目标机自己的 xlings 载荷 | 目标机已装 xlings |
| **B** | 单个静态文件 | `mcpp build --target x86_64-linux-musl` | 不来自任何地方 —— 已链接进去 | 需要一个没有任何运行时依赖的单文件 |
| **C** | 自带运行时 | `mcpp pack --mode self-contained` | 随 bundle 一起分发 | 任意 Linux，含比构建机更旧的版本 |

**关于路径 A。** 刚构建出的二进制中记录的 `PT_INTERP` 指向构建机的载荷，因此
把该文件手工复制到另一台机器上不会运行：那条路径在目标机上不存在。这是「手工
复制」这个动作的性质，不是产物本身的性质 —— 经 `xlings` 安装时，包内的 ELF 会
在安装期被重新指向目标机自己的载荷。记录的路径是构建机的细节，不是分发格式的
一部分。需要在机器间手工搬运二进制时，能存活下来的是路径 B 与 C。

**关于路径 B。** `--target …-musl` 隐含静态链接，因此没有 loader、没有
RUNPATH，运行期不需要查找任何东西。它的结果最小也最可移植，是程序不需要
glibc 专有行为（NSS 查询、`dlopen` 宿主插件）时的首选。

**关于路径 C。** bundle 内携带这套工具链的 glibc 及其 loader，因此能在比构建机
更旧的发行版上运行 —— 这是路径 B 覆盖不到、而又确实需要 glibc 时的那一格。选
它之前先读下面 `/proc/self/exe` 一节：经由内置 loader 启动会改变程序对「自己
在哪」的认知。

## 两条轴：target（libc）× mode（打包深度）

分发有两项正交的选择：

- **libc / static** 是一项*构建 target* 属性：`--target …-linux-gnu`（glibc）
  与 `--target …-linux-musl`（musl，静态）。`--target …-musl` 隐含 `static`。
- **打包深度** 是一项*pack* 属性：产物随身携带多少共享库闭包，由 `--mode`
  选择。

| Mode | 宿主必须提供 | 体积 | 使用场景 |
|---|---|---|---|
| `system` | 每一个 `.so`（含第三方） | 最小 | `.deb`/`.rpm`，同发行版机群（由包管理器声明依赖） |
| `vendored`（默认） | libc / libstdc++ / loader | +几 MB | 主流发行版（Ubuntu 22+、Debian 12+、RHEL 9+） |
| `self-contained` | 无 | +30–50 MB | 任意 Linux，含较旧的 glibc；携带闭包与 `run.sh` wrapper |
| `static` | 无（单文件） | +5–10 MB | musl；架构匹配的 Linux x86_64 或 aarch64，Docker scratch，Alpine |

如何选择：

- 分发格式打包（`.deb`/`.rpm`）或同发行版内部部署 → `system`
- 面向主流发行版的桌面 / 服务端发布 → `vendored`（默认）
- 跨发行版，或目标是较旧的 glibc（旧版 CentOS、麒麟等） → `self-contained`
- 单个便携文件、不依赖宿主 → `static`

**没有任何 mode 会携带构建机的路径。** 开发构建有意寻址的是本机：它的
`DT_RPATH` 命名工具链的载荷目录与 SubOS 的库视图（`<subos>/lib`），它的
`PT_INTERP` 命名一个私有 loader。每一种 mode 都改写这两者 —— `vendored` 与
`self-contained` 改写为相对 `$ORIGIN` 的路径，`system` 则整个清空搜索路径并
恢复平台标准的 interpreter。`system` 不是「原样保留构建时的样子」，而是「目标
机会提供一切」这一断言 —— 这是关于目标机的陈述，不可能用本机的绝对路径拼出
来。e2e 215 会扫描 bundle 里每一个 ELF，查找任何落在 `$MCPP_HOME` 之下的路径，
命中即失败。

### 需要宿主提供某种能力的程序

「自包含」有一个下限。有些库只能来自目标机器：图形驱动的用户态部分与正在
运行的内核模块版本绑定，而对专有栈而言，再分发是不被允许的。这类依赖应声明为
运行期能力需求（`docs/04-mcpp-toml.md` §2.11），模式表因此多出一列：

| Mode | 需要宿主提供能力的程序 |
|---|---|
| `system` | yes |
| `vendored`（默认） | **对这类程序而言正确的默认值** |
| `self-contained` | **打包期拒绝** |
| `static` | **打包期拒绝** |

两处拒绝出自同一个事实：**自带 libc 的 bundle 无法消费宿主提供的库。** 那个
`.so` 带着它对*目标机* libc 的要求到达，而该进程没有那份 libc —— 双向实测记录
于 mcpp#392 / mcpp#401：私有 glibc 遇上宿主加载的对象，会在重定位阶段、
`main` 之前崩溃。此前这两种 mode 的行为要么是链接通过、启动时失败，要么是
静默降级（就图形栈而言表现为软件渲染，且没有任何提示）。

`vendored` 会打包这类程序，并在 bundle 根目录写出一个 **`HOST-REQUIREMENTS`**
文件，说明目标机必须提供什么：

```
capability=opengl.glx.driver discovery=rpath-of-dispatch
```

`discovery` 是可操作的那一半 —— 各机制彼此独立，满足其一不代表满足另一个。该
文件只在确有内容需要说明时才写出：一个空文件等于**声称**什么都不需要。

### 模式名的兼容性

上表给出规范名称。旧名称仍是**永久性的兼容别名**：`bundle-project` =
`vendored`，`bundle-all` = `self-contained`。tarball 的文件名后缀是被冻结的
wire 格式（由 `install.sh` 消费），**不**跟随改名：`vendored` 无后缀，
`self-contained` 是 `-bundle-all`，`static` 是 `-static`，`system` 是
`-system`。

## 命令

```bash
mcpp pack                          # vendored by default
mcpp pack --mode system
mcpp pack --mode static
mcpp pack --mode self-contained        # alias: --mode bundle-all
mcpp pack --target x86_64-linux-musl   # equivalent to --mode static
mcpp pack --target aarch64-linux-musl  # ARM64 equivalent
mcpp pack --format dir                 # output as a directory, no tarball
mcpp pack --format appimage             # a format a package in the graph provides
mcpp pack -o myapp.tar.gz              # filename only: lands at target/dist/myapp.tar.gz
mcpp pack -o /abs/path/myapp.tar.gz    # includes a directory: output to the literal path
mcpp pack --profile dev                # build with a different profile (default: release)
mcpp pack --dev                        # the same, as `build` and `run` spell it; --profile wins over it
mcpp pack --message-format json        # one mcpp.pack envelope on stdout (mcpp 2026.9.16.1+)
mcpp pack --no-strip                   # ship the artifacts as built
mcpp pack --debug-symbols dbg/         # write the separated *.debug files under dbg/
mcpp pack --format msi --features installer   # activate root-package features for the pack
```

`--features <LIST>`（mcpp 2026.9.15.2+）为打包所执行的每一次构建启用根包
feature：每一条 `--target` 腿，以及被分派格式的两遍构建。它接受的取值与
`mcpp build --features` 相同，因此只为某一种发布才需要的宿主工具，可以声明在
带 `tools = [...]` 的 `[feature-deps.<f>]` 下，只由点名 `<f>` 的那次打包构建
出来。`mcpp run --format <name> --features <LIST>` 把同样的 feature 交给它
执行的那次打包。

`--release` 与 `--dev`（mcpp 2026.9.16.1+）是 `build`、`run` 已接受的简写，
优先级相同：三条命令上都是 `--profile` 优先于它们。

`--message-format json`（mcpp 2026.9.16.1+）在命令结束后于 stdout 上输出一个
`mcpp.pack` 信封，所有给人看的行都改走 stderr。它的 `data.artifacts` 列出
产出的每一个文件或目录，带绝对路径、`type`（`file` 或 `directory`）、
`--format` 取值以及各条腿的三元组；`data.stage` 给出暂存树、它的 manifest，
以及闭包是否已经走通（见 [50 —— 机器输出](50-machine-output.md)）。这条命令
上的 `--format` 表示的是包格式，因此机器输出改用 `mcpp test` 请求它的那种
方式。

### `--format` 只是一根轴，引擎只拥有它的两个取值

`tar` 与 `dir` 回答的问题，和 `msi` 与 `appimage` 回答的问题是同一个 ——
输出取什么形状 —— 所以它们是同一个 flag 的不同取值，而不是第二个 flag 的开端。
引擎持有什么、包持有什么，分界如下：

> **`mcpp pack` 拥有机制，以及那一种通用格式。其余每一种格式都住在某个包里，
> 由 `mcpp pack` 分派过去。**

那种通用格式就是它已经在产出的东西：一个解开即可运行的归档。它「通用」只在这
里唯一要紧的那个意义上成立 —— 它不需要了解任何别人的发布方式。此外的一切都
需要。dpkg 的 control 字段、AppImage 的 runtime、WiX 的 schema、Apple 的
公证、Android 的签名方案：其中任何一个被绑进引擎，都会把一次 mcpp 的发布耦合
到一次 mcpp 并不控制的发布上。这与本项目已经为语言做过的论证是同一个 ——
Slang 被支持，而引擎里没有它的名字。

所以取值集合是开放的（mcpp 2026.9.11.1+）。`--format <name>` 在解析后的图里
找到声明了 `<name>` 的那个包，并把暂存树交给它；一个未知的取值点名的是
**当下确实可用**的那些，而不是一份固定清单：

```
error: unknown --format 'bogus'.
  available in this build: tar, dir, appimage
  A format past `tar` and `dir` comes from a package in the resolved graph, which declares
  it with `mcpp::provides_pack_format("<name>")` in its build program. Add the package
  that provides 'bogus' to [build-dependencies] and activate its feature.
```

这次拒绝发生在任何东西被编译之前。怎样写这样一个包，见
[产出可分发物](30-build-mcpp.md#产出可分发物pack_format-与-stage_dir20269111)；
引擎为此新增的三样东西是：一棵 `artifact` action 可以消费的暂存树、
`[package]` 里其余字段进入构建程序、以及这次分派本身。每一样都与具体格式无关
—— 「与格式无关」正是判断某样东西该不该进引擎的判据。

被分派的格式作用于一个**程序** target —— `kind = "app"` 也不例外，无论这一行
把它链接成什么文件（见 [04 §2.2](04-mcpp-toml.md)）。库包发布的是一份接口
加上按三元组给出的预构建产物，没有单独一棵暂存树，所以
`mcpp pack <库> --format <name>` 会被拒绝，而不是被忽略。

**产物是共享目标文件的 `kind = "app"` target 可以接受一个以上的 `--target`**
（mcpp 2026.9.13.2+）：在每一行 Android 上，应用*就是*平台加载的那个共享库，
因此 `mcpp pack myapp --target aarch64-linux-android --target x86_64-linux-android`
会构建两条腿并把它们暂存进同一棵树，与库包本已具备的多三元组做法完全一致。
每条腿落在 `lib/<abi>/lib<name>.so`（`aarch64` → `arm64-v8a`，`x86_64` →
`x86_64`），各自的闭包暂存在它旁边（见
[Android](#android应用目标文件与它的闭包在-lib-下)），声明的部署文件只暂存
一次，随后只对这棵合并后的树跑一次分派 —— 这正是 `dist-apk` 这样的成员能构建
出一个通用 APK 的原因。只给一个 `--target` 时，今天这种扁平的
`lib/lib<name>.so` 布局保持不变。产物在任何被请求的一行上是可执行文件的
target，第二个 `--target` 依旧被拒绝：为多个三元组打包同一个可执行文件需要
多个可执行文件，那是另一种机制（`lipo` 的通用二进制），不在这份能力之内。

`-o` 接受裸文件名时，输出落在 `target/dist/` 下；含目录（相对或绝对）时，按
字面路径输出。

完整选项参见 `mcpp pack --help`。

### `mcpp run --format <name>`（mcpp 2026.9.12.3+）

```bash
mcpp run --target x86_64-linux-android --format apk
mcpp run --target aarch64-ios-sim      --format app
```

`--format` 与 `mcpp pack` 用的是同一个 flag，这里复用它是为了覆盖一种普通
`mcpp run` 够不到的情形：一个 Android 应用程序是一个 `.apk`，一个已安装的 iOS
应用程序是一个 `.app`，两者都不是 `mcpp run` 默认执行的链接产物。
`mcpp run --format <name>` 先为 `<name>` 打包 —— 与 `mcpp pack --format
<name>` 相同的两遍构建与暂存树 —— 再经由为一个程序解析出的 runner 运行打包
报出的那个产物：先看项目的 `[target.<triple>] runner`，其次看依赖的
`mcpp::runner(...)`，再次看载荷描述文件的。

打包报出的产物是这次请求的**终端**产物：在这次请求引入的 `artifact` 动作中，
没有被任何其他被引入的动作当作输入的那一个输出。一个提供者常常是一条链
（`dist-apk`：链接、加库、对齐、签名），链上每个输出都会被核验存在，但只有
最后一个是发布物，也只有它以 `Packed` 报出。链的末端产出两个文件的格式会被
`mcpp run --format` 拒绝并点名两者，因为一个 runner 只接受一个操作数。

未知的 `<name>` 会被拒绝，点名已解析图提供的格式集合，与
`mcpp pack --format bogus` 报出的是同一个集合。`--format` 与 `--no-runner`
同时出现会被拒绝 —— 一个 `.apk` 或一个已安装的 `.app` 无法被直接执行。在
`kind = "app"` 的形态是一个库的那一行上、不带 `--format` 运行它，同样会被
拒绝，点名该 flag 与同一个格式集合。`mcpp test` 不受影响：测试二进制在每一行
上都是一个程序。

### 打包产物的构建输入，以及随包内容

与 `mcpp build` 相比有两点不同，都源于同一个原因：「这个产物要离开本机」。

**profile 的兜底值是 `release`，不是 `dev`。** 其余优先级不变 ——
`--profile` 优先于 `[build] default-profile`，后者优先于兜底值。只有最后
一步不同，因此已经声明过 profile 的工程仍然拿到它声明的那个，`mcpp pack` 也
绝不会产出一个 `mcpp build` 在同样条件下产不出来的 flag 组合。

> 有一条后果值得知道：裸 `mcpp build` 与裸 `mcpp pack` 现在会写进**不同的**
> `target/<triple>/<fingerprint>/` 目录 —— 指纹把 profile 算了进去。手工放到
> 构建产物旁边的文件（一个 DLL、一份数据）因此只在两条命令解析到同一个
> profile 时才会被 `pack` 看见：要么把它写进 `[build] default-profile`，要么
> 两条命令都带上 `--profile`。声明式通道（`[runtime] deploy_files`、
> `runtime_search_dirs`）不受影响。

**调试信息会被剥离，发布者的路径随之消失。** 未 strip 的产物携带 DWARF，而
DWARF 携带发布者源码树与构建目录的绝对路径。具体剥离什么取决于产物**是什么**
—— 这是 dh_strip 的分档方式，而其中归档那一行是要紧的：

| 产物 | strip 参数 | 不能剥得更彻底的原因 |
|---|---|---|
| 可执行文件 | `--strip-all` | 没有谁会链接它 |
| 共享库 | `--strip-unneeded` | 保留 `.dynsym` —— 那**就是**导出表 |
| 静态归档 | `--strip-debug --enable-deterministic-archives` | `--strip-all` 会删掉归档的**符号索引**，消费方链接时随即报出 `archive has no index; run ranlib to add one` |

三档都会去掉 `.comment` 与 `.note`。段的删除按**精确名字**匹配，因此
`.note.gnu.build-id` 会被保留，`--add-gnu-debuglink` 仍然有东西可以配对。

`--no-strip`（或 `[pack] strip = false`）按构建原样发货。
`--debug-symbols <dir>` 则是分离而不是丢弃：写出 `<dir>/<产物>.debug`，并给
发货的产物加上指向它的 `.gnu_debuglink` —— 这正是调试器与 `debuginfod` 所
遵循的约定。

> `[pack] strip` 不是 `[profile.<name>].strip`。后者给**链接**加上 `-s`，
> 既碰不到静态归档，也无法分离出任何东西；前者管的是**包里带什么**。两个不同
> 的决定，两个不同的名字。

**图构建出来的东西，在每一条把调试信息放在映像内部的行上都会被剥离；来自
store 或宿主的库不会**（mcpp 2026.9.16.1+）。规则是 dh_strip 的「一个包
只剥离它自己构建的东西」，覆盖范围如下：

| 暂存的文件 | 是否剥离 | 原因 |
|---|---|---|
| 程序 | 是，按可执行文件剥离；在 Android 行上按共享库剥离，因为那里的程序本身就是共享库 | 本次构建编译了它 |
| 图中 `SharedLibrary` 链接单元产出的共享库 | 是，`--strip-unneeded` | 本次构建从源码编译了它 |
| 工具链自身运行时的暂存副本（NDK 的 `libc++_shared.so`） | 是，`--strip-unneeded` | 树里的副本不是那份共享载荷本身；Android Gradle 插件也会剥离同一个文件 |
| 来自 store、宿主，或某个包部署的预构建库 | 否 | 它的字节属于它的发布者，供应商的库可能带着签名 |

Mach-O 与 MSVC 的 PE 把调试信息放在映像之外，因此那里什么都不剥离，`Packing`
这一行只在确实发生剥离的那一行上写「stripped」。`--no-strip` 与
`--debug-symbols` 管辖每一个被剥离的文件。自己暂存库的构建程序，通过
`mcpp::pack_strip()` 与 `mcpp::pack_debug_symbols_dir()` 读到同一个决定
（[30 —— build.mcpp](30-build-mcpp.md)）。

## 产物布局

tarball 的内容包在一个顶层目录里，该目录的名字与 tarball 文件名（去掉
`.tar.gz` 后）保持一致 —— 这样图形界面的「右键解压」和命令行的 `tar -xzf`
都会得到同一个自包含的目录，而不会把内容散落到当前路径下。

**闭包会被记录，一个不完整的闭包会使归档被拒绝**（mcpp 2026.9.14.2+）。
暂存树旁边的 `<tree>.stage-manifest` 列出闭包解析过的每一个库名：树里携带的
库给出其暂存路径，目标机自己提供的库记为 `platform`，哪里都找不到的记为
`unresolved`（行格式见 [50](50-machine-output.md#暂存清单)）。只要有一个名字
是 unresolved，`--format tar` 与 `--format dir` 就会失败并点名它，因为一棵
缺了这个库的树在安装处无法启动；被分派的格式仍然会收到这棵树，并带上
`closure = not-walked`。目标机提供、而本机没有的库，写进
`[pack.bundle-project] also_skip`。不打包任何东西的 mode（`system`、
`static`）不记录闭包。

### Mode `static`

```
target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz
└── myapp-0.1.0-x86_64-linux-musl-static/
    ├── bin/myapp                ← fully static ELF (no PT_INTERP / RUNPATH)
    ├── myapp                    ← top-level entry point (thin shell wrapper, run ./myapp directly)
    ├── README.md                ← copied automatically from the project root
    └── LICENSE
```

### Mode `vendored`（默认；别名：`bundle-project`）

```
target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu/
    ├── bin/myapp                ← dynamically linked, RUNPATH=$ORIGIN/../lib
    │                                PT_INTERP=/lib64/ld-linux-x86-64.so.2
    ├── lib/
    │   ├── libcurl.so.4         ← project third-party dependency
    │   ├── libssl.so.3
    │   └── ...
    ├── myapp                    ← top-level entry point
    ├── README.md
    └── LICENSE
```

跳过列表遵循
[PEP 600 / manylinux2014](https://peps.python.org/pep-0600/) ——
默认假定目标系统已经具备 `libc`、`libm`、`libstdc++`、`libgcc_s`、
`ld-linux-*` 一类基础库，因此不把它们打进 tarball。

### Mode `self-contained`（别名：`bundle-all`）

```
target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu-bundle-all/
    ├── bin/myapp
    ├── lib/
    │   ├── ld-linux-x86-64.so.2  ← complete loader and libc
    │   ├── libc.so.6
    │   ├── libstdc++.so.6
    │   ├── libgcc_s.so.1
    │   └── ...project dependencies
    ├── myapp                     ← one of two entry points
    ├── run.sh                    ← the other entry point (identical contents)
    ├── README.md
    └── LICENSE
```

带 `-o foo.tar.gz` 时，顶层目录名也会变成 `foo`（包名与目录名始终保持一致）。

ELF 规范禁止 `PT_INTERP` 使用 `$ORIGIN`，因此在 `self-contained` 模式下，
loader 经由 `run.sh`（以及顶层的同名 wrapper）以绝对路径的方式被调用：

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

上面的布局与 wrapper 以 x86_64 为例。打包器会按 target 推导 loader 的名字；
在 aarch64 上它是 `ld-linux-aarch64.so.1`。

#### 经由内置 loader 启动时 `/proc/self/exe` 的取值

「由 loader 启动」带来一个上述布局看不出的后果：内核会把 `/proc/self/exe`
设为指向 **loader**，而不是程序本身；`/proc/self/cmdline` 里也混入了
`--library-path` 这个参数。于是每一处「到可执行文件旁边找资源」的逻辑，解析
出来的都是 `lib/` 而不是 bundle 根目录 —— 而且是**静默**发生的。实际表现
是：GUI 工具箱因为找不到字体而渲染出空白文字、一个 `assets/` 目录看起来像是
缺失了、随包分发的辅助二进制找不到自己的位置。从 `/proc/self/cmdline` 解析
`argv` 的代码，拿到的会是混入了 loader 参数的结果。

这只影响 `self-contained`。`vendored`、`system` 与 `static` 携带的都是内核
能直接使用的 `PT_INTERP`，`/proc/self/exe` 在那三种模式下是正确的。

wrapper 为此导出了 **`MCPP_BUNDLE_DIR`**（bundle 根目录）。应优先按它解析，
只在它未设置时才回退：

```c
const char *base = getenv("MCPP_BUNDLE_DIR");   /* set by run.sh */
if (!base) {
    /* not launched through the wrapper — /proc/self/exe is trustworthy */
}
```

如果应用本身改不了 —— 比如某个第三方 GUI 框架自己做路径解析 —— 改用
`--mode vendored`。它把 `PT_INTERP` 重新指向宿主的 loader，代价是要求宿主的
glibc 版本不低于产物构建时所用的那份。

### WebAssembly（`wasm32-emscripten`）：词干家族

```
target/dist/myapp-0.1.0-wasm32-emscripten.tar.gz
└── myapp-0.1.0-wasm32-emscripten/
    ├── bin/myapp.js              ← the launcher; the file a runner executes
    ├── bin/myapp.wasm            ← implicit output of the link, staged with it
    ├── bin/myapp.data            ← present only when the link carries --preload-file
    ├── README.md
    └── LICENSE
```

打包器暂存启动器，以及同一次链接在它旁边写出的每一个 `myapp.<任意后缀>` ——
即词干家族。`myapp.wasm` 是必需的：链接边把它声明为隐式输出，它的缺席说明
构建目录与图对不上，`mcpp pack` 因此会拒绝，而不是暂存一个没有模块的启动器。
`myapp.data` 以及 emcc 用同一个词干写出的其余文件（`myapp.worker.js`、
`myapp.wasm.map`）是可选的，链接产出了才会跟着走。

### Windows（PE）：产物是一个 `.zip`，DLL 与 `.exe` 同目录

一个 Windows target 产出的是一个 **`.zip`**，不是 `.tar.gz`，并且是扁平
布局：

```
target/dist/myapp-0.1.0-x86_64-pc-windows-msvc.zip
└── myapp-0.1.0-x86_64-pc-windows-msvc/
    ├── myapp.exe
    ├── vcruntime140.dll        ← only under cxx_runtime = "toolchain-coupled"
    ├── mydep.dll               ← third-party dependencies
    ├── README.md
    └── LICENSE
```

没有 `bin/` + `lib/` 的分层，也没有入口 wrapper —— 这两点都不是风格选择。
Win32 的 loader 从**可执行文件所在目录**解析一个 DLL；PE 没有 `RUNPATH` 可以
指向别处，因此「放在 `.exe` 旁边」**就是** ELF 上 `$ORIGIN/../lib` 提供的
那个机制。

**Windows 自己的 DLL 永远不会被打进包里** —— `kernel32.dll`、`ntdll.dll`、
`ucrtbase.dll`，以及 `api-ms-win-*` 一族 API set。携带一份操作系统组件的私有
拷贝，不是让包变大，而是**让程序坏掉**（进程里出现了两份本应唯一的东西），
Microsoft 的再分发条款也从另一个角度说了同一件事。`[pack.bundle-project]
force_bundle` 仍然可以覆盖这条规则，就像它能覆盖 ELF 的跳过列表一样。

`vcruntime140.dll` 与 `msvcp140.dll` **不是** Windows 自己的：它们属于 MSVC
toolset，正如 `libstdc++.so` 属于 gcc。它们要不要随产物一起走，由
`cxx_runtime` 决定（见 `docs/04-mcpp-toml.md`），不由上面这张表决定 ——
而 `mcpp pack` 会拒绝那些无法兑现契约的组合：

```
$ mcpp pack --mode system          # with cxx_runtime = "toolchain-coupled"
error: cxx_runtime = "toolchain-coupled" and --mode system contradict each other.
```

#### 在 Linux 或 macOS 上给 Windows 打包

这是可行的，而且不是什么特殊模式 —— 只需针对 Windows target 构建，然后打包：

```bash
mcpp pack --target x86_64-windows-gnu     # from a Linux host
```

`mcpp pack` 曾经直接拒绝 Windows。真正的原因不在打包工具：ELF 的依赖闭包是
靠**把产物跑起来**（用 `LD_TRACE_LOADED_OBJECTS`）求出来的，这既跨不了操作
系统，也跨不了架构。PE 的闭包改为直接从文件的导入表里**读**出来，因此不需要
执行任何东西，打包所在的宿主也就自由了。压缩包本身同样由 mcpp 自己写出，理由
相同 —— 不是每个宿主上都存在某个 zip 工具。

有两点值得知道：

- 条目是**存储而非压缩**的，因此 Windows 包的体积大致等于其内容之和。压缩是
  体积上的优化，不是正确性问题，目前尚未实现。
- 压缩包是**确定性的**：不读取任何时间戳，因此同一棵树打两次包字节完全一致，
  公布出去的校验和才有意义。

反方向 —— 在 Windows 上给 Linux 产物打包 —— 仍然不支持：ELF 程序的闭包要靠
目标自己的动态链接器解析，而 Windows 宿主没有办法运行它。Mach-O 程序的闭包是
从文件里读出来的，因此一个 macOS 程序可以在任何宿主上打包。

### macOS（Mach-O）：dylib 与程序放在同一目录

```
target/dist/myapp-0.1.0-aarch64-macos.tar.gz
└── myapp-0.1.0-aarch64-macos/
    ├── bin/myapp
    ├── bin/libmydep.dylib      ← a dependency's dylib, beside the program
    ├── myapp                   ← entry-point wrapper
    ├── README.md
    └── LICENSE
```

Mach-O 程序的闭包从它的 load command 里读出（mcpp 2026.9.14.2+），因此打包
时不运行任何东西。它的 dylib 暂存在它旁边的 `bin/` 下，这也正是一个图内构建的
dylib 的消费者，其链接所带的 `@loader_path` rpath 本就会查找的位置：不编辑
任何 load command，不重新签名，也不 strip 这个程序。

一个被需要的名字，只有在加载器能把它解析到程序所在目录时才会被暂存：

| 名字 | 处理方式 |
|---|---|
| `/usr/lib/…`、`/System/Library/…` | 不暂存；由操作系统提供 |
| `@rpath/<file>` | 当需要它的映像或程序本身，有一条恰好等于 `@loader_path` 或 `@executable_path` 的 rpath 时暂存 |
| `@loader_path/<file>`、`@executable_path/<file>` | 暂存到该路径，该路径必须留在程序所在目录之内 |

任何其他名字都会使闭包不完整，`tar` 与 `dir` 会拒绝并点名它。最常见的情形是
落在操作系统目录之外的绝对 install name：加载器会在每一台机器上都从那个路径
去读，无论树里携带了什么。`--mode system` 与 `--mode static` 在这一行上不
打包任何东西，与 PE 相同。`kind = "lib"` / `"shared"` 的 target 走库打包
路线，不受影响。

### Android：应用目标文件与它的闭包在 `lib/` 下

```
target/dist/myapp-0.1.0-x86_64-linux-android/
├── lib/libmyapp.so             ← the application object
├── lib/libmydep.so             ← a dependency's shared library
├── lib/libc++_shared.so        ← the NDK's C++ runtime, when the object needs it
└── bin/<to>/…                  ← files `[runtime] deploy` placed
```

在 Android 行上，应用是一个暂存在 `lib/` 下的共享目标文件，它的闭包从文件中
读出并暂存在它旁边（mcpp 2026.9.14.2+）。设备提供的名字不会被暂存：即出现在
该 API level 桩目录中的名字，这个目录由 mcpp 向该行自己的编译器询问得到（即
编译器找到 `libc.so` 的那个目录）—— `libc.so`、`libm.so`、`libdl.so`、
`liblog.so` 以及其他平台库。其余每一个名字都必须能在链接用过的某个目录中
解析到：构建的输出目录、图中各包的 `[runtime] library_dirs` 与
`link_library_dirs`，以及编译器的库搜索路径（`libc++_shared.so` 就在其中）。
哪里都解析不到的名字会使 `tar` 与 `dir` 拒绝，并点名它以及被搜索过的那些
目录。

给出多个 `--target` 三元组时，每条腿暂存在 `lib/<abi>/` 下，各自带着自己的
闭包。`--mode` 不适用于这一行：设备恰好提供了平台库，因此既没有可以留给宿主
的库集合，也没有需要携带的 loader。

## 配置项

打包行为通过 `mcpp.toml` 中的 `[pack]` 节配置，常用字段如下：

```toml
[pack]
default_mode  = "static"            # override the normal vendored default for bare `mcpp pack`
strip         = true                # default. false ships the artifacts as built
debug_symbols = "dist/debug"        # separate the debug info here instead of discarding it
include       = ["share/**", "config/*.toml"]   # extra files to bundle
exclude       = ["debug/**"]

# Fine-tune the vendored filtering policy. The configuration key keeps its
# established `bundle-project` spelling.
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # libraries assumed to exist on the target system
force_bundle = ["libfoo.so"]        # bundle even if matched by the PEP 600 list
```

`[pack].default_mode` 当前接受既有的 manifest 拼写 `static`、
`bundle-project` 与 `bundle-all`；`system` 这一 mode 只能通过
`mcpp pack --mode system` 显式选择。CLI 输入同时接受上文的规范名称与兼容
名称。

`static` mode 还需要在 `[target.<triple>]` 下配置一套 musl 工具链；完整写法
参见 [`examples/03-pack-static`](../../examples/03-pack-static/) 中的
`mcpp.toml`。

## 待支持

当前 `.zip` 之外的 Windows DLL 分发方式在规划中。

`.deb`、`.rpm`、AppImage、`.msi` 这些分发格式**不在**这份清单上，而这是一个
决定而不是一处遗漏：它们住在各自的包里，经由 `--format <name>` 到达用户，
理由见上一节。`[pack]` 内建的这几种 mode 不需要再添任何新成员。

本文档随 `mcpp pack` 的实现演进；最新选项以 `mcpp pack --help` 为准。
