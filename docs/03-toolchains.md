# 03 — 工具链管理

> mcpp 维护一个独立的工具链沙盒,与系统 PATH 完全隔离。

## 设计动机

C++23 模块对编译器版本较为敏感,不同版本的 GCC / Clang 在模块语义
处理上存在明显差异。系统包管理器提供的版本通常滞后,且多版本共存
存在维护成本。mcpp 将工具链统一安装在沙盒目录
(`~/.mcpp/registry/data/xpkgs/`)中,允许项目按需选择版本,且不会
影响系统环境。

## 自动安装

首次运行 `mcpp build` 时,若尚未配置工具链,mcpp 会自动安装一份
musl-gcc 15.1(适用于全静态构建)并将其设为全局默认:

```
First run no toolchain configured — installing gcc@15.1.0-musl (musl, static) as default
Downloading xim:musl-gcc@15.1.0 [====>      ] 312 MB / 808 MB  3.7 MB/s
Default set to gcc@15.1.0-musl
```

后续构建不再触发该流程。

> [!TIP]
> 在 CI 或离线环境中,可通过设置 `MCPP_NO_AUTO_INSTALL=1` 关闭自动
> 安装行为。此时若未安装工具链,`mcpp build` 将直接报错而不会发起
> 网络请求。

## 手动安装

```bash
mcpp toolchain install gcc 16.1.0           # GNU libc,适用于动态链接默认场景
mcpp toolchain install gcc 15.1.0-musl      # musl libc,适用于全静态构建
mcpp toolchain install musl-gcc 15.1.0      # 等价于上一条
```

版本号支持部分匹配:

```bash
mcpp toolchain install gcc 15               # 安装 15.x.y 中的最高版本(15.1.0)
mcpp toolchain install gcc@16               # 同样支持 @ 形式
```

## 切换默认工具链

```bash
mcpp toolchain default gcc@16.1.0
mcpp toolchain default gcc 15               # 部分版本时,从已安装的版本中选择最高
```

## 查看工具链状态

```bash
mcpp toolchain list
```

输出形式如下:

```
Installed:
     TOOLCHAIN               BINARY
     gcc 15.1.0-musl         @mcpp/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++
  *  gcc 16.1.0              @mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++

Available (run `mcpp toolchain install <compiler> <version>`):
     TOOLCHAIN
     gcc 13.3.0
     gcc 11.5.0
     ...
```

带有 `*` 标记的条目为当前默认工具链。`@mcpp/...` 是 `~/.mcpp/...`
的简写形式,用于减少输出宽度。

## 项目级版本锁定

若项目需固定特定版本而不依赖全局默认,可在项目的 `mcpp.toml` 中声明:

```toml
[toolchain]
default = "gcc@16.1.0"

# 也可按平台分发
[toolchain]
linux = "gcc@15.1.0-musl"
macos = "llvm@20"
```

项目级声明优先于全局默认配置。

## 跨工具链构建

```bash
mcpp build --target x86_64-linux-musl
```

mcpp 将读取 `mcpp.toml` 中 `[target.x86_64-linux-musl]` 节,覆盖默认的
工具链与 linkage 设置。该机制配合 `mcpp pack --mode static` 可生成
全静态发布包,完整示例参见
[`examples/03-pack-static`](../examples/03-pack-static/)。

## 卸载

```bash
mcpp toolchain remove gcc@16.1.0
```

## 重置沙盒

```bash
rm -rf ~/.mcpp                              # 删除整个沙盒
mcpp build                                  # 后续构建将再次触发首次安装
```

## 环境变量

mcpp 的运行行为可通过以下环境变量调整:

| 变量 | 用途 |
|---|---|
| `MCPP_HOME` | 覆盖沙盒位置(默认 `~/.mcpp/`),绝对路径优先级最高 |
| `MCPP_NO_AUTO_INSTALL=1` | 禁用工具链自动安装,适用于 CI 与离线环境 |
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | 禁用彩色输出 |
| `MCPP_LOG=trace\|debug\|info\|warn\|error` | 日志级别 |

未显式设置 `MCPP_HOME` 时,mcpp 将基于二进制所在目录的上一级路径
自动定位沙盒位置(release tarball 解压至 `~/.mcpp/` 后,`~/.mcpp/`
即为 home),因此 release 版本无需任何环境变量配置即可运行。
