# 02 — 发布打包

> 默认的动态链接 `mcpp build` 产物通常会把 loader 与 RUNPATH 指向构建沙盒。
> 如需分发至其他机器或部署至服务器,应使用 `mcpp pack` 生成带有适当运行时闭包的
> 发布 tarball 或目录。

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
