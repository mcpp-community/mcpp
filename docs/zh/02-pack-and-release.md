# 02 — 发布打包

> 默认的动态链接 `mcpp build` 产物会把 loader 与 RUNPATH 指向构建沙盒。它是
> 开发产物,不是交付物。有三条路把它变成交付物 —— **没有一条使用宿主的 C 库**。

## 三种分发路径

下面每一条产出的产物,其 C 运行时都来自生态,而不是 `/lib64`。这是有意的:
mcpp 之所以针对私有 glibc 构建,正是为了让产物的行为不取决于底下是哪个发行版;
如果最后一步又伸手去拿宿主的 libc,前面这件事就白做了。

| | 方式 | 命令 | C 运行时来自哪里 | 何时选它 |
|---|---|---|---|---|
| **A** | 走生态 | `mcpp emit xpkg` → `xlings install <pkg>` | 目标机自己的 xlings 载荷 | 目标机装了 xlings |
| **B** | 静态单文件 | `mcpp build --target x86_64-linux-musl` | 不来自任何地方 —— 已链进去 | 想要一个无任何运行时依赖的单文件 |
| **C** | 自带运行时 | `mcpp pack --mode self-contained` | 随 bundle 一起分发 | 任何 Linux,含比构建机更老的 |

**关于 A。** 刚构建出的二进制中记录的 `PT_INTERP` 指向**构建机**的载荷,因此
手工复制该文件到另一台机器无法运行 —— 该路径在目标机上不存在。这是「手工复制」
这一动作的性质,而非产物的性质:经 `xlings` 安装时,包内的 ELF 会在**安装期被
重指到目标机自己的载荷**。记录的路径属于构建机细节,不属于分发格式。需要在机器
之间手工复制二进制时,应选择 B 或 C。

**关于 B。** `--target …-musl` 隐含静态链接,所以没有 loader、没有 RUNPATH、
运行期不需要找任何东西。它的结果最小也最可移植,在程序不需要 glibc 专有行为
(NSS 查询、`dlopen` 宿主插件)时应当首选。

**关于 C。** bundle 里带着这套工具链的 glibc 与 loader,因此能在比构建机更老的
发行版上跑 —— 这是 B 覆盖不了、而又确实需要 glibc 时的那一格。选它之前先读下面
的 `/proc/self/exe` 一节:经 bundled loader 启动会改变程序对「自己在哪」的认知。

## 两条轴:target(libc) × mode(打包深度)

发布有两项正交选择:

- **libc / static** 是*构建 target* 属性:`--target …-linux-gnu`(glibc) 与
  `--target …-linux-musl`(musl,static)。`--target …-musl` 隐含 `static`。
- **打包深度** 是*pack* 属性:产物携带多少共享库闭包,由 `--mode` 选择。

| 模式 | 宿主必须提供 | 体积 | 使用场景 |
|---|---|---|---|
| `system` | 所有 `.so`(含第三方) | 最小 | `.deb`/`.rpm`,同发行版集群(包管理器声明依赖) |
| `vendored`(默认) | libc / libstdc++ / loader | +几 MB | 主流发行版(Ubuntu 22+,Debian 12+,RHEL 9+) |
| `self-contained` | 无 | +30–50 MB | 任意 Linux(含旧 glibc);携带闭包与 `run.sh` wrapper |
| `static` | 无(单文件) | +5–10 MB | musl;匹配架构的 Linux x86_64 或 aarch64,Docker scratch,Alpine |

选择建议:

- `.deb`/`.rpm` 或同发行版内部部署 → `system`
- 桌面或服务端发布,目标为主流 Linux 发行版 → `vendored`(默认)
- 需兼容老旧 CentOS、麒麟等 glibc 版本较低的环境 → `self-contained`
- 单个便携文件、无宿主依赖 → `static`

### 需要宿主提供能力的程序

