# 02 — 发布打包

> 默认的动态链接 `mcpp build` 产物会把 loader 与 RUNPATH 指向构建沙盒。它是
> 开发产物,不是交付物。有三条路把它变成交付物 —— **没有一条使用宿主的 C 库**。

## 三种分发方式

下面每一条产出的产物,其 C 运行时都来自生态,而不是 `/lib64`。这是有意的:
mcpp 之所以针对私有 glibc 构建,正是为了让产物的行为不取决于底下是哪个发行版;
如果最后一步又伸手去拿宿主的 libc,前面这件事就白做了。

| | 方式 | 命令 | C 运行时来自哪里 | 何时选它 |
|---|---|---|---|---|
| **A** | 走生态 | `mcpp emit xpkg` → `xlings install <pkg>` | 目标机自己的 xlings 载荷 | 目标机装了 xlings |
| **B** | 静态单文件 | `mcpp build --target x86_64-linux-musl` | 不来自任何地方 —— 已链进去 | 想要一个无任何运行时依赖的单文件 |
| **C** | 自带运行时 | `mcpp pack --mode self-contained` | 随 bundle 一起分发 | 任何 Linux,含比构建机更老的 |

**关于 A,以及那个让人意外的地方。** 刚构建出的二进制里烙的 `PT_INTERP` 指向
**你这台机器**的载荷,所以手工把这个文件拷到另一台机器上跑不起来——那个路径
在那边不存在。与其说这是产物的性质,不如说是「手工拷贝」这个动作的性质:经
`xlings` 安装时,包里的 ELF 会在**装机期被重指到目标机自己的载荷**。烙进去的
路径是构建机的细节,不是分发格式。如果你就是要在机器之间手工拷二进制,那你要
的是 B 或 C。

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
```

`-o` 接受裸文件名时自动归到 `target/dist/`;含目录(相对或绝对)
时按字面路径输出。

完整选项参见 `mcpp pack --help`。

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

#### 陷阱:经 bundled loader 启动后的 `/proc/self/exe`

「由 loader 启动」有一个上面的布局看不出来的后果:内核会把 `/proc/self/exe`
指向 **loader**,而不是你的程序;`/proc/self/cmdline` 里也混进了
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

## 配置项

打包行为通过 `mcpp.toml` 中的 `[pack]` 节配置,常用字段如下:

```toml
[pack]
default_mode = "static"             # 覆盖裸 `mcpp pack` 的正常 vendored 默认值
include      = ["share/**", "config/*.toml"]   # 额外打包的文件
exclude      = ["debug/**"]

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

macOS dylib、Windows DLL,以及 `.deb` / `.rpm` / AppImage 等分发格式
尚在规划中。本文档随 `mcpp pack` 实现演进,最新选项以
`mcpp pack --help` 为准。
