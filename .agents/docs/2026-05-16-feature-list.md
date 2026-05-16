# mcpp 功能特性清单

> v0.0.15 (2026-05-16)

---

## 一、核心亮点

### 1. 纯 C++23 模块自举
- mcpp 用 C++23 模块写成（43+ 个模块，零 .h 头文件），用自己构建自己
- 这意味着 mcpp 的模块系统经过了最严格的实战验证
- 目前没有其他 C++ 构建工具做到这一点

### 2. 3 行 TOML 替代 11 行 CMake
- C++23 模块项目只需 `mcpp.toml` 3 行配置
- `import std` 自动处理，无需手动配置 FILE_SET / SCAN_FOR_MODULES
- 对比 CMake 需要 11 行且需要 3.28+ 版本

```
# mcpp.toml                          # CMakeLists.txt
[package]                            cmake_minimum_required(VERSION 3.28)
name = "hello"                       project(hello LANGUAGES CXX)
                                     set(CMAKE_CXX_STANDARD 23)
[targets.hello]                      set(CMAKE_CXX_STANDARD_REQUIRED ON)
kind = "bin"                         set(CMAKE_CXX_SCAN_FOR_MODULES ON)
main = "src/main.cpp"                add_executable(hello src/main.cpp)
                                     target_sources(hello
                                       PUBLIC FILE_SET CXX_MODULES
                                       FILES src/greet.cppm)
```

### 3. 一键安装，开箱即用
- `xlings install mcpp -y` 或一条 curl 命令
- 首次运行自动安装 GCC 16 / musl-gcc 工具链到隔离沙盒 (`~/.mcpp/`)
- 不污染系统环境，删除 `~/.mcpp` 即干净卸载
- 无需预装任何编译器、CMake、ninja 等依赖

### 4. `import std` 全自动
- GCC 15+/16+：自动检测 `bits/std.cc` → 预编译 `std.gcm`
- Clang 17+/LLVM 20：自动检测 `std.cppm` + `std.compat.cppm` → 预编译 `std.pcm`
- 跨项目缓存：编译一次，所有项目复用
- 用户零配置

### 5. 内置 GCC 16 + LLVM 20 双编译器
- GCC 16.1.0（最新 C++23/26 特性）
- GCC 15.1.0-musl（全静态二进制）
- LLVM/Clang 20.1.7（libc++ + clang-scan-deps）
- 通过 `mcpp toolchain install` 一键安装，多版本共存

---

## 二、构建系统特性

### 6. C++20/23 模块原生支持
- 模块接口单元（`.cppm`）+ 实现单元（`.cpp`）
- 模块分区（`export module X:Y`）完整支持
- 多模块项目自动依赖图分析
- `import std` / `import std.compat` 自动处理

### 7. 模块感知增量构建（三层优化）
- **P0 前端脏检查**：输入未变则跳过整个 prepare_build
- **P1 逐文件 dyndep**：只重编变化的模块（P1689 格式）
- **P2 BMI restat**：接口未变时 copy-if-different 截断级联重编
- GCC 内置扫描 / Clang 用 `clang-scan-deps`

### 8. 指纹化 BMI 缓存
- 按编译器/标志/标准库/mcpp版本/锁文件哈希生成指纹
- 跨项目共享：同指纹的 `std.gcm` / `std.pcm` 只编译一次
- 多指纹 LRU 淘汰（避免缓存无限膨胀）
- 布局：`~/.mcpp/bmi/<fingerprint>/deps/<index>/<pkg>@<ver>/`

### 9. Ninja 后端
- 自动生成 `build.ninja`
- dyndep 动态依赖支持（GCC P1689 + Clang scan-deps）
- 增量构建 + 并行编译
- restat 规则防止 BMI 级联重编

### 10. compile_commands.json
- 每次构建自动生成
- clangd / ccls / VS Code 即用
- 内容变化时才重写（避免 IDE 频繁重索引）
- C 和 C++ 编译命令分别追踪

### 11. C 语言一等支持
- `.c` 文件自动检测，用对应的 C 编译器编译
- 支持混合 C/C++ 项目（如 mbedtls 108 个 .c 文件 + C++ 模块）
- 可配置 `[build].c_standard` 和 `[build].cflags`
- 编译器自动推导：`g++` → `gcc`，`clang++` → `clang`