「自包含」有一个下限。有些库只能来自目标机器:图形驱动的用户态部分与正在运行的
内核模块版本绑定,而对专有栈而言,再分发是不被允许的。这类依赖应声明为运行期能力
需求(`docs/zh/05-mcpp-toml.md` §2.11),模式表随之多出一列:

| Mode | 需要宿主提供能力的程序 |
|---|---|
| `system` | ✅ |
| `vendored`(默认) | ✅ **这类程序的正确默认值** |
| `self-contained` | ❌ **打包期拒绝** |
| `static` | ❌ **打包期拒绝** |

两处拒绝出自同一个事实:**自带 libc 的 bundle 无法消费宿主提供的库。** 那个 `.so`
带着它对**目标机 libc** 的要求到达,而该进程没有那份 libc —— 双向实测记录于
mcpp#392 / mcpp#401:私有 glibc 遇上宿主加载的对象,会在重定位阶段、`main` 之前
崩溃。此前这两种模式都是链接通过、启动时失败,或静默降级(图形栈的表现是软件
渲染,而没有任何提示)。

`vendored` 会打包这类程序,并在 bundle 根目录写出一个 **`HOST-REQUIREMENTS`**
文件,说明目标机必须提供什么:

```
capability=opengl.glx.driver discovery=rpath-of-dispatch
```

`discovery` 是可行动的那一半 —— 各机制彼此独立,满足其一并不等于满足另一个。
该文件仅在确有内容时写出:一个空文件等于**声称**什么都不需要。

### 模式名兼容性

上表是规范名称。旧名称仍是**永久兼容别名**:`bundle-project` = `vendored`,
`bundle-all` = `self-contained`。tarball 后缀是被冻结的 wire 格式(由
`install.sh` 消费),不会跟随名称改变:`vendored` 无后缀,`self-contained` 是
`-bundle-all`,`static` 是 `-static`,`system` 是 `-system`。

## 命令

```bash
mcpp pack                          # 默认 vendored
mcpp pack --mode system
mcpp pack --mode static
mcpp pack --mode self-contained     # 别名:--mode bundle-all
mcpp pack --target x86_64-linux-musl   # 等价 --mode static
mcpp pack --target aarch64-linux-musl  # ARM64 等价写法
mcpp pack --format dir                 # 输出为目录,不打包 tarball
mcpp pack -o myapp.tar.gz              # 仅文件名:落到 target/dist/myapp.tar.gz
mcpp pack -o /abs/path/myapp.tar.gz    # 含目录:按字面路径输出
mcpp pack --profile dev                # 换一个 profile 构建(默认 release)
mcpp pack --no-strip                   # 按构建原样发货,不剥符号
mcpp pack --debug-symbols dbg/         # 把分离出的 *.debug 写到 dbg/
```

`-o` 接受裸文件名时自动归到 `target/dist/`;含目录(相对或绝对)
时按字面路径输出。

完整选项参见 `mcpp pack --help`。

### 打包产物用什么构建,里面带什么走

与 `mcpp build` 有两点不同,都因为「这个产物要离开本机」:

**profile 的兜底是 `release` 而不是 `dev`。** 其余优先级不变 ——
`--profile` > `[build] default-profile` > 兜底。只有最后一步不同,所以声明过
profile 的工程仍然拿到它声明的那个,`mcpp pack` 也不会产出一个 `mcpp build`
产不出来的 flag 组合。

> 有一条后果要知道:裸 `mcpp build` 与裸 `mcpp pack` 现在会写进**不同的**
> `target/<triple>/<fingerprint>/` 目录 —— 指纹把 profile 算进去了。手工放到
> 构建产物旁边的文件(一个 DLL、一份数据)因此只在两条命令解析到同一个
> profile 时才被 `pack` 看见:把它写进 `[build] default-profile`,或者两条命令
> 都带 `--profile`。声明式通道(`[runtime] deploy_files`、
> `runtime_search_dirs`)不受影响。

