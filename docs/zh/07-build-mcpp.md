# `build.mcpp` —— 原生构建程序

[English](../07-build-mcpp.md) | **简体中文**

绝大多数工程只需要 `mcpp.toml`。当你需要构建期逻辑——探测主机、生成源码、依据环境
决定某个编译开关——就在工程根目录放一个 `build.mcpp`。它是 mcpp 版的 Zig `build.zig`
/ Cargo `build.rs`,但用 **C++** 编写:不引入第二种语言,而且 mcpp 自己吃自己的狗粮。

mcpp 用你的工具链编译 `build.mcpp`,并在主构建**之前**运行它。程序通过向 stdout 打印
`mcpp:` 指令与 mcpp 通信,这些指令会增补本次构建。

## 快速示例

```cpp
// build.mcpp
#include <cstdio>
#include <fstream>

int main() {
    // 生成一份源码,主构建会编译 + 链接它。
    std::ofstream("src/generated.cpp") << "const char* banner() { return \"hi\"; }\n";

    std::puts("mcpp:generated=src/generated.cpp");   // 加入构建
    std::puts("mcpp:cxxflag=-DHAVE_BANNER=1");        // 为所有 C++ TU 定义宏

    if (std::getenv("USE_FAST")) std::puts("mcpp:cxxflag=-DFAST_PATH=1");
    std::puts("mcpp:rerun-if-env-changed=USE_FAST");  // USE_FAST 变化时重跑我
    return 0;
}
```

```bash
mcpp build      # 编译 + 运行 build.mcpp,然后构建工程
```

## 指令

把这些打印到 stdout(每行一条)。任何不以 `mcpp:` 开头的行都会被忽略,因此你可以
自由打印诊断日志。

| 指令 | 作用 |
|---|---|
| `mcpp:cxxflag=<flag>`              | 给 C++ 编译追加 `<flag>` |
| `mcpp:cflag=<flag>`                | 给 C 编译追加 `<flag>` |
| `mcpp:link-lib=<name>`             | 链接 `-l<name>` |
| `mcpp:link-search=<dir>`           | 增加库搜索目录(`-L`;相对路径按工程根目录解析) |
| `mcpp:cfg=<name>`                  | 为 C 与 C++ 同时定义 `-D<name>` |
| `mcpp:generated=<path>`            | 把生成的源码(相对工程根目录)加入构建 |
| `mcpp:rerun-if-changed=<path>`     | 该文件变化时重跑 `build.mcpp` |
| `mcpp:rerun-if-env-changed=<VAR>`  | 该环境变量变化时重跑 `build.mcpp` |

程序**请求**构建边(开关、库、源码),它**不能**新增注册表依赖——请把依赖图保持在
`mcpp.toml` 里声明式管理(包括平台条件依赖 `[target.'cfg(...)'.dependencies]`)。
`build.mcpp` 用于*叶子*决策:开关、代码生成、链接需求。

## 增量:声明输入(避免无谓重跑)

mcpp **不会**每次构建都重跑 `build.mcpp`。它会缓存程序产出的指令,只有当它依赖的东西
变化时才重跑:

- `build.mcpp` 源码本身,
- 工具链,
- 任何用 `rerun-if-changed` 声明的文件,
- 任何用 `rerun-if-env-changed` 声明的环境变量,
- (或某个 `generated` 产物丢失了)。

所以请**声明你的输入**:如果程序读了 `config.h` 或 `USE_FAST` 变量,就分别 emit
`mcpp:rerun-if-changed=config.h` / `mcpp:rerun-if-env-changed=USE_FAST`。这用一份明确的
输入/输出契约取代了过去「进程退出码为 0 就当成功」的猜测——让增量构建保持正确。

无变化时你会看到 `build.mcpp up to date (cached)`;否则是 `build.mcpp compiling` /
`running`。

## 说明与限制

- **在主机上运行。** `build.mcpp` 用主机工具链编译并运行。在交叉构建
  (`mcpp build --target <triple>`)下目前会**跳过并给出警告**(主机工具链交叉是计划中的
  后续项)。要按目标平台门控*依赖*,请改用 `[target.'cfg(...)']` 表——它们按解析后的目标
  求值。参见 [05 - mcpp.toml 工程文件指南](05-mcpp-toml.md)。
- **当前工作目录是工程根目录**,因此相对路径(`src/generated.cpp`)会落在你预期的位置。
- `build.mcpp` 非零退出会中止构建并打印其输出。