---

## 三、包管理与依赖

### 12. SemVer 依赖解析
- 支持 `^`（兼容）、`~`（补丁）、`>=/<` 范围、精确版本
- 三级解析：
  - Level 2：约束合并（重叠区间取交集）
  - Level 1：多版本共存（模块名 mangling 回退）
  - Level 0：精确匹配
- 传递依赖自动递归解析

### 13. 锁文件（mcpp.lock）
- v2 格式：包含 `[indices.<name>]` 索引快照 + 每包 `namespace` 字段
- `mcpp build/run/test/pack` 只读锁，不联网
- `mcpp update` 重新解析写回锁
- v1 → v2 自动迁移

### 14. 命名空间系统
- 默认命名空间 `mcpplibs`
- 自定义命名空间：`[dependencies.myteam] foo = "1.0"`
- CLI 格式：`mcpp add myteam:foo@1.0`
- 兼容旧版点号格式：`mcpplibs.cmdline`

### 15. 自定义包索引（v0.0.15）
- `[indices]` 在 mcpp.toml 中配置自定义索引仓库
- 支持 git URL / 本地路径 / tag / rev / branch
- 项目级隔离：自定义索引在 `.mcpp/` 下，不污染全局
- 内置 `mcpplibs` 走全局路径，其他走项目级
- `mcpp index list/update/pin/unpin` CLI 命令
- Workspace 成员自动继承根项目的 `[indices]`

### 16. 依赖来源
- **索引**：mcpplibs 官方 + 自定义 index 仓库
- **Git**：`git = "https://..."` 直接引用
- **本地路径**：`path = "../core"` 开发联调
- **Workspace 继承**：`.workspace = true` 版本由根统一管理

---

## 四、工具链管理

### 17. 一键安装多版本编译器
- `mcpp toolchain install gcc 16`
- `mcpp toolchain install llvm 20`
- `mcpp toolchain install musl-gcc 15`
- 版本部分匹配：`gcc 15` → 自动选最高 15.x.y

### 18. 多工具链共存 & 切换
- `mcpp toolchain list` 查看已安装
- `mcpp toolchain default gcc@16.1.0` 设置默认
- mcpp.toml 中按平台指定：`linux = "gcc@16"`, `macos = "llvm@20"`
- 按 target triple 覆盖：`[target.x86_64-linux-musl] toolchain = "gcc@15.1.0-musl"`

### 19. 隔离沙盒环境
- 所有工具链安装在 `~/.mcpp/registry/` 下
- 不影响系统 PATH 或全局包管理器
- 包含 ninja、patchelf、binutils 等构建依赖
- xlings 提供底层管理能力

### 20. GCC + Clang 编译管线平权
- 同一套 `BmiTraits` 抽象层驱动两种编译器
- GCC：`gcm.cache/*.gcm`，单步编译，内置 P1689 扫描
- Clang：`pcm.cache/*.pcm`，两步 `--precompile` + `-c`，外部 `clang-scan-deps`
- 指纹不同 → 缓存自动隔离，互不干扰

---

## 五、打包与发布

### 21. `mcpp pack` — 三种打包模式
- **static**：musl 全静态 ELF，单文件可分发，无 glibc 依赖
- **bundle-project**（默认）：捆绑项目第三方 .so
- **bundle-all**：捆绑所有动态依赖（含 libc/libstdc++）
- 自动 patchelf 修正 RPATH
- 支持 `--format tar|dir` 输出格式

### 22. `mcpp publish` — 包发布
- 生成 xpkg.lua 描述符
- `--dry-run` 预览，`--allow-dirty` 跳过 git 检查
- 支持多平台（linux/macosx/windows）artifact 声明

### 23. musl 全静态二进制
- mcpp 自身的 release 二进制就是 musl 全静态的
- `mcpp pack --mode static` 用户项目也能全静态打包
- 适合容器部署 / 嵌入式 / 无 glibc 环境

---

## 六、工作空间（Workspace）