**调试信息会被剥掉,发布者的路径随之消失。** 未 strip 的产物带着 DWARF,而
DWARF 带着发布者源码树与构建目录的绝对路径。剥什么取决于产物**是什么** ——
这是 dh_strip 的分档,而其中归档那一行是要命的:

| 产物 | strip 参数 | 为什么不能更狠 |
|---|---|---|
| 可执行文件 | `--strip-all` | 没有人链接它 |
| 共享库 | `--strip-unneeded` | 保留 `.dynsym` —— 那**就是**导出表 |
| 静态归档 | `--strip-debug --enable-deterministic-archives` | `--strip-all` 会删掉归档的**符号索引**,消费方链接时报 `archive has no index; run ranlib to add one` |

三档都会去掉 `.comment` 与 `.note`。段删除按**精确名字**匹配,所以
`.note.gnu.build-id` 会保留,`--add-gnu-debuglink` 仍然有东西可配对。

`--no-strip`(或 `[pack] strip = false`)按构建原样发货。
`--debug-symbols <目录>` 则是分离而不是丢弃:写出 `<目录>/<产物>.debug`,
并给发货的产物加上指向它的 `.gnu_debuglink` —— 调试器与 `debuginfod` 认这个。

> `[pack] strip` 不是 `[profile.<name>].strip`。后者是给**链接**加 `-s`,
> 既碰不到静态归档、也无法分离出任何东西;前者管的是**包里带什么**。
> 两个不同的决定,两个不同的名字。

**被捆绑进来的库永远不 strip。** 它们来自 store 或宿主,不是 mcpp 构建的,
为了这一个 bundle 去改写别人的共享载荷不是打包器该做的事。

## 产物布局

tarball 内容包在一个顶层目录里,该目录的名字与 tarball 文件名(去掉
`.tar.gz`)保持一致 —— 这样图形界面"右键解压"和命令行 `tar -xzf` 都
得到同一个自包含的目录,不会把内容散到当前路径。

### Mode `static`

```
target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz
└── myapp-0.1.0-x86_64-linux-musl-static/
    ├── bin/myapp                ← 全静态 ELF(无 PT_INTERP / RUNPATH)
    ├── myapp                    ← 顶层入口(thin shell wrapper,可直接执行 ./myapp)
    ├── README.md                ← 自动从项目根目录拷贝
    └── LICENSE
```

### Mode `vendored`(默认;别名:`bundle-project`)

```
target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu/
    ├── bin/myapp                ← 动态链接,RUNPATH=$ORIGIN/../lib
    │                                PT_INTERP=/lib64/ld-linux-x86-64.so.2
    ├── lib/
    │   ├── libcurl.so.4         ← 项目第三方依赖
    │   ├── libssl.so.3
    │   └── ...
    ├── myapp                    ← 顶层入口
    ├── README.md
    └── LICENSE
```

