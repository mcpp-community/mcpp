# mcpp 基础用法指南

本文档面向 AI Agent，说明 mcpp 的基础使用方法以及如何帮助用户解决问题。

## mcpp 是什么

mcpp 是一个现代 C++ 模块化构建工具，纯 C++23 模块编写，已实现自举。

- 仓库：https://github.com/mcpp-community/mcpp
- 文档：https://github.com/mcpp-community/mcpp/tree/main/docs
- 包索引：https://github.com/mcpp-community/mcpp-index
- 模块化库集合：https://github.com/mcpplibs

## 安装

```bash
# 方式一：xlings 安装（推荐）
xlings install mcpp -y

# 方式二：一键安装脚本
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

安装到 `~/.mcpp/`，自动加入 shell PATH。删除 `~/.mcpp` 即可卸载。

首次运行会自动安装 GCC 工具链到隔离沙盒，不影响系统。

## 核心命令

### 项目管理

```bash
mcpp new <name>          # 创建新项目（默认 C++23 模块模板）
mcpp build               # 构建
mcpp run [-- args]       # 构建并运行（args 透传给可执行文件）
mcpp test [-- args]      # 构建并运行测试
mcpp clean               # 清理构建产物
```

### 依赖管理

```bash
mcpp add <pkg>[@<ver>]     # 添加依赖（如 mcpp add gtest@1.15.2）
mcpp remove <pkg>          # 移除依赖
mcpp update [pkg]          # 更新依赖版本
mcpp search <keyword>      # 搜索包索引
```

### 工具链管理

```bash
mcpp toolchain list                    # 查看已安装工具链
mcpp toolchain install gcc 16          # 安装 GCC 16
mcpp toolchain install llvm 20         # 安装 LLVM/Clang 20
mcpp toolchain default gcc@16.1.0     # 设置默认工具链
```

### 包索引管理

```bash
mcpp index list               # 查看已配置的包索引
mcpp index update [<name>]    # 刷新索引
mcpp index pin <name> [rev]   # 锁定索引到指定 commit
mcpp index unpin <name>       # 解除锁定
```

### 打包发布

```bash
mcpp pack                      # 打包（默认 bundle-project 模式）
mcpp pack --mode static        # musl 全静态打包
mcpp publish --dry-run         # 预览发布内容
```

### 诊断

```bash
mcpp self doctor               # 环境健康检查
mcpp self env                  # 打印路径和配置
mcpp explain E0001             # 查看错误码详细解释
mcpp self config --mirror CN   # 切换镜像（CN / GLOBAL）
```

## mcpp.toml 基础配置

```toml
[package]
name = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "bin"              # bin / lib / shared / test
main = "src/main.cpp"

[dependencies]
gtest = "1.15.2"          # 从默认索引安装

[toolchain]
default = "gcc@16.1.0"    # 指定编译器
```

更多配置项参见：https://github.com/mcpp-community/mcpp/blob/main/docs/05-mcpp-toml.md

## 工作空间（多包项目）

```toml
# 根 mcpp.toml
[workspace]
members = ["libs/*", "apps/*"]

[workspace.dependencies]
gtest = "1.15.2"
```

```bash
mcpp build                    # 构建整个工作空间
mcpp build -p member-name     # 只构建指定成员
```

## 自定义包索引

```toml
# mcpp.toml
[indices]
my-index = "git@gitlab.example.com:team/mcpp-index.git"
local-dev = { path = "/path/to/local/index" }

[dependencies.my-index]
internal-lib = "1.0.0"
```

## 常见问题处理

### 首次构建慢

首次运行需要下载工具链（GCC/Clang），这是正常的。后续构建会使用缓存。

### command not found

`~/.mcpp/bin` 未加入 PATH。重启终端或执行：
```bash
source ~/.bashrc    # bash
source ~/.zshrc     # zsh
exec fish           # fish
```

### 编译错误

1. 确认 mcpp 版本：`mcpp --version`
2. 确认工具链：`mcpp toolchain list`
3. 尝试清理重建：`mcpp clean && mcpp build`

### 依赖找不到

1. 更新索引：`mcpp index update`
2. 搜索确认包名：`mcpp search <keyword>`
3. 确认 mcpp.toml 中的依赖声明格式

## 问题反馈

如果遇到无法解决的问题：

1. **优先在项目 Issue 中反馈**：https://github.com/mcpp-community/mcpp/issues
   - 使用 `gh issue create` 或在页面上创建
   - 描述复现步骤、期望行为、实际行为、mcpp 版本和操作系统

2. **社区论坛讨论**：https://forum.d2learn.org/category/20
   - 适合使用疑问、最佳实践讨论等

如果 AI Agent 无法直接创建 Issue，请提示用户手动创建，并提供整理好的问题描述。