### 24. 多包工作空间
- `[workspace] members = ["libs/*", "apps/*"]`
- 统一锁文件 + 统一 target 目录
- 选择性构建：`mcpp build -p member-name`
- 自动 cwd 检测：在成员目录也能找到 workspace 根

### 25. 版本统一管理
- `[workspace.dependencies]` 声明共享版本
- 成员用 `.workspace = true` 继承
- 成员可覆盖（member-level override）

### 26. 配置继承
- 工具链、构建标志、target 覆盖从根级联到成员
- `[indices]` 自动继承（v0.0.15）
- 成员有声明则优先用自己的

---

## 七、开发体验

### 27. `mcpp new` 项目脚手架
- 生成 mcpp.toml + src/ 目录结构
- 模板包含 `import std;` 示例

### 28. `mcpp run [-- args...]`
- 构建 + 运行，args 透传给可执行文件

### 29. `mcpp test [-- args...]`
- 自动发现 `tests/**/*.cpp`，构建并运行
- 支持 workspace 成员选择性测试
- 测试参数透传

### 30. `mcpp search <keyword>`
- 在已配置的索引中搜索包

### 31. `mcpp add / remove / update`
- `mcpp add gtest@1.15.2` — 添加依赖到 mcpp.toml
- `mcpp remove gtest` — 移除
- `mcpp update [pkg]` — 重新解析版本约束

### 32. 错误码解释
- `mcpp explain E0001` — 查看错误详细描述
- 5 个错误码：E0001(依赖名不匹配)、E0002(模块未提供)、E0003(版本不满足)、E0004(工具链 pin 不匹配)、E0005(BMI 缓存损坏)

### 33. 自诊断
- `mcpp self doctor` — 环境健康检查
- `mcpp self env` — 打印路径和配置
- `mcpp self config --mirror CN|GLOBAL` — 切换镜像

---

## 八、平台支持

### 34. Linux x86_64（完整支持）
- GCC (glibc) ✅ + GCC (musl) ✅ 默认
- Clang/LLVM ✅（v0.0.14 达到 GCC 平权）
- CI 自动验证

### 35. Clang/LLVM Linux 平权（v0.0.14）
- 12 个 Blocker 全部解决
- `import std` + `import std.compat` ✅
- 多模块 dyndep 增量构建 ✅
- BMI 缓存 ✅
- 6 个专用 E2E 测试

### 36. 跨平台路线
- macOS Apple Clang（复用 clang.cppm）
- Windows Clang-cl
- Windows MSVC（`.ifc` 格式 + `/std:c++latest`）
- `BmiTraits` 抽象层已为多编译器预留扩展点

---

## 九、技术架构

### 37. 43+ 个 C++23 模块
- 全模块化，无 .h 头文件
- 模块分层：cli / build / pm / toolchain / modgraph / pack / ui / xlings
- 每个子系统独立模块，import 关系清晰

### 38. xlings 集成抽象层
- `src/xlings.cppm`：统一的 xlings 命令构建 + NDJSON 事件解析
- 支持全局模式 + 项目级模式（`XLINGS_PROJECT_DIR`）
- 镜像配置（CN/GLOBAL）
- 沙盒引导：自动安装 xlings + ninja + patchelf

### 39. 模块图分析
- `src/modgraph/scanner.cppm`：源码扫描 `import`/`export module` 声明
- `src/modgraph/graph.cppm`：构建模块依赖图
- `src/modgraph/validate.cppm`：检测循环依赖、重复模块名
- `src/modgraph/p1689.cppm`：P1689 JSON 格式输出

### 40. 版本约束求解器
- `src/version_req.cppm`：SemVer 语法解析
- `src/pm/resolver.cppm`：约束合并 + 版本选择
- `src/pm/mangle.cppm`：Level 1 多版本模块名 mangling

---

## 十、CI/CD 与自动化

### 41. GitHub Actions CI
- 自举构建：mcpp 用 mcpp 构建自己
- E2E 测试套件：40+ 个测试脚本
- 缓存优化：xlings 工具链 + BMI 缓存
- 单 workflow，15-20 分钟完成

### 42. 安装脚本
- `install.sh`：一键安装到 `~/.mcpp/`
- 自动加入 shell PATH（bash/zsh/fish）
- 支持 xlings 安装方式和直接 curl 安装