跳过列表参考
[PEP 600 / manylinux2014](https://peps.python.org/pep-0600/) ——
`libc`、`libm`、`libstdc++`、`libgcc_s`、`ld-linux-*` 等基础库默认
假设目标系统已具备,不打包进 tarball。

### Mode `self-contained`(别名:`bundle-all`)

```
target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu-bundle-all/
    ├── bin/myapp
    ├── lib/
    │   ├── ld-linux-x86-64.so.2  ← 完整 loader 与 libc
    │   ├── libc.so.6
    │   ├── libstdc++.so.6
    │   ├── libgcc_s.so.1
    │   └── ...项目依赖
    ├── myapp                     ← 双入口之一
    ├── run.sh                    ← 双入口之二(内容相同)
    ├── README.md
    └── LICENSE
```

`-o foo.tar.gz` 时顶层目录名也会变成 `foo`(包名 - 目录名 始终一致)。

ELF 规范限制 `PT_INTERP` 不能使用 `$ORIGIN`,因此 `self-contained` 模式
通过 `run.sh`(及顶层同名 wrapper)以绝对路径方式调用 loader:

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

上面的布局与 wrapper 以 x86_64 为例。打包器会按 target 推导 loader 名称；aarch64
对应 `ld-linux-aarch64.so.1`。

#### 经 bundled loader 启动时 `/proc/self/exe` 的取值

「由 loader 启动」有一个上述布局看不出的后果:内核会把 `/proc/self/exe`
指向 **loader** 而非目标程序;`/proc/self/cmdline` 中也混入了
`--library-path`。于是所有「在可执行文件旁边找资源」的逻辑都会解析到 `lib/`
而不是 bundle 根目录——而且是**静默**的。实际表现是:GUI 框架找不到字体因而
文字渲染空白、`assets/` 目录看起来不存在、随包分发的辅助二进制定位失败。
按 `/proc/self/cmdline` 解析 argv 的代码则会拿到混入 loader 参数的结果。

这只影响 `self-contained`。`vendored`、`system`、`static` 的 `PT_INTERP`
都能被内核直接使用,`/proc/self/exe` 是正确的。

wrapper 为此导出 **`MCPP_BUNDLE_DIR`**(bundle 根目录)。优先用它,只在未设置
时回退:

```c
const char *base = getenv("MCPP_BUNDLE_DIR");   /* 由 run.sh 设置 */
if (!base) {
    /* 不是经 wrapper 启动的 —— 此时 /proc/self/exe 可信 */
}
```

如果应用本身改不了(比如第三方 GUI 框架自己做解析),改用 `--mode vendored`:
它把 `PT_INTERP` 重指到宿主 loader,`/proc/self/exe` 正常,代价是要求宿主
glibc 不低于构建时所用的那份。

### Windows(PE):产物为 `.zip`,DLL 与 `.exe` 同目录

Windows 目标产出的是 **`.zip`** 而不是 `.tar.gz`,并且是扁平布局:

```
target/dist/myapp-0.1.0-x86_64-pc-windows-msvc.zip
└── myapp-0.1.0-x86_64-pc-windows-msvc/
    ├── myapp.exe
    ├── vcruntime140.dll        ← 仅在 cxx_runtime = "toolchain-coupled" 时
    ├── mydep.dll               ← 第三方依赖
    ├── README.md
    └── LICENSE
```

没有 `bin/` + `lib/` 的分层,也没有入口 wrapper —— 这两点都不是风格选择。
Win32 loader 解析 DLL 的第一顺位就是**可执行文件所在目录**,而 PE 没有
`RUNPATH` 可以指向别处,所以"放在 `.exe` 旁边"**就是** ELF 上
`$ORIGIN/../lib` 所提供的那个机制。

**Windows 自己的 DLL 永远不会被打进包里** —— `kernel32.dll`、`ntdll.dll`、
`ucrtbase.dll`、`api-ms-win-*` API set。带一份系统组件的私有拷贝不是"包变大
了",而是**程序坏了**(进程里出现了两份本应唯一的东西),而 Microsoft 的再分发
条款也从另一侧说了同一件事。`[pack.bundle-project] force_bundle` 仍然可以覆盖
这条,和它覆盖 ELF 跳过表一样。

`vcruntime140.dll` / `msvcp140.dll` **不是** Windows 自己的:它们属于 MSVC
toolset,就像 `libstdc++.so` 属于 gcc。它们要不要跟着产物走,由 `cxx_runtime`
决定(见 `docs/zh/05-mcpp-toml.md`),不由这张表决定 —— 而 `mcpp pack` 会拒绝
那些无法兑现契约的组合:

```
$ mcpp pack --mode system          # 且 cxx_runtime = "toolchain-coupled"
error: cxx_runtime = "toolchain-coupled" and --mode system contradict each other.
```

#### 在 Linux / macOS 上给 Windows 打包

这是可以的,而且不是什么特殊模式 —— 指定 Windows 目标构建,然后打包即可:

```bash
mcpp pack --target x86_64-windows-gnu     # 在 Linux 宿主上
```

`mcpp pack` 以前直接拒绝 Windows。真正的原因不是打包工具:ELF 的依赖闭包是靠
**把产物跑起来**(`LD_TRACE_LOADED_OBJECTS`)求出来的,所以它既跨不了 OS 也跨
不了架构。PE 的闭包改为从文件的导入表里**读**出来,于是不需要执行任何东西,
打包宿主也就自由了。压缩包本身也由 mcpp 自己写,理由相同 —— 没有哪个 zip 工具
在每个宿主上都存在。

有两点值得知道:

- 条目是 **stored(不压缩)** 的,所以 Windows 包的体积约等于其内容之和。压缩
  是体积优化而不是正确性问题,目前尚未实现。
- 压缩包是**确定性**的:不读取任何时间戳,同一棵树打两次字节一致,公布的校验和
  才有意义。

反方向 —— 在 Windows 上给 Linux / macOS 产物打包 —— 仍然不支持,原因还是最初
那个:那条闭包要由目标自己的动态链接器解析,而 Windows 宿主没有办法运行它。

#### Mach-O 程序会被拒绝 —— 在所有宿主上,包括 macOS

同一步闭包解析是靠 `LD_TRACE_LOADED_OBJECTS=1` **运行产物**来问动态链接器要
依赖表的。这个变量属于 glibc 的 ld.so,dyld 从来不认(它的对应物是
`DYLD_PRINT_LIBRARIES`)。所以在 Mac 上这条命令不会 trace 任何东西 ——
**它会把用户的程序跑起来**,然后把程序的输出当成依赖表解析。mcpp 现在直接拒绝,
并在信息里点名缺的是哪个机制。

判定按产物的**格式**而不是宿主,理由与 Windows 那条完全相同:
`LD_TRACE_LOADED_OBJECTS` 在 Linux 上也 trace 不了一个 Mach-O。

`kind = "lib"` / `"shared"` 目标在 macOS 上照常打包 —— 库打包从不运行产物。
这条限制只针对程序。

## 配置项

打包行为通过 `mcpp.toml` 中的 `[pack]` 节配置,常用字段如下:

```toml
[pack]
default_mode  = "static"            # 覆盖裸 `mcpp pack` 的正常 vendored 默认值
strip         = true                # 默认值。false = 按构建原样发货
debug_symbols = "dist/debug"        # 把调试信息分离到这里,而不是丢弃
include       = ["share/**", "config/*.toml"]   # 额外打包的文件
exclude       = ["debug/**"]

# 微调 vendored 的过滤策略。配置键保留既有的 `bundle-project` 拼写。
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # 假定目标系统已具备的库
force_bundle = ["libfoo.so"]        # 即使命中 PEP 600 名单也强制打包
```

`[pack].default_mode` 当前接受既有 manifest 拼写 `static`、`bundle-project` 和
`bundle-all`; `system` 通过 `mcpp pack --mode system` 显式选择。CLI 输入同时接受
上文的规范名称和兼容名称。

`static` 模式还需在 `[target.<triple>]` 中配置 musl 工具链,完整写法
参见 [`examples/03-pack-static`](../../examples/03-pack-static/) 的 `mcpp.toml`。

## 待支持

macOS **程序** bundling(Mach-O 依赖闭包,走 `otool -L` / `LC_LOAD_DYLIB`,
重定位走 `install_name_tool`)仍在规划中;在它落地之前,`mcpp pack <程序>`
会在该格式上拒绝,而不是产出一个只是看起来像 bundle 的东西。当前 `.zip`
之外的 Windows DLL 分发,以及 `.deb` / `.rpm` / AppImage 等格式,同样在规划中。本文档随 `mcpp pack` 实现演进,最新选项以
`mcpp pack --help` 为准。
