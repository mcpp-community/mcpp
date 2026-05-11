# compile_commands.json 设计方案

> mcpp 0.0.9 — IDE 集成(clangd / VSCode / CLion)

## 1. 动机

C++ IDE 和语言服务器(clangd)需要 `compile_commands.json` 来理解编译参数,
提供跳转、补全、诊断等功能。CMake/Meson/xmake 都有此能力,mcpp 需要补齐。

## 2. 模块文件划分

```
src/build/
  plan.cppm                  ← BuildPlan 数据结构(不变)
  flags.cppm                 ← 🆕 共用 flag 计算逻辑
  compile_commands.cppm      ← 🆕 compile_commands.json 生成
  ninja_backend.cppm         ← ninja build.ninja 生成(瘦身,复用 flags)
  backend.cppm               ← Backend 抽象接口(不变)
```

### 2.1 `src/build/flags.cppm`

职责:从 BuildPlan 计算出所有编译/链接 flag。

```cpp
export module mcpp.build.flags;
import mcpp.build.plan;

export namespace mcpp::build {

struct CompileFlags {
    std::string cxx;                   // "-std=c++23 -fmodules -O2 -I... --sysroot=..."
    std::string cc;                    // "-std=c11 -O2 -I... --sysroot=..."
    std::string ld;                    // "-static -static-libstdc++ --sysroot=..."
    std::filesystem::path cxxBinary;   // g++ 路径
    std::filesystem::path ccBinary;    // gcc 路径(派生)
    std::filesystem::path arBinary;    // ar 路径
};

CompileFlags compute_flags(const BuildPlan& plan);

}
```

从 ninja_backend.cppm 中提取:
- include_dirs → `-I` 拼接
- sysroot → `--sysroot=`
- binutils → `-B`
- opt_flag (`-O2` / `-Og`)
- pic_flag、user_cxxflags/cflags
- C 编译器路径推导 (`derive_c_compiler`)

**一处计算,多处消费**(ninja、compile_commands、未来后端)。

### 2.2 `src/build/compile_commands.cppm`

职责:生成标准 compile_commands.json。

```cpp
export module mcpp.build.compile_commands;
import mcpp.build.plan;
import mcpp.build.flags;

export namespace mcpp::build {

std::string emit_compile_commands(const BuildPlan& plan,
                                   const CompileFlags& flags);

void write_compile_commands(const BuildPlan& plan,
                             const CompileFlags& flags);

}
```

- 用 `arguments` 数组格式(clangd 推荐,避免 shell 转义问题)
- 写到 `<projectRoot>/compile_commands.json`(clangd 默认向上查找)

### 2.3 `ninja_backend.cppm` 瘦身

- 删除 `compute_cxxflags()` / `compute_cflags()` 重复代码
- `import mcpp.build.flags;` 复用 `CompileFlags`
- `emit_ninja_string` 里直接用 `flags.cxx` / `flags.cc`

## 3. 调用链

```
cli.cppm: cmd_build()
  ├── BuildPlan plan = make_plan(...)
  ├── CompileFlags flags = compute_flags(plan)
  ├── write_compile_commands(plan, flags)     ← 每次 build 自动
  └── backend->build(plan, opts)              ← ninja 也用 flags
```

## 4. JSON 格式

```json
[
  {
    "directory": "/home/user/myproject",
    "file": "src/main.cpp",
    "arguments": [
      "/path/to/g++",
      "-std=c++23", "-fmodules", "-O2",
      "-I/path/to/include",
      "-c", "src/main.cpp",
      "-o", "target/.../obj/main.o"
    ],
    "output": "target/.../obj/main.o"
  }
]
```

对标 clang JSON Compilation Database 规范:
- `directory` + `file`: 必填
- `arguments`: 推荐(优于 `command` 字符串)
- `output`: 可选

## 5. 输出位置

`<projectRoot>/compile_commands.json` — clangd 从源文件向上找,根目录放置
零配置即生效。

## 6. 后续扩展

| 扩展方向 | 实现方式 |
|---|---|
| 新后端(Makefile 等) | import `flags.cppm`,不重复计算 |
| 增量更新 | diff 旧文件,避免无谓重写触发 clangd 重建索引 |
| `mcpp compile-commands` 子命令 | 单独生成,不依赖完整 build |
| per-target 过滤 | 函数参数加 target filter |
| `.clangd` 配置 | `mcpp init` 可选生成 |
