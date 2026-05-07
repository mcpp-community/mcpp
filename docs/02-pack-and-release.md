# 02 — 发布打包

> `mcpp build` 产生的二进制仅可在本机运行 —— loader 与 RUNPATH 均指向
> `~/.mcpp/`。如需分发至其他机器或部署至服务器,应使用 `mcpp pack`
> 生成自包含 tarball。

## 三种模式

| 模式 | 说明 | 体积增量 | 兼容性 |
|---|---|---|---|
| `static` | musl 全静态,无运行期依赖 | +5–10 MB | 任意 Linux x86_64 |
| `bundle-project`(默认) | 仅打包项目第三方 .so | +几 MB | 主流发行版(Ubuntu 22+, Debian 12+, RHEL 9+ 等) |
| `bundle-all` | 包含 ld-linux、libc、libstdc++ 与项目 .so | +30–50 MB | 包含老旧版本在内的任意 Linux |

选择建议:

- 命令行工具,或目标为 Docker scratch、Alpine 等最小镜像 → `static`
- 桌面或服务端发布,目标为主流 Linux 发行版 → `bundle-project`(默认)
- 需兼容老旧 CentOS、麒麟等 glibc 版本较低的环境 → `bundle-all`

## 命令

```bash
mcpp pack                          # 默认 bundle-project
mcpp pack --mode static
mcpp pack --mode bundle-all
mcpp pack --target x86_64-linux-musl   # 等价 --mode static
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

### Mode `bundle-project`(默认)

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

### Mode `bundle-all`

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

ELF 规范限制 `PT_INTERP` 不能使用 `$ORIGIN`,因此 bundle-all 模式
通过 `run.sh`(及顶层同名 wrapper)以绝对路径方式调用 loader:

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

## 配置项

打包行为通过 `mcpp.toml` 中的 `[pack]` 节配置,常用字段如下:

```toml
[pack]
default_mode = "static"             # 不带 --mode 时的默认模式
include      = ["share/**", "config/*.toml"]   # 额外打包的文件
exclude      = ["debug/**"]

# 微调 bundle-project 的过滤策略
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # 假定目标系统已具备的库
force_bundle = ["libfoo.so"]        # 即使命中 PEP 600 名单也强制打包
```

`static` 模式还需在 `[target.<triple>]` 中配置 musl 工具链,完整写法
参见 [`examples/03-pack-static`](../examples/03-pack-static/) 的 `mcpp.toml`。

## 待支持

macOS dylib、Windows DLL,以及 `.deb` / `.rpm` / AppImage 等分发格式
尚在规划中。本文档随 `mcpp pack` 实现演进,最新选项以
`mcpp pack --help` 为准。
