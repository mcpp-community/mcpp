# Changelog

> 本文件追踪 `mcpp-community/mcpp` 公开仓的版本演进。
> 格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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
